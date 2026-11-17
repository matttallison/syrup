// DS-1000 — drum machine for Nintendo 2DS / 3DS (homebrew)
// Soul Provider Records
//
// Workflow and look after PICO (Matt's SP-1200-style FM-1 firmware): seven-ish menus of pages, each page a
// 2x2 knob panel, SELECT steps through every page, pads + one-press jobs, faders, per-step detail
// (velocity, nudge, condition, ratchet), punches, notes, song, Euclidean patterns, per-pad time.
// It is a drum machine, not a sampler: 16 synthesised 808/909-style voices.
//
// Hardware: the core-1 worker renders each pad's hit only when its sound changes; the DSP plays the hits
// on 16 channels (rate = tune, hardware biquad = filter, one-pole = low cut, mix = level / pan / echo
// send). All channels go through DSP aux bus 0 (punch, bus filter, drive, auto-pan, volume) and aux bus 1
// (ping-pong echo), using Syrup's copy of the sound driver. The sequencer runs in the DSP frame callback.

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
#define NSTEP 64
#define NPAT 16
#define NSONG 16
#define MAXLEN 65536
#define MICBYTES 0x8000
#define MICRATE 16364.0f
#define SAVEPATH "sdmc:/3ds/ds1000/ds1000.sav"
#define SAVEVER 3

// ---------------------------------------------------------------- voices, kit, globals
enum { BD, SD, LT, MT, HT, RS, CP, CH, OH, CC, RC, CB, B9, S9, CL, MA, NVOICE };
static const char* VNAME[NVOICE] = {"KICK", "SNARE", "TOM L", "TOM M", "TOM H", "RIM", "CLAP", "HAT",
	"OHAT", "CRASH", "RIDE", "COWBL", "KICK9", "SNR9", "CLAVE", "MARAC"};

// per-pad sound (integers, ranges in KR)
enum { K_VOICE, K_TUNE, K_FINE, K_TONE, K_START, K_DIR, K_CHOKE, K_BITS, K_SRATE, K_CUT, K_RES, K_FENV, K_FTYPE, K_LOCUT,
	K_ATTACK, K_DECAY, K_LEVEL, K_PAN, K_DRIVE, K_SEND, NK };
static const s16 KR[NK][3] = {   // lo, hi, default
	{0, 15, 0}, {-24, 24, 0}, {-50, 50, 0}, {0, 100, 50}, {0, 90, 0}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 7, 0},
	{0, 100, 100}, {0, 100, 20}, {-100, 100, 0}, {0, 5, 0}, {0, 100, 0}, {0, 100, 0}, {0, 100, 50}, {0, 127, 110},
	{-50, 50, 0}, {0, 100, 0}, {0, 100, 0}};
static s16 gKit[NT][NK];

// global settings (integers, ranges in GR)
enum { G_BPM, G_SWING, G_QUANT, G_METER, G_BFILT, G_BDRIVE, G_ECHO, G_ECHOT, G_APRATE, G_APDEPTH, G_VOLUME, G_FADER,
	G_PTYPE, G_PRATE, G_PDECAY, G_PLEVEL, G_MOTION, G_TILT, G_TWIST, G_SHAKE, G_CLICK, G_COUNT, G_TICK, G_CLKLVL,
	G_OUTPUT, G_BLIGHT, G_VELMODE, G_CURVE, G_FIXVEL, G_ROLL, G_SCALE, G_KEY, G_SONG, NG };
static const s16 GR[NG][3] = {
	{60, 200, 96}, {0, 25, 8}, {0, 1, 1}, {0, 1, 0}, {-100, 100, 0}, {0, 100, 0}, {0, 100, 40}, {0, 4, 2}, {0, 4, 0}, {0, 100, 50},
	{0, 100, 90}, {0, 3, 0}, {0, 4, 0}, {0, 3, 1}, {0, 100, 40}, {0, 100, 100}, {0, 1, 0}, {0, 2, 1}, {0, 1, 1}, {0, 2, 1},
	{0, 2, 1}, {0, 2, 1}, {0, 1, 0}, {0, 100, 60}, {0, 2, 2}, {0, 1, 0}, {0, 3, 1}, {-5, 5, 0}, {1, 127, 110}, {0, 3, 1},
	{0, 5, 0}, {0, 11, 0}, {0, 1, 0}};
static volatile s16 gG[NG];

typedef struct { u8 on[NSTEP], vel[NSTEP], cond[NSTEP], sub[NSTEP]; s8 nud[NSTEP], note[NSTEP]; u8 len, rate, pad[2]; } Track;
typedef struct { u8 type, start, len, rate; } Punch;   // type 0 = empty, else punch type + 1
typedef struct { Track t[NT]; Punch pu[4]; u8 bars, pad[3]; } Pattern;
typedef struct { s8 pat; u8 rep; s8 tr; u8 pad; } SongStep;   // pat -1 = end

static Pattern gPat[NPAT];
static SongStep gSong[NSONG];
static u8 gMute[NT];
static volatile int gCur = 0, gQueued = 0;
static int gSel = 0;   // selected pad

static const char* NAMES_ONOFF[2] = {"OFF", "ON"};
static const char* FT_NAMES[6] = {"OFF", "LP", "HP", "BP", "NOTCH", "PEAK"};
static const int BITS_V[6] = {16, 12, 10, 8, 6, 4};
static const char* ECHOT_NAMES[5] = {"1/16", "1/8", "3/16", "1/4", "3/8"};
static const float ECHOT_STEPS[5] = {1, 2, 3, 4, 6};
static const char* AP_NAMES[5] = {"OFF", "1 BAR", "1/4", "1/8", "1/16"};
static const float AP_STEPS[5] = {0, 16, 4, 2, 1};
static const char* FADER_NAMES[4] = {"LEVEL", "TUNE", "DECAY", "CUTOFF"};
static const char* PT_NAMES[5] = {"REPEAT", "STUTTER", "TAPE STOP", "FADE", "REVERSE"};
static const char* PR_NAMES[4] = {"1/4", "1/8", "1/16", "1/32"};
static const float PR_STEPS[4] = {4, 2, 1, 0.5f};
static const char* TILT_NAMES[3] = {"OFF", "FILTER", "ECHO"};
static const char* TWIST_NAMES[2] = {"OFF", "PITCH"};
static const char* SHAKE_NAMES[3] = {"OFF", "ROLL", "PUNCH"};
static const char* CLICK_NAMES[3] = {"OFF", "REC", "ON"};
static const char* COUNT_NAMES[3] = {"OFF", "1 BAR", "2 BARS"};
static const char* TICK_NAMES[2] = {"1/4", "1/8"};
static const char* OUT_NAMES[3] = {"STEREO", "SURROUND", "AUTO"};
static const char* VM_NAMES[4] = {"FIXED", "POSITION", "MIC", "TAP MIC"};
static const char* ROLL_NAMES[4] = {"1/8", "1/16", "1/32", "1/64"};
static const float ROLL_STEPS[4] = {2, 1, 0.5f, 0.25f};
static const u8 ROLL_SUB[4] = {1, 1, 2, 4};
static const char* SCALE_NAMES[6] = {"CHROM", "MAJOR", "MINOR", "PENTA", "BLUES", "DORIAN"};
static const u16 SCALE_MASK[6] = {0xFFF, 0xAB5, 0x5AD, 0x295, 0x4E9, 0x6AD};   // bit n = semitone n in key
static const char* NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* RATE_NAMES[7] = {"X1", "1/2", "X2", "3/4", "3/2", "1/4", "1/8"};
static const float RATE_MUL[7] = {1, 0.5f, 2, 0.75f, 1.5f, 0.25f, 0.125f};
static const char* COND_NAMES[12] = {"ALWAYS", "75%", "50%", "25%", "1ST", "1:2", "2:2", "1:4", "2:4", "3:4", "4:4", "NOT 1ST"};
static const char* STYLE_NAMES[9] = {"EUCLID", "4 FLOOR", "BACKBEAT", "OFFBEAT", "8THS", "16THS", "TRESILLO", "CLAVE", "RANDOM"};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int spBar(void) { return gG[G_METER] ? 12 : 16; }
static int spBeat(void) { return gG[G_METER] ? 3 : 4; }
static int loopLen(const Pattern* p) { int b = p->bars < 1 ? 1 : p->bars > 4 ? 4 : p->bars; return b * spBar(); }
static int trLenOf(const Pattern* p, int t) { int l = p->t[t].len ? p->t[t].len : loopLen(p); return l > NSTEP ? NSTEP : l; }

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


// ---------------------------------------------------------------- rendered hits (core-1 worker)
static s16* gSmp[NT][2];
static volatile int gSmpCur[NT], gSmpLen[NT][2];
static float* gTmp;
static volatile u32 gDirty = 0;
static LightEvent gWorkEv;
static volatile int gWorkQuit = 0;

static void markDirty(int t) { __atomic_fetch_or(&gDirty, 1u << t, __ATOMIC_SEQ_CST); LightEvent_Signal(&gWorkEv); }

static void renderTrack(int t) {
	const s16* k = gKit[t];
	float* b = gTmp;
	int n = synth(clampi(k[K_VOICE], 0, NVOICE - 1), k[K_DECAY] / 100.0f, k[K_TONE] / 100.0f, b, MAXLEN);
	int st = n * k[K_START] / 100;   // START trims into the hit
	if (st > 0 && st < n - 16) { memmove(b, b + st, (n - st) * sizeof(float)); n -= st; for (int i = 0; i < 48 && i < n; i++) b[i] *= i / 48.0f; }
	if (k[K_FENV]) {   // filter envelope baked into the hit: + sweeps down (pluck), - sweeps up
		float amt = k[K_FENV] / 100.0f, e = 1, ek = dk(0.12f + k[K_DECAY] * 0.004f), lo, bp, hi;
		Svf f = {0};
		for (int i = 0; i < n; i++) {
			if ((i & 15) == 0) { float fc = amt > 0 ? 150 + amt * 12000 * e : 150 - amt * 12000 * (1 - e); svfSet(&f, fc, 0.9f); }
			svfRun(&f, b[i], &lo, &bp, &hi); b[i] = lo; e *= ek;
		}
	}
	while (n > 32 && fabsf(b[n - 1]) < 0.0005f) n--;
	if (n < 1) n = 1;
	if (k[K_DIR]) for (int i = 0; i < n / 2; i++) { float x = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = x; }
	int at = (int)(k[K_ATTACK] * 0.001f * DSPRATE);   // 0-100 ms
	for (int i = 0; i < at && i < n; i++) b[i] *= (float)i / at;
	float d = k[K_DRIVE] / 100.0f;
	if (d > 0.01f) { float g = 1 + d * 8; for (int i = 0; i < n; i++) { float y = b[i] * g; b[i] = y / (1 + fabsf(y)) * 0.95f; } }
	int h = 1 + k[K_SRATE];   // SP-style rate crush
	if (h > 1) { float hold = 0; for (int i = 0; i < n; i++) { if (i % h == 0) hold = b[i]; b[i] = hold; } }
	int bits = BITS_V[clampi(k[K_BITS], 0, 5)];
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
static volatile int gTransp = 0;   // song transpose
static int gHeadphones = 0;
static volatile int gLastVel = 110, gLastPad = 0;
static u8 gPadVel[NT];

static void outputApply(void) {
	if (!gAudioOK) return;
	int m = gG[G_OUTPUT], sur = m == 1 || (m == 2 && gHeadphones);
	ndspSetOutputMode(sur ? NDSP_OUTPUT_SURROUND : NDSP_OUTPUT_STEREO);
	ndspSurroundSetDepth(0x5FFF);
	ndspSetClippingMode(NDSP_CLIP_SOFT);
}
static void chanApply(int t) {   // hardware filter + low cut for pad t
	if (!gAudioOK) return;
	const s16* k = gKit[t];
	int ty = clampi(k[K_FTYPE], 0, 5);
	if (!ty) ndspChnIirBiquadSetEnable(t, false);
	else {
		float f = 30.0f * powf(470.0f, k[K_CUT] / 100.0f), r = k[K_RES] / 100.0f, q = 0.5f + r * r * 9.5f;
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
	if (k[K_LOCUT] > 0) ndspChnIirMonoSetEnable(t, ndspChnIirMonoSetParamsHighPassFilter(t, 20.0f * powf(40.0f, k[K_LOCUT] / 100.0f)));
	else ndspChnIirMonoSetEnable(t, false);
}
static float velGain(int v) { return powf(clampi(v, 1, 127) / 127.0f, 1.6f); }
static void fire(int t, float gain, int note) {
	if (!gAudioOK) return;
	int c = gSmpCur[t], len = gSmpLen[t][c];
	if (len < 1) return;
	const s16* k = gKit[t];
	if (k[K_CHOKE]) for (int o = 0; o < NT; o++) if (o != t && gKit[o][K_CHOKE] == k[K_CHOKE]) ndspChnWaveBufClear(o);
	ndspChnWaveBufClear(t);
	ndspWaveBuf* wb = &gWB[t][gWBi[t] ^= 1];
	memset(wb, 0, sizeof(*wb));
	wb->data_pcm16 = gSmp[t][c]; wb->nsamples = len; wb->looping = false; wb->status = NDSP_WBUF_DONE;
	float semi = k[K_TUNE] + k[K_FINE] / 100.0f + note + gTransp + gBend;
	ndspChnSetRate(t, DSPRATE * powf(2.0f, semi / 12.0f));
	float lev = k[K_LEVEL] / 110.0f, g = gain * lev * lev * 1.25f, pan = (k[K_PAN] + 50) / 100.0f;
	float pl = pan < 0.5f ? 1 : 2 * (1 - pan), pr = pan > 0.5f ? 1 : 2 * pan;
	float mix[12] = {0};
	if (gBusOK && !gBusFail) { mix[4] = g * pl; mix[5] = g * pr; } else { mix[0] = g * pl; mix[1] = g * pr; }
	mix[8] = mix[9] = g * k[K_SEND] / 100.0f;
	ndspChnSetMix(t, mix);
	ndspChnWaveBufAdd(t, wb);
	gFlash[t] = gain > 0.15f ? gain : 0.15f;
}

// ---------------------------------------------------------------- bus: punch, filter, drive, auto-pan (aux 0); echo (aux 1)
static float gTiltX = 0, gTiltY = 0, gTwist = 0, gShakeV = 0;
static Svf gDjL, gDjR; static float gDjLast = -1; static int gDjMode = 0;
static float* gDlL; static float* gDlR;
#define DLMAX 65536
static int gDlW = 0; static float gDlLp[2];
#define PRING 65536
static float* gPrL; static float* gPrR; static int gPrW = 0;
static volatile int gPunchLive = 0, gPunchSeq = 0, gPunchSeqRate = 1;   // live (held) / sequenced punch: type + 1
static int gPunchCur = 0, gPLoopS = 0, gPLoopN = 1; static float gPT = 0, gPRp = 0;
static float gApPh = 0;

static float spbStep(void) { return DSPRATE * 60.0f / (gG[G_BPM] * (float)spBeat()); }

static void auxMaster(void* data, int n, void* samples[4]) {
	(void)data;
	s32* L = (s32*)samples[0]; s32* R = (s32*)samples[1];
	// punch: live punch wins over the sequenced one
	int want = gPunchLive ? gPunchLive : gPunchSeq, rate = gPunchLive ? gG[G_PRATE] : gPunchSeqRate;
	if (want != gPunchCur) {
		gPunchCur = want; gPT = 0;
		gPLoopN = (int)(spbStep() * PR_STEPS[clampi(rate, 0, 3)]); if (gPLoopN < 64) gPLoopN = 64; if (gPLoopN > PRING / 2) gPLoopN = PRING / 2;
		gPLoopS = (gPrW - gPLoopN) & (PRING - 1);
		gPRp = (float)((gPrW - 1) & (PRING - 1));
	}
	float plev = gG[G_PLEVEL] / 100.0f, pdec = 0.1f + gG[G_PDECAY] * 0.029f;   // 0.1 - 3 s
	// bus filter (+ tilt)
	float v = gG[G_BFILT] / 100.0f + (gG[G_MOTION] && gG[G_TILT] == 1 ? gTiltX * 0.9f : 0);
	if (v < -1) v = -1;
	if (v > 1) v = 1;
	if (fabsf(v - gDjLast) > 0.004f) {
		gDjLast = v;
		if (v < -0.04f) { gDjMode = 1; float f = 18000.0f * powf(0.008f, -v); svfSet(&gDjL, f, 1.0f); svfSet(&gDjR, f, 1.0f); }
		else if (v > 0.04f) { gDjMode = 2; float f = 20.0f * powf(200.0f, v); svfSet(&gDjL, f, 1.0f); svfSet(&gDjR, f, 1.0f); }
		else gDjMode = 0;
	}
	float d = gG[G_BDRIVE] / 100.0f, g = 1 + d * 7, vol = gG[G_VOLUME] / 90.0f;
	int apr = clampi(gG[G_APRATE], 0, 4); float apd = gG[G_APDEPTH] / 100.0f, apInc = apr ? 1.0f / (spbStep() * AP_STEPS[apr]) : 0;
	int seen = 0;
	for (int s = 0; s < n; s++) {
		if (L[s] | R[s]) seen = 1;
		float l = L[s] * (1.0f / 32768.0f), r = R[s] * (1.0f / 32768.0f), lo, bp, hi;
		gPrL[gPrW] = l; gPrR[gPrW] = r;
		if (gPunchCur) {
			float ol = l, orr = r; int ty = gPunchCur - 1;
			if (ty == 0 || ty == 1) {   // REPEAT / STUTTER
				int ph = (int)gPT % gPLoopN, rep = (int)gPT / gPLoopN, i = (gPLoopS + ph) & (PRING - 1);
				float gg = powf(1.0f - gG[G_PDECAY] * 0.006f, (float)rep);
				if (ty == 1 && ph > gPLoopN / 2) gg = 0;
				int fe = ph < 32 ? ph : gPLoopN - ph < 32 ? gPLoopN - ph : 32;   // de-click the loop seam
				ol = gPrL[i] * gg * fe / 32.0f; orr = gPrR[i] * gg * fe / 32.0f;
			} else if (ty == 2) {   // TAPE STOP
				float spd = 1.0f - gPT / (pdec * DSPRATE); if (spd < 0) spd = 0;
				int i0 = (int)gPRp; float fr = gPRp - i0; int i1 = (i0 + 1) & (PRING - 1);
				ol = (gPrL[i0 & (PRING - 1)] * (1 - fr) + gPrL[i1] * fr) * (spd > 0.02f ? 1 : spd * 50);
				orr = (gPrR[i0 & (PRING - 1)] * (1 - fr) + gPrR[i1] * fr) * (spd > 0.02f ? 1 : spd * 50);
				gPRp += spd; if (gPRp >= PRING) gPRp -= PRING;
			} else if (ty == 3) {   // FADE
				float k = 1.0f - gPT / (pdec * DSPRATE); if (k < 0) k = 0;
				ol = l * k; orr = r * k;
			} else {   // REVERSE: plays back what just went by
				int i = (gPLoopS + gPLoopN - 1 - ((int)gPT % gPLoopN)) & (PRING - 1);
				ol = gPrL[i]; orr = gPrR[i];
			}
			l = l * (1 - plev) + ol * plev; r = r * (1 - plev) + orr * plev;
			gPT += 1;
		}
		gPrW = (gPrW + 1) & (PRING - 1);
		if (gDjMode) {
			svfRun(&gDjL, l, &lo, &bp, &hi); l = gDjMode == 1 ? lo : hi;
			svfRun(&gDjR, r, &lo, &bp, &hi); r = gDjMode == 1 ? lo : hi;
		}
		if (d > 0.01f) { float a = l * g, b = r * g; l = a / (1 + fabsf(a)) * 0.9f; r = b / (1 + fabsf(b)) * 0.9f; }
		if (apr) {
			gApPh += apInc; if (gApPh >= 1) gApPh -= 1;
			float lf = fsin(gApPh);
			l *= 1 - apd * (lf > 0 ? lf : 0); r *= 1 - apd * (lf < 0 ? -lf : 0);
		}
		l *= vol; r *= vol;
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
	int d = (int)(spbStep() * ECHOT_STEPS[clampi(gG[G_ECHOT], 0, 4)]); if (d >= DLMAX) d = DLMAX - 1;
	float fb = gG[G_ECHO] / 100.0f * 0.92f, ig = 1.0f + (gG[G_MOTION] && gG[G_TILT] == 2 && gTiltY > 0 ? gTiltY * 2.5f : 0);
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
	gPrL = (float*)calloc(PRING, sizeof(float)); gPrR = (float*)calloc(PRING, sizeof(float));
	if (!gDlL || !gDlR || !gPrL || !gPrR) return;
	ndspAuxSetCallback(0, auxMaster, NULL); ndspAuxSetCallback(1, auxDelay, NULL);
	for (int i = 0; i < 2; i++) { ndspAuxSetFrontBypass(i, false); ndspAuxSetVolume(i, 1.0f); ndspAuxSetEnable(i, true); }
	gBusOK = 1;
}


// ---------------------------------------------------------------- sequencer (DSP frame callback)
// A global clock (loop position, click, count-in, punches, song) plus a clock per pad (PAD TIME: its own
// rate and length). Nudged-late hits are scheduled after their step starts; nudged-early ones from the
// step before. Live hits record with QUANT on (snapped) or off (timing kept as nudge).
static volatile int gPlaying = 0, gPlayReq = 0, gRec = 0, gTick = -1000, gLoopPos = 0, gSnap = -1, gBeat = 0, gCounting = 0;
static int gNextTick = 0, gTrRun = 0; static float gStepCd = 0, gStepLen = 1;
static int gTrK[NT]; static float gTrCd[NT], gTrLen[NT];
typedef struct { float cd, gain; s8 note; u8 sub, on, pad; } Pend;
static Pend gPend[NT][2];
static int gSubLeft[NT], gSubNote[NT]; static float gSubCd[NT], gSubInt[NT], gSubGain[NT];
static volatile int gPos[NT];
static volatile s8 gSkip[NT];   // a step just recorded live (pos + 1): it was heard already, skip it this pass
static int gPunchLeft = 0;
static volatile int gSongPos = 0; static int gSongRep = 0;
#define TQ 64
typedef struct { u8 t, rec; u8 vel; s8 note; } TapEv;
static TapEv gTapQ[TQ]; static volatile int gTapW = 0, gTapR = 0;
static volatile int gRollT = -1; static float gRollCd = 0;
static volatile int gEraseT = -1;
static int patUsed(int p);

// metronome / count-in click on its own DSP channel, straight to the outputs
#define CLICK_CH 16
static s16* gClickSmp[2]; static int gClickLen = 0; static ndspWaveBuf gClickWB[2]; static int gClickI = 0;
static void clickInit(void) {
	gClickLen = (int)(0.03f * DSPRATE);
	for (int k = 0; k < 2; k++) {
		gClickSmp[k] = (s16*)linearAlloc(gClickLen * 2); if (!gClickSmp[k]) { gClickLen = 0; return; }
		float f = k ? 2400.0f : 1500.0f;
		for (int i = 0; i < gClickLen; i++) gClickSmp[k][i] = (s16)(sinf(6.2831853f * f * i / DSPRATE) * expf(-i / (0.006f * DSPRATE)) * 16000.0f);
		DSP_FlushDataCache(gClickSmp[k], gClickLen * 2);
	}
	ndspChnReset(CLICK_CH); ndspChnSetInterp(CLICK_CH, NDSP_INTERP_NONE); ndspChnSetFormat(CLICK_CH, NDSP_FORMAT_MONO_PCM16); ndspChnSetRate(CLICK_CH, DSPRATE);
}
static void clickFire(int down) {
	if (!gAudioOK || !gClickLen) return;
	float mix[12] = {0}; mix[0] = mix[1] = gG[G_CLKLVL] / 100.0f;
	ndspChnSetMix(CLICK_CH, mix);
	ndspChnWaveBufClear(CLICK_CH);
	ndspWaveBuf* wb = &gClickWB[gClickI ^= 1];
	memset(wb, 0, sizeof(*wb));
	wb->data_pcm16 = gClickSmp[down ? 1 : 0]; wb->nsamples = gClickLen; wb->status = NDSP_WBUF_DONE;
	ndspChnWaveBufAdd(CLICK_CH, wb);
}

static void clearStep(Track* tr, int s) { tr->on[s] = 0; tr->vel[s] = 110; tr->cond[s] = 0; tr->sub[s] = 1; tr->nud[s] = 0; tr->note[s] = 0; }
static void clearTrack(Track* tr) { for (int s = 0; s < NSTEP; s++) clearStep(tr, s); }
static void clearPattern(Pattern* p) { for (int t = 0; t < NT; t++) { clearTrack(&p->t[t]); p->t[t].len = 0; p->t[t].rate = 0; } memset(p->pu, 0, sizeof(p->pu)); p->bars = 1; }

static u32 rnd(void) { gRng = gRng * 1664525u + 1013904223u; return gRng >> 8; }
static int condPass(int c, int it) {
	switch (c) {
	case 1: return rnd() % 100 < 75;
	case 2: return rnd() % 100 < 50;
	case 3: return rnd() % 100 < 25;
	case 4: return it == 0;
	case 5: return it % 2 == 0;
	case 6: return it % 2 == 1;
	case 7: return it % 4 == 0;
	case 8: return it % 4 == 1;
	case 9: return it % 4 == 2;
	case 10: return it % 4 == 3;
	case 11: return it != 0;
	}
	return 1;
}
static void sched(int t, float delay, float gain, int sub, int note) {
	Pend* p = gPend[t][0].on ? &gPend[t][1] : &gPend[t][0];
	p->cd = delay; p->gain = gain; p->sub = (u8)sub; p->note = (s8)note; p->on = 1;
}
static float trackStepLen(const Track* tr, int k) {
	float b = spbStep() / RATE_MUL[tr->rate % 7];
	if (tr->rate == 0) { float sw = gG[G_SWING] / 50.0f; b *= (k & 1) ? 1 - sw : 1 + sw; }
	return b;
}
static void tracksReset(float cd) { for (int t = 0; t < NT; t++) { gTrK[t] = 0; gTrCd[t] = cd; gPos[t] = -1; gPend[t][0].on = gPend[t][1].on = 0; } gTrRun = 1; }

static void recordHit(int t, int vel, int note) {
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[t];
	int len = trLenOf(p, t), target, nd = 0;
	if (!gTrRun) target = 0;   // during the count-in: the first step
	else {
		int k = gTrK[t] - 1; float sl = gTrLen[t], el = sl - gTrCd[t], off;
		if (el < sl * 0.5f) { target = k; off = el / sl; } else { target = k + 1; off = -gTrCd[t] / sl; }
		if (!gG[G_QUANT]) { nd = (int)roundf(off * 12); nd = clampi(nd, -6, 6); }
		if (target < 0) target = 0;
	}
	int s = target % len;
	tr->on[s] = 1; tr->vel[s] = (u8)vel; tr->cond[s] = 0; tr->nud[s] = (s8)nd; tr->note[s] = (s8)note;
	tr->sub[s] = gRollT == t ? ROLL_SUB[clampi(gG[G_ROLL], 0, 3)] : 1;
	if ((!gTrRun || target > gTrK[t] - 1) && nd >= 0) gSkip[t] = s + 1;
}
static void hit(int t, int vel, int rec, int note) {
	fire(t, velGain(vel), note);
	gLastVel = vel; gLastPad = t; gPadVel[t] = (u8)vel;
	if (rec && gRec && gPlaying) recordHit(t, vel, note);
}

static void trackStep(int t) {
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[t];
	int len = trLenOf(p, t), k = gTrK[t]++, s = k % len, it = k / len;
	float sl = trackStepLen(tr, k);
	gTrLen[t] = sl; gPos[t] = s;
	if (gEraseT == t && tr->on[s]) clearStep(tr, s);   // ERASE: hits vanish as they pass
	if (gSkip[t] == s + 1) gSkip[t] = 0;
	else if (tr->on[s] && tr->nud[s] >= 0 && !gMute[t] && condPass(tr->cond[s], it))
		{ sched(t, sl * tr->nud[s] / 12.0f, velGain(tr->vel[s]), tr->sub[s], tr->note[s]); gPadVel[t] = tr->vel[s]; }
	int s2 = (k + 1) % len, it2 = (k + 1) / len;   // the next step, if it's nudged early
	if (tr->on[s2] && tr->nud[s2] < 0 && !gMute[t] && gEraseT != t && condPass(tr->cond[s2], it2))
		sched(t, sl * (1 + tr->nud[s2] / 12.0f), velGain(tr->vel[s2]), tr->sub[s2], tr->note[s2]);
	gTrCd[t] += sl;
}

static void globalStep(void) {
	int tick = gNextTick++;
	float spb = spbStep(), sw = gG[G_SWING] / 50.0f;
	gStepLen = spb * ((tick & 1) ? 1 - sw : 1 + sw);
	int bar = spBar(), beat = spBeat();
	if (tick < 0) {   // count-in
		gCounting = (-tick + bar - 1) / bar;
		if (((tick % beat) + beat) % beat == 0) clickFire(((tick % bar) + bar) % bar == 0);
		gTick = tick; gStepCd += gStepLen; return;
	}
	gCounting = 0;
	if (tick == 0) { gLoopPos = 0; tracksReset(gStepCd + 160); gSongRep = 0; if (gG[G_SONG] && gSong[gSongPos].pat >= 0) { gCur = gQueued = gSong[gSongPos].pat; gTransp = gSong[gSongPos].tr; } }
	else {
		int lp = gLoopPos + 1;
		if (lp >= loopLen(&gPat[gCur])) {   // loop end: song, queued pattern
			lp = 0;
			int np = gCur;
			if (gG[G_SONG]) {
				if (++gSongRep >= gSong[gSongPos].rep) {
					gSongRep = 0; gSongPos++;
					if (gSongPos >= NSONG || gSong[gSongPos].pat < 0 || !gSong[gSongPos].rep) { gSongPos = 0; gPlayReq = 2; say("SONG END"); }
				}
				np = gSong[gSongPos].pat < 0 ? gCur : gSong[gSongPos].pat; gTransp = gSong[gSongPos].tr;
			} else if (gQueued != gCur) np = gQueued;
			if (np != gCur) { gCur = gQueued = np; tracksReset(gStepCd + 160); }
		}
		gLoopPos = lp;
	}
	int lp = gLoopPos;
	int tickRes = gG[G_TICK] && beat == 4 ? 2 : beat;
	if (lp % tickRes == 0 && (gG[G_CLICK] == 2 || (gG[G_CLICK] == 1 && gRec))) clickFire(lp % bar == 0);
	if (lp % beat == 0) gBeat = lp % bar == 0 ? 2 : 1;
	// sequenced punches
	if (gPunchLeft > 0 && --gPunchLeft == 0) gPunchSeq = 0;
	Pattern* p = &gPat[gCur];
	for (int i = 0; i < 4; i++) if (p->pu[i].type && p->pu[i].start == lp) { gPunchSeq = p->pu[i].type; gPunchSeqRate = p->pu[i].rate; gPunchLeft = p->pu[i].len ? p->pu[i].len : 1; }
	gTick = tick;
	gStepCd += gStepLen;
}

static void seqStopAll(void) {
	for (int t = 0; t < NT; t++) { gSubLeft[t] = 0; gPend[t][0].on = gPend[t][1].on = 0; gSkip[t] = 0; gPos[t] = -1; }
	gPunchSeq = 0; gPunchLeft = 0; gTrRun = 0; gTransp = 0;
}
static void seqTick(void* data) {
	(void)data;
	const float F = 160;
	while (gTapR != gTapW) { TapEv e = gTapQ[gTapR]; gTapR = (gTapR + 1) % TQ; hit(e.t, e.vel, e.rec, e.note); }
	int rt = gRollT;
	if (rt >= 0) {
		gRollCd -= F;
		if (gRollCd <= 0) { hit(rt, gLastVel, 1, 0); gRollCd += spbStep() * ROLL_STEPS[clampi(gG[G_ROLL], 0, 3)]; if (gRollCd < F) gRollCd = F; }
	}
	if (gPlayReq == 1) {
		gPlayReq = 0; seqStopAll(); gPlaying = 1; gStepCd = 0; gTick = -1000;
		gNextTick = gRec && gG[G_COUNT] ? -gG[G_COUNT] * spBar() : 0;
		if (gG[G_SONG]) gSongPos = 0;
	}
	if (gPlayReq == 2) { gPlayReq = 0; gPlaying = 0; gCounting = 0; seqStopAll(); }
	if (!gPlaying) return;
	if (gSnap >= 0) {   // link follower: jump to the host's bar position
		int s = gSnap; gSnap = -1;
		if (gTrRun && s == 0 && gLoopPos != 0) { gLoopPos = loopLen(&gPat[gCur]) - 1; gStepCd = 0; }
	}
	gStepCd -= F;
	int guard = 0;
	while (gStepCd <= 0 && guard++ < 4) globalStep();
	if (!gTrRun) return;
	for (int t = 0; t < NT; t++) {
		gTrCd[t] -= F;
		int g2 = 0;
		while (gTrCd[t] <= 0 && g2++ < 8) trackStep(t);
		for (int k = 0; k < 2; k++) {
			Pend* p = &gPend[t][k];
			if (!p->on || (p->cd -= F) > 0) continue;
			p->on = 0;
			fire(t, p->gain, p->note);
			if (p->sub > 1) { gSubLeft[t] = p->sub - 1; gSubInt[t] = gTrLen[t] / p->sub; gSubCd[t] = gSubInt[t] + p->cd; gSubGain[t] = p->gain * 0.8f; gSubNote[t] = p->note; }
			else gSubLeft[t] = 0;
		}
		if (gSubLeft[t] > 0 && (gSubCd[t] -= F) <= 0) { fire(t, gSubGain[t], gSubNote[t]); gSubLeft[t]--; gSubCd[t] += gSubInt[t]; }
	}
}

static void tapPush(int t, int vel, int rec, int note) {
	int n = (gTapW + 1) % TQ; if (n == gTapR) return;
	gTapQ[gTapW].t = t; gTapQ[gTapW].vel = (u8)clampi(vel, 1, 127); gTapQ[gTapW].rec = rec; gTapQ[gTapW].note = (s8)note;
	__sync_synchronize(); gTapW = n;
}
static void rollStart(int t) { gRollCd = spbStep() * ROLL_STEPS[clampi(gG[G_ROLL], 0, 3)]; gRollT = t; }

// undo / redo (one level, swaps)
static Pattern gUndo; static int gUndoP = -1;
static void undoKeep(int p) { gUndo = gPat[p]; gUndoP = p; }
static void undoDo(void) {
	if (gUndoP < 0) { say("NOTHING TO UNDO"); return; }
	Pattern t = gPat[gUndoP]; gPat[gUndoP] = gUndo; gUndo = t;
	say("UNDONE (AGAIN: REDO)");
}


// ---------------------------------------------------------------- sensors, beat light, mic velocity (as in Syrup)
static float gTiltCX = 0, gTiltCY = 0; static int gTiltCal = 0;
static int gMicOK = 0; static u8* gMicBuf = NULL; static u32 gMicSize = 0;
static int gLcdOK = 0, gLcdFlash = 0; static u32 gBrightTop = 3, gBrightBot = 3; static float gLightT = 0;
static int gPtmOK = 0;

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
	gPtmOK = R_SUCCEEDED(ptmuInit());
}
static void sensorsExit(void) {
	if (gMicOK) MICU_StopSampling();
	if (gMicBuf) { micExit(); free(gMicBuf); }
	HIDUSER_DisableAccelerometer(); HIDUSER_DisableGyroscope();
	if (gLcdOK) { GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, gBrightTop); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, gBrightBot); gspLcdExit(); }
	if (gPtmOK) ptmuExit();
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
		gShakeV += (d - gShakeV) * (d > gShakeV ? 0.7f : 0.05f);
	}
	pa = a; have = 1;
	gBend = gG[G_MOTION] && gG[G_TWIST] ? gTwist * 7.0f : 0;
	static float hpT = 0;
	if ((hpT -= dt) <= 0) {
		hpT = 0.5f; bool hp = false;
		if (gAudioOK && R_SUCCEEDED(DSP_GetHeadphoneStatus(&hp))) { int h = hp ? 1 : 0; if (h != gHeadphones) { gHeadphones = h; if (gG[G_OUTPUT] == 2) outputApply(); } }
	}
}
static void beatLight(float dt) {
	if (!gLcdOK) return;
	int b = gBeat; gBeat = 0;
	if (b && gG[G_BLIGHT]) {
		u32 up = b == 2 ? 2 : 1;
		u32 t = gBrightTop + up <= 5 ? gBrightTop + up : gBrightTop - up, bt = gBrightBot + up <= 5 ? gBrightBot + up : gBrightBot - up;
		GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, t); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, bt);
		gLcdFlash = 1; gLightT = 0.06f;
	}
	if (gLcdFlash && (gLightT -= dt) <= 0) { GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, gBrightTop); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, gBrightBot); gLcdFlash = 0; }
}

// velocity: PICO's curve, velocity = 1 + 126 * (1 - t)^c, c = 0.70 * 2^(CURVE / 5); t = 0 is the firmest hit.
// t comes from where the pad is touched (POSITION: top = firm) or how loud the tap sounds (MIC).
static float gMicMax = 4000;
static int micPeak(int ms) {
	if (!gMicOK) return 0;
	u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2; int k = (int)(MICRATE * ms / 1000), pk = 0;
	const s16* m = (const s16*)gMicBuf;
	for (int i = 0; i < k; i++) { int v = abs(m[(end + N - 1 - i) % N]); if (v > pk) pk = v; }
	return pk;
}
static float micT(int pk) { if (pk > gMicMax) gMicMax = pk; float r = sqrtf(pk / gMicMax); return 1.0f - (r > 1 ? 1 : r); }
static int velCurve(float t) {
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	float c = 0.70f * powf(2.0f, gG[G_CURVE] / 5.0f);
	return clampi((int)roundf(1 + 126 * powf(1 - t, c)), 1, 127);
}
static int padVel(float posT) {   // posT: 0 at the top of the pad, 1 at the bottom
	switch (gG[G_VELMODE]) {
	case 1: return velCurve(posT);
	case 2: case 3: return gMicOK ? velCurve(micT(micPeak(50))) : gG[G_FIXVEL];
	}
	return gG[G_FIXVEL];
}
static void micVelTick(float dt) {   // TAP MIC: tap near the mic to play the selected pad
	gMicMax *= 1.0f - dt * 0.03f; if (gMicMax < 2500) gMicMax = 2500;
	if (gG[G_VELMODE] != 3 || !gMicOK) return;
	static float hold = 0, prev = 0;
	int pk = micPeak(18);
	hold -= dt;
	if (hold <= 0 && pk > 1800 && pk > gMicMax * 0.12f && pk > prev * 1.8f) { tapPush(gSel, velCurve(micT(pk)), 1, 0); hold = 0.07f; }
	prev = pk;
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
	if (bpm >= 60 && bpm <= 200) gG[G_BPM] = (s16)bpm;
	if (step >= 0) { if (!gPlaying) gPlayReq = 1; gSnap = step & 15; }
}
static void linkTick(float dt) {
	if (!gLinkUp) return;
	static int lastTick = -2; static float hb = 0;
	int tk = gPlaying && gTick >= 0 ? gLoopPos : -1, send = 0;
	if (tk != lastTick && tk >= 0) send = 1;
	if ((hb -= dt) <= 0) { hb = 0.5f; send = 1; }
	lastTick = tk;
	int bpm = gG[G_BPM], step = tk < 0 ? -1 : tk % 16;
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
static void setSteps(int p, int t, const char* s) {   // "x" = hit (110), "X" = loud (127), "o" = ghost (60), "." = rest
	Track* tr = &gPat[p].t[t];
	for (int i = 0; i < NSTEP && s[i]; i++) { tr->on[i] = s[i] != '.'; tr->vel[i] = s[i] == 'X' ? 127 : s[i] == 'o' ? 60 : 110; }
}
static void kitDefaults(void) {
	static const s8 PAN[NT] = {0, 0, -20, 0, 20, -10, 5, 12, 12, -10, 15, -15, 0, 0, 10, -5};
	for (int t = 0; t < NT; t++) {
		for (int k = 0; k < NK; k++) gKit[t][k] = KR[k][2];
		gKit[t][K_VOICE] = t; gKit[t][K_PAN] = PAN[t];
	}
	gKit[CH][K_CHOKE] = gKit[OH][K_CHOKE] = 1;
	gKit[CP][K_SEND] = 25; gKit[RS][K_SEND] = 30;
}
static void defaults(void) {
	for (int p = 0; p < NPAT; p++) clearPattern(&gPat[p]);
	kitDefaults();
	for (int i = 0; i < NG; i++) gG[i] = GR[i][2];
	memset(gMute, 0, sizeof(gMute));
	for (int i = 0; i < NSONG; i++) { gSong[i].pat = i < 4 ? i : -1; gSong[i].rep = i < 4 ? 2 : 0; gSong[i].tr = 0; }
	// A: boom bap
	setSteps(0, BD, "X......x..x....."); setSteps(0, SD, "....X.......X..o"); setSteps(0, CH, "x.x.x.x.x.x.x.xx"); setSteps(0, RS, ".......o......o.");
	for (int i = 1; i < 16; i += 2) gPat[0].t[CH].vel[i] = 64;
	gPat[0].t[SD].nud[12] = 2;
	// B: house
	setSteps(1, B9, "X...x...X...x..."); setSteps(1, CP, "....x.......x..."); setSteps(1, CH, "x.x.x.x.x.x.x.x."); setSteps(1, OH, "..x...x...x...x.");
	// C: electro
	setSteps(2, BD, "X.....x...x....."); setSteps(2, SD, "....X.......X..."); setSteps(2, CH, "xxXxxxXxxxXxxxXx"); setSteps(2, CB, "..x.....x..x....");
	setSteps(2, CP, "............x...");
	// D: techno, with a 12-step hat (polymeter) and a tom on its 4th pass
	setSteps(3, B9, "X...X...X...X..."); setSteps(3, RC, "..x...x...x...x."); setSteps(3, OH, "..x...x...x...x."); setSteps(3, S9, "....x.......x..x");
	setSteps(3, LT, "..........x....."); gPat[3].t[LT].cond[10] = 10; gPat[3].t[CH].len = 12; setSteps(3, CH, "x.xx.xx.xx.x");
}
static void save(void) {
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/ds1000", 0777);
	FILE* f = fopen(SAVEPATH, "wb"); if (!f) { say("COULD NOT SAVE"); return; }
	u32 hdr[4] = {0x4B315344, SAVEVER, sizeof(Pattern), NG};
	s16 g[NG]; for (int i = 0; i < NG; i++) g[i] = gG[i];
	fwrite(hdr, 4, 4, f); fwrite(gPat, sizeof(gPat), 1, f); fwrite(gKit, sizeof(gKit), 1, f); fwrite(g, sizeof(g), 1, f);
	fwrite(gMute, sizeof(gMute), 1, f); fwrite(gSong, sizeof(gSong), 1, f);
	fclose(f); say("SAVED");
}
static void load(void) {
	FILE* f = fopen(SAVEPATH, "rb"); if (!f) return;
	u32 hdr[4] = {0};
	s16 g[NG];
	int ok = fread(hdr, 4, 4, f) == 4 && hdr[0] == 0x4B315344 && hdr[1] == SAVEVER && hdr[2] == sizeof(Pattern) && hdr[3] == NG &&
		fread(gPat, sizeof(gPat), 1, f) == 1 && fread(gKit, sizeof(gKit), 1, f) == 1 && fread(g, sizeof(g), 1, f) == 1 &&
		fread(gMute, sizeof(gMute), 1, f) == 1 && fread(gSong, sizeof(gSong), 1, f) == 1;
	fclose(f);
	if (!ok) { defaults(); say("OLD SAVE NOT READ - DEFAULTS"); return; }
	for (int i = 0; i < NG; i++) gG[i] = clampi(g[i], GR[i][0], GR[i][1]);
	for (int t = 0; t < NT; t++) for (int k = 0; k < NK; k++) gKit[t][k] = clampi(gKit[t][k], KR[k][0], KR[k][1]);
	say("LOADED");
}
static int patUsed(int p) { for (int t = 0; t < NT; t++) for (int s = 0; s < NSTEP; s++) if (gPat[p].t[t].on[s]) return 1; return 0; }

// PATTERN: Euclidean or preset bars written onto the selected pad, over the whole loop
static int gStyle = 0, gHits = 5, gESteps = 16, gRotate = 0;
static int styleHit(int i, int n) {
	switch (gStyle) {
	case 0: return ((i * gHits) % n) < gHits;
	case 1: return i % 4 == 0;
	case 2: return i % 8 == 4;
	case 3: return i % 4 == 2;
	case 4: return i % 2 == 0;
	case 5: return 1;
	case 6: return i % 8 == 0 || i % 8 == 3 || i % 8 == 6;
	case 7: { int j = i % 16; return j == 0 || j == 3 || j == 6 || j == 10 || j == 12; }
	}
	return (int)(rnd() % (u32)n) < gHits;
}
static void patternWrite(void) {
	undoKeep(gCur);
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[gSel];
	int len = trLenOf(p, gSel), n = gStyle == 0 ? gESteps : 16;
	clearTrack(tr);
	for (int s = 0; s < len; s++) { int i = ((s - gRotate) % n + n) % n; if (styleHit(i, n)) { tr->on[s] = 1; tr->vel[s] = 110; } }
	say("WRITTEN");
}
static void shiftTrack(int t, int dir) {
	undoKeep(gCur);
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[t]; Track o = *tr;
	int len = trLenOf(p, t);
	for (int s = 0; s < len; s++) {
		int d = (s + dir + len) % len;
		tr->on[d] = o.on[s]; tr->vel[d] = o.vel[s]; tr->cond[d] = o.cond[s]; tr->sub[d] = o.sub[s]; tr->nud[d] = o.nud[s]; tr->note[d] = o.note[s];
	}
}
static void mutate(int t) {   // the selected pad only: move some hits a step, vary velocities, maybe add or drop one
	undoKeep(gCur);
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[t];
	int len = trLenOf(p, t);
	for (int s = 0; s < len; s++) {
		if (!tr->on[s]) continue;
		tr->vel[s] = (u8)clampi(tr->vel[s] + (int)(rnd() % 33) - 16, 20, 127);
		if (rnd() % 4 == 0) { int d = (s + (rnd() % 2 ? 1 : len - 1)) % len; if (!tr->on[d]) { tr->on[d] = 1; tr->vel[d] = tr->vel[s]; tr->on[s] = 0; } }
	}
	if (rnd() % 2) { int s = rnd() % len; if (!tr->on[s]) { tr->on[s] = 1; tr->vel[s] = 70; } else if (s % 4) tr->on[s] = 0; }
	say("MUTATED");
}


// ---------------------------------------------------------------- PICO-style menus, pages and knobs
enum { MN_MIX, MN_SND, MN_ENV, MN_FX, MN_SEQ, MN_GLO, NMENU };
static const char* MENU_NAMES[NMENU] = {"MIX", "SND", "ENV", "FX", "SEQ", "GLO"};

#define KN_K0 1
#define KN_G0 (KN_K0 + NK)
#define KK(x) (KN_K0 + (x))
#define KG(x) (KN_G0 + (x))
enum { KN_PAD = KN_G0 + NG, KN_BAR, KN_CLRPAD, KN_SVEL, KN_SNUDGE, KN_SCOND, KN_SRATCH, KN_PUTYPE, KN_PUBAR, KN_PUMOVE, KN_PULEN,
	KN_NSTEP, KN_NNOTE, KN_NVEL, KN_LOOP, KN_BARS, KN_SSTEP, KN_SLOOP, KN_SREP, KN_STRANSP, KN_STYLE, KN_HITS, KN_ESTEPS,
	KN_WRITE, KN_ROTATE, KN_SHL, KN_SHR, KN_MUTATE, KN_PRATE2, KN_PLEN, KN_LCLEAR, KN_COPYTO, KN_COPY, KN_SCOPYTO, KN_SCOPY,
	KN_SAVE, KN_RESET, KN_VER, KN_LINK, KN_LASTV, KN_END };
static const char* KNAME_K[NK] = {"VOICE", "TUNE", "FINE", "TONE", "START", "DIR", "CHOKE", "BITS", "RATE", "CUTOFF", "RESO",
	"ENV", "TYPE", "LOCUT", "ATTACK", "DECAY", "LEVEL", "PAN", "DRIVE", "ECHO"};
static const char* KNAME_G[NG] = {"TEMPO", "SWING", "QUANT", "STEPS", "FILTER", "DRIVE", "ECHO", "ECHO T", "AUTOPAN", "DEPTH",
	"VOLUME", "FADER", "PUNCH", "RATE", "DECAY", "LEVEL", "MOTION", "TILT", "TWIST", "SHAKE", "CLICK", "COUNT", "TICK", "LEVEL",
	"OUTPUT", "BEAT LT", "VELOCITY", "CURVE", "FIXED", "ROLL", "SCALE", "KEY", "SONG"};
static const char* KNAME_S[KN_END - KN_PAD] = {"PAD", "BAR", "CLR PAD", "VEL", "NUDGE", "COND", "RATCH", "PUNCH", "BAR", "MOVE",
	"LENGTH", "STEP", "NOTE", "VEL", "LOOP", "BARS", "STEP", "LOOP", "REPEAT", "TRANSP", "STYLE", "HITS", "STEPS", "WRITE",
	"ROTATE", "SHIFT <", "SHIFT >", "MUTATE", "RATE", "LEN", "CLEAR", "COPY >", "COPY", "TO PAD", "COPY", "SAVE", "RESET",
	"VERSION", "LINK", "LAST"};

typedef struct { const char* name; u8 menu, sub; u16 k[4]; const char* hint; } Page;
static const Page PAGES[] = {
	{"HOME", MN_MIX, 0, {0, 0, 0, 0}, "DRAG A FADER   L: BANK A/B"},
	{"BUS FX", MN_MIX, 0, {KG(G_BFILT), KG(G_BDRIVE), KG(G_ECHO), KG(G_ECHOT)}, "FILTER: LEFT LOW PASS, RIGHT HIGH PASS"},
	{"PAN", MN_MIX, 1, {KG(G_APRATE), KG(G_APDEPTH), KG(G_VOLUME), 0}, "AUTO-PAN THE WHOLE BUS IN TIME"},
	{"OUT", MN_MIX, 0, {KK(K_LEVEL), KK(K_PAN), KK(K_DRIVE), KK(K_SEND)}, "THE SELECTED PAD   D-PAD UP/DOWN: PAD"},
	{"FADER", MN_MIX, 1, {KG(G_FADER), 0, 0, 0}, "WHAT THE HOME FADERS MOVE"},
	{"PAD", MN_SND, 0, {KK(K_VOICE), KK(K_TUNE), KK(K_FINE), KK(K_TONE)}, "VOICE: ANY OF THE 16 SOUNDS ON THIS PAD"},
	{"TRIM", MN_SND, 1, {KK(K_START), KK(K_DIR), KK(K_CHOKE), 0}, "CHOKE: PADS IN A GROUP CUT EACH OTHER"},
	{"CRUSH", MN_SND, 0, {KK(K_BITS), KK(K_SRATE), 0, 0}, "12 BIT AND 26K FOR THE SP SOUND"},
	{"COPY", MN_SND, 0, {KN_SCOPYTO, KN_SCOPY, 0, 0}, "KNOB 2 TWICE: COPY THE SOUND"},
	{"FILTER", MN_ENV, 0, {KK(K_CUT), KK(K_RES), KK(K_FENV), KK(K_FTYPE)}, "ENV +: SWEEPS DOWN   -: SWEEPS UP"},
	{"LOCUT", MN_ENV, 1, {KK(K_LOCUT), 0, 0, 0}, "LOW CUT ON THE PAD"},
	{"AMP", MN_ENV, 0, {KK(K_ATTACK), KK(K_DECAY), 0, 0}, "ATTACK AND DECAY OF THE HIT"},
	{"PUNCH", MN_FX, 0, {KG(G_PTYPE), KG(G_PRATE), KG(G_PDECAY), KG(G_PLEVEL)}, "HOLD X OR THE PUNCH KEY TO PUNCH IN"},
	{"MOTION", MN_FX, 0, {KG(G_MOTION), KG(G_TILT), KG(G_TWIST), KG(G_SHAKE)}, "TILT, TWIST AND SHAKE THE CONSOLE"},
	{"STEP", MN_SEQ, 0, {KN_PAD, KN_BAR, KG(G_ROLL), KN_CLRPAD}, "KEYS: THE PAD'S STEPS   R: PADS / STEPS"},
	{"DETAIL", MN_SEQ, 0, {KN_SVEL, KN_SNUDGE, KN_SCOND, KN_SRATCH}, "CIRCLE PAD: VELOCITY / NUDGE"},
	{"PUNCHES", MN_SEQ, 0, {KN_PUTYPE, KN_PUBAR, KN_PUMOVE, KN_PULEN}, "SELECT: NEXT PUNCH"},
	{"NOTES", MN_SEQ, 0, {KN_NSTEP, KN_NNOTE, KN_NVEL, KG(G_SCALE)}, "SELECT: NEXT NOTE   KEYS: ADD / DEL"},
	{"KEY", MN_SEQ, 1, {KG(G_KEY), 0, 0, 0}, "THE SCALE'S ROOT"},
	{"TEMPO", MN_SEQ, 0, {KG(G_BPM), KG(G_QUANT), KN_LOOP, KN_BARS}, "LOOP: NEXT PATTERN, AT THE LOOP END"},
	{"METER", MN_SEQ, 1, {KG(G_SWING), KG(G_METER), 0, 0}, "STEPS 12: TRIPLET GRID"},
	{"SONG", MN_SEQ, 0, {KG(G_SONG), KN_SSTEP, KN_SLOOP, KN_SREP}, "REPEAT 0 OR LOOP END: THE SONG ENDS"},
	{"TRANSP", MN_SEQ, 1, {KN_STRANSP, 0, 0, 0}, "TRANSPOSE THE KIT ON THIS SONG STEP"},
	{"PATTERN", MN_SEQ, 0, {KN_STYLE, KN_HITS, KN_ESTEPS, KN_WRITE}, "KNOB 4 TWICE: WRITE ONTO THE PAD"},
	{"ROTATE", MN_SEQ, 1, {KN_ROTATE, KN_SHL, KN_SHR, KN_MUTATE}, "MUTATE CHANGES THE SELECTED PAD ONLY"},
	{"PAD TIME", MN_SEQ, 0, {KN_PRATE2, KN_PLEN, 0, 0}, "THE PAD'S OWN SPEED AND LENGTH"},
	{"LOOP TOOLS", MN_SEQ, 0, {KN_LCLEAR, KN_COPYTO, KN_COPY, 0}, "TWICE TO CLEAR OR COPY"},
	{"SETUP", MN_GLO, 0, {KN_SAVE, KN_RESET, KN_VER, 0}, "START: SAVE AND QUIT"},
	{"CLICK", MN_GLO, 0, {KG(G_CLICK), KG(G_COUNT), KG(G_TICK), KG(G_CLKLVL)}, "COUNT-IN WHEN RECORDING"},
	{"LINK", MN_GLO, 0, {KN_LINK, 0, 0, 0}, "SYNC WITH SYRUP OR ANOTHER DS-1000"},
	{"OUTPUT", MN_GLO, 0, {KG(G_OUTPUT), KG(G_BLIGHT), 0, 0}, "AUTO: SURROUND ON HEADPHONES"},
	{"TOUCH", MN_GLO, 0, {KG(G_VELMODE), KG(G_CURVE), KN_LASTV, KG(G_FIXVEL)}, "POSITION: TOP OF THE PAD IS LOUDEST"},
};
#define NPAGES ((int)(sizeof(PAGES) / sizeof(PAGES[0])))
enum { PG_HOME = 0, PG_STEP = 14, PG_DETAIL = 15, PG_PUNCHES = 16, PG_NOTES = 17 };

static int gPage = PG_STEP, gLastPage[NMENU] = {0, 5, 9, 12, 14, 27};
static int gKnobSel = 0, gBank = 0, gStepsView = 1;
static int gViewBar = 0, gStepSel = 0, gPuSel = 0, gNoteRow = 0, gSongSel = 0, gCopyTo = 1, gSCopyTo = 1;
static int gRollLatch = 0, gEraseLatch = 0, gArmK = -1; static float gArmT = 0;

static int isKit(int id) { return id >= KN_K0 && id < KN_K0 + NK; }
static int isGlo(int id) { return id >= KN_G0 && id < KN_G0 + NG; }
static int isAct(int id) { return id == KN_CLRPAD || id == KN_WRITE || id == KN_SHL || id == KN_SHR || id == KN_MUTATE || id == KN_LCLEAR || id == KN_COPY || id == KN_SCOPY || id == KN_SAVE || id == KN_RESET; }
static int isTwice(int id) { return isAct(id) && id != KN_SHL && id != KN_SHR && id != KN_SAVE; }
static const char* knName(int id) { return isKit(id) ? KNAME_K[id - KN_K0] : isGlo(id) ? KNAME_G[id - KN_G0] : id >= KN_PAD && id < KN_END ? KNAME_S[id - KN_PAD] : ""; }

static int inKey(int n) { int r = ((n - gG[G_KEY]) % 12 + 12) % 12; return (SCALE_MASK[clampi(gG[G_SCALE], 0, 5)] >> r) & 1; }
static int snapNote(int v, int dir) { if (!dir) dir = 1; for (int i = 0; i < 12 && !inKey(v); i++) v += dir; return clampi(v, -12, 12); }

static void knRange(int id, int* lo, int* hi) {
	if (isKit(id)) { *lo = KR[id - KN_K0][0]; *hi = KR[id - KN_K0][1]; return; }
	if (isGlo(id)) { *lo = GR[id - KN_G0][0]; *hi = GR[id - KN_G0][1]; return; }
	Pattern* p = &gPat[gCur];
	*lo = 0; *hi = 0;
	switch (id) {
	case KN_PAD: case KN_LOOP: case KN_COPYTO: case KN_SCOPYTO: *hi = 15; break;
	case KN_BAR: case KN_PUBAR: *lo = 1; *hi = p->bars; break;
	case KN_SVEL: case KN_NVEL: *lo = 1; *hi = 127; break;
	case KN_SNUDGE: *lo = -6; *hi = 6; break;
	case KN_SCOND: *hi = 11; break;
	case KN_SRATCH: *lo = 1; *hi = 4; break;
	case KN_PUTYPE: *hi = 5; break;
	case KN_PUMOVE: *lo = 1; *hi = spBar(); break;
	case KN_PULEN: *lo = 1; *hi = 64; break;
	case KN_NSTEP: *lo = 1; *hi = trLenOf(p, gSel); break;
	case KN_NNOTE: case KN_STRANSP: *lo = -12; *hi = 12; break;
	case KN_BARS: *lo = 1; *hi = 4; break;
	case KN_SSTEP: *lo = 1; *hi = NSONG; break;
	case KN_SLOOP: *lo = -1; *hi = 15; break;
	case KN_SREP: *hi = 16; break;
	case KN_STYLE: *hi = 8; break;
	case KN_HITS: *lo = 1; *hi = 16; break;
	case KN_ESTEPS: *lo = 2; *hi = 16; break;
	case KN_ROTATE: *hi = 15; break;
	case KN_PRATE2: *hi = 6; break;
	case KN_PLEN: *hi = 64; break;
	case KN_LINK: *hi = 4; break;
	}
}
static int knGet(int id) {
	if (isKit(id)) return gKit[gSel][id - KN_K0];
	if (isGlo(id)) return gG[id - KN_G0];
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[gSel]; int s = gStepSel;
	switch (id) {
	case KN_PAD: return gSel;
	case KN_BAR: return gViewBar + 1;
	case KN_SVEL: case KN_NVEL: return tr->vel[s];
	case KN_SNUDGE: return tr->nud[s];
	case KN_SCOND: return tr->cond[s];
	case KN_SRATCH: return tr->sub[s] ? tr->sub[s] : 1;
	case KN_PUTYPE: return p->pu[gPuSel].type;
	case KN_PUBAR: return p->pu[gPuSel].start / spBar() + 1;
	case KN_PUMOVE: return p->pu[gPuSel].start % spBar() + 1;
	case KN_PULEN: return p->pu[gPuSel].len ? p->pu[gPuSel].len : 1;
	case KN_NSTEP: return s + 1;
	case KN_NNOTE: return tr->on[s] ? tr->note[s] : gNoteRow;
	case KN_LOOP: return gQueued;
	case KN_BARS: return p->bars;
	case KN_SSTEP: return gSongSel + 1;
	case KN_SLOOP: return gSong[gSongSel].pat;
	case KN_SREP: return gSong[gSongSel].rep;
	case KN_STRANSP: return gSong[gSongSel].tr;
	case KN_STYLE: return gStyle;
	case KN_HITS: return gHits;
	case KN_ESTEPS: return gESteps;
	case KN_ROTATE: return gRotate;
	case KN_PRATE2: return tr->rate;
	case KN_PLEN: return tr->len;
	case KN_COPYTO: return gCopyTo;
	case KN_SCOPYTO: return gSCopyTo;
	case KN_LINK: return gLink;
	case KN_LASTV: return gPadVel[gSel];
	}
	return 0;
}
static void stepOn(Track* tr, int s) { if (!tr->on[s]) { tr->on[s] = 1; tr->vel[s] = (u8)gG[G_FIXVEL]; tr->nud[s] = 0; tr->cond[s] = 0; tr->sub[s] = 1; tr->note[s] = (s8)gNoteRow; } }
static void selPad(int t) { gSel = clampi(t, 0, NT - 1); gBank = gSel / 8; }
static void knSet(int id, int v) {
	int lo, hi; knRange(id, &lo, &hi);
	if (lo == hi && !isKit(id) && !isGlo(id)) return;
	int old = knGet(id);
	v = clampi(v, lo, hi);
	if (id == KN_NNOTE) v = snapNote(v, v - old);
	if (v == old) return;
	if (isKit(id)) {
		int k = id - KN_K0; gKit[gSel][k] = (s16)v;
		if (k == K_CUT || k == K_RES || k == K_FTYPE || k == K_LOCUT) chanApply(gSel);
		else if (k != K_TUNE && k != K_FINE && k != K_CHOKE && k != K_LEVEL && k != K_PAN && k != K_SEND) markDirty(gSel);
		return;
	}
	if (isGlo(id)) {
		int g = id - KN_G0; gG[g] = (s16)v;
		if (g == G_OUTPUT) outputApply();
		if (g == G_MOTION && v) gTiltCal = 0;
		if (g == G_METER) { gViewBar = 0; gStepSel = 0; }
		return;
	}
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[gSel]; int s = gStepSel; Punch* pu = &p->pu[gPuSel];
	switch (id) {
	case KN_PAD: selPad(v); break;
	case KN_BAR: gViewBar = v - 1; break;
	case KN_SVEL: case KN_NVEL: stepOn(tr, s); tr->vel[s] = (u8)v; break;
	case KN_SNUDGE: stepOn(tr, s); tr->nud[s] = (s8)v; break;
	case KN_SCOND: stepOn(tr, s); tr->cond[s] = (u8)v; break;
	case KN_SRATCH: stepOn(tr, s); tr->sub[s] = (u8)v; break;
	case KN_PUTYPE: pu->type = (u8)v; if (v && !pu->len) pu->len = 2; pu->rate = (u8)gG[G_PRATE]; break;
	case KN_PUBAR: pu->start = (u8)((v - 1) * spBar() + pu->start % spBar()); break;
	case KN_PUMOVE: pu->start = (u8)((pu->start / spBar()) * spBar() + v - 1); break;
	case KN_PULEN: pu->len = (u8)v; break;
	case KN_NSTEP: gStepSel = v - 1; gViewBar = gStepSel / spBar(); break;
	case KN_NNOTE: gNoteRow = v; if (tr->on[s]) tr->note[s] = (s8)v; if (!gPlaying) tapPush(gSel, tr->on[s] ? tr->vel[s] : gG[G_FIXVEL], 0, v); break;
	case KN_LOOP: gQueued = v; if (!gPlaying) gCur = v; break;
	case KN_BARS: p->bars = (u8)v; if (gViewBar >= v) gViewBar = v - 1; break;
	case KN_SSTEP: gSongSel = v - 1; break;
	case KN_SLOOP: gSong[gSongSel].pat = (s8)v; if (v >= 0 && !gSong[gSongSel].rep) gSong[gSongSel].rep = 1; break;
	case KN_SREP: gSong[gSongSel].rep = (u8)v; break;
	case KN_STRANSP: gSong[gSongSel].tr = (s8)v; break;
	case KN_STYLE: gStyle = v; break;
	case KN_HITS: gHits = v > gESteps ? gESteps : v; break;
	case KN_ESTEPS: gESteps = v; if (gHits > v) gHits = v; break;
	case KN_ROTATE: gRotate = v; break;
	case KN_PRATE2: tr->rate = (u8)v; break;
	case KN_PLEN: tr->len = (u8)v; break;
	case KN_COPYTO: gCopyTo = v; break;
	case KN_SCOPYTO: gSCopyTo = v; break;
	case KN_LINK: linkSet(v); break;
	}
}
static void knText(int id, char* o, int n) {
	int v = knGet(id);
	if (isAct(id)) {
		static const char* AL[] = {"CLEAR", "WRITE", "<", ">", "MUTATE", "CLEAR", "COPY", "COPY", "SAVE", "RESET"};
		static const int AI[] = {KN_CLRPAD, KN_WRITE, KN_SHL, KN_SHR, KN_MUTATE, KN_LCLEAR, KN_COPY, KN_SCOPY, KN_SAVE, KN_RESET};
		const char* l = "";
		for (int i = 0; i < 10; i++) if (AI[i] == id) l = AL[i];
		snprintf(o, n, "%s", gArmK == id ? "AGAIN" : l); return;
	}
	if (isKit(id)) {
		switch (id - KN_K0) {
		case K_VOICE: snprintf(o, n, "%s", VNAME[clampi(v, 0, 15)]); return;
		case K_TUNE: case K_FINE: case K_FENV: snprintf(o, n, "%+d", v); return;
		case K_DIR: snprintf(o, n, v ? "REV" : "FWD"); return;
		case K_CHOKE: if (v) snprintf(o, n, "GROUP %d", v); else snprintf(o, n, "OFF"); return;
		case K_BITS: snprintf(o, n, "%d BIT", BITS_V[clampi(v, 0, 5)]); return;
		case K_SRATE: snprintf(o, n, "%.1fK", 32.7f / (1 + v)); return;
		case K_CUT: { float f = 30.0f * powf(470.0f, v / 100.0f); if (f >= 1000) snprintf(o, n, "%.1fK", f / 1000); else snprintf(o, n, "%d", (int)f); } return;
		case K_FTYPE: snprintf(o, n, "%s", FT_NAMES[clampi(v, 0, 5)]); return;
		case K_LOCUT: if (v) snprintf(o, n, "%d HZ", (int)(20.0f * powf(40.0f, v / 100.0f))); else snprintf(o, n, "OFF"); return;
		case K_ATTACK: snprintf(o, n, "%d MS", v); return;
		case K_PAN: if (!v) snprintf(o, n, "C"); else snprintf(o, n, "%c%d", v < 0 ? 'L' : 'R', abs(v) * 2); return;
		}
		snprintf(o, n, "%d", v); return;
	}
	if (isGlo(id)) {
		switch (id - KN_G0) {
		case G_SWING: snprintf(o, n, "%d%%", 50 + v); return;
		case G_QUANT: case G_MOTION: case G_BLIGHT: case G_SONG: snprintf(o, n, "%s", NAMES_ONOFF[v & 1]); return;
		case G_METER: snprintf(o, n, v ? "12" : "16"); return;
		case G_BFILT: if (v < 0) snprintf(o, n, "LP %d", -v); else if (v > 0) snprintf(o, n, "HP %d", v); else snprintf(o, n, "OFF"); return;
		case G_ECHOT: snprintf(o, n, "%s", ECHOT_NAMES[clampi(v, 0, 4)]); return;
		case G_APRATE: snprintf(o, n, "%s", AP_NAMES[clampi(v, 0, 4)]); return;
		case G_FADER: snprintf(o, n, "%s", FADER_NAMES[clampi(v, 0, 3)]); return;
		case G_PTYPE: snprintf(o, n, "%s", PT_NAMES[clampi(v, 0, 4)]); return;
		case G_PRATE: snprintf(o, n, "%s", PR_NAMES[clampi(v, 0, 3)]); return;
		case G_TILT: snprintf(o, n, "%s", TILT_NAMES[clampi(v, 0, 2)]); return;
		case G_TWIST: snprintf(o, n, "%s", TWIST_NAMES[clampi(v, 0, 1)]); return;
		case G_SHAKE: snprintf(o, n, "%s", SHAKE_NAMES[clampi(v, 0, 2)]); return;
		case G_CLICK: snprintf(o, n, "%s", CLICK_NAMES[clampi(v, 0, 2)]); return;
		case G_COUNT: snprintf(o, n, "%s", COUNT_NAMES[clampi(v, 0, 2)]); return;
		case G_TICK: snprintf(o, n, "%s", TICK_NAMES[clampi(v, 0, 1)]); return;
		case G_OUTPUT: snprintf(o, n, "%s", OUT_NAMES[clampi(v, 0, 2)]); return;
		case G_VELMODE: snprintf(o, n, "%s", VM_NAMES[clampi(v, 0, 3)]); return;
		case G_CURVE: snprintf(o, n, "%+d", v); return;
		case G_ROLL: snprintf(o, n, "%s", ROLL_NAMES[clampi(v, 0, 3)]); return;
		case G_SCALE: snprintf(o, n, "%s", SCALE_NAMES[clampi(v, 0, 5)]); return;
		case G_KEY: snprintf(o, n, "%s", NOTE_NAMES[clampi(v, 0, 11)]); return;
		}
		snprintf(o, n, "%d", v); return;
	}
	Track* tr = &gPat[gCur].t[gSel];
	switch (id) {
	case KN_PAD: case KN_SCOPYTO: snprintf(o, n, "%c%d %s", 'A' + v / 8, v % 8 + 1, VNAME[clampi(gKit[v][K_VOICE], 0, 15)]); return;
	case KN_BAR: snprintf(o, n, "%d/%d", v, gPat[gCur].bars); return;
	case KN_SVEL: case KN_NVEL: if (tr->on[gStepSel]) snprintf(o, n, "%d", v); else snprintf(o, n, "--"); return;
	case KN_SNUDGE: snprintf(o, n, "%+d/12", v); return;
	case KN_SCOND: snprintf(o, n, "%s", COND_NAMES[clampi(v, 0, 11)]); return;
	case KN_SRATCH: snprintf(o, n, "X%d", v); return;
	case KN_PUTYPE: snprintf(o, n, "%d %s", gPuSel + 1, v ? PT_NAMES[clampi(v - 1, 0, 4)] : "--"); return;
	case KN_PULEN: snprintf(o, n, "%d ST", v); return;
	case KN_NNOTE: snprintf(o, n, "%s %+d", NOTE_NAMES[((gG[G_KEY] + v) % 12 + 12) % 12], v); return;
	case KN_LOOP: case KN_COPYTO: snprintf(o, n, "%c", 'A' + v); return;
	case KN_SLOOP: if (v < 0) snprintf(o, n, "END"); else snprintf(o, n, "%c", 'A' + v); return;
	case KN_SREP: if (v) snprintf(o, n, "X%d", v); else snprintf(o, n, "END"); return;
	case KN_STRANSP: snprintf(o, n, "%+d", v); return;
	case KN_STYLE: snprintf(o, n, "%s", STYLE_NAMES[clampi(v, 0, 8)]); return;
	case KN_PRATE2: snprintf(o, n, "%s", RATE_NAMES[clampi(v, 0, 6)]); return;
	case KN_PLEN: if (v) snprintf(o, n, "%d", v); else snprintf(o, n, "LOOP"); return;
	case KN_LINK: snprintf(o, n, "%s", LINK_NAMES[clampi(v, 0, 4)]); return;
	case KN_LASTV: snprintf(o, n, "V%d", v); return;
	case KN_VER: snprintf(o, n, "%s", DS_VER); return;
	}
	snprintf(o, n, "%d", v);
}
static void knDo(int id) {
	Pattern* p = &gPat[gCur];
	char m[48];
	switch (id) {
	case KN_CLRPAD: undoKeep(gCur); clearTrack(&p->t[gSel]); say("PAD CLEARED"); break;
	case KN_WRITE: patternWrite(); break;
	case KN_SHL: shiftTrack(gSel, -1); say("SHIFTED <"); break;
	case KN_SHR: shiftTrack(gSel, 1); say("SHIFTED >"); break;
	case KN_MUTATE: mutate(gSel); break;
	case KN_LCLEAR: undoKeep(gCur); for (int t = 0; t < NT; t++) clearTrack(&p->t[t]); memset(p->pu, 0, sizeof(p->pu)); say("LOOP CLEARED"); break;
	case KN_COPY: if (gCopyTo == gCur) { say("PICK ANOTHER LOOP"); break; } undoKeep(gCopyTo); gPat[gCopyTo] = *p; snprintf(m, sizeof(m), "COPIED %c > %c", 'A' + gCur, 'A' + gCopyTo); say(m); break;
	case KN_SCOPY: if (gSCopyTo == gSel) { say("PICK ANOTHER PAD"); break; } memcpy(gKit[gSCopyTo], gKit[gSel], sizeof(gKit[0])); markDirty(gSCopyTo); chanApply(gSCopyTo); say("SOUND COPIED"); break;
	case KN_SAVE: save(); break;
	case KN_RESET: defaults(); for (int t = 0; t < NT; t++) { markDirty(t); chanApply(t); } outputApply(); say("FACTORY STATE"); break;
	}
}
static void knPress(int id) {
	if (!isAct(id)) return;
	if (!isTwice(id) || (gArmK == id && gArmT > 0)) { gArmK = -1; knDo(id); return; }
	gArmK = id; gArmT = 2.5f; say("AGAIN TO CONFIRM");
}
static void pageGo(int pg) {
	gPage = clampi(pg, 0, NPAGES - 1); gLastPage[PAGES[gPage].menu] = gPage; gKnobSel = 0; gArmK = -1;
	if (gPage == PG_STEP || gPage == PG_DETAIL || gPage == PG_NOTES) gStepsView = 1;
}
static int pageParent(int pg) { while (pg > 0 && PAGES[pg].sub) pg--; return pg; }
static void nextHit(void) {   // NOTES: SELECT moves to the next note
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[gSel]; int len = trLenOf(p, gSel);
	for (int i = 1; i <= len; i++) { int s = (gStepSel + i) % len; if (tr->on[s]) { gStepSel = s; gViewBar = s / spBar(); gNoteRow = tr->note[s]; return; } }
}

// ---------------------------------------------------------------- drawing
static C2D_TextBuf gTb;
#define RGB(r, g, b) C2D_Color32(r, g, b, 255)
#define C_BG RGB(80, 90, 108)
#define C_HDR RGB(52, 60, 76)
#define C_TXT RGB(255, 255, 255)
#define C_BLUE RGB(58, 122, 224)
#define C_PANEL RGB(208, 212, 218)
#define C_LBL RGB(42, 50, 66)
#define C_RED RGB(222, 64, 60)
#define C_KEY RGB(62, 70, 86)
#define C_LIT RGB(234, 238, 244)
#define C_DIM RGB(96, 106, 124)
static void rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0.5f, w, h, c); }
static void frame(float x, float y, float w, float h, u32 c) { rect(x, y, w, 1, c); rect(x, y + h - 1, w, 1, c); rect(x, y, 1, h, c); rect(x + w - 1, y, 1, h, c); }
static void txt(float x, float y, float sc, u32 c, int align, const char* s) {   // align 0 left, 1 centre, 2 right; bold (drawn twice)
	C2D_Text t; C2D_TextParse(&t, gTb, s); C2D_TextOptimize(&t);
	u32 f = C2D_WithColor | (align == 1 ? C2D_AlignCenter : align == 2 ? C2D_AlignRight : 0);
	C2D_DrawText(&t, f, x, y, 0.5f, sc, sc, c);
	C2D_DrawText(&t, f, x + 0.6f, y, 0.5f, sc, sc, c);
}
static void padName(int t, char* o, int n) { snprintf(o, n, "%c%d %s", 'A' + t / 8, t % 8 + 1, VNAME[clampi(gKit[t][K_VOICE], 0, 15)]); }

static void drawTop(void) {
	char b[80];
	rect(0, 0, 400, 240, C_BG);
	// header: menu, bank, position, tempo, play state, battery
	rect(0, 0, 400, 18, C_HDR);
	txt(4, 1, 0.5f, C_TXT, 0, MENU_NAMES[PAGES[gPage].menu]);
	snprintf(b, sizeof(b), "BANK %c", 'A' + gBank); txt(46, 2, 0.44f, C_TXT, 0, b);
	int bar = spBar(), lp = gLoopPos;
	if (gPlaying && gTick >= 0) snprintf(b, sizeof(b), "%c %d.%d", 'A' + gCur, lp / bar + 1, (lp % bar) / spBeat() + 1);
	else snprintf(b, sizeof(b), "%c %d BAR%s", 'A' + gCur, gPat[gCur].bars, gPat[gCur].bars > 1 ? "S" : "");
	txt(112, 2, 0.44f, C_TXT, 0, b);
	if (gQueued != gCur) { snprintf(b, sizeof(b), "> %c", 'A' + gQueued); txt(180, 2, 0.44f, RGB(255, 210, 120), 0, b); }
	snprintf(b, sizeof(b), "%d BPM", (int)gG[G_BPM]); txt(214, 2, 0.44f, C_TXT, 0, b);
	const char* st = gCounting ? "COUNT" : gPlaying ? (gRec ? "REC" : "PLAY") : (gRec ? "ARMED" : "STOP");
	txt(286, 2, 0.44f, gRec ? RGB(255, 120, 110) : gPlaying ? RGB(120, 230, 140) : C_TXT, 0, st);
	if (gG[G_SONG]) { snprintf(b, sizeof(b), "S%d", gSongPos + 1); txt(334, 2, 0.4f, C_TXT, 0, b); }
	static u8 batt = 5; static int bt = 0;
	if (gPtmOK && bt-- <= 0) { bt = 120; u8 l = 5; if (R_SUCCEEDED(PTMU_GetBatteryLevel(&l))) batt = l; }
	frame(368, 4, 24, 10, C_TXT); rect(392, 7, 2, 4, C_TXT);
	rect(370, 6, 20.0f * (batt > 5 ? 5 : batt) / 5, 6, batt <= 1 ? C_RED : C_TXT);
	// message line: pad + last velocity, or the last action; page name on the right
	rect(0, 18, 400, 17, RGB(70, 80, 98));
	if (gMsgT > 0) snprintf(b, sizeof(b), "%s", gMsg);
	else { char pn[24]; padName(gSel, pn, sizeof(pn)); snprintf(b, sizeof(b), "%s  V%d", pn, gPadVel[gSel]); }
	txt(4, 19, 0.46f, C_TXT, 0, b);
	txt(396, 19, 0.46f, RGB(170, 200, 255), 2, PAGES[gPage].name);
	Pattern* p = &gPat[gCur];
	int cols = bar, base = gViewBar * bar;
	float cx = 46, cw = 350.0f / cols;
	if (gPage == PG_NOTES) {   // piano roll for the selected pad: a row per semitone, in-key rows lit, root red
		Track* tr = &p->t[gSel]; int len = trLenOf(p, gSel);
		float rh = 7.0f, y0 = 37;
		for (int r = 0; r < 25; r++) {
			int note = 12 - r; float y = y0 + r * rh;
			int root = ((note - gG[G_KEY]) % 12 + 12) % 12 == 0;
			u32 c = note == gNoteRow ? C_BLUE : root ? RGB(150, 70, 70) : inKey(note) ? RGB(104, 114, 132) : RGB(64, 72, 88);
			rect(cx, y, 350, rh - 1, c);
			if (r % 2 == 0 || note == gNoteRow) { snprintf(b, sizeof(b), "%s%d", NOTE_NAMES[((gG[G_KEY] + note) % 12 + 12) % 12], note); txt(42, y - 2, 0.3f, C_TXT, 2, b); }
		}
		for (int c = 0; c < cols; c++) {
			int s = base + c; float x = cx + c * cw;
			if (s >= len) { rect(x, y0, cw, 25 * rh, RGB(50, 56, 70)); continue; }
			if (tr->on[s]) { int r = 12 - tr->note[s]; rect(x + 1, y0 + r * rh, cw - 2, rh - 1, tr->vel[s] > 100 ? C_LIT : RGB(190, 196, 206)); }
			if (s == gStepSel) frame(x, y0, cw, 25 * rh, RGB(255, 200, 80));
			if (gPlaying && gPos[gSel] == s) rect(x, y0 + 25 * rh, cw, 3, C_TXT);
		}
	} else {   // the loop: every pad, the steps of the bar in view
		int punch = gPage == PG_PUNCHES || PAGES[gPage].menu == MN_FX;
		float rh = punch ? 10 : 11, y0 = 37;
		for (int t = 0; t < NT; t++) {
			float y = y0 + t * rh;
			Track* tr = &p->t[t]; int len = trLenOf(p, t);
			float fl = gFlash[t];
			u32 lab = t == gSel ? C_BLUE : RGB(60 + (int)(fl * 140), 68 + (int)(fl * 140), 84 + (int)(fl * 140));
			rect(2, y, 42, rh - 1, lab);
			txt(23, y - 1.5f, 0.32f, gMute[t] ? RGB(255, 140, 140) : C_TXT, 1, gMute[t] ? "MUTE" : VNAME[clampi(gKit[t][K_VOICE], 0, 15)]);
			for (int c = 0; c < cols; c++) {
				int s = base + c; float x = cx + c * cw;
				u32 off = (c / spBeat()) & 1 ? RGB(68, 77, 94) : RGB(60, 68, 84);
				if (s >= len) { rect(x, y, cw - 1, rh - 1, RGB(48, 54, 66)); continue; }
				rect(x, y, cw - 1, rh - 1, off);
				if (tr->on[s]) {
					float h = 2 + (rh - 3) * tr->vel[s] / 127.0f;
					rect(x + tr->nud[s] * cw / 24.0f, y + rh - 1 - h, cw - 1, h, gMute[t] ? RGB(130, 110, 110) : t == gSel ? C_LIT : RGB(196, 202, 212));
					if (tr->cond[s]) rect(x + 1, y + 1, 3, 3, RGB(255, 200, 80));
					if (tr->sub[s] > 1) rect(x + cw - 5, y + 1, 3, 3, C_BLUE);
				}
				if (t == gSel && s == gStepSel) frame(x, y, cw - 1, rh - 1, RGB(255, 200, 80));
				if (gPlaying && gTrRun && gPos[t] == s) rect(x, y + rh - 2, cw - 1, 1, RGB(120, 230, 140));
			}
		}
		if (punch) {   // punch lane
			float y = y0 + NT * rh + 2;
			rect(cx, y, 350, 12, RGB(56, 62, 78));
			txt(23, y - 1, 0.32f, C_TXT, 1, "PUNCH");
			for (int i = 0; i < 4; i++) {
				Punch* u = &p->pu[i]; if (!u->type) continue;
				int a = u->start - base, e = a + (u->len ? u->len : 1);
				if (e <= 0 || a >= cols) continue;
				if (a < 0) a = 0;
				if (e > cols) e = cols;
				rect(cx + a * cw, y + 1, (e - a) * cw - 1, 10, i == gPuSel ? RGB(255, 170, 70) : RGB(170, 120, 200));
				snprintf(b, sizeof(b), "%d", i + 1); txt(cx + a * cw + 2, y - 1, 0.3f, C_LBL, 0, b);
			}
		}
	}
	// hint line
	rect(0, 222, 400, 18, C_HDR);
	txt(4, 223, 0.42f, RGB(200, 210, 228), 0, PAGES[gPage].hint);
	if (gCounting) { rect(150, 90, 100, 60, C_BLUE); snprintf(b, sizeof(b), "%d", gCounting); txt(200, 92, 1.6f, C_TXT, 1, b); }
}

static const char* JOB_NAMES[8] = {"REC", "ROLL", "ERASE", "PUNCH", "QUANT", "SWING", "UNDO", "TAP"};
#define KEY_Y 21
#define MENU_Y 102
#define PANEL_X 88
static void drawBottom(void) {
	char b[48];
	rect(0, 0, 320, 240, C_BG);
	// menu tabs
	for (int m = 0; m < NMENU; m++) {
		float x = m * (320.0f / NMENU), w = 320.0f / NMENU - 1;
		int act = PAGES[gPage].menu == m;
		rect(x, 0, w, 19, act ? C_BLUE : C_HDR);
		txt(x + w / 2, 2, 0.46f, C_TXT, 1, MENU_NAMES[m]);
	}
	// keys: pads + jobs, or the selected pad's steps
	Pattern* p = &gPat[gCur]; Track* tr = &p->t[gSel];
	if (gStepsView) {
		int bar = spBar(), len = trLenOf(p, gSel);
		for (int i = 0; i < 16; i++) {
			float x = (i % 8) * 40, y = KEY_Y + (i / 8) * 40;
			if (i >= bar) { rect(x + 1, y + 1, 38, 38, RGB(56, 62, 76)); continue; }
			int s = gViewBar * bar + i, on = s < len && tr->on[s];
			rect(x + 1, y + 1, 38, 38, s >= len ? RGB(52, 58, 72) : on ? C_LIT : ((i / spBeat()) & 1 ? RGB(70, 79, 96) : C_KEY));
			if (on) {
				float h = 34.0f * tr->vel[s] / 127.0f;
				rect(x + 33, y + 37 - h, 4, h, C_BLUE);
				if (tr->nud[s]) rect(x + 18 + tr->nud[s] * 2.5f, y + 33, 4, 4, C_LBL);
				if (tr->cond[s]) { txt(x + 17, y + 4, 0.32f, C_LBL, 1, COND_NAMES[tr->cond[s] % 12]); }
				if (tr->sub[s] > 1) { snprintf(b, sizeof(b), "X%d", tr->sub[s]); txt(x + 17, y + 16, 0.36f, C_LBL, 1, b); }
				if (gPage == PG_NOTES) { snprintf(b, sizeof(b), "%s", NOTE_NAMES[((gG[G_KEY] + tr->note[s]) % 12 + 12) % 12]); txt(x + 17, y + 16, 0.36f, C_LBL, 1, b); }
			}
			snprintf(b, sizeof(b), "%d", s + 1); txt(x + 4, y + 25, 0.34f, on ? C_LBL : RGB(170, 178, 192), 0, b);
			if (s == gStepSel) { frame(x + 1, y + 1, 38, 38, RGB(255, 200, 80)); frame(x + 2, y + 2, 36, 36, RGB(255, 200, 80)); }
			if (gPlaying && gTrRun && gPos[gSel] == s) rect(x + 2, y + 2, 36, 3, RGB(120, 230, 140));
		}
	} else {
		for (int i = 0; i < 8; i++) {   // pads of the bank
			int t = gBank * 8 + i; float x = i * 40, y = KEY_Y, fl = gFlash[t];
			u32 c = gEraseLatch ? RGB(120 + (int)(fl * 120), 60, 60) : RGB(62 + (int)(fl * 170), 70 + (int)(fl * 166), 86 + (int)(fl * 158));
			rect(x + 1, y + 1, 38, 38, c);
			if (t == gSel) { frame(x + 1, y + 1, 38, 38, C_BLUE); frame(x + 2, y + 2, 36, 36, C_BLUE); }
			snprintf(b, sizeof(b), "%c%d", 'A' + gBank, i + 1); txt(x + 20, y + 4, 0.38f, C_TXT, 1, b);
			txt(x + 20, y + 20, 0.32f, gMute[t] ? RGB(255, 140, 140) : C_TXT, 1, VNAME[clampi(gKit[t][K_VOICE], 0, 15)]);
		}
		for (int i = 0; i < 8; i++) {   // one-press jobs
			float x = i * 40, y = KEY_Y + 40;
			int lit = (i == 0 && gRec) || (i == 1 && gRollLatch) || (i == 2 && gEraseLatch) || (i == 3 && gPunchLive) || (i == 4 && gG[G_QUANT]);
			rect(x + 1, y + 1, 38, 38, i == 0 && gRec ? C_RED : lit ? C_BLUE : RGB(70, 76, 92));
			txt(x + 20, y + 6, 0.36f, C_TXT, 1, JOB_NAMES[i]);
			if (i == 5) { snprintf(b, sizeof(b), "%d%%", 50 + gG[G_SWING]); txt(x + 20, y + 20, 0.34f, RGB(200, 210, 228), 1, b); }
			else { snprintf(b, sizeof(b), "%d", 9 + i); txt(x + 20, y + 20, 0.32f, RGB(170, 180, 200), 1, b); }
		}
	}
	// page list (current menu; the selected page's second entry indented under it)
	rect(0, MENU_Y, PANEL_X - 2, 240 - MENU_Y, C_HDR);
	int m = PAGES[gPage].menu, par = pageParent(gPage), row = 0;
	for (int i = 0; i < NPAGES; i++) {
		if (PAGES[i].menu != m) continue;
		if (PAGES[i].sub && pageParent(i) != par) continue;
		float y = MENU_Y + 2 + row * 13.5f;
		if (i == gPage) rect(1, y, PANEL_X - 4, 13, C_BLUE);
		txt(PAGES[i].sub ? 12 : 4, y - 0.5f, 0.38f, C_TXT, 0, PAGES[i].sub ? "> " : "");
		txt(PAGES[i].sub ? 20 : 4, y - 0.5f, 0.38f, C_TXT, 0, PAGES[i].name);
		row++;
	}
	// knob panel
	rect(PANEL_X, MENU_Y, 320 - PANEL_X, 240 - MENU_Y, C_PANEL);
	if (gPage == PG_HOME) {   // SP-1200-style faders for the bank
		int mode = gG[G_FADER];
		static const int FK[4] = {K_LEVEL, K_TUNE, K_DECAY, K_CUT};
		int k = FK[clampi(mode, 0, 3)];
		for (int i = 0; i < 8; i++) {
			int t = gBank * 8 + i; float x = PANEL_X + 6 + i * 28.5f;
			float v = (gKit[t][k] - KR[k][0]) / (float)(KR[k][1] - KR[k][0]);
			rect(x + 10, MENU_Y + 10, 4, 100, RGB(150, 156, 166));
			rect(x + 2, MENU_Y + 104 - v * 94, 20, 12, t == gSel ? C_BLUE : C_LBL);
			snprintf(b, sizeof(b), "%c%d", 'A' + gBank, i + 1); txt(x + 12, MENU_Y + 117, 0.32f, C_LBL, 1, b);
		}
		snprintf(b, sizeof(b), "FADERS: %s", FADER_NAMES[clampi(mode, 0, 3)]); txt(PANEL_X + 116, MENU_Y - 1, 0.3f, C_LBL, 1, b);
		return;
	}
	for (int i = 0; i < 4; i++) {
		float x = PANEL_X + 3 + (i % 2) * 115, y = MENU_Y + 3 + (i / 2) * 68;
		int id = PAGES[gPage].k[i];
		if (!id) continue;
		if (i == gKnobSel) frame(x - 1, y - 1, 113, 66, C_BLUE);
		rect(x + 2, y + 3, 14, 13, C_BLUE);
		snprintf(b, sizeof(b), "%d", i + 1); txt(x + 9, y + 2, 0.4f, C_TXT, 1, b);
		txt(x + 20, y + 2, 0.42f, C_LBL, 0, knName(id));
		rect(x + 2, y + 19, 106, 1, C_LBL);
		knText(id, b, sizeof(b));
		float sc = strlen(b) > 8 ? 0.55f : 0.75f;
		txt(x + 55, y + 26, sc, gArmK == id ? C_RED : C_LBL, 1, b);
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
	for (int t = 0; t < NT; t++) gPadVel[t] = 110;
	gTmp = (float*)malloc(MAXLEN * sizeof(float));
	for (int t = 0; t < NT; t++) for (int k = 0; k < 2; k++) gSmp[t][k] = (s16*)linearAlloc(MAXLEN * 2);

	sensorsInit();
	gAudioOK = R_SUCCEEDED(ndspInit());
	if (gAudioOK) {
		ndspSetMasterVol(1.0f);
		outputApply();
		for (int t = 0; t < NT; t++) {
			ndspChnReset(t); ndspChnSetInterp(t, NDSP_INTERP_LINEAR); ndspChnSetFormat(t, NDSP_FORMAT_MONO_PCM16); ndspChnSetRate(t, DSPRATE);
			chanApply(t);
		}
		busInit();
		clickInit();
		ndspSetCallback(seqTick, NULL);
	}

	// sound rendering on the second core (Syrup's worker trick)
	LightEvent_Init(&gWorkEv, RESET_ONESHOT);
	APT_SetAppCpuTimeLimit(80);
	s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Thread worker = threadCreate(workerMain, NULL, 64 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, 1, false);
	if (!worker) worker = threadCreate(workerMain, NULL, 64 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, -2, false);
	if (worker) { __atomic_store_n(&gDirty, 0xFFFF, __ATOMIC_SEQ_CST); LightEvent_Signal(&gWorkEv); }
	else for (int t = 0; t < NT; t++) renderTrack(t);

	hidSetRepeatParameters(18, 4);
	float playT = 0, dragAcc = 0;
	int shakeRoll = 0, shakePunch = 0, dragK = -1, lastX = 0, lastY = 0, touchWhat = 0, rollHeld = 0;
	u64 last = osGetTime();
	while (aptMainLoop()) {
		u64 now = osGetTime(); float dt = (now - last) / 1000.0f; last = now;
		if (dt > 0.1f) dt = 0.1f;
		hidScanInput();
		u32 kd = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp(), kr = hidKeysDownRepeat();
		if (kd & KEY_START) { save(); break; }
		if (kd & KEY_A) gPlayReq = gPlaying ? 2 : 1;
		if (kd & KEY_B) { gRec = !gRec; say(gRec ? (gPlaying ? "RECORDING" : "ARMED: A COUNTS IN") : "REC OFF"); }
		if (kd & KEY_L) { selPad((gSel + 8) % NT); }
		if (kd & KEY_R) gStepsView = !gStepsView;
		if (kr & KEY_DUP) selPad((gSel + NT - 1) % NT);
		if (kr & KEY_DDOWN) selPad((gSel + 1) % NT);
		if ((kr & (KEY_DLEFT | KEY_DRIGHT)) && gPage != PG_HOME) {
			int id = PAGES[gPage].k[gKnobSel];
			if (id && !isAct(id)) knSet(id, knGet(id) + ((kr & KEY_DRIGHT) ? 1 : -1));
		}
		if (kd & KEY_SELECT) {
			if (gPage == PG_PUNCHES) gPuSel = (gPuSel + 1) % 4;
			else if (gPage == PG_NOTES) nextHit();
			else pageGo((gPage + 1) % NPAGES);
		}
		// X: live punch (FX > PUNCH)   Y: note repeat on the selected pad
		int punchKey = 0;
		if (kh & KEY_X) punchKey = 1;
		if (kd & KEY_Y) { tapPush(gSel, gG[G_FIXVEL], 1, 0); rollStart(gSel); rollHeld = 1; }
		if ((ku & KEY_Y) && rollHeld) { rollHeld = 0; gRollT = -1; }
		// circle pad: velocity (up/down) and nudge (left/right) of the selected step
		if (kr & (KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT)) {
			Track* tr = &gPat[gCur].t[gSel]; int s = gStepSel;
			stepOn(tr, s);
			if (kr & KEY_CPAD_UP) tr->vel[s] = (u8)clampi(tr->vel[s] + 8, 1, 127);
			if (kr & KEY_CPAD_DOWN) tr->vel[s] = (u8)clampi(tr->vel[s] - 8, 1, 127);
			if (kr & KEY_CPAD_RIGHT) tr->nud[s] = (s8)clampi(tr->nud[s] + 1, -6, 6);
			if (kr & KEY_CPAD_LEFT) tr->nud[s] = (s8)clampi(tr->nud[s] - 1, -6, 6);
			if ((kr & (KEY_CPAD_UP | KEY_CPAD_DOWN)) && !gPlaying) tapPush(gSel, tr->vel[s], 0, tr->note[s]);
			char m[40]; snprintf(m, sizeof(m), "STEP %d  V%d  NUDGE %+d/12", s + 1, tr->vel[s], tr->nud[s]); say(m);
		}

		touchPosition tp; hidTouchRead(&tp);
		int tx = tp.px, ty = tp.py;
		if (kd & KEY_TOUCH) {
			touchWhat = 0; lastX = tx; lastY = ty; dragAcc = 0;
			if (ty < 20) {   // menu tabs: open the menu; again steps through its pages
				int m = clampi(tx * NMENU / 320, 0, NMENU - 1);
				if (PAGES[gPage].menu == m) { int n = gPage + 1; if (n >= NPAGES || PAGES[n].menu != m) { n = gPage; while (n > 0 && PAGES[n - 1].menu == m) n--; } pageGo(n); }
				else pageGo(gLastPage[m]);
			} else if (ty >= KEY_Y && ty < KEY_Y + 80) {
				int i = clampi(tx / 40, 0, 7) + (ty >= KEY_Y + 40 ? 8 : 0);
				if (gStepsView) {
					Pattern* p = &gPat[gCur]; Track* tr = &p->t[gSel];
					int s = gViewBar * spBar() + i;
					if (i < spBar() && s < trLenOf(p, gSel)) {
						gStepSel = s;
						if (tr->on[s]) { if (gPage == PG_NOTES && tr->note[s] != gNoteRow) tr->note[s] = (s8)gNoteRow; else clearStep(tr, s); }
						else { stepOn(tr, s); tr->note[s] = (s8)(gPage == PG_NOTES ? gNoteRow : 0); if (!gPlaying) tapPush(gSel, tr->vel[s], 0, tr->note[s]); }
					}
				} else if (i < 8) {   // pad
					int t = gBank * 8 + i;
					selPad(t);
					if (gEraseLatch) { undoKeep(gCur); if (gPlaying) gEraseT = t; else { clearTrack(&gPat[gCur].t[t]); say("PAD ERASED"); } }
					else {
						int v = padVel((ty - KEY_Y) / 40.0f);
						tapPush(t, v, 1, 0);
						if (gRollLatch) rollStart(t);
					}
					touchWhat = 1;
				} else {   // one-press jobs (keys 9-16)
					switch (i - 8) {
					case 0: gRec = !gRec; say(gRec ? (gPlaying ? "RECORDING" : "ARMED: A COUNTS IN") : "REC OFF"); break;
					case 1: gRollLatch = !gRollLatch; say(gRollLatch ? "ROLL: HOLD A PAD" : "ROLL OFF"); break;
					case 2: gEraseLatch = !gEraseLatch; say(gEraseLatch ? "ERASE: HOLD A PAD" : "ERASE OFF"); break;
					case 3: touchWhat = 2; break;
					case 4: gG[G_QUANT] = !gG[G_QUANT]; say(gG[G_QUANT] ? "QUANT ON" : "QUANT OFF: LIVE HITS KEEP THEIR FEEL"); break;
					case 5: { static const int SW[5] = {0, 4, 8, 12, 16}; int k = 0; while (k < 5 && SW[k] <= gG[G_SWING]) k++; gG[G_SWING] = SW[k % 5]; char m[24]; snprintf(m, sizeof(m), "SWING %d%%", 50 + gG[G_SWING]); say(m); } break;
					case 6: undoDo(); break;
					case 7: {   // tap tempo
						static u64 tt[4]; static int tn = 0;
						if (tn && now - tt[(tn - 1) % 4] > 2000) tn = 0;
						tt[tn % 4] = now; tn++;
						if (tn >= 2) { int k = tn < 4 ? tn : 4; float iv = (now - tt[(tn - k) % 4]) / (float)(k - 1); gG[G_BPM] = (s16)clampi((int)roundf(60000.0f / iv), 60, 200); }
						char m[24]; snprintf(m, sizeof(m), "TAP %d BPM", (int)gG[G_BPM]); say(m);
					} break;
					}
				}
			} else if (ty >= MENU_Y && tx < PANEL_X) {   // page list
				int m = PAGES[gPage].menu, par = pageParent(gPage), row = 0, want = (int)((ty - MENU_Y - 2) / 13.5f);
				for (int i = 0; i < NPAGES; i++) {
					if (PAGES[i].menu != m || (PAGES[i].sub && pageParent(i) != par)) continue;
					if (row++ == want) { pageGo(i); break; }
				}
			} else if (ty >= MENU_Y) {   // knob panel
				if (gPage == PG_HOME) touchWhat = 4;
				else {
					int k = clampi((tx - PANEL_X) / 115, 0, 1) + (ty >= MENU_Y + 70 ? 2 : 0), id = PAGES[gPage].k[k];
					if (id) { gKnobSel = k; if (isAct(id)) knPress(id); else { dragK = id; touchWhat = 3; } }
				}
			}
		}
		if (kh & KEY_TOUCH) {
			if (touchWhat == 3 && dragK > 0) {   // drag right or up to turn the knob
				int lo, hi; knRange(dragK, &lo, &hi);
				float ppu = 150.0f / (hi - lo > 0 ? hi - lo : 1); if (ppu < 1.2f) ppu = 1.2f; if (ppu > 30) ppu = 30;
				dragAcc += ((tx - lastX) - (ty - lastY)) / ppu;
				int st = (int)dragAcc;
				if (st) { dragAcc -= st; knSet(dragK, knGet(dragK) + st); }
			}
			if (touchWhat == 4) {   // faders
				int i = clampi((tx - PANEL_X - 6) / 28.5f, 0, 7), t = gBank * 8 + i;
				static const int FK[4] = {K_LEVEL, K_TUNE, K_DECAY, K_CUT};
				int k = FK[clampi(gG[G_FADER], 0, 3)];
				float v = (MENU_Y + 104 - ty + 6) / 94.0f; v = v < 0 ? 0 : v > 1 ? 1 : v;
				int nv = KR[k][0] + (int)roundf(v * (KR[k][1] - KR[k][0]));
				selPad(t);
				if (gKit[t][k] != nv) { gKit[t][k] = (s16)nv; if (k == K_CUT) chanApply(t); else if (k == K_DECAY) markDirty(t); }
			}
			if (touchWhat == 2) punchKey = 1;
			lastX = tx; lastY = ty;
		} else {
			if (touchWhat == 1) { gEraseT = -1; if (!shakeRoll && !rollHeld) gRollT = -1; }
			touchWhat = 0; dragK = -1;
		}

		sensorsUpdate(dt);
		int shake = gG[G_MOTION] ? gG[G_SHAKE] : 0;
		if (shake == 1) {
			if (gShakeV > 0.35f && gRollT < 0) { shakeRoll = 1; tapPush(gSel, gG[G_FIXVEL], 1, 0); rollStart(gSel); }
			if (shakeRoll && gShakeV < 0.15f) { shakeRoll = 0; if (!(kh & KEY_TOUCH) && !rollHeld) gRollT = -1; }
		} else if (shakeRoll) { shakeRoll = 0; gRollT = -1; }
		if (shake == 2) { if (gShakeV > 0.35f) shakePunch = 1; else if (gShakeV < 0.15f) shakePunch = 0; } else shakePunch = 0;
		gPunchLive = (punchKey || shakePunch) ? gG[G_PTYPE] + 1 : 0;

		beatLight(dt); micVelTick(dt); linkTick(dt);
		if (gMsgT > 0) gMsgT -= dt;
		if (gArmT > 0 && (gArmT -= dt) <= 0) gArmK = -1;
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
