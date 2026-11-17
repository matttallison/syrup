// DS-1000 — TR-style drum machine for Nintendo 2DS / 3DS (homebrew)
// Soul Provider Records
//
// Sound: 16 synthesised 808/909-style voices (plus mic samples). The core-1 worker renders each
// track's hit into a sample only when its sound changes; the DSP chip plays the hits on 16
// channels, with each channel's own biquad as the track filter, its rate as TUNE and its mix
// as LEVEL / PAN / DELAY send. Every channel goes through DSP aux bus 0 (master chain: DJ filter
// + drive) and aux bus 1 (ping-pong delay), using Syrup's copy of the sound driver.
// The sequencer clock runs in the DSP frame callback (every 160 samples, ~4.9 ms).

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>

#ifndef DS_VER
#define DS_VER "dev"
#endif

#define DSPRATE 32728.0f
#define NT 16
#define NS 16
#define NPAT 16
#define MAXLEN 65536
#define MICBYTES 0x10000
#define MICRATE 16364.0f
#define SAVEPATH "sdmc:/3ds/ds1000/ds1000.sav"

// ---------------------------------------------------------------- instruments
enum { BD, SD, LT, MT, HT, RS, CP, CH, OH, CC, RC, CB, B9, S9, CL, MA };
static const char* TN[NT] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CC", "RC", "CB", "BD9", "SD9", "CL", "MA"};
static const char* TLONG[NT] = {"808 KICK", "808 SNARE", "LOW TOM", "MID TOM", "HI TOM", "RIMSHOT", "HANDCLAP", "CLOSED HAT",
	"OPEN HAT", "CRASH", "RIDE", "COWBELL", "909 KICK", "909 SNARE", "CLAVES", "MARACAS"};

// per-track sound parameters (kit, shared by all patterns), all 0..1
enum { P_TUNE, P_DECAY, P_TONE, P_LEVEL, P_PAN, P_DLY, P_DRIVE, P_LASTX, P_FTYPE, P_CUT, P_RES, P_BITS, P_SR, P_START, P_REV, P_SRC, NP };
// master parameters
enum { M_BPM, M_SWING, M_ACC, M_ROLL, M_DTIME, M_DFB, M_FILT, M_DRIVE, NM };

typedef struct { u8 on[NS], acc[NS], prob[NS], sub[NS]; u8 len, pad[3]; } Track;
typedef struct { Track t[NT]; } Pattern;

static Pattern gPat[NPAT];
static float gKp[NT][NP];
static float gMp[NM];
static u8 gMute[NT];
static volatile int gCur = 0, gQueued = 0;

static const char* FT_NAMES[6] = {"OFF", "LP", "HP", "BP", "NOTCH", "PEAK"};
static const int BITS_V[6] = {16, 12, 10, 8, 6, 4};
static const char* ROLL_NAMES[5] = {"OFF", "1/8", "1/16", "1/32", "1/64"};
static const float ROLL_STEPS[5] = {0, 2, 1, 0.5f, 0.25f};
static const u8 ROLL_SUB[5] = {1, 1, 1, 2, 4};
static const char* DT_NAMES[5] = {"1/16", "1/8", "3/16", "1/4", "3/8"};
static const float DT_STEPS[5] = {1, 2, 3, 4, 6};

static int idx(float v, int n) { int i = (int)(v * (n - 0.01f)); return i < 0 ? 0 : i >= n ? n - 1 : i; }
static float bpmOf(void) { return roundf(60 + gMp[M_BPM] * 140); }

static char gMsg[64] = ""; static float gMsgT = 0;
static void say(const char* m) { snprintf(gMsg, sizeof(gMsg), "%s", m); gMsgT = 3.0f; }

// ---------------------------------------------------------------- DSP helpers (rendering)
typedef struct { float a1, a2, a3, k, ic1, ic2; } Svf;
static void svfSet(Svf* s, float fc, float q) {
	if (fc > DSPRATE * 0.45f) fc = DSPRATE * 0.45f;
	if (fc < 20) fc = 20;
	float g = tanf(3.14159265f * fc / DSPRATE);
	s->k = 1.0f / q; s->a1 = 1.0f / (1.0f + g * (g + s->k)); s->a2 = g * s->a1; s->a3 = g * s->a2;
}
static inline void svfRun(Svf* s, float x, float* lp, float* bp, float* hp) {
	float v3 = x - s->ic2, v1 = s->a1 * s->ic1 + s->a2 * v3, v2 = s->ic2 + s->a2 * s->ic1 + s->a3 * v3;
	s->ic1 = 2 * v1 - s->ic1; s->ic2 = 2 * v2 - s->ic2;
	*lp = v2; *bp = v1; *hp = x - s->k * v1 - v2;
}
static u32 gRng = 0x9E3779B9;
static inline float nz(void) { gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5; return (s32)gRng * (1.0f / 2147483648.0f); }
static inline float fsin(float p) {   // sin(2*pi*p), p in [0,1)
	float x = p * 2 - 1, y = 4 * x * (1 - fabsf(x));
	y = 0.225f * (y * fabsf(y) - y) + y;
	return -y;
}
static inline float adv(float* ph, float f) { *ph += f * (1.0f / DSPRATE); if (*ph >= 1) *ph -= (int)*ph; return *ph; }
static inline float sq(float p) { return p < 0.5f ? 1.0f : -1.0f; }
static inline float dk(float sec) { if (sec < 0.0005f) sec = 0.0005f; return expf(-1.0f / (sec * DSPRATE)); }

static const float M808[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

// one hit of instrument `type` into out[], returns its length
static int synth(int type, float dc, float to, float* out, int max) {
	float dm = powf(3.0f, (dc - 0.5f) * 2.0f);
	float ph[6] = {0}, lo, bp, hi, lo2, bp2, hi2;
	Svf f1 = {0}, f2 = {0};
	int n = 0;
	switch (type) {
	case BD: {
		float amp = 1, ak = dk(0.5f * dm), pit = 1, pk = dk(0.009f), cl = 1, ck = dk(0.0025f), clk = 0.1f + to * 0.7f;
		svfSet(&f1, 2500 + to * 3000, 0.7f);
		for (n = 0; n < max; n++) {
			float s = fsin(adv(&ph[0], 49 * (1 + 1.6f * pit))) * amp;
			svfRun(&f1, nz(), &lo, &bp, &hi); s += lo * cl * clk;
			pit *= pk; amp *= ak; cl *= ck;
			out[n] = s;
			if (amp < 0.0003f && n > 400) break;
		}
	} break;
	case B9: {
		float amp = 1, ak = dk(0.38f * dm), pit = 1, pk = dk(0.022f), cl = 1, ck = dk(0.004f);
		svfSet(&f1, 1500, 0.7f);
		for (n = 0; n < max; n++) {
			float s = fsin(adv(&ph[0], 52 * (1 + 4.0f * pit))) * 1.8f;
			s = s / (1 + fabsf(s)) * 1.3f * amp;
			svfRun(&f1, nz(), &lo, &bp, &hi); s += hi * cl * (0.15f + to * 0.8f);
			pit *= pk; amp *= ak; cl *= ck;
			out[n] = s;
			if (amp < 0.0003f && n > 400) break;
		}
	} break;
	case SD: {
		float amp = 1, ak = dk(0.11f * dm), na = 1, nk = dk(0.17f * dm), pit = 1, pk = dk(0.01f);
		svfSet(&f1, 1800, 0.7f);
		for (n = 0; n < max; n++) {
			float s = (fsin(adv(&ph[0], 185 * (1 + 0.3f * pit))) * 0.55f + fsin(adv(&ph[1], 330)) * 0.45f) * amp * (1.0f - to * 0.4f);
			svfRun(&f1, nz(), &lo, &bp, &hi); s += hi * na * (0.25f + to * 0.9f);
			amp *= ak; na *= nk; pit *= pk;
			out[n] = s * 0.85f;
			if (amp + na < 0.0003f) break;
		}
	} break;
	case S9: {
		float amp = 1, ak = dk(0.1f * dm), na = 1, nk = dk(0.24f * dm), pit = 1, pk = dk(0.018f), mix = 0.45f + to * 0.6f;
		svfSet(&f1, 5000 + to * 7000, 0.7f); svfSet(&f2, 600, 0.7f);
		for (n = 0; n < max; n++) {
			float s = (fsin(adv(&ph[0], 190 * (1 + 0.6f * pit))) * 0.6f + fsin(adv(&ph[1], 335 * (1 + 0.3f * pit))) * 0.3f) * amp;
			svfRun(&f1, nz(), &lo, &bp, &hi); svfRun(&f2, lo, &lo2, &bp2, &hi2);
			s += hi2 * na * mix;
			amp *= ak; na *= nk; pit *= pk;
			out[n] = s * 0.85f;
			if (amp + na < 0.0003f) break;
		}
	} break;
	case LT: case MT: case HT: {
		float base = type == LT ? 90 : type == MT ? 128 : 175, dec = type == LT ? 0.42f : type == MT ? 0.36f : 0.3f;
		float amp = 1, ak = dk(dec * dm), pit = 1, pk = dk(0.07f), na = 1, nk = dk(0.03f), mix = to * 0.4f;
		svfSet(&f1, 2500, 0.7f);
		for (n = 0; n < max; n++) {
			float s = fsin(adv(&ph[0], base * (1 + 0.45f * pit))) * amp;
			svfRun(&f1, nz(), &lo, &bp, &hi); s += lo * na * mix;
			amp *= ak; pit *= pk; na *= nk;
			out[n] = s * 0.85f;
			if (amp < 0.0003f) break;
		}
	} break;
	case RS: {
		float amp = 1, ak = dk(0.018f * dm), na = 1, nk = dk(0.004f), mix = 0.3f + to * 0.6f;
		svfSet(&f1, 3000, 1.5f);
		for (n = 0; n < max; n++) {
			float s = (fsin(adv(&ph[0], 500)) * 0.6f + fsin(adv(&ph[1], 1720)) * 0.5f) * amp;
			svfRun(&f1, nz(), &lo, &bp, &hi); s += bp * na * mix;
			amp *= ak; na *= nk;
			out[n] = s * 0.6f;
			if (amp + na < 0.0003f) break;
		}
	} break;
	case CP: {
		int b1 = (int)(0.0105f * DSPRATE), b2 = (int)(0.021f * DSPRATE), tl = (int)(0.032f * DSPRATE);
		float amp = 1, ak = dk(0.0045f), amp2 = 0, ak2 = dk(0.16f * dm);
		svfSet(&f1, 1000 + to * 1400, 1.6f);
		for (n = 0; n < max; n++) {
			if (n == b1 || n == b2) amp = 1;
			if (n == tl) amp2 = 0.75f;
			svfRun(&f1, nz(), &lo, &bp, &hi);
			out[n] = bp * (amp + amp2) * 2.2f;
			amp *= ak; amp2 *= ak2;
			if (n > tl && amp2 < 0.0003f) break;
		}
	} break;
	case CH: case OH: {
		float amp = 1, ak = dk((type == CH ? 0.042f : 0.33f) * dm);
		svfSet(&f1, 7500 + to * 2500, 1.4f); svfSet(&f2, 6000, 0.7f);
		for (n = 0; n < max; n++) {
			float m = 0;
			for (int i = 0; i < 6; i++) m += sq(adv(&ph[i], M808[i] * 1.0f));
			svfRun(&f1, m * (1.0f / 6) + nz() * 0.15f, &lo, &bp, &hi); svfRun(&f2, bp, &lo2, &bp2, &hi2);
			out[n] = hi2 * amp * 2.5f;
			amp *= ak;
			if (amp < 0.0003f) break;
		}
	} break;
	case CC: {
		float amp = 1, ak = dk(1.2f * dm);
		svfSet(&f1, 3800 + to * 3000, 0.7f);
		for (n = 0; n < max; n++) {
			float m = 0;
			for (int i = 0; i < 6; i++) m += sq(adv(&ph[i], M808[i] * 1.12f));
			svfRun(&f1, m * (0.5f / 6) + nz() * 0.6f, &lo, &bp, &hi);
			out[n] = hi * amp * 1.4f;
			amp *= ak;
			if (amp < 0.0003f) break;
		}
	} break;
	case RC: {
		float amp = 1, ak = dk(0.95f * dm);
		svfSet(&f1, 4800 + to * 2500, 1.0f); svfSet(&f2, 7000, 0.7f);
		for (n = 0; n < max; n++) {
			float m = 0;
			for (int i = 0; i < 6; i++) m += sq(adv(&ph[i], M808[i] * 1.45f));
			svfRun(&f1, m * (1.0f / 6), &lo, &bp, &hi); svfRun(&f2, nz(), &lo2, &bp2, &hi2);
			out[n] = (bp + hi2 * 0.12f) * amp * 2.0f;
			amp *= ak;
			if (amp < 0.0003f) break;
		}
	} break;
	case CB: {
		float a = 0.7f, ak = dk(0.012f), b = 0.3f, bk = dk(0.28f * dm);
		svfSet(&f1, 900 + to * 1500, 2.2f);
		for (n = 0; n < max; n++) {
			float m = (sq(adv(&ph[0], 540)) + sq(adv(&ph[1], 800))) * 0.5f;
			svfRun(&f1, m, &lo, &bp, &hi);
			out[n] = bp * (a + b) * 2.0f;
			a *= ak; b *= bk;
			if (a + b < 0.0003f) break;
		}
	} break;
	case CL: {
		float amp = 1, ak = dk(0.028f * dm);
		for (n = 0; n < max; n++) {
			out[n] = (fsin(adv(&ph[0], 2500)) + fsin(adv(&ph[1], 5000)) * to * 0.3f) * amp * 0.7f;
			amp *= ak;
			if (amp < 0.0003f) break;
		}
	} break;
	case MA: {
		float amp = 1, ak = dk(0.045f * dm), att = 0;
		svfSet(&f1, 6000 + to * 3500, 0.7f);
		for (n = 0; n < max; n++) {
			svfRun(&f1, nz(), &lo, &bp, &hi);
			att += (1 - att) * 0.02f;
			out[n] = hi * amp * att * 1.5f;
			amp *= ak;
			if (amp < 0.0003f) break;
		}
	} break;
	}
	return n < max ? n + 1 : max;
}

// ---------------------------------------------------------------- samples (rendered hits)
static s16* gSmp[NT][2];
static volatile int gSmpCur[NT], gSmpLen[NT][2];
static s16* gMicSmp[NT]; static int gMicLen[NT];   // recorded at MICRATE
static float* gTmp;
static volatile u32 gDirty = 0;
static LightEvent gWorkEv;
static volatile int gWorkQuit = 0;

static void markDirty(int t) { __atomic_fetch_or(&gDirty, 1u << t, __ATOMIC_SEQ_CST); LightEvent_Signal(&gWorkEv); }

static int renderMic(int t, float* out, int max) {
	int n0 = gMicLen[t]; if (n0 < 2) return 0;
	int st = (int)(gKp[t][P_START] * 0.9f * n0);
	float dc = gKp[t][P_DECAY], to = gKp[t][P_TONE];
	float ak = dc < 0.95f ? dk(0.02f * powf(100.0f, dc / 0.95f)) : 1.0f, amp = 1, lp = 0, a = 0.05f + to * to * 1.9f;
	if (a > 1) a = 1;
	int n = 0;
	for (; n < max; n++) {
		float pos = st + n * (MICRATE / DSPRATE);
		int i = (int)pos; if (i + 1 >= n0) break;
		float fr = pos - i, x = (gMicSmp[t][i] * (1 - fr) + gMicSmp[t][i + 1] * fr) * (1.0f / 32768.0f);
		lp += (x - lp) * a;
		if (n < 64) amp = n / 64.0f; else if (n == 64) amp = 1;   // de-click the cut
		out[n] = lp * amp;
		if (n >= 64) { amp *= ak; if (amp < 0.0003f) break; }
	}
	return n;
}

static void renderTrack(int t) {
	float* b = gTmp; int n;
	if (gKp[t][P_SRC] >= 0.5f && gMicLen[t] > 0) n = renderMic(t, b, MAXLEN);
	else n = synth(t, gKp[t][P_DECAY], gKp[t][P_TONE], b, MAXLEN);
	while (n > 32 && fabsf(b[n - 1]) < 0.0005f) n--;
	if (n < 1) n = 1;
	if (gKp[t][P_REV] >= 0.5f) for (int i = 0; i < n / 2; i++) { float x = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = x; }
	float d = gKp[t][P_DRIVE];
	if (d > 0.01f) { float g = 1 + d * 8; for (int i = 0; i < n; i++) { float y = b[i] * g; b[i] = y / (1 + fabsf(y)) * 0.95f; } }
	int h = 1 + (int)(gKp[t][P_SR] * 7.99f);   // SP-style sample-rate crush
	if (h > 1) { float hold = 0; for (int i = 0; i < n; i++) { if (i % h == 0) hold = b[i]; b[i] = hold; } }
	int bits = BITS_V[idx(gKp[t][P_BITS], 6)];
	if (bits < 16) { float q = (float)(1 << (bits - 1)); for (int i = 0; i < n; i++) b[i] = floorf(b[i] * q + 0.5f) / q; }
	int nb = !gSmpCur[t];
	s16* o = gSmp[t][nb];
	for (int i = 0; i < n; i++) { float x = b[i] * 32767.0f; o[i] = x > 32767 ? 32767 : x < -32768 ? -32768 : (s16)x; }
	DSP_FlushDataCache(o, n * 2);
	gSmpLen[t][nb] = n;
	__sync_synchronize();
	gSmpCur[t] = nb;
}

static void workerMain(void* a) {
	(void)a;
	while (!gWorkQuit) {
		LightEvent_Wait(&gWorkEv);
		u32 d;
		while (!gWorkQuit && (d = __atomic_exchange_n(&gDirty, 0, __ATOMIC_SEQ_CST)))
			for (int t = 0; t < NT; t++) if (d & (1u << t)) renderTrack(t);
	}
}

// ---------------------------------------------------------------- DSP playback
static int gAudioOK = 0, gBusOK = 0, gBusFail = 0;
static volatile int gBusSeen = 0;
static ndspWaveBuf gWB[NT][2]; static int gWBi[NT];
static volatile float gFlash[NT];
static volatile float gBend = 0;   // semitones (gyro twist)
static int gOutMode = 2, gHeadphones = 0;
static const char* OUT_NAMES[3] = {"STEREO", "SURROUND", "AUTO"};

static void outputApply(void) {
	if (!gAudioOK) return;
	int sur = gOutMode == 1 || (gOutMode == 2 && gHeadphones);
	ndspSetOutputMode(sur ? NDSP_OUTPUT_SURROUND : NDSP_OUTPUT_STEREO);
	ndspSurroundSetDepth(0x5FFF);
	ndspSetClippingMode(NDSP_CLIP_SOFT);
}

static void filtApply(int t) {
	if (!gAudioOK) return;
	int ty = idx(gKp[t][P_FTYPE], 6);
	if (!ty) { ndspChnIirBiquadSetEnable(t, false); return; }
	float f = 30.0f * powf(470.0f, gKp[t][P_CUT]), r = gKp[t][P_RES], q = 0.5f + r * r * 9.5f;
	if (f > 14000) f = 14000;
	bool ok = true;
	switch (ty) {
	case 1: ok = ndspChnIirBiquadSetParamsLowPassFilter(t, f, q); break;
	case 2: ok = ndspChnIirBiquadSetParamsHighPassFilter(t, f, q); break;
	case 3: ok = ndspChnIirBiquadSetParamsBandPassFilter(t, f, q * 0.5f + 0.5f); break;
	case 4: ok = ndspChnIirBiquadSetParamsNotchFilter(t, f, q * 0.3f + 0.4f); break;
	case 5: {   // peaking boost (RBJ); libctru's own peaking EQ comes out flat
		float w0 = 2 * 3.14159265f * f / DSPRATE, al = sinf(w0) / (2 * (0.7f + r * 3)), A = 2.5f, c = cosf(w0);
		ok = ndspChnIirBiquadSetParamsCustomFilter(t, 1 + al / A, -2 * c, 1 - al / A, 1 + al * A, -2 * c, 1 - al * A);
	} break;
	}
	ndspChnIirBiquadSetEnable(t, ok);
}

static void fire(int t, float vel) {
	if (!gAudioOK) return;
	int c = gSmpCur[t], len = gSmpLen[t][c];
	if (len < 1) return;
	ndspChnWaveBufClear(t);
	if (t == CH) ndspChnWaveBufClear(OH);   // closed hat chokes the open hat
	ndspWaveBuf* wb = &gWB[t][gWBi[t] ^= 1];
	memset(wb, 0, sizeof(*wb));
	wb->data_pcm16 = gSmp[t][c]; wb->nsamples = len; wb->looping = false; wb->status = NDSP_WBUF_DONE;
	float semi = (gKp[t][P_TUNE] - 0.5f) * 24.0f + gBend;
	ndspChnSetRate(t, DSPRATE * powf(2.0f, semi / 12.0f));
	float lev = gKp[t][P_LEVEL], g = vel * lev * lev * 1.6f, pan = gKp[t][P_PAN];
	float pl = pan < 0.5f ? 1 : 2 * (1 - pan), pr = pan > 0.5f ? 1 : 2 * pan;
	float mix[12] = {0};
	if (gBusOK && !gBusFail) { mix[4] = g * pl; mix[5] = g * pr; } else { mix[0] = g * pl; mix[1] = g * pr; }
	mix[8] = mix[9] = g * gKp[t][P_DLY] * 0.9f;
	ndspChnSetMix(t, mix);
	ndspChnWaveBufAdd(t, wb);
	gFlash[t] = vel;
}

// ---------------------------------------------------------------- master chain (aux bus 0) + delay (aux bus 1)
static float gTiltX = 0, gTiltY = 0, gTwist = 0, gShake = 0;
static int gMotion = 0;
static Svf gDjL, gDjR; static float gDjLast = -1; static int gDjMode = 0;
static float* gDlL; static float* gDlR;
#define DLMAX 65536
static int gDlW = 0; static float gDlLp[2];

static void auxMaster(void* data, int n, void* samples[4]) {
	(void)data;
	s32* L = (s32*)samples[0]; s32* R = (s32*)samples[1];
	float v = gMp[M_FILT] + (gMotion ? gTiltX * 0.45f : 0);
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	if (fabsf(v - gDjLast) > 0.002f) {
		gDjLast = v;
		if (v < 0.47f) { gDjMode = 1; float f = 18000.0f * powf(0.008f, (0.47f - v) / 0.47f); svfSet(&gDjL, f, 1.0f); svfSet(&gDjR, f, 1.0f); }
		else if (v > 0.53f) { gDjMode = 2; float f = 20.0f * powf(200.0f, (v - 0.53f) / 0.47f); svfSet(&gDjL, f, 1.0f); svfSet(&gDjR, f, 1.0f); }
		else gDjMode = 0;
	}
	float d = gMp[M_DRIVE], g = 1 + d * 7;
	int seen = 0;
	for (int s = 0; s < n; s++) {
		if (L[s] | R[s]) seen = 1;
		float l = L[s] * (1.0f / 32768.0f), r = R[s] * (1.0f / 32768.0f), lo, bp, hi;
		if (gDjMode) {
			svfRun(&gDjL, l, &lo, &bp, &hi); l = gDjMode == 1 ? lo : hi;
			svfRun(&gDjR, r, &lo, &bp, &hi); r = gDjMode == 1 ? lo : hi;
		}
		if (d > 0.01f) { float a = l * g, b = r * g; l = a / (1 + fabsf(a)) * 0.9f; r = b / (1 + fabsf(b)) * 0.9f; }
		float yl = l * 32768.0f, yr = r * 32768.0f;
		if (yl > 2e9f) yl = 2e9f;
		if (yl < -2e9f) yl = -2e9f;
		if (yr > 2e9f) yr = 2e9f;
		if (yr < -2e9f) yr = -2e9f;
		L[s] = (s32)yl; R[s] = (s32)yr;
	}
	memset(samples[2], 0, n * 4); memset(samples[3], 0, n * 4);
	if (seen) gBusSeen = 1;
}
static void auxDelay(void* data, int n, void* samples[4]) {
	(void)data;
	s32* L = (s32*)samples[0]; s32* R = (s32*)samples[1];
	float spb = DSPRATE * 60.0f / (bpmOf() * 4.0f);
	int d = (int)(spb * DT_STEPS[idx(gMp[M_DTIME], 5)]); if (d >= DLMAX) d = DLMAX - 1;
	float fb = gMp[M_DFB] * 0.92f, ig = 1.0f + (gMotion && gTiltY > 0 ? gTiltY * 2.5f : 0);
	for (int s = 0; s < n; s++) {
		float in = (L[s] + R[s]) * (0.5f / 32768.0f) * ig;
		int r = gDlW - d; if (r < 0) r += DLMAX;
		float yl = gDlL[r], yr = gDlR[r];
		gDlLp[0] += (yr - gDlLp[0]) * 0.5f; gDlLp[1] += (yl - gDlLp[1]) * 0.5f;
		gDlL[gDlW] = in + gDlLp[0] * fb;   // ping-pong
		gDlR[gDlW] = gDlLp[1] * fb;
		if (++gDlW >= DLMAX) gDlW = 0;
		L[s] = (s32)(yl * 32768.0f * 0.8f); R[s] = (s32)(yr * 32768.0f * 0.8f);
	}
	memset(samples[2], 0, n * 4); memset(samples[3], 0, n * 4);
}
static void busInit(void) {
	gDlL = (float*)calloc(DLMAX, sizeof(float)); gDlR = (float*)calloc(DLMAX, sizeof(float));
	if (!gDlL || !gDlR) return;
	ndspAuxSetCallback(0, auxMaster, NULL); ndspAuxSetCallback(1, auxDelay, NULL);
	for (int i = 0; i < 2; i++) { ndspAuxSetFrontBypass(i, false); ndspAuxSetVolume(i, 1.0f); ndspAuxSetEnable(i, true); }
	gBusOK = 1;
}

// ---------------------------------------------------------------- sequencer (DSP frame callback)
static volatile int gPlaying = 0, gPlayReq = 0, gTick = -1, gSnap = -1, gBeat = 0;
static int gNextTick = 0; static float gStepCd = 0, gStepLen = 1;
static int gSubLeft[NT]; static float gSubCd[NT], gSubInt[NT], gSubVel[NT];
static volatile int gPos[NT];
#define TQ 64
typedef struct { u8 t; u8 rec; float vel; } TapEv;
static TapEv gTapQ[TQ]; static volatile int gTapW = 0, gTapR = 0;
static volatile int gRollT = -1; static float gRollCd = 0;
static int gModeTap = 0;

static float spbNow(void) { return DSPRATE * 60.0f / (bpmOf() * 4.0f); }
static float velN(void) { return 1.0f - 0.5f * gMp[M_ACC]; }

static void recordHit(int t) {
	int target = gStepCd > gStepLen * 0.5f ? gTick : gTick + 1;   // nearest step
	if (target < 0) target = 0;
	Track* tr = &gPat[gCur].t[t];
	int len = tr->len ? tr->len : 16, p = target % len;
	tr->on[p] = 1; tr->prob[p] = 0;
	int r = idx(gMp[M_ROLL], 5);
	tr->sub[p] = gRollT == t ? ROLL_SUB[r] : 1;
}
static void hit(int t, float vel, int rec) {
	fire(t, vel);
	if (rec && gPlaying) recordHit(t);
}

static void doStep(void) {
	int tick = gNextTick++;
	float spb = spbNow(), sw = gMp[M_SWING] * 0.5f;
	gStepLen = spb * ((tick & 1) ? 1 - sw : 1 + sw);
	if (tick % 16 == 0 && gQueued != gCur) gCur = gQueued;
	Pattern* p = &gPat[gCur];
	for (int t = 0; t < NT; t++) {
		Track* tr = &p->t[t];
		int len = tr->len ? tr->len : 16, pos = tick % len;
		gPos[t] = pos;
		if (!tr->on[pos] || gMute[t]) continue;
		static const float PR[4] = {1.0f, 0.75f, 0.5f, 0.25f};
		if (tr->prob[pos] && (gRng = gRng * 1664525u + 1013904223u, (gRng >> 8) * (1.0f / 16777216.0f)) >= PR[tr->prob[pos] & 3]) continue;
		float v = tr->acc[pos] ? 1.0f : velN();
		fire(t, v);
		int r = tr->sub[pos];
		if (r > 1) { gSubLeft[t] = r - 1; gSubInt[t] = gStepLen / r; gSubCd[t] = gSubInt[t]; gSubVel[t] = v * 0.85f; }
		else gSubLeft[t] = 0;
	}
	if (tick % 4 == 0) gBeat = tick % 16 == 0 ? 2 : 1;
	gTick = tick;
	gStepCd += gStepLen;
}

static void seqTick(void* data) {
	(void)data;
	const float F = 160;
	while (gTapR != gTapW) { TapEv e = gTapQ[gTapR]; gTapR = (gTapR + 1) % TQ; hit(e.t, e.vel, e.rec); }
	int rt = gRollT;
	if (rt >= 0) {
		gRollCd -= F;
		if (gRollCd <= 0) { hit(rt, velN(), gModeTap); gRollCd += spbNow() * ROLL_STEPS[idx(gMp[M_ROLL], 5)]; if (gRollCd < F) gRollCd = F; }
	}
	if (gPlayReq == 1) { gPlayReq = 0; gPlaying = 1; gNextTick = 0; gStepCd = 0; gTick = -1; }
	if (gPlayReq == 2) { gPlayReq = 0; gPlaying = 0; for (int t = 0; t < NT; t++) gSubLeft[t] = 0; }
	if (!gPlaying) return;
	if (gSnap >= 0) { int s = gSnap; gSnap = -1; gNextTick = gNextTick - (gNextTick % 16) + s; gStepCd = 0; }
	gStepCd -= F;
	int guard = 0;
	while (gStepCd <= 0 && guard++ < 4) doStep();
	for (int t = 0; t < NT; t++)
		if (gSubLeft[t] > 0 && (gSubCd[t] -= F) <= 0) { fire(t, gSubVel[t]); gSubLeft[t]--; gSubCd[t] += gSubInt[t]; }
}

static void tapPush(int t, float vel, int rec) {
	int n = (gTapW + 1) % TQ; if (n == gTapR) return;
	gTapQ[gTapW].t = t; gTapQ[gTapW].vel = vel; gTapQ[gTapW].rec = rec;
	__sync_synchronize(); gTapW = n;
}
static void rollStart(int t) {
	int r = idx(gMp[M_ROLL], 5); if (!r) return;
	gRollCd = spbNow() * ROLL_STEPS[r]; gRollT = t;
}

// ---------------------------------------------------------------- sensors, beat light (as in Syrup)
static float gTiltCX = 0, gTiltCY = 0; static int gTiltCal = 0;
static int gMicOK = 0; static u8* gMicBuf = NULL; static u32 gMicSize = 0;
static int gBeatLight = 0, gLcdOK = 0, gLcdFlash = 0; static u32 gBrightTop = 3, gBrightBot = 3; static float gLightT = 0;

static void sensorsInit(void) {
	osSetSpeedupEnable(true);   // 804 MHz on New 3DS / New 2DS
	HIDUSER_EnableAccelerometer(); HIDUSER_EnableGyroscope();
	gMicBuf = (u8*)memalign(0x1000, MICBYTES);
	if (gMicBuf && R_SUCCEEDED(micInit(gMicBuf, MICBYTES))) {
		gMicSize = micGetSampleDataSize();
		gMicOK = R_SUCCEEDED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, gMicSize, true));
	}
	if (R_SUCCEEDED(gspLcdInit())) {
		gLcdOK = 1;
		GSPLCD_GetBrightness(GSPLCD_SCREEN_TOP, &gBrightTop); GSPLCD_GetBrightness(GSPLCD_SCREEN_BOTTOM, &gBrightBot);
		if (gBrightTop < 1 || gBrightTop > 5) gBrightTop = 3;
		if (gBrightBot < 1 || gBrightBot > 5) gBrightBot = 3;
	}
}
static void sensorsExit(void) {
	if (gMicOK) MICU_StopSampling();
	if (gMicBuf) { micExit(); free(gMicBuf); }
	HIDUSER_DisableAccelerometer(); HIDUSER_DisableGyroscope();
	if (gLcdOK) { GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, gBrightTop); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, gBrightBot); gspLcdExit(); }
}
static float clamp1(float x) { return x > 1 ? 1 : x < -1 ? -1 : x; }
static void sensorsUpdate(float dt) {
	accelVector a; hidAccelRead(&a);
	float ax = a.x / 512.0f, ay = atan2f((float)a.z, (float)a.y);
	if (gTiltCal < 20) { gTiltCX += (ax - gTiltCX) * 0.3f; gTiltCY += (ay - gTiltCY) * 0.3f; gTiltCal++; }
	float tx = (ax - gTiltCX) * 2.2f, ty = (ay - gTiltCY) * 1.6f;
	if (ty > 3.14159f) ty -= 6.28318f;
	if (ty < -3.14159f) ty += 6.28318f;
	if (fabsf(tx) < 0.08f) tx = 0;
	if (fabsf(ty) < 0.08f) ty = 0;
	float k = 1.0f - expf(-dt / 0.05f);
	gTiltX += (clamp1(tx) - gTiltX) * k; gTiltY += (clamp1(ty) - gTiltY) * k;
	angularRate g; hidGyroRead(&g);
	float tw = g.z / 1800.0f; if (fabsf(tw) < 0.05f) tw = 0;
	gTwist += (clamp1(tw) - gTwist) * (1.0f - expf(-dt / 0.06f));
	static accelVector pa; static int have = 0;
	if (have) {
		float d = (fabsf((float)a.x - pa.x) + fabsf((float)a.y - pa.y) + fabsf((float)a.z - pa.z)) / 600.0f;
		if (d > 1) d = 1;
		gShake += (d - gShake) * (d > gShake ? 0.7f : 0.05f);
	}
	pa = a; have = 1;
	gBend = gMotion ? gTwist * 7.0f : 0;   // twist bends every hit up to +/-7 semitones
	static float hpT = 0;
	if ((hpT -= dt) <= 0) {
		hpT = 0.5f; bool hp = false;
		if (gAudioOK && R_SUCCEEDED(DSP_GetHeadphoneStatus(&hp))) { int h = hp ? 1 : 0; if (h != gHeadphones) { gHeadphones = h; if (gOutMode == 2) outputApply(); } }
	}
}
static void beatLight(float dt) {
	if (!gLcdOK) return;
	int b = gBeat; gBeat = 0;
	if (b && gBeatLight) {
		u32 up = b == 2 ? 2 : 1;
		u32 t = gBrightTop + up <= 5 ? gBrightTop + up : gBrightTop - up, bt = gBrightBot + up <= 5 ? gBrightBot + up : gBrightBot - up;
		GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, t); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, bt);
		gLcdFlash = 1; gLightT = 0.06f;
	}
	if (gLcdFlash && (gLightT -= dt) <= 0) { GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, gBrightTop); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, gBrightBot); gLcdFlash = 0; }
}

// mic sampling: record 2 s into the selected track, trimmed to the first sound
static float gMicRecT = 0; static int gMicRecTrack = 0;
static void micRecStart(int t) { if (!gMicOK) { say("MIC NOT AVAILABLE"); return; } gMicRecT = 2.0f; gMicRecTrack = t; }
static void micRecTick(float dt) {
	if (gMicRecT <= 0) return;
	gMicRecT -= dt;
	char m[48]; snprintf(m, sizeof(m), "SAMPLING %s... %.1f", TN[gMicRecTrack], gMicRecT > 0 ? gMicRecT : 0); say(m);
	if (gMicRecT > 0) return;
	u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2;
	const s16* src = (const s16*)gMicBuf;
	int peak = 0;
	for (u32 i = 0; i < N; i++) { int v = abs(src[(end + i) % N]); if (v > peak) peak = v; }
	if (peak < 600) { say("NOTHING HEARD - TRY AGAIN"); return; }
	u32 st = 0;
	while (st < N && abs(src[(end + st) % N]) < peak / 8) st++;
	if (st > 80) st -= 80; else st = 0;   // a little pre-roll
	int len = (int)(N - st); if (len > (int)(MICRATE * 1.6f)) len = (int)(MICRATE * 1.6f);
	s16* d = (s16*)malloc(len * 2); if (!d) return;
	float gain = 26000.0f / peak;
	for (int i = 0; i < len; i++) { float v = src[(end + st + i) % N] * gain; d[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }
	int t = gMicRecTrack;
	s16* old = gMicSmp[t]; gMicLen[t] = 0; gMicSmp[t] = d; gMicLen[t] = len; free(old);
	gKp[t][P_SRC] = 1; gKp[t][P_START] = 0;
	markDirty(t);
	snprintf(m, sizeof(m), "SAMPLED INTO %s", TN[t]); say(m);
}

// ---------------------------------------------------------------- link (Syrup-compatible: local wireless + infrared)
static const char* LINK_NAMES[5] = {"OFF", "WIFI HOST", "WIFI JOIN", "IR SEND", "IR RECEIVE"};
static int gLink = 0, gLinkUp = 0, gLinkPeers = 0;
static udsBindContext gUdsBind;
static u32* gIrMem = NULL; static Thread gIrThread = NULL;
static volatile int gIrQuit = 0, gIrStep = -1, gIrBpm = 0;
typedef struct { u8 magic, step; u16 bpm; } LinkPkt;
#define LINK_COMM 0x53595255
static void irRecvMain(void* a) {
	(void)a;
	while (!gIrQuit) {
		u8 b[2]; u32 n = 0;
		if (R_SUCCEEDED(iruRecvData(b, 2, 0, &n, true)) && n == 2 && (b[0] & 0xF0) == 0xA0) { gIrBpm = 40 + b[1]; gIrStep = b[0] & 15; }
		else svcSleepThread(2000000);
	}
}
static void linkStop(int was) {
	if (was == 1 || was == 2) { if (gLinkUp) { if (was == 1) udsDestroyNetwork(); else udsDisconnectNetwork(); udsUnbind(&gUdsBind); } udsExit(); }
	gIrQuit = 1;
	if (gIrMem) { iruExit(); free(gIrMem); gIrMem = NULL; }
	if (gIrThread) { if (R_SUCCEEDED(threadJoin(gIrThread, 1000000000ULL))) threadFree(gIrThread); gIrThread = NULL; }
	gLinkUp = 0; gLinkPeers = 0;
}
static void linkSet(int want) {
	linkStop(gLink); gLink = want;
	if (want == 1 || want == 2) {
		if (R_FAILED(udsInit(0x3000, NULL))) { say("LOCAL WIRELESS NOT AVAILABLE"); gLink = 0; return; }
		udsNetworkStruct net;
		if (want == 1) {
			udsGenerateDefaultNetworkStruct(&net, LINK_COMM, 0, 4);
			if (R_SUCCEEDED(udsCreateNetwork(&net, "syrup", 5, &gUdsBind, 1, UDS_DEFAULT_RECVBUFSIZE))) { gLinkUp = 1; say("HOSTING - SYRUP / DS-1000 CAN JOIN"); }
			else say("COULD NOT START HOSTING");
		} else {
			static u8 scan[0x4000]; udsNetworkScanInfo* nets = NULL; size_t total = 0;
			if (R_SUCCEEDED(udsScanBeacons(scan, sizeof(scan), &nets, &total, LINK_COMM, 0, NULL, false)) && total > 0 &&
				R_SUCCEEDED(udsConnectNetwork(&nets[0].network, "syrup", 5, &gUdsBind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, 1, UDS_DEFAULT_RECVBUFSIZE))) { gLinkUp = 1; say("JOINED - FOLLOWING THE HOST"); }
			else say("NO HOST FOUND - TAP JOIN AGAIN");
			free(nets);
		}
	} else if (want == 3 || want == 4) {
		gIrMem = (u32*)memalign(0x1000, 0x1000);
		if (!gIrMem || R_FAILED(iruInit(gIrMem, 0x1000))) { say("INFRARED NOT AVAILABLE"); free(gIrMem); gIrMem = NULL; gLink = 0; return; }
		gLinkUp = 1;
		if (want == 4) { gIrQuit = 0; gIrThread = threadCreate(irRecvMain, NULL, 0x2000, 0x30, -2, false); say("IR: POINT THE CONSOLES AT EACH OTHER"); }
		else say("IR SENDING TEMPO");
	}
}
static void linkFollow(int step, int bpm) {
	if (bpm >= 60 && bpm <= 200) gMp[M_BPM] = (bpm - 60) / 140.0f;
	if (step >= 0) { if (!gPlaying) gPlayReq = 1; gSnap = step & 15; }
}
static void linkTick(float dt) {
	if (!gLinkUp) return;
	static int lastTick = -2; static float hb = 0;
	int tk = gPlaying ? gTick : -1, send = 0;
	if (tk != lastTick && tk >= 0) send = 1;
	if ((hb -= dt) <= 0) { hb = 0.5f; send = 1; }
	lastTick = tk;
	int bpm = (int)bpmOf(), step = tk < 0 ? -1 : tk % 16;
	if (gLink == 1 && send) { LinkPkt p = {'S', (u8)(step < 0 ? 255 : step), (u16)bpm}; udsSendTo(UDS_BROADCAST_NETWORKNODEID, 1, UDS_SENDFLAG_Default, &p, sizeof(p)); }
	if (gLink == 3 && send) { u8 b[2] = {(u8)(0xA0 | (step < 0 ? 15 : step)), (u8)(bpm - 40)}; iruSendData(b, 2, false); }
	if (gLink == 2) {
		LinkPkt p; size_t got = 0; u16 src;
		while (R_SUCCEEDED(udsPullPacket(&gUdsBind, &p, sizeof(p), &got, &src)) && got == sizeof(p)) { if (p.magic == 'S') linkFollow(p.step == 255 ? -1 : p.step, p.bpm); got = 0; }
	}
	if (gLink == 4 && gIrBpm) { int st = gIrStep; gIrStep = -1; linkFollow(st == 15 ? -1 : st, gIrBpm); gIrBpm = 0; }
	if (gLink == 1) { udsConnectionStatus cs; if (R_SUCCEEDED(udsGetConnectionStatus(&cs))) gLinkPeers = cs.total_nodes > 0 ? cs.total_nodes - 1 : 0; }
}

// ---------------------------------------------------------------- patterns, defaults, save
static void setSteps(int p, int t, const char* s) {   // "x" = hit, "X" = accent, "." = rest
	Track* tr = &gPat[p].t[t];
	for (int i = 0; i < NS && s[i]; i++) { tr->on[i] = s[i] == 'x' || s[i] == 'X'; tr->acc[i] = s[i] == 'X'; }
}
static void defaults(void) {
	memset(gPat, 0, sizeof(gPat));
	for (int p = 0; p < NPAT; p++) for (int t = 0; t < NT; t++) { gPat[p].t[t].len = 16; for (int s = 0; s < NS; s++) gPat[p].t[t].sub[s] = 1; }
	static const float PAN[NT] = {.5f, .5f, .3f, .5f, .7f, .4f, .55f, .62f, .62f, .4f, .65f, .35f, .5f, .5f, .6f, .45f};
	for (int t = 0; t < NT; t++) {
		float* k = gKp[t];
		k[P_TUNE] = .5f; k[P_DECAY] = .5f; k[P_TONE] = .5f; k[P_LEVEL] = .75f; k[P_PAN] = PAN[t]; k[P_DLY] = 0; k[P_DRIVE] = 0;
		k[P_FTYPE] = 0; k[P_CUT] = 1; k[P_RES] = .2f; k[P_BITS] = 0; k[P_SR] = 0; k[P_START] = 0; k[P_REV] = 0; k[P_SRC] = 0;
	}
	gKp[CP][P_DLY] = 0.25f; gKp[RS][P_DLY] = 0.3f;
	gMp[M_BPM] = (120 - 60) / 140.0f; gMp[M_SWING] = 0.12f; gMp[M_ACC] = 0.6f; gMp[M_ROLL] = 0.5f;
	gMp[M_DTIME] = 0.5f; gMp[M_DFB] = 0.4f; gMp[M_FILT] = 0.5f; gMp[M_DRIVE] = 0;
	// A: house
	setSteps(0, B9, "X...x...X...x..."); setSteps(0, CP, "....x.......x..."); setSteps(0, CH, "x.x.x.x.x.x.x.x."); setSteps(0, OH, "..x...x...x...x.");
	// B: 808 boom bap
	setSteps(1, BD, "X......x..x....."); setSteps(1, SD, "....X.......X..."); setSteps(1, CH, "x.x.x.x.x.x.x.xx"); setSteps(1, RS, ".......x......x.");
	// C: electro
	setSteps(2, BD, "X.....x...x....."); setSteps(2, SD, "....X.......X..."); setSteps(2, CH, "xxXxxxXxxxXxxxXx"); setSteps(2, CB, "..x.....x..x....");
	setSteps(2, CP, "............x...");
	// D: techno
	setSteps(3, B9, "X...X...X...X..."); setSteps(3, RC, "..x...x...x...x."); setSteps(3, OH, "..x...x...x...x."); setSteps(3, S9, "....x.......x..x");
	setSteps(3, LT, "..........x....."); gPat[3].t[CH].len = 12; setSteps(3, CH, "x.xx.xx.xx.x");
}
static void save(void) {
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/ds1000", 0777);
	FILE* f = fopen(SAVEPATH, "wb"); if (!f) { say("COULD NOT SAVE"); return; }
	u32 hdr[2] = {0x4B315344, 1};
	fwrite(hdr, 4, 2, f); fwrite(gPat, sizeof(gPat), 1, f); fwrite(gKp, sizeof(gKp), 1, f); fwrite(gMp, sizeof(gMp), 1, f); fwrite(gMute, sizeof(gMute), 1, f);
	for (int t = 0; t < NT; t++) { s32 n = gMicSmp[t] ? gMicLen[t] : 0; fwrite(&n, 4, 1, f); if (n) fwrite(gMicSmp[t], 2, n, f); }
	fclose(f); say("SAVED TO /3ds/ds1000/");
}
static void load(void) {
	FILE* f = fopen(SAVEPATH, "rb"); if (!f) return;
	u32 hdr[2] = {0};
	if (fread(hdr, 4, 2, f) == 2 && hdr[0] == 0x4B315344 && hdr[1] == 1 &&
		fread(gPat, sizeof(gPat), 1, f) == 1 && fread(gKp, sizeof(gKp), 1, f) == 1 && fread(gMp, sizeof(gMp), 1, f) == 1 && fread(gMute, sizeof(gMute), 1, f) == 1) {
		for (int t = 0; t < NT; t++) {
			s32 n = 0; if (fread(&n, 4, 1, f) != 1 || n < 0 || n > 200000) break;
			if (n) { gMicSmp[t] = (s16*)malloc(n * 2); if (!gMicSmp[t] || fread(gMicSmp[t], 2, n, f) != (size_t)n) { free(gMicSmp[t]); gMicSmp[t] = NULL; break; } gMicLen[t] = n; }
		}
		say("LOADED");
	} else defaults();
	fclose(f);
}
static int patUsed(int p) { for (int t = 0; t < NT; t++) for (int s = 0; s < NS; s++) if (gPat[p].t[t].on[s]) return 1; return 0; }
static void randomTrack(int t) {
	Track* tr = &gPat[gCur].t[t];
	int busy = t == CH || t == MA || t == RC || t == CB;
	for (int s = 0; s < NS; s++) {
		float p = s % 4 == 0 ? 0.55f : s % 2 == 0 ? 0.3f : 0.14f;
		if (busy) p = s % 2 == 0 ? 0.8f : 0.4f;
		if (t == SD || t == S9 || t == CP) p = (s % 8 == 4) ? 0.9f : 0.08f;
		gRng = gRng * 1664525u + 1013904223u;
		tr->on[s] = ((gRng >> 8) & 0xFFFF) / 65536.0f < p;
		tr->acc[s] = tr->on[s] && s % 4 == 0; tr->prob[s] = 0; tr->sub[s] = 1;
	}
}

// ---------------------------------------------------------------- UI
static C2D_TextBuf gTb;
#define RGB(r, g, b) C2D_Color32(r, g, b, 255)
static void rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0.5f, w, h, c); }
static void frame(float x, float y, float w, float h, u32 c) { rect(x, y, w, 1, c); rect(x, y + h - 1, w, 1, c); rect(x, y, 1, h, c); rect(x + w - 1, y, 1, h, c); }
static void txt(float x, float y, float sc, u32 c, int center, const char* s) {
	C2D_Text t; C2D_TextParse(&t, gTb, s); C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | (center ? C2D_AlignCenter : 0), x, y, 0.5f, sc, sc, c);
}
static const u32 GRP[4][2] = {{0xFF2030E0, 0xFF0A0C48}, {0xFF1880F0, 0xFF082848}, {0xFF20D0F0, 0xFF083848}, {0xFFD0E8F0, 0xFF404448}};   // ABGR: red, orange, yellow, cream
static u32 trackCol(int t) { static const u32 C[4] = {0xFF2848F0, 0xFF2090F8, 0xFF30D8F8, 0xFFD8E8F0}; return C[(t / 4) & 3]; }

enum { MD_STEP, MD_ACC, MD_PROB, MD_SUB, MD_TAP, MD_MUTE };
static const char* MODE_NAMES[6] = {"STEP", "ACC", "PROB", "SUB", "TAP", "MUTE"};
enum { PG_TRK, PG_FLT, PG_MST, PG_HW };
static const char* PAGE_NAMES[4] = {"TRK", "FLT", "MST", "HW"};
static int gSel = 0, gMode = MD_STEP, gPage = PG_TRK, gDrag = -1;

static const char* SL_TRK[8] = {"TUNE", "DECAY", "TONE", "LEVEL", "PAN", "DELAY", "DRIVE", "LAST"};
static const int SL_TRK_P[8] = {P_TUNE, P_DECAY, P_TONE, P_LEVEL, P_PAN, P_DLY, P_DRIVE, P_LASTX};
static const char* SL_FLT[8] = {"FILTER", "CUTOFF", "RESO", "BITS", "RATE", "START", "REVERSE", "SOURCE"};
static const int SL_FLT_P[8] = {P_FTYPE, P_CUT, P_RES, P_BITS, P_SR, P_START, P_REV, P_SRC};
static const char* SL_MST[8] = {"BPM", "SWING", "ACCENT", "ROLL", "DLY TIME", "DLY FDBK", "FILTER", "DRIVE"};

static float slGet(int i) {
	if (gPage == PG_MST) return gMp[i];
	int p = gPage == PG_TRK ? SL_TRK_P[i] : SL_FLT_P[i];
	if (p == P_LASTX) { int l = gPat[gCur].t[gSel].len; return ((l ? l : 16) - 1) / 15.0f; }
	return gKp[gSel][p];
}
static void slSet(int i, float v) {
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	if (gPage == PG_MST) { gMp[i] = v; return; }
	int p = gPage == PG_TRK ? SL_TRK_P[i] : SL_FLT_P[i];
	if (p == P_LASTX) { gPat[gCur].t[gSel].len = 1 + (int)(v * 15.0f + 0.5f); return; }
	if (p == P_REV || p == P_SRC) v = v >= 0.5f ? 1 : 0;
	if (gKp[gSel][p] == v) return;
	gKp[gSel][p] = v;
	if (p == P_DECAY || p == P_TONE || p == P_DRIVE || p == P_BITS || p == P_SR || p == P_START || p == P_REV || p == P_SRC) markDirty(gSel);
	if (p == P_FTYPE || p == P_CUT || p == P_RES) filtApply(gSel);
}
static void slText(int i, char* o, int n) {
	float v = slGet(i);
	if (gPage == PG_MST) {
		switch (i) {
		case M_BPM: snprintf(o, n, "%d", (int)bpmOf()); return;
		case M_SWING: snprintf(o, n, "%d%%", 50 + (int)(v * 25 + 0.5f)); return;
		case M_ROLL: snprintf(o, n, "%s", ROLL_NAMES[idx(v, 5)]); return;
		case M_DTIME: snprintf(o, n, "%s", DT_NAMES[idx(v, 5)]); return;
		case M_FILT: if (v < 0.47f) snprintf(o, n, "LP %d", (int)((0.47f - v) / 0.47f * 100)); else if (v > 0.53f) snprintf(o, n, "HP %d", (int)((v - 0.53f) / 0.47f * 100)); else snprintf(o, n, "OFF"); return;
		}
		snprintf(o, n, "%d", (int)(v * 100 + 0.5f)); return;
	}
	int p = gPage == PG_TRK ? SL_TRK_P[i] : SL_FLT_P[i];
	switch (p) {
	case P_TUNE: snprintf(o, n, "%+d", (int)roundf((v - 0.5f) * 24)); return;
	case P_PAN: if (fabsf(v - 0.5f) < 0.02f) snprintf(o, n, "C"); else snprintf(o, n, "%c%d", v < 0.5f ? 'L' : 'R', (int)(fabsf(v - 0.5f) * 200)); return;
	case P_LASTX: snprintf(o, n, "%d", gPat[gCur].t[gSel].len ? gPat[gCur].t[gSel].len : 16); return;
	case P_FTYPE: snprintf(o, n, "%s", FT_NAMES[idx(v, 6)]); return;
	case P_CUT: { float f = 30.0f * powf(470.0f, v); if (f >= 1000) snprintf(o, n, "%.1fk", f / 1000); else snprintf(o, n, "%d", (int)f); } return;
	case P_BITS: snprintf(o, n, "%d BIT", BITS_V[idx(v, 6)]); return;
	case P_SR: snprintf(o, n, "%.1fk", 32.7f / (1 + (int)(v * 7.99f))); return;
	case P_REV: snprintf(o, n, v >= 0.5f ? "ON" : "OFF"); return;
	case P_SRC: snprintf(o, n, v >= 0.5f ? (gMicLen[gSel] ? "MIC" : "MIC (NONE)") : "SYNTH"); return;
	}
	snprintf(o, n, "%d", (int)(v * 100 + 0.5f));
}

static const char* HW_LABEL[8] = {"MIC SAMPLE", "MOTION", "BEAT LIGHT", "LINK", "OUTPUT", "TILT CENTER", "SAVE", "CLEAR PATTERN"};
static void hwText(int i, char* o, int n) {
	switch (i) {
	case 0: snprintf(o, n, gMicRecT > 0 ? "REC..." : "-> %s", TN[gSel]); return;
	case 1: snprintf(o, n, gMotion ? "ON" : "OFF"); return;
	case 2: snprintf(o, n, gBeatLight ? "ON" : "OFF"); return;
	case 3: snprintf(o, n, "%s", LINK_NAMES[gLink]); return;
	case 4: snprintf(o, n, "%s", OUT_NAMES[gOutMode]); return;
	default: o[0] = 0;
	}
}
static void hwPress(int i) {
	switch (i) {
	case 0: micRecStart(gSel); break;
	case 1: gMotion = !gMotion; gTiltCal = 0; say(gMotion ? "TILT: FILTER / DELAY  TWIST: PITCH  SHAKE: ROLL" : "MOTION OFF"); break;
	case 2: gBeatLight = !gBeatLight; break;
	case 3: linkSet((gLink + 1) % 5); break;
	case 4: gOutMode = (gOutMode + 1) % 3; outputApply(); break;
	case 5: gTiltCal = 0; say("TILT CENTERED"); break;
	case 6: save(); break;
	case 7: { Pattern* p = &gPat[gCur]; for (int t = 0; t < NT; t++) { memset(p->t[t].on, 0, NS); memset(p->t[t].acc, 0, NS); memset(p->t[t].prob, 0, NS); memset(p->t[t].sub, 1, NS); } say("PATTERN CLEARED"); } break;
	}
}

static void drawTop(void) {
	rect(0, 0, 400, 240, RGB(26, 26, 30));
	rect(0, 0, 400, 26, RGB(40, 40, 46));
	txt(6, 2, 0.72f, RGB(240, 120, 30), 0, "DS-1000");
	txt(86, 9, 0.38f, RGB(160, 160, 170), 0, "DRUM MACHINE");
	char b[64];
	snprintf(b, sizeof(b), "%d BPM  SW %d%%", (int)bpmOf(), 50 + (int)(gMp[M_SWING] * 25 + 0.5f));
	txt(190, 6, 0.48f, RGB(230, 230, 230), 0, b);
	if (gPlaying) { rect(340, 6, 4, 14, RGB(60, 220, 90)); txt(350, 6, 0.48f, RGB(60, 220, 90), 0, "PLAY"); }
	else txt(350, 6, 0.48f, RGB(150, 150, 150), 0, "STOP");
	// pattern bank
	for (int p = 0; p < NPAT; p++) {
		float x = 6 + p * 24.3f, y = 29;
		u32 c = p == gCur ? RGB(240, 120, 30) : patUsed(p) ? RGB(80, 80, 90) : RGB(46, 46, 52);
		rect(x, y, 22, 13, c);
		if (p == gQueued && gQueued != gCur) frame(x - 1, y - 1, 24, 15, RGB(255, 255, 255));
		char l[2] = {(char)('A' + p), 0};
		txt(x + 11, y, 0.4f, p == gCur ? RGB(20, 20, 20) : RGB(210, 210, 210), 1, l);
	}
	// grid
	Pattern* pt = &gPat[gCur];
	for (int t = 0; t < NT; t++) {
		float y = 46 + t * 10.2f;
		Track* tr = &pt->t[t];
		int len = tr->len ? tr->len : 16;
		float fl = gFlash[t];
		u32 lab = t == gSel ? RGB(240, 120, 30) : RGB(50 + (int)(fl * 150), 50 + (int)(fl * 150), 56 + (int)(fl * 150));
		rect(2, y, 34, 9, lab);
		txt(19, y - 1.5f, 0.34f, t == gSel ? RGB(20, 20, 20) : gMute[t] ? RGB(120, 60, 60) : RGB(230, 230, 230), 1, gMute[t] ? "--" : TN[t]);
		for (int s = 0; s < NS; s++) {
			float x = 40 + s * 22.5f;
			u32 c;
			if (s >= len) c = RGB(30, 30, 33);
			else if (tr->on[s]) c = tr->acc[s] ? RGB(255, 255, 255) : trackCol(t);
			else c = GRP[s / 4][1];
			if (gMute[t] && tr->on[s] && s < len) c = RGB(90, 70, 70);
			rect(x, y, 20.5f, 8.5f, c);
			if (s < len && tr->on[s] && tr->sub[s] > 1) rect(x + 15, y + 1, 4, 6, RGB(30, 30, 30));
			if (s < len && tr->on[s] && tr->prob[s]) rect(x + 1, y + 6, 6, 2, RGB(30, 30, 30));
			if (gPlaying && s == gPos[t]) frame(x - 1, y - 1, 22.5f, 10.5f, RGB(255, 255, 255));
		}
	}
	// footer
	rect(0, 211, 400, 29, RGB(40, 40, 46));
	snprintf(b, sizeof(b), "%s %s   %s / %s", TN[gSel], TLONG[gSel], MODE_NAMES[gMode], PAGE_NAMES[gPage]);
	txt(6, 212, 0.42f, RGB(240, 120, 30), 0, b);
	if (gMsgT > 0) txt(6, 225, 0.4f, RGB(255, 230, 120), 0, gMsg);
	else {
		snprintf(b, sizeof(b), "%s%s%s%s", gLinkUp ? LINK_NAMES[gLink] : "", gLinkUp && gLink == 1 ? (gLinkPeers ? " +1" : " (WAITING)") : "",
			gMotion ? "  MOTION" : "", gAudioOK ? ((gBusOK && !gBusFail) ? "" : "  FX BUS OFF") : "  NO DSP: RUN DSP1 ONCE");
		txt(6, 225, 0.4f, RGB(150, 200, 255), 0, b);
	}
	snprintf(b, sizeof(b), "v%s", DS_VER); txt(360, 214, 0.34f, RGB(100, 100, 110), 0, b);
}

static void drawBottom(void) {
	rect(0, 0, 320, 240, RGB(22, 22, 26));
	char b[48];
	// instrument row
	for (int i = 0; i < NT; i++) {
		float x = (i % 8) * 40, y = (i / 8) * 24;
		float fl = gFlash[i];
		u32 c = i == gSel ? RGB(240, 120, 30) : gMute[i] ? RGB(70, 30, 30) : RGB(55 + (int)(fl * 120), 55 + (int)(fl * 120), 62 + (int)(fl * 120));
		rect(x + 1, y + 1, 38, 22, c);
		txt(x + 20, y + 4, 0.45f, i == gSel ? RGB(20, 20, 20) : RGB(235, 235, 235), 1, TN[i]);
		if (gMute[i]) txt(x + 34, y + 1, 0.3f, RGB(255, 90, 90), 1, "M");
	}
	// steps / pads
	Track* tr = &gPat[gCur].t[gSel];
	int len = tr->len ? tr->len : 16;
	for (int s = 0; s < NS; s++) {
		float x = (s % 8) * 40, y = 50 + (s / 8) * 40;
		if (gMode == MD_TAP) {   // 16 pads, one per instrument
			float fl = gFlash[s];
			u32 c = s == gSel ? RGB(240, 120, 30) : RGB(60 + (int)(fl * 160), 60 + (int)(fl * 140), 70 + (int)(fl * 120));
			rect(x + 2, y + 2, 36, 36, c);
			txt(x + 20, y + 12, 0.48f, s == gSel ? RGB(20, 20, 20) : RGB(240, 240, 240), 1, TN[s]);
			continue;
		}
		int on = tr->on[s] && s < len;
		u32 c = s >= len ? RGB(34, 34, 38) : on ? GRP[s / 4][0] : GRP[s / 4][1];
		rect(x + 2, y + 2, 36, 36, c);
		if (on && tr->acc[s]) { rect(x + 4, y + 4, 32, 5, RGB(255, 255, 255)); }
		if (on && tr->prob[s]) { static const char* PN[4] = {"", "75%", "50%", "25%"}; txt(x + 20, y + 13, 0.42f, RGB(20, 20, 20), 1, PN[tr->prob[s] & 3]); }
		if (on && tr->sub[s] > 1) { snprintf(b, sizeof(b), "x%d", tr->sub[s]); txt(x + 20, y + 24, 0.42f, RGB(20, 20, 20), 1, b); }
		snprintf(b, sizeof(b), "%d", s + 1); txt(x + 6, y + 26, 0.32f, on ? RGB(40, 40, 40) : RGB(120, 120, 120), 0, b);
		if (gPlaying && s == gPos[gSel]) frame(x + 1, y + 1, 38, 38, RGB(255, 255, 255));
	}
	// tabs
	for (int i = 0; i < 7; i++) {
		float x = i * (320.0f / 7), w = 320.0f / 7 - 2;
		int act = i < 6 ? gMode == i : 0;
		rect(x + 1, 132, w, 22, i == 6 ? RGB(70, 70, 120) : act ? RGB(240, 120, 30) : RGB(52, 52, 58));
		if (i < 6) txt(x + w / 2 + 1, 135, 0.42f, act ? RGB(20, 20, 20) : RGB(220, 220, 220), 1, MODE_NAMES[i]);
		else { snprintf(b, sizeof(b), "%s >", PAGE_NAMES[gPage]); txt(x + w / 2 + 1, 135, 0.42f, RGB(230, 230, 255), 1, b); }
	}
	// sliders / buttons
	for (int i = 0; i < 8; i++) {
		float x = (i % 2) * 160, y = 158 + (i / 2) * 20.5f;
		rect(x + 1, y, 158, 19, RGB(36, 36, 42));
		if (gPage == PG_HW) {
			txt(x + 5, y + 3, 0.4f, RGB(210, 210, 210), 0, HW_LABEL[i]);
			hwText(i, b, sizeof(b)); txt(x + 120, y + 3, 0.4f, RGB(240, 160, 60), 1, b);
			continue;
		}
		const char* lab = gPage == PG_TRK ? SL_TRK[i] : gPage == PG_FLT ? SL_FLT[i] : SL_MST[i];
		txt(x + 4, y + 3, 0.38f, RGB(200, 200, 200), 0, lab);
		float v = slGet(i), bx = x + 56, bw = 98;
		rect(bx, y + 4, bw, 11, RGB(20, 20, 24));
		rect(bx, y + 4, bw * v, 11, gDrag == i ? RGB(255, 160, 60) : RGB(200, 100, 30));
		slText(i, b, sizeof(b)); txt(bx + bw / 2, y + 3, 0.38f, RGB(255, 255, 255), 1, b);
	}
}

// ---------------------------------------------------------------- main
int main(void) {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	gTb = C2D_TextBufNew(4096);

	defaults(); load();
	gTmp = (float*)malloc(MAXLEN * sizeof(float));
	for (int t = 0; t < NT; t++) for (int k = 0; k < 2; k++) gSmp[t][k] = (s16*)linearAlloc(MAXLEN * 2);

	sensorsInit();
	gAudioOK = R_SUCCEEDED(ndspInit());
	if (gAudioOK) {
		ndspSetMasterVol(1.0f);
		outputApply();
		for (int t = 0; t < NT; t++) {
			ndspChnReset(t); ndspChnSetInterp(t, NDSP_INTERP_LINEAR); ndspChnSetFormat(t, NDSP_FORMAT_MONO_PCM16); ndspChnSetRate(t, DSPRATE);
			filtApply(t);
		}
		busInit();
		ndspSetCallback(seqTick, NULL);
	}

	// sound rendering on the second core (Syrup's worker trick)
	LightEvent_Init(&gWorkEv, RESET_ONESHOT);
	APT_SetAppCpuTimeLimit(80);
	s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Thread worker = threadCreate(workerMain, NULL, 64 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, 1, false);
	if (!worker) worker = threadCreate(workerMain, NULL, 64 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, -2, false);
	__atomic_store_n(&gDirty, 0xFFFF, __ATOMIC_SEQ_CST); LightEvent_Signal(&gWorkEv);
	if (!worker) for (int t = 0; t < NT; t++) renderTrack(t);

	hidSetRepeatParameters(18, 4);
	float playT = 0; int shakeRoll = 0;
	u64 last = osGetTime();
	while (aptMainLoop()) {
		u64 now = osGetTime(); float dt = (now - last) / 1000.0f; last = now;
		if (dt > 0.1f) dt = 0.1f;
		hidScanInput();
		u32 kd = hidKeysDown(), kh = hidKeysHeld(), kr = hidKeysDownRepeat();
		if (kd & KEY_START) { save(); break; }
		if (kd & KEY_A) gPlayReq = gPlaying ? 2 : 1;
		if (kd & KEY_B) tapPush(gSel, velN(), 0);
		if (kd & KEY_X) { Track* tr = &gPat[gCur].t[gSel]; memset(tr->on, 0, NS); memset(tr->acc, 0, NS); memset(tr->prob, 0, NS); memset(tr->sub, 1, NS); say("TRACK CLEARED"); }
		if (kd & KEY_Y) { randomTrack(gSel); say("TRACK RANDOMIZED"); }
		if (kr & KEY_DUP) gSel = (gSel + NT - 1) % NT;
		if (kr & KEY_DDOWN) gSel = (gSel + 1) % NT;
		if (kr & KEY_DLEFT) { gMp[M_BPM] -= 1.0f / 140; if (gMp[M_BPM] < 0) gMp[M_BPM] = 0; }
		if (kr & KEY_DRIGHT) { gMp[M_BPM] += 1.0f / 140; if (gMp[M_BPM] > 1) gMp[M_BPM] = 1; }
		if (kd & (KEY_L | KEY_R)) {
			int q = (gQueued + (kd & KEY_L ? NPAT - 1 : 1)) % NPAT;
			gQueued = q; if (!gPlaying) gCur = q;
		}
		if (kd & KEY_SELECT) {
			int n = (gCur + 1) % NPAT; gPat[n] = gPat[gCur]; gQueued = n; if (!gPlaying) gCur = n;
			char m[40]; snprintf(m, sizeof(m), "COPIED %c TO %c", 'A' + gCur, 'A' + n); say(m);
		}
		gModeTap = gMode == MD_TAP;

		touchPosition tp; hidTouchRead(&tp);
		int tx = tp.px, ty = tp.py;
		if (kd & KEY_TOUCH) {
			if (ty < 48) {
				int i = (ty / 24) * 8 + tx / 40; if (i > 15) i = 15;
				if (gMode == MD_MUTE) gMute[i] = !gMute[i];
				else { gSel = i; if (gMode == MD_TAP) { tapPush(i, velN(), 1); rollStart(i); } }
			} else if (ty >= 50 && ty < 130) {
				int s = ((ty - 50) / 40) * 8 + tx / 40; if (s > 15) s = 15;
				Track* tr = &gPat[gCur].t[gSel];
				int len = tr->len ? tr->len : 16;
				if (gMode == MD_TAP) { gSel = s; tapPush(s, velN(), 1); rollStart(s); }
				else if (s < len) {
					if (gMode == MD_STEP || gMode == MD_MUTE) { tr->on[s] = !tr->on[s]; if (!tr->on[s]) { tr->acc[s] = 0; tr->prob[s] = 0; tr->sub[s] = 1; } }
					else if (gMode == MD_ACC) { if (!tr->on[s]) { tr->on[s] = 1; tr->acc[s] = 1; } else tr->acc[s] = !tr->acc[s]; }
					else if (gMode == MD_PROB && tr->on[s]) tr->prob[s] = (tr->prob[s] + 1) & 3;
					else if (gMode == MD_SUB && tr->on[s]) tr->sub[s] = tr->sub[s] >= 4 ? 1 : tr->sub[s] + 1;
				}
			} else if (ty >= 132 && ty < 155) {
				int i = tx * 7 / 320; if (i > 6) i = 6;
				if (i < 6) gMode = i; else gPage = (gPage + 1) % 4;
			} else if (ty >= 158) {
				int i = ((ty - 158) / 20) * 2 + tx / 160; if (i > 7) i = 7;
				if (gPage == PG_HW) hwPress(i);
				else gDrag = i;
			}
		}
		if ((kh & KEY_TOUCH) && gDrag >= 0 && gPage != PG_HW) {
			float x0 = (gDrag % 2) * 160 + 56;
			slSet(gDrag, (tx - x0) / 98.0f);
		}
		if (!(kh & KEY_TOUCH)) { gDrag = -1; if (!shakeRoll) gRollT = -1; }

		sensorsUpdate(dt);
		if (gMotion) {   // shake = note-repeat roll on the selected track
			if (gShake > 0.35f && gRollT < 0) { shakeRoll = 1; tapPush(gSel, velN(), gModeTap); if (!idx(gMp[M_ROLL], 5)) gMp[M_ROLL] = 0.5f; rollStart(gSel); }
			if (shakeRoll && gShake < 0.15f) { shakeRoll = 0; if (!(kh & KEY_TOUCH)) gRollT = -1; }
		} else if (shakeRoll) { shakeRoll = 0; gRollT = -1; }
		beatLight(dt); micRecTick(dt); linkTick(dt);
		if (gMsgT > 0) gMsgT -= dt;
		for (int t = 0; t < NT; t++) gFlash[t] *= 0.85f;

		// effect-bus self check: if the DSP never hands back aux audio, play straight to the outputs
		if (gAudioOK && gBusOK && !gBusFail && gPlaying) { playT += dt; if (playT > 2.0f && !gBusSeen) { gBusFail = 1; say("FX BUS UNAVAILABLE - DIRECT OUT"); } }

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TextBufClear(gTb);
		C2D_TargetClear(top, RGB(0, 0, 0)); C2D_SceneBegin(top); drawTop();
		C2D_TargetClear(bot, RGB(0, 0, 0)); C2D_SceneBegin(bot); drawBottom();
		C3D_FrameEnd(0);
	}

	linkStop(gLink);
	gWorkQuit = 1; LightEvent_Signal(&gWorkEv);
	if (worker) { threadJoin(worker, U64_MAX); threadFree(worker); }
	if (gAudioOK) { ndspSetCallback(NULL, NULL); ndspAuxSetCallback(0, NULL, NULL); ndspAuxSetCallback(1, NULL, NULL); ndspExit(); }
	sensorsExit();
	C2D_TextBufDelete(gTb);
	C2D_Fini(); C3D_Fini(); gfxExit();
	return 0;
}
