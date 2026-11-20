/* SPDX-License-Identifier: GPL-3.0-only */
/* PICO: the screen and the controls. A header (menu and page, banks, loop / bar, tempo, play state), a message
 * line (the pad and its velocity, or what just happened), the body (the faders and pads; or the menu's pages
 * listed left and its knobs 2 x 2 on a light panel right), a hint line. Held SEQ turns the faders and pads into the step grid.
 * Drawn in regions, each only when what it shows changed. No hardware here: zp12.c reads the panel and lights
 * the LEDs (ui_leds), the host test draws the same screens (tests/zp12_ui_test.c).
 *
 * The knobs are the faders (channels 1-4, SEL held 5-8) unless a page is open; a page closes by itself after
 * a few seconds untouched, or with HOME. */

/* the panel's colours */
#define P_FRAME RGB(206, 208, 201)
#define P_NAVY RGB(80, 90, 108)               /* (PICO: the slate) */
#define P_BG RGB(80, 90, 108)
#define P_DARK RGB(42, 50, 66)
#define P_LABEL RGB(42, 50, 66)                 /* dark labels on the light knob panel */
#define P_PANEL RGB(208, 214, 224)
#define P_PANELOFF RGB(94, 104, 122)
#define P_CHIP RGB(46, 102, 204)
#define P_DIM RGB(168, 178, 194)
#define P_GO RGB(80, 220, 120)
#define P_WAVE RGB(130, 200, 255)
#define P_WAVEDIM RGB(84, 96, 118)
#define P_INK RGB(30, 48, 96)                   /* the frame's printing */
#define P_RED RGB(200, 46, 50)
#define P_LCD RGB(178, 190, 146)
#define P_LCDINK RGB(16, 24, 14)
#define P_LCDDIM RGB(84, 98, 70)
#define P_LED RGB(255, 40, 32)
#define P_LEDOFF RGB(70, 20, 22)
#define P_CAP RGB(214, 216, 220)
#define P_SLOT RGB(10, 12, 18)
#define P_PAD RGB(18, 18, 20)
#define P_RULE RGB(120, 140, 180)
#define P_STEP RGB(240, 200, 60)
static const uint16_t FAM_COL[5] = {RGB(38, 62, 112), RGB(236, 166, 44), RGB(52, 176, 160), RGB(200, 46, 50), RGB(150, 156, 172)};
static const uint16_t FAM_INK[5] = {RGB(255, 255, 255), RGB(30, 24, 10), RGB(8, 30, 28), RGB(255, 255, 255), RGB(20, 24, 34)};

/* a page untouched this long: back to the faders (GLO > SETUP > BACK; 0: never) */
static const uint8_t UI_BACK_S[5] = {6, 12, 30, 60, 0};
static const char *const UI_BACK_NAME[5] = {"6 S", "12 S", "30 S", "60 S", "OFF"};
#define UI_HOLD_MS 1300u                        /* REC held 0.7 s, then this much more (2 s in all): the loop cleared */
#ifndef ZP12_VER
#define ZP12_VER "0.0"                          /* build.py --release X.Y */
#endif
static const char ZP12_VERSION[] = ZP12_VER;

enum { PG_HOME, PG_PAD, PG_WAVE, PG_SOUND, PG_TRUNC, PG_OUT, PG_SFX, PG_FXMIX, PG_FILT, PG_CHO, PG_DLY, PG_REV, PG_SEG, PG_SEG2, PG_SONG, PG_SETUP, PG_CLICK, PG_OUTPUT,
       PG_SAMPLE, PG_N };
static const char *const PG_NAME[PG_N] = {"MIX", "PAD", "WAVE", "SOUND", "TRUNC", "OUT", "SENDS", "BLEND", "FILTER", "CHORUS", "DELAY", "REVERB",
                                          "LOOP", "TOOLS", "SONG", "SETUP", "CLICK", "OUTPUT", "SAMPLE"};
#ifndef FELUCCA_UAI
#define FELUCCA_UAI 0
#endif
#if FELUCCA_UAI
/* the sampler (sp_sampler.c): its settings and state, here because the pages show them */
static const uint16_t SMP_RATE[3] = {26040, 27500, 20000};
static const char *const SMP_RATE_NAME[3] = {"26K", "27K", "20K"};
static const char *const SMP_THR_NAME[7] = {"OFF", "-48", "-42", "-36", "-30", "-24", "-18"};
static const int32_t SMP_LP[3] = {25854, 26443, 22872};   /* its low-pass: 1 - exp(-2 pi 0.42 rate / 44100), Q15 */
enum { SM_IDLE, SM_ERASE, SM_WAIT, SM_REC, SM_DONE };
static struct {
    uint8_t state, rate, thr, mon, slot;         /* rate: SMP_RATE; thr: SMP_THR_NAME; mon 0..127: the input heard */
    uint16_t len10;                              /* TIME, tenths of a second */
    uint32_t off, end, erased, n, want;          /* the run [off, end), erased up to; samples taken, wanted */
    uint32_t peak;                               /* the input meter (falls slowly) */
} smp = {0, 0, 0, 80, 0, 10, 0, 0, 0, 0, 0, 0};
static void smp_press(void);
static uint32_t smp_room_samples(void);
static void smp_delete_pad(uint32_t pad);
#endif
/* buttons: the printed labels' matrix ids (as SLOOP's PANEL_DEFAULT); SEL is SLOOP's SCL */
enum { B_OCTDN = 0, B_OCTUP = 1, B_FX = 2, B_SEL = 3, B_ENV = 4, B_LFO = 5, B_EDIT = 6, B_GLO = 7, B_HOME = 8,
       B_SAVE = 9, B_ARP = 10, B_SEQ = 11, B_PLAY = 12, B_REC = 13 };
/* the page families (the button that opens them): their name, colour, pages; on a page the header says where
 * (EDIT > SOUND) and the tabs take the faders' place */
static const char *const FAM_NAME[5] = {"MIX", "EDIT", "FX", "SEQ", "GLO"};
/* PAD and BLEND open their menus: the four controls used most, side by side (the full pages follow them) */
static const uint8_t FAM_FIRST[5] = {PG_HOME, PG_PAD, PG_FXMIX, PG_SEG, PG_SETUP}, FAM_N[5] = {1, 6, 5, 3, 3 + FELUCCA_UAI};
static uint32_t fam_of(uint32_t pg) { return pg == PG_HOME ? 0u : pg <= PG_SFX ? 1u : pg <= PG_REV ? 2u : pg <= PG_SONG ? 3u : 4u; }
static const char *const DTIME_NAME[6] = {"1/4", "1/8", "1/16", "1/32", "8T", "16T"};
static const char *const CLICK_NAME[3] = {"OFF", "REC", "ON"};
static const char *const CIN_NAME[3] = {"OFF", "1 BAR", "2 BARS"};

static struct {
    uint8_t pair;                               /* 0: banks A + B on the keys, 1: C + D */
    uint8_t sel;                                /* the sound the knobs edit, 0..31 */
    uint8_t page;
    uint8_t multi;                              /* MULTI PITCH */
    uint8_t shift;                              /* SEL pressed (lit): the knobs are the faders 5-8 */
    uint8_t steps;                              /* SEQ held: the step grid on the keys */
    uint8_t step_bar;                           /* the bar the step grid shows */
    uint8_t song_cur;                           /* SONG page: the step under the knobs */
    uint8_t copy_to;                            /* SEG TOOLS: the target */
    uint8_t copy_pad;                           /* WAVE: the pad the sound goes to */
    uint8_t arm;                                /* a destructive knob turned once (it wants AGAIN): its page + 1 */
    uint8_t back;                               /* UI_BACK_S index (saved) */
    uint8_t turn_pad;                           /* the pad whose TUNE / DECAY / CUT was turned last ... */
    uint32_t turn_ms;                           /* ... and when (recording: its hits take them) */
    uint32_t held;                              /* the buttons held (bit = matrix id) */
    uint32_t keys;                              /* the keys held (bit = key 0..26; zp12.c): OUT's CHOP sets them all */
    uint8_t rec_on, rec_prev, holding;          /* REC: pressed, the state before, held into a CLEAR */
    uint8_t save_used, prev_page, fx_used;     /* SAVE held: a loop saved; the page before EDIT; FX held: a key pressed (a punch, not the pages) */
    uint8_t glo_used;                           /* GLO held: a key pressed (mute / solo, not the pages) */
    uint8_t lfo_used;                           /* LFO held: a pad pressed (ERASE, not the SAMPLE page) */
    uint32_t rec_t0, hold_t0;
    uint8_t mix[SP_NCH];
    uint32_t arm_ms, touch_ms;
    uint32_t hit_ms[32];                        /* when each pad was last heard (its key and pad light) */
    uint32_t chan_ms[SP_NCH];
    uint32_t tap_ms[4];                         /* TAP: the last taps */
    char msg[24];
    uint32_t msg_until;
    uint32_t sig_lcd, sig_mid, sig_pad, sig_led, sig_head;   /* what each region shows now */
    uint8_t force, save_req, factory_req;
    uint8_t vel, mode;                          /* the last hit's velocity; the body: faders, a page, the step grid */
} ui;

/* text blended onto bg (gfx.c's cv_text blends onto black) */
static int32_t cv_text_on(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t bg)
{
    uint16_t ramp[16];
    uint32_t a;
    int32_t r0 = bg >> 11, g0 = (bg >> 5) & 63, b0 = bg & 31, r1 = c >> 11, g1 = (c >> 5) & 63, b1 = c & 31;
    for (a = 0; a < 16u; a++)
        ramp[a] = (uint16_t)(((r0 + (r1 - r0) * (int32_t)a / 15) << 11) | ((g0 + (g1 - g0) * (int32_t)a / 15) << 5) |
                             (b0 + (b1 - b0) * (int32_t)a / 15));
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v)
                    cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[v]);
            }
        x += f->adv[gi];
    }
    return x;
}

static void put_int(char *b, int32_t v, uint32_t digits, int sign)
{
    uint32_t u = (uint32_t)(v < 0 ? -v : v), i;
    if (sign)
        *b++ = v < 0 ? '-' : '+';
    for (i = digits; i; i--) {
        b[i - 1u] = (char)('0' + u % 10u);
        u /= 10u;
    }
    b[digits] = 0;
}
static char *cat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *num(char *p, int32_t v, uint32_t digits, int sign) { put_int(p, v, digits, sign); return p + digits + (sign != 0); }

/* the samples: the kit's, then the own ones from the web editor (sp_samples.c fills their names and flags) */
#define SP_NUSER 24
static char ui_uname[SP_NUSER][9];
static uint8_t ui_uflags[SP_NUSER];              /* bit 0: stored for 45->33, bit 1: at double speed (an octave down) */
static const char *wave_name(uint32_t w)
{
    return w < KIT_NWAVE ? KIT_WAVE[w].name : w < KIT_NWAVE + SP_NUSER && sp_wave[w].n ? ui_uname[w - KIT_NWAVE] : "-----";
}
static const char *sound_name(uint32_t k)
{
    return wave_name(sp_sound[k].wave);
}
static void pad_label(char *b, uint32_t k) { b[0] = (char)('A' + k / 8u); b[1] = (char)('1' + k % 8u); b[2] = 0; }

/* the four columns of the page: label and value of KNOB 1-4 ("" = nothing on that knob) */
static void page_cols(char lab[4][8], char val[4][12])   /* (val: an own sample's name is 8 letters + 0) */
{
    const sp_sound_t *s = &sp_sound[ui.sel];
    uint32_t i;
    for (i = 0; i < 4u; i++) lab[i][0] = val[i][0] = 0;
#define COL(i, l, ...) do { cat(lab[i], l); __VA_ARGS__; } while (0)
    switch (ui.page) {
    case PG_HOME:
        for (i = 0; i < 4u; i++) {
            uint32_t c = (ui.shift ? 4u : 0u) + i;
            lab[i][0] = 'C'; lab[i][1] = 'H'; lab[i][2] = (char)('1' + c); lab[i][3] = 0;
            num(val[i], ui.mix[c], 3, 0);
        }
        break;
    case PG_PAD:                                 /* the pad: its sample, tune, start, decay (each also on its own page) */
        COL(0, "WAVE", cat(val[0], wave_name(s->wave))); COL(1, "TUNE", num(val[1], s->tune, 2, 1));
        COL(2, "START", num(val[2], s->start / 10, 3, 0)); COL(3, "DECAY", num(val[3], s->decay, 3, 0));
        break;
    case PG_FXMIX:                               /* the master filter and the three FX mixes, to balance while playing */
        COL(0, "FILTER", djf.v ? (void)(cat(val[0], djf.v < 0 ? "LP" : "HP"), num(val[0] + 2, djf.v < 0 ? -djf.v : djf.v, 2, 0))
                                : (void)cat(val[0], "OFF"));
        COL(1, "CHO", num(val[1], fxp.cmix, 3, 0)); COL(2, "DLY", num(val[2], fxp.dmix, 3, 0));
        COL(3, "SIZE", num(val[3], fxp.size, 3, 0));   /* (the reverb's amount is each pad's send; SENDS page) */
        break;
    case PG_WAVE:
        COL(0, "WAVE", cat(val[0], wave_name(s->wave)));
        COL(1, "COPY>", pad_label(val[1], ui.copy_pad));
        COL(2, "COPY", cat(val[2], ui.arm == PG_WAVE + 1u ? "AGAIN" : "-->"));
#if FELUCCA_UAI
        COL(3, "DELETE", cat(val[3], s->wave < KIT_NWAVE ? "--" : ui.arm == PG_WAVE + 2u ? "AGAIN" : "-->"));
#endif
        break;
    case PG_SOUND:
        COL(0, "TUNE", num(val[0], s->tune, 2, 1)); COL(1, "FINE", num(val[1], s->fine, 2, 1));
        COL(2, "DECAY", num(val[2], s->decay, 3, 0)); COL(3, "LEVEL", num(val[3], s->level, 3, 0));
        break;
    case PG_TRUNC:
        COL(0, "START", num(val[0], s->start / 10, 3, 0)); COL(1, "END", num(val[1], s->end / 10, 3, 0));
        COL(2, "DIR", cat(val[2], (s->flags & SPF_REVERSE) ? "REV" : "FWD"));
        COL(3, "SPEED", cat(val[3], (s->flags & SPF_33) ? "33" : "45"));
        break;
    case PG_OUT:
        lab[0][0] = 'C'; lab[0][1] = 'H'; lab[0][2] = (char)('1' + SP_CH_OF(ui.sel)); lab[0][3] = 0;   /* the channel (its position) */
        SPF_CHOP(s->flags) ? (void)num(cat(val[0], "CHOP"), (int32_t)SPF_CHOP(s->flags), 1, 0) : (void)cat(val[0], "--"); COL(1, "PAN", num(val[1], s->pan, 2, 1));
        if (SP_CH_OF(ui.sel) < 2u) { COL(2, "CUT", num(val[2], s->cut, 3, 0)); COL(3, "RESO", num(val[3], s->reso, 3, 0)); }
        else { COL(2, "CUT", cat(val[2], "--")); COL(3, "RESO", cat(val[3], "--")); }   /* (only channels 1-2 filter) */
        break;
    case PG_SFX:
        COL(0, "DRIVE", num(val[0], s->drive, 3, 0)); COL(1, "CHO", num(val[1], s->send[0], 3, 0));
        COL(2, "DLY", num(val[2], s->send[1], 3, 0)); COL(3, "REV", num(val[3], s->send[2], 3, 0));
        break;
    case PG_FILT:                                /* the DJ filter: LP 64 .. OFF .. HP 63 */
        COL(0, "FILTER", djf.v ? (void)(cat(val[0], djf.v < 0 ? "LP" : "HP"), num(val[0] + 2, djf.v < 0 ? -djf.v : djf.v, 2, 0))
                                : (void)cat(val[0], "OFF"));
        COL(1, "RESO", num(val[1], djf.res, 3, 0));
        break;
    case PG_CHO:
        COL(0, "RATE", num(val[0], fxp.crate, 3, 0)); COL(1, "DEPTH", num(val[1], fxp.cdepth, 3, 0));
        COL(2, "MIX", num(val[2], fxp.cmix, 3, 0));
        break;
    case PG_DLY:
        COL(0, "TIME", cat(val[0], DTIME_NAME[fxp.dtime % 6])); COL(1, "FDBK", num(val[1], fxp.fdbk, 3, 0));
        COL(2, "COLOR", num(val[2], fxp.colr, 3, 0)); COL(3, "MIX", num(val[3], fxp.dmix, 3, 0));
        break;
    case PG_REV:
        COL(0, "SIZE", num(val[0], fxp.size, 3, 0)); COL(1, "DAMP", num(val[1], fxp.damp, 3, 0));
        COL(2, "PRE", num(val[2], fxp.pre, 2, 0));
        break;
    case PG_SEG: {
        const sq_seg_t *g = &sq_seg[sq.seg % SQ_NSEG];
        COL(0, "LOOP", num(val[0], sq.seg + 1, 2, 0));
        COL(1, "BARS", g->bars ? (void)num(val[1], g->bars, 2, 0) : (void)cat(val[1], "AUTO"));
        COL(2, "QUANT", cat(val[2], SQ_GRID_NAME[sq.quant % 7u]));
        COL(3, "SWING", num(val[3], SQ_SWING[sq.swing % 6u], 2, 0));
        break;
    }
    case PG_SEG2:
        COL(0, "CLEAR", cat(val[0], ui.arm == PG_SEG2 + 1u ? "AGAIN" : "-->"));
        COL(1, "COPY>", num(val[1], ui.copy_to + 1, 2, 0));
        COL(2, "COPY", cat(val[2], ui.arm == PG_SEG2 + 2u ? "AGAIN" : "-->"));
        break;
    case PG_SONG:
        COL(0, "STEP", num(val[0], ui.song_cur + 1, 2, 0));
        if (ui.song_cur < SQ_SONG_N) {
            COL(1, "SEG", num(val[1], sq_song[ui.song_cur].seg + 1, 2, 0));
            COL(2, "REPEAT", num(val[2], sq_song[ui.song_cur].rep, 2, 0));
        } else {
            COL(1, "SEG", cat(val[1], "END"));
        }
        COL(3, "SONG", sq.song_mode ? (void)num(val[3], sq.song_sel + 1, 1, 0) : (void)cat(val[3], "OFF"));
        break;
    case PG_SETUP:
        COL(0, "TEMPO", num(val[0], (int32_t)(sq.bpm10 / 10u), 3, 0)); COL(1, "BACK", cat(val[1], UI_BACK_NAME[ui.back % 5u]));
        COL(3, "RESET", cat(val[3], ui.arm == PG_SETUP + 1u ? "AGAIN" : "-->"));
        break;
    case PG_CLICK:
        COL(0, "CLICK", cat(val[0], CLICK_NAME[sq.click % 3u])); COL(1, "COUNT", cat(val[1], CIN_NAME[sq.cin_bars % 3u]));
        COL(2, "DUB", cat(val[2], sq.dub_bar ? "BAR" : "NOW"));
        break;
    case PG_OUTPUT:
        COL(0, "SMOOTH", cat(val[0], sp_smooth ? "ON" : "OFF"));
        COL(1, "CLK OUT", cat(val[1], sq_clk_out ? "ON" : "OFF"));   /* (PICO: MIDI clock to USB, for SYRUP LINK) */
        break;
#if FELUCCA_UAI
    case PG_SAMPLE: {
        char *q;
        COL(0, "RATE", cat(val[0], SMP_RATE_NAME[smp.rate % 3u]));
        q = num(val[1], smp.len10 / 10u, smp.len10 >= 100u ? 2u : 1u, 0);
        *q++ = '.';
        q = num(q, smp.len10 % 10u, 1, 0);
        cat(q, "S");
        cat(lab[1], "TIME");
        COL(2, "THRESH", cat(val[2], SMP_THR_NAME[smp.thr % 7u]));
        COL(3, "MON", num(val[3], smp.mon, 3, 0));
        break;
    }
#endif
    default:
        break;
    }
#undef COL
}

/* ---- the regions (PICO: header, message line, the menu's pages listed left and its knobs 2 x 2 right, a hint) */
static uint32_t on_page(void) { return ui.page != PG_HOME; }   /* the knobs are a page's, not the faders */

static int32_t txt_b(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t bg)
{
    cv_text_on(x, y, f, s, c, bg);
    return cv_text_on(x + 1, y, f, s, c, bg);   /* (bold: twice, a pixel apart) */
}

static const char *const PG_HINT[PG_N] = {
    "KNOBS: FADERS  SEL: 5-8", "SAMPLE, TUNE, START, DECAY", "K1 SAMPLE  K3 COPY: TURN 2X", "PLAY A PAD TO EDIT IT", "K1 START  K2 END  K3 DIR",
    "PADS HELD TAKE THE CHOP", "SENDS TO THE FX BUS", "FILTER, MIXES, REV SIZE", "MASTER FILTER: LP / HP", "FX BUS: CHORUS", "FX BUS: DELAY",
    "FX BUS: REVERB", "BLACK KEYS PICK THE LOOP", "TURN TWICE: CLEAR / COPY", "K4: SONG ON / OFF", "SELECT: EVERY MENU",
    "CLICK, COUNT-IN, DUB", "CLK OUT: MIDI CLOCK TO USB", "REC: SAMPLE INTO THE PAD"};

/* the loop and where it is: L4 2.3 (song: S1.03 L4) */
static void pos_text(char *p)
{
    uint32_t t = sq.playing && sq.countin <= 0 ? sq.pos >> 16 : 0u;
    if (sq.song_mode && SQ_SONG_N) {
        p = cat(p, "S"); p = num(p, sq.song_sel + 1, 1, 0); p = cat(p, "."); p = num(p, sq.song_i + 1, 2, 0); p = cat(p, " ");
    }
    p = cat(p, "L");
    p = num(p, sq.seg + 1, sq.seg + 1u >= 10u ? 2u : 1u, 0);
    if (!sq.playing) return;
    p = cat(p, " ");
    p = num(p, (int32_t)(t / SQ_BAR + 1u), t / SQ_BAR + 1u >= 10u ? 2u : 1u, 0);
    p = cat(p, ".");
    num(p, (int32_t)(t % SQ_BAR / SQ_PPQ + 1u), 1, 0);
}

static void draw_head(void)                     /* menu > page; bank, loop / bar, tempo, play state */
{
    char b[24], *p;
    int32_t x;
    cv_begin(240, 22, P_DARK);
    if (on_page()) {
        uint32_t fm = fam_of(ui.page);
        p = cat(b, FAM_NAME[fm]);
        if (FAM_N[fm] > 1u) { p = cat(p, " "); cat(p, PG_NAME[ui.page]); }
    } else {
        cat(b, "PICO");
    }
    txt_b(4, 3, &FONT_S, b, C_WHITE, P_DARK);
    x = 222;                                    /* the play state: a triangle, a red dot (recording), a square */
    if (sq.recording || ((sq.rec_arm || sq.dub_wait) && (sq.beat & 1u) == 0u)) {
        cv_rect(x + 2, 5, 8, 12, P_LED); cv_rect(x, 7, 12, 8, P_LED);
    } else if (sq.playing) {
        int32_t r;
        for (r = 0; r < 12; r++) cv_rect(x + 1, 5 + r, (r < 6 ? r : 11 - r) * 2 + 1, 1, P_GO);
    } else {
        cv_rect(x + 1, 6, 10, 10, P_DIM);
    }
    p = b;
    pos_text(p);
    p = b + 0;
    while (*p) p++;
    p = cat(p, "  ");
    num(p, (int32_t)(sq.bpm10 / 10u), 3, 0);
    x = 216 - text_w(&FONT_S, b);
    txt_b(x, 3, &FONT_S, b, C_WHITE, P_DARK);
    b[0] = ui.pair ? 'C' : 'A'; b[1] = ui.pair ? 'D' : 'B'; b[2] = 0;   /* the banks on the keys */
    cv_text_on(x - 26, 3, &FONT_S, b, P_DIM, P_DARK);
    cv_blit(0, 0);
}

/* the message line: the pad and its last velocity (A3 HAT  V110), or what just happened; the page on the right */
static void draw_msg(void)
{
    char big[28], *p = big;
    big[0] = 0;
    cv_begin(240, 20, P_BG);
    if (ui.holding) {                           /* REC held: what letting go late does, and how long it still takes */
        uint32_t w = (fm1_ms - ui.hold_t0) * 232u / UI_HOLD_MS;
        p = cat(big, "CLEAR LOOP ");
        num(p, sq.seg + 1, sq.seg + 1u >= 10u ? 2u : 1u, 0);
        cv_rect(4, 17, 232, 2, P_DARK);
        cv_rect(4, 17, (int32_t)(w < 232u ? w : 232u), 2, P_LED);
    } else if (ui.msg[0] && (int32_t)(fm1_ms - ui.msg_until) < 0) {
        cat(big, ui.msg);
    } else if (ui.steps) {
        pad_label(p, ui.sel);
        p = cat(p + 2, " BAR ");
        p = num(p, ui.step_bar + 1, 1, 0);
        p = cat(p, "/");
        num(p, sq_seg[sq.seg % SQ_NSEG].bars ? sq_seg[sq.seg % SQ_NSEG].bars : 1, 1, 0);
    } else if (sq.playing && (sq.countin > 0 || sq.dub_wait)) {
        uint32_t left = sq.countin > 0 ? ((uint32_t)sq.countin + (SQ_PPQ << 16) - 1u) / (SQ_PPQ << 16)
                                        : (SQ_BAR - (sq.pos >> 16) % SQ_BAR + SQ_PPQ - 1u) / SQ_PPQ;
        p = cat(big, "COUNT ");
        num(p, (int32_t)left, left >= 10u ? 2u : 1u, 0);
    } else if (!sq.playing && sq.rec_arm) {
        p = cat(big, "ARMED LOOP ");
        num(p, sq.seg + 1, sq.seg + 1u >= 10u ? 2u : 1u, 0);
    } else {
        pad_label(p, ui.sel);
        p = cat(p + 2, " ");
        p = cat(p, sound_name(ui.sel));
        if (ui.vel) { p = cat(p, "  V"); num(p, ui.vel, 3, 0); }
    }
    txt_b(4, 1, &FONT_S, big, C_WHITE, P_BG);
    if (on_page() && 232 - text_w(&FONT_S, PG_NAME[ui.page]) > 8 + text_w(&FONT_S, big))
        cv_text_on(236 - text_w(&FONT_S, PG_NAME[ui.page]), 1, &FONT_S, PG_NAME[ui.page], P_DIM, P_BG);
    cv_blit(0, 22);
}

static void draw_hint(void)
{
    const char *h = PG_HINT[ui.page % PG_N];
    if (ui.steps) h = "WHITE KEYS: STEPS  OCT: BAR";
    cv_begin(240, 24, P_DARK);
    cv_text_on(120 - text_w(&FONT_S, h) / 2, 4, &FONT_S, h, P_DIM, P_DARK);
    cv_blit(0, 216);
}

/* the menu's pages down the left, this one lit */
static void draw_list(void)
{
    uint32_t fm = fam_of(ui.page), i;
    cv_begin(76, 174, P_BG);
    for (i = 0; i < FAM_N[fm]; i++) {
        uint32_t pg = FAM_FIRST[fm] + i;
        int32_t y = 4 + (int32_t)i * 26;
        int cur = pg == ui.page;
        if (cur) cv_rect(2, y, 72, 22, P_PANEL);
        txt_b(7, y + 3, &FONT_S, PG_NAME[pg], cur ? P_LABEL : C_WHITE, cur ? P_PANEL : P_BG);
    }
    cv_blit(0, 42);
}

static uint32_t wave_page(void)
{
    return ui.page == PG_PAD || ui.page == PG_WAVE || ui.page == PG_TRUNC
#if FELUCCA_UAI
        || ui.page == PG_SAMPLE
#endif
        ;
}

static void draw_wave(int32_t x0, int32_t y0, int32_t w, int32_t h)   /* the pad's sample, START / END marked */
{
    const sp_sound_t *s = &sp_sound[ui.sel];
    const sp_wave_t *wv = &sp_wave[s->wave % SP_NWAVE];
    int32_t i, mid = y0 + h / 2, a = (int32_t)s->start * w / 1000, e = (int32_t)s->end * w / 1000;
    cv_rect(x0, y0, w, h, P_DARK);
#if FELUCCA_UAI
    if (ui.page == PG_SAMPLE) {                 /* the sampler: what it does, the input meter */
        char big[24], *p = big;
        uint32_t m = smp.peak > 32767u ? 32767u : smp.peak, b = 0;
        while ((b + 1u) * (b + 1u) <= m) b++;    /* (a square root: the meter reads roughly in dB) */
        if (smp.state == SM_ERASE) {
            uint32_t need = ((smp.off + (smp.want + 1u) / 2u * 3u + 0xFFFu) & ~0xFFFu) - smp.off;
            p = cat(big, "ERASE ");
            p = num(p, (int32_t)((smp.erased - smp.off) * 100u / (need ? need : 1u)), 2, 0);
            cat(p, "%");
        } else if (smp.state == SM_WAIT) {
            cat(big, "WAIT INPUT");
        } else if (smp.state == SM_REC || smp.state == SM_DONE) {
            uint32_t t10 = smp.n * 10u / SMP_RATE[smp.rate % 3u];
            p = cat(big, "REC ");
            p = num(p, (int32_t)(t10 / 10u), t10 >= 100u ? 2u : 1u, 0);
            *p++ = '.';
            p = num(p, (int32_t)(t10 % 10u), 1, 0);
            cat(p, "S");
        } else if (!UAI_LIVE()) {
            cat(big, "PLAY INTO USB");
        } else {
            p = cat(big, "SAMPLE ");
            pad_label(p, ui.sel);
        }
        txt_b(x0 + 6, y0 + 6, &FONT_S, big, C_WHITE, P_DARK);
        cv_rect(x0 + 6, y0 + h - 14, w - 12, 6, P_BG);
        cv_rect(x0 + 6, y0 + h - 14, (int32_t)(b * (uint32_t)(w - 12) / 181u), 6, b > 176u ? P_LED : P_GO);
        return;
    }
#endif
    if (!wv->n) { txt_b(x0 + 6, y0 + 6, &FONT_S, "EMPTY", P_DIM, P_DARK); return; }
    for (i = 0; i < w; i++) {
        uint32_t i0 = wv->n * (uint32_t)i / (uint32_t)w, i1 = wv->n * (uint32_t)(i + 1) / (uint32_t)w, st, j;
        int32_t lo = 0, hi = 0;
        if (i1 <= i0) i1 = i0 + 1u;
        st = (i1 - i0) / 16u + 1u;              /* (16 looks a column: quick enough every frame) */
        for (j = i0; j < i1 && j < wv->n; j += st) {
            int32_t v = sp_sample_w(wv, j) * (h / 2 - 2) / 32768;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        cv_rect(x0 + i, mid - hi, 1, hi - lo + 1, i >= a && i <= e ? P_WAVE : P_WAVEDIM);
    }
    cv_rect(x0 + (a < w ? a : w - 1), y0, 1, h, P_STEP);
    cv_rect(x0 + (e < w ? e : w - 1), y0, 1, h, P_STEP);
}

/* the knobs 2 x 2 (1 2 / 3 4) on the light panel: number chip, name, rule, the value big; drawn whole, values
 * and all, in one pass (a blank panel between flickered) */
static void panel_draw(void)
{
    char lab[4][8], val[4][12];
    uint32_t i;
    int32_t top = 0, ch = 87;
    page_cols(lab, val);
    cv_rect(0, 0, 164, 174, P_BG);
    if (wave_page()) {
        draw_wave(2, 2, 160, 54);
        top = 58;
        ch = 58;
    }
    for (i = 0; i < 4u; i++) {
        int32_t x = 2 + (int32_t)(i & 1u) * 81, y = top + (int32_t)(i >> 1) * ch;
        char d[2] = {(char)('1' + i), 0};
        const felucca_font_t *f = text_w(&FONT_L, val[i]) <= 74 ? &FONT_L : &FONT_S;
        cv_rect(x, y + 1, 79, ch - 2, lab[i][0] ? P_PANEL : P_PANELOFF);
        if (!lab[i][0]) continue;
        cv_rect(x + 4, y + 4, 15, 17, P_CHIP);
        cv_text_on(x + 8, y + 5, &FONT_S, d, C_WHITE, P_CHIP);
        txt_b(x + 23, y + 5, &FONT_S, lab[i], P_LABEL, P_PANEL);
        cv_rect(x + 4, y + 23, 71, 1, P_LABEL);
        if (f == &FONT_L) cv_text_on(x + 5, y + (ch > 60 ? 38 : 25), f, val[i], P_LABEL, P_PANEL);
        else txt_b(x + 5, y + (ch > 60 ? 46 : 33), f, val[i], P_LABEL, P_PANEL);
    }
}

static void draw_panel(void)                    /* (in two halves: the device's canvas holds 80 rows of 232) */
{
    uint32_t h;
    for (h = 0; h < 174u; h += 87u) {
        cv_begin(164, 87, P_BG);
        cv_oy = -(int32_t)h;
        panel_draw();
        cv_oy = 0;
        cv_blit(76, 42 + h);
    }
}

/* the faders' page (and the step grid): under them KNOB 1-4, what they do now */
static void draw_strip(void)
{
    char lab[4][8], val[4][12];
    uint32_t i;
    cv_begin(232, 58, P_BG);
    page_cols(lab, val);
    for (i = 0; i < 4u; i++) {
        int32_t x = (int32_t)i * 58;
        char d[2] = {(char)('1' + i), 0};
        cv_rect(x, 2, 56, 54, P_PANEL);
        cv_rect(x + 3, 5, 13, 16, P_CHIP);
        cv_text_on(x + 6, 5, &FONT_S, d, C_WHITE, P_CHIP);
        txt_b(x + 19, 5, &FONT_S, lab[i], P_LABEL, P_PANEL);
        txt_b(x + 6, 30, &FONT_S, val[i], P_LABEL, P_PANEL);
    }
    cv_blit(4, 156);
}

static uint32_t fader_y(uint32_t lv) { return 44u - lv * 38u / 127u; }   /* the cap's top, in the region */

static void draw_faders(void)                   /* the eight channel levels; a cap lights while it sounds */
{
    uint32_t i, k;
    cv_begin(232, 62, P_NAVY);
    for (k = 0; k < 7u; k++)                    /* the scale lines behind the faders */
        cv_rect(12, 6 + (int32_t)k * 7, 208, 1, P_RULE);
    for (i = 0; i < SP_NCH; i++) {
        int32_t cx = 14 + (int32_t)i * 29, y = (int32_t)fader_y(ui.mix[i]);
        int on = (int32_t)(fm1_ms - ui.chan_ms[i]) < 90 && sp_ch[i].on;
        int knobbed = ui.page == PG_HOME && (i >= 4u) == (ui.shift != 0);
        int solo = (sp_solo >> i) & 1u, off = sp_solo ? !solo : (int)((sp_mute >> i) & 1u);
        char n[2] = {(char)(solo ? 'S' : (sp_mute >> i) & 1u ? 'M' : '1' + i), 0};   /* M muted, S soloed */
        cv_rect(cx - 1, 3, 3, 46, P_SLOT);
        cv_rect(cx - 9, y, 19, 7, on && !off ? P_LED : off ? RGB(84, 88, 98) : P_CAP);
        cv_rect(cx - 9, y + 3, 19, 1, on && !off ? RGB(120, 10, 10) : RGB(90, 90, 96));
        cv_text_on(cx - 4, 47, &FONT_S, n, solo ? P_STEP : (sp_mute >> i) & 1u ? P_LED : knobbed ? C_WHITE : P_RULE, P_NAVY);
    }
    cv_blit(4, 44);
}

static void draw_pads(void)                     /* the eight pads of the bank the last hit was in, their labels */
{
    uint32_t i, bank = ui.sel / 8u;
    cv_begin(232, 46, P_NAVY);
    for (i = 0; i < 8u; i++) {
        int32_t cx = 14 + (int32_t)i * 29;
        uint32_t k = bank * 8u + i;
        int lit = (int32_t)(fm1_ms - ui.hit_ms[k]) < 110;
        char l[3];
        cv_rect(cx - 11, 2, 23, 23, RGB(6, 6, 8));
        cv_rect(cx - 10, 3, 21, 21, lit ? P_LED : P_PAD);
        cv_rect(cx - 8, 5, 17, 2, lit ? RGB(255, 140, 120) : RGB(48, 48, 52));
        pad_label(l, k);
        cv_text_on(cx - 8, 28, &FONT_S, l, k == ui.sel ? C_WHITE : P_FRAME, P_NAVY);
    }
    cv_blit(4, 108);
}

/* SEQ held: the step grid in place of the faders and pads: the eight pads of the bank, 16 steps of the bar,
 * the pad on the keys bright, the playhead's column marked */
static void grid_rows(void)
{
    const sq_seg_t *g = &sq_seg[sq.seg % SQ_NSEG];
    uint32_t bank = ui.sel / 8u, r, i, t0 = ui.step_bar * SQ_BAR, ph = 0xFFFFu;
    uint32_t len = g->bars ? g->bars * SQ_BAR : SQ_BAR;
    if (sq.playing && sq.countin <= 0) {
        uint32_t t = (sq.pos >> 16) % len;
        if (t >= t0 && t < t0 + SQ_BAR) ph = (t - t0) / 24u;
    }
    for (r = 0; r < 8u; r++) {
        uint32_t k = bank * 8u + r, row = 0, lks = 0;
        int32_t y = 4 + (int32_t)r * 13;
        char l[3];
        for (i = 0; i < g->n; i++)
            if (g->ev[i].pad == k && g->ev[i].t >= t0 && g->ev[i].t < t0 + SQ_BAR) {
                row |= 1u << ((g->ev[i].t - t0) / 24u);
                lks |= (uint32_t)g->ev[i].lk << ((g->ev[i].t - t0) / 24u);
            }
        pad_label(l, k);
        cv_text_on(2, y - 3, &FONT_S, l, k == ui.sel ? C_WHITE : P_RULE, P_NAVY);
        for (i = 0; i < 16u; i++) {
            int32_t x = 24 + (int32_t)i * 12 + (int32_t)(i / 4u) * 2;
            uint16_t c = (row >> i) & 1u ? (k == ui.sel ? P_STEP : P_FRAME) : (k == ui.sel ? RGB(120, 132, 156) : RGB(96, 108, 128));
            cv_rect(x, y, 10, 10, c);
            if ((lks >> i) & 1u)                 /* a hit with its own TUNE / DECAY / CUT: a dot */
                cv_rect(x + 3, y + 3, 4, 4, P_RED);
            if (i == ph)
                cv_rect(x, y + 10, 10, 2, P_LED);
        }
    }
}

static void draw_grid(void)                      /* (in two halves: the device's canvas holds 80 rows, this is 108) */
{
    uint32_t h;
    for (h = 0; h < 108u; h += 54u) {
        cv_begin(232, 54, P_NAVY);
        cv_oy = -(int32_t)h;
        grid_rows();
        cv_oy = 0;
        cv_blit(4, 44 + h);
    }
}

static uint32_t sig_of(const void *p, uint32_t n, uint32_t h)
{
    const uint8_t *b = p;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

static void ui_note_played(void)                /* the pads the sequencer or the keys played: their lights */
{
    uint32_t m = sq.played, k;
    sq.played = 0;
    for (k = 0; m; k++, m >>= 1)
        if (m & 1u) {
            ui.hit_ms[k] = fm1_ms;
            ui.chan_ms[SP_CH_OF(k)] = fm1_ms;
        }
}

static void ui_draw(void)
{
    uint32_t i, s, lit = 0, on = 0, bank = ui.sel / 8u, mode;
    ui_note_played();
    sq.turn = sq.recording && (uint32_t)(fm1_ms - ui.turn_ms) < 600u ? 1u << ui.turn_pad : 0u;
    if (ui.page != PG_HOME && UI_BACK_S[ui.back % 5u] && (uint32_t)(fm1_ms - ui.touch_ms) > UI_BACK_S[ui.back % 5u] * 1000u && !ui.steps
#if FELUCCA_UAI
        && !(ui.page == PG_SAMPLE && smp.state != SM_IDLE)
#endif
        )
        ui.page = PG_HOME;                      /* untouched: the knobs are the faders again */
    if (ui.arm && (uint32_t)(fm1_ms - ui.arm_ms) > 1500u)
        ui.arm = 0;
    mode = ui.steps ? 2u : on_page() ? 1u : 0u;
    if (mode != ui.mode || ui.force) {          /* the body changes its kind: cleared, all drawn again */
        ui.mode = (uint8_t)mode;
        lcd_fill(0, 42, 240, 174, P_BG);
        ui.force = 1;
    }
    if (ui.force)
        ui.sig_lcd = ui.sig_mid = ui.sig_pad = ui.sig_led = ui.sig_head = 0;
    s = sig_of(&sp_sound[ui.sel], sizeof(sp_sound_t), 2166136261u ^ ui.sel * 7u ^ ui.page * 131u ^ sq.bpm10 * 7919u ^ ui.multi);
    s = sig_of(ui.mix, sizeof ui.mix, s ^ ui.shift ^ ui.steps * 3u ^ ui.step_bar * 29u ^ ui.arm * 37u ^ ui.copy_to * 41u);
    s = sig_of(&fxp, sizeof fxp, s ^ (uint32_t)(djf.v + 64) * 6151u ^ (uint32_t)djf.res * 97u ^ ui.back * 2203u ^ sp_smooth * 4409u);
    s = sig_of(sq_songs, sizeof sq_songs, s ^ ui.song_cur * 31u ^ SQ_SONG_N * 17u ^ sq.song_mode ^ sq.song_sel * 7u);
    s ^= (sq.seg * 977u) ^ (sq.quant * 31u) ^ (sq.swing * 7u) ^ (sq.click * 3u) ^ sq_seg[sq.seg % SQ_NSEG].bars * 101u;
    s ^= sq.recording * 5u ^ sq.rec_arm * 11u ^ sq.dub_wait * 19u ^ sq.cin_bars * 6007u ^ sq.dub_bar * 7121u;
    s ^= ui.vel * 40503u ^ ui.pair * 8191u ^ sq_clk_out * 131071u ^ ui.copy_pad * 524287u;
    if (sq.playing)
        s ^= ((sq.countin > 0 ? (uint32_t)sq.countin / (SQ_PPQ << 16) + 977u : (sq.pos >> 16) / SQ_PPQ) + 1u) * 2654435761u ^
             (sq.countin > 0) * 9u ^ sq.song_i * 13u ^ ((sq.beat & 1u) + 1u) * 33u;
    if (ui.msg[0] && (int32_t)(fm1_ms - ui.msg_until) < 0)
        s = sig_of(ui.msg, sizeof ui.msg, s ^ 0x55u);
    if (ui.holding)
        s ^= ((fm1_ms - ui.hold_t0) / 40u + 1u) * 0x9E3779B1u;
#if FELUCCA_UAI
    if (ui.page == PG_SAMPLE) {                   /* the meter: up at once, down slowly */
        uint32_t pk = uai.peak;
        uai.peak = 0;
        smp.peak = pk > smp.peak ? pk : smp.peak - smp.peak / 8u;
        s ^= (smp.peak / 512u + 1u) * 2246822519u ^ smp.state * 977u ^ (smp.erased >> 12) * 31u ^ (smp.n / 4000u) * 7919u ^
             (uint32_t)UAI_LIVE() * 3u ^ smp.mon * 5u ^ smp.len10 * 13u ^ smp.rate * 17u ^ smp.thr * 19u;
    }
#endif
    if (s != ui.sig_lcd || ui.force) {          /* the header, the message, the knobs: values and panel in one pass */
        ui.sig_lcd = s;
        draw_head();
        draw_msg();
        draw_hint();
        if (mode == 1u) { draw_list(); draw_panel(); }
        else draw_strip();
    }
    if (ui.steps) {
        const sq_seg_t *g = &sq_seg[sq.seg % SQ_NSEG];
        uint32_t len = g->bars ? g->bars * SQ_BAR : SQ_BAR;
        s = 0x9E37u ^ sq.gen * 2654435761u ^ ui.sel * 131u ^ ui.step_bar * 7u;
        if (sq.playing) s ^= (((sq.pos >> 16) % len) / 24u + 1u) * 40503u;
        if (s != ui.sig_mid || ui.force) { ui.sig_mid = s; draw_grid(); }
    } else if (mode == 0u) {                     /* the faders and the pads */
        for (i = 0; i < SP_NCH; i++)
            on |= (uint32_t)((int32_t)(fm1_ms - ui.chan_ms[i]) < 90 && sp_ch[i].on) << i;
        s = sig_of(ui.mix, sizeof ui.mix, on * 2654435761u ^ ui.shift * 3u ^ (sp_mute | sp_solo << 8) * 40503u) | 1u;
        if (s != ui.sig_mid || ui.force) { ui.sig_mid = s; draw_faders(); }
        for (i = 0; i < 8u; i++)
            lit |= (uint32_t)((int32_t)(fm1_ms - ui.hit_ms[bank * 8u + i]) < 110) << i;
        s = lit * 31u + ui.sel * 7919u + 1u;
        if (s != ui.sig_pad || ui.force) { ui.sig_pad = s; draw_pads(); }
    }
    ui.force = 0;
}

static void ui_say(const char *m)
{
    uint32_t i;
    for (i = 0; m[i] && i + 1u < sizeof ui.msg; i++) ui.msg[i] = m[i];
    ui.msg[i] = 0;
    ui.msg_until = fm1_ms + 1100u;
}

/* ---- playing. SP_HIT starts a sound: on the FM-1 a request to the audio ISR, on the host at once */
#ifndef SP_HIT
#define SP_HIT(k, vel, semis) sq_hit(k, vel, semis)
#endif
static void pad_hit_at(uint32_t k, uint32_t vel, int32_t semis)
{
    k %= SP_NSOUND;
    ui.sel = (uint8_t)k;
    ui.hit_ms[k] = fm1_ms;
    ui.chan_ms[SP_CH_OF(k)] = fm1_ms;
    ui.vel = (uint8_t)vel;
    SP_HIT(k, vel, semis);
}
static void pad_hit(uint32_t k, uint32_t vel) { pad_hit_at(k, vel, 0); }

/* the white key index of key k (0 = F3 .. 26 = G5), 0xFF for a black key */
static uint32_t white_of(uint32_t k)
{
    static const uint8_t WHITE_OF[27] = {0, 0xFF, 1, 0xFF, 2, 0xFF, 3, 4, 0xFF, 5, 0xFF, 6, 7, 0xFF, 8, 0xFF, 9,
                                         0xFF, 10, 11, 0xFF, 12, 0xFF, 13, 14, 0xFF, 15};   /* F3 .. G5 */
    return k < 27u ? WHITE_OF[k] : 0xFFu;
}
static uint32_t key_pad(uint32_t k)            /* white keys 1-8: bank A (C), 9-16: B (D) */
{
    uint32_t w = white_of(k);
    return w != 0xFFu ? ui.pair * 16u + w : 0xFFu;
}

/* loop (segment) b: now when stopped, else at the end of the loop playing (a black key, a MIDI Program Change); say it */
static void ui_loop(uint32_t b)
{
    char m[24], *p = cat(m, "LOOP ");
    sq_post(RQ_LOOP, 0, b);
    p = num(p, (int32_t)b + 1, b + 1u >= 10u ? 2u : 1u, 0);
    cat(p, sq.playing && b != sq.seg ? " NEXT" : "");
    ui_say(m);
}

/* key k down; erase: LFO held (the pad's hits go: as the playhead passes, or all at once when stopped) */
static void key_down(uint32_t k, int erase)
{
    uint32_t pad;
    if (k >= 27u)
        return;
    if ((ui.held >> B_FX) & 1u) {                /* FX held: white keys 1-4 punch in (zp12.c, sp_punch.c), no pad */
        static const char *const PUNCH[4] = {"ROLL 1/8", "ROLL 1/16", "REVERSE", "TAPE STOP"};
        ui.fx_used = 1;
        if (white_of(k) < 4u)
            ui_say(PUNCH[white_of(k)]);
        return;
    }
    if ((ui.held >> B_GLO) & 1u) {               /* GLO held: white keys 1-8 mute channels 1-8, 9-16 solo them */
        uint32_t w = white_of(k), c = w % 8u;
        char m[24], *p;
        ui.glo_used = 1;
        if (w == 0xFFu)
            return;
        if (w < 8u) sp_mute ^= (uint8_t)(1u << c);
        else sp_solo ^= (uint8_t)(1u << c);
        p = cat(m, w < 8u ? "MUTE " : "SOLO ");
        p = num(p, (int32_t)c + 1, 1, 0);
        cat(p, ((w < 8u ? sp_mute : sp_solo) >> c) & 1u ? "" : " OFF");
        ui_say(m);
        return;
    }
    if ((ui.held >> B_SAVE) & 1u) {              /* SAVE held: a black key saves the loop into that loop */
        if (white_of(k) == 0xFFu) {
            uint32_t b = 0, i;
            char m[24], *p = cat(m, "LOOP ");
            for (i = 0; i < k; i++) b += white_of(i) == 0xFFu;
            if (b != sq.seg)
                sq_post(RQ_COPY, 0, b);
            p = num(p, (int32_t)b + 1, b + 1u >= 10u ? 2u : 1u, 0);
            cat(p, " SAVED");
            ui_say(m);
            ui.save_req = 1;
            ui.save_used = 1;
        }
        return;
    }
    if (ui.steps) {                              /* SEQ held: a white key sets / clears the selected pad's step */
        uint32_t w = white_of(k);
        if (w != 0xFFu) {
            sq_post(RQ_STEP, ui.sel, ui.step_bar * SQ_BAR + w * 24u);
            if (!sq_seg[sq.seg % SQ_NSEG].bars)
                sq_seg[sq.seg % SQ_NSEG].bars = 1;   /* (a step written makes an AUTO segment a bar long) */
        }
        return;
    }
    if (white_of(k) == 0xFFu && !ui.multi && !erase) {   /* a black key: loop (segment) 1-11, at the loop's end */
        uint32_t b = 0, i;
        for (i = 0; i < k; i++) b += white_of(i) == 0xFFu;
        ui_loop(b);
        return;
    }
    if (ui.multi && !erase) {                    /* MULTI PITCH: the sound, F4 (key 12) as written */
        pad_hit_at(ui.sel, 110, (int32_t)k - 12);
        return;
    }
    pad = ui.multi ? ui.sel : key_pad(k);
    if (pad == 0xFFu)
        return;
    if (erase) {
        ui.lfo_used = 1;
        ui.sel = (uint8_t)pad;
        if (!sq.playing) {
            sq_post(RQ_WIPE, pad, 0);
            ui_say("ERASED");
        }
        return;
    }
    pad_hit(pad, 110);
}

static int again(uint32_t which)                 /* a destructive turn: the first arms (AGAIN), the second acts */
{
    if (ui.arm == which && (uint32_t)(fm1_ms - ui.arm_ms) < 1500u) {
        ui.arm = 0;
        return 1;
    }
    ui.arm = (uint8_t)which;
    ui.arm_ms = fm1_ms;
    return 0;
}

/* a 0..127 value's step: 3 a detent, more when turned fast (a half turn from 0 to 127) */
static int32_t accel(int32_t d)
{
    int32_t a = d < 0 ? -d : d;
    return d * (a >= 3 ? 8 : a == 2 ? 5 : 3);
}

/* the sample the selected pad plays, one on (the factory's, then the own ones): any sample on as many pads as
 * wanted, each with its own TUNE, DECAY, ... */
static void wave_set(uint32_t pad, uint32_t w)
{
    sp_sound_t *s = &sp_sound[pad % SP_NSOUND];
    char m[24];
    uint32_t was = s->wave >= KIT_NWAVE && s->wave < KIT_NWAVE + SP_NUSER ? ui_uflags[s->wave - KIT_NWAVE] : 0u;
    uint32_t now = w >= KIT_NWAVE ? ui_uflags[w - KIT_NWAVE] : 0u;
    s->wave = (uint8_t)w;
    if (w >= KIT_NWAVE)                          /* an own sample: 45->33 as it was stored */
        s->flags = (uint8_t)((now & 1u) ? s->flags | SPF_33 : s->flags & ~SPF_33);
    if ((now ^ was) & 2u)                        /* stored at double speed: played an octave down (and back) */
        s->tune = (now & 2u) ? -12 : 0;
    s->start = 0;
    s->end = 1000;
    ui.sel = (uint8_t)(pad % SP_NSOUND);
    SP_HIT(ui.sel, 100, 0);                    /* (heard at once) */
    pad_label(m, ui.sel);
    m[2] = ' ';
    cat(m + 3, wave_name(w));
    ui_say(m);
}
static void wave_step(int32_t one)
{
    uint32_t n = KIT_NWAVE + SP_NUSER, w = sp_sound[ui.sel].wave < n ? sp_sound[ui.sel].wave : 0u, i;
    for (i = 0; i < n; i++) {                   /* the next that is there */
        w = (w + (one > 0 ? 1u : n - 1u)) % n;
        if (sp_wave[w].n)
            break;
    }
    wave_set(ui.sel, w);
}

/* PRESETS turned: the sample of the pad played last */
static void ui_preset(int32_t d)
{
    ui.touch_ms = fm1_ms;
    wave_step(d > 0 ? 1 : -1);
}

/* KNOB n turned by d */
static void knob(uint32_t n, int32_t d)
{
    sp_sound_t *s = &sp_sound[ui.sel];
    int32_t one = d > 0 ? 1 : -1, dd = accel(d);
    ui.touch_ms = fm1_ms;
    switch (ui.page) {
    case PG_HOME: {
        uint32_t c = (ui.shift ? 4u : 0u) + n;     /* the faders: channels 1-4, SEL lit 5-8 */
        ui.mix[c] = (uint8_t)sp_clamp(ui.mix[c] + dd, 0, 127);
        sp_mix[c] = ui.mix[c];
        break;
    }
    case PG_PAD:
        if (n == 0u) wave_step(one);
        if (n >= 1u) { ui.turn_pad = ui.sel; ui.turn_ms = fm1_ms; }
        if (n == 1u) s->tune = (int8_t)sp_clamp(s->tune + one, -24, 12);
        if (n == 2u) s->start = (uint16_t)sp_clamp(s->start + dd * 3, 0, s->end - 10);
        if (n == 3u) s->decay = (uint8_t)sp_clamp(s->decay + dd, 0, 127);
        break;
    case PG_FXMIX:
        if (n == 0u) djf.v = (int8_t)sp_clamp(djf.v + d * 2, -64, 63);
        if (n == 1u) fxp.cmix = (int16_t)sp_clamp(fxp.cmix + dd, 0, 127);
        if (n == 2u) fxp.dmix = (int16_t)sp_clamp(fxp.dmix + dd, 0, 127);
        if (n == 3u) fxp.size = (int16_t)sp_clamp(fxp.size + dd, 0, 127);
        break;
    case PG_WAVE:
        if (n == 0u)
            wave_step(one);
        if (n == 1u) ui.copy_pad = (uint8_t)((ui.copy_pad + SP_NSOUND + (one > 0 ? 1u : SP_NSOUND - 1u)) % SP_NSOUND);
#if FELUCCA_UAI
        if (n == 3u && d > 0 && s->wave >= KIT_NWAVE && again(PG_WAVE + 2u))   /* the pad's own sample deleted (turn twice) */
            smp_delete_pad(ui.sel);
#endif
        if (n == 2u && d > 0 && again(PG_WAVE + 1u) && ui.copy_pad != ui.sel) {
            sp_sound[ui.copy_pad] = *s;
            ui_say("SOUND COPIED");
        }
        break;
    case PG_SOUND:
        if (n < 3u) { ui.turn_pad = ui.sel; ui.turn_ms = fm1_ms; }   /* (recording: the hits take TUNE / FINE / DECAY) */
        if (n == 0u) s->tune = (int8_t)sp_clamp(s->tune + one, -24, 12);
        if (n == 1u) s->fine = (int8_t)sp_clamp(s->fine + d, -50, 50);
        if (n == 2u) s->decay = (uint8_t)sp_clamp(s->decay + dd, 0, 127);
        if (n == 3u) s->level = (uint8_t)sp_clamp(s->level + dd, 0, 127);
        break;
    case PG_TRUNC:
        if (n == 0u) s->start = (uint16_t)sp_clamp(s->start + dd * 3, 0, s->end - 10);
        if (n == 1u) s->end = (uint16_t)sp_clamp(s->end + dd * 3, s->start + 10, 1000);
        if (n == 2u) s->flags = (uint8_t)(one > 0 ? s->flags | SPF_REVERSE : s->flags & ~SPF_REVERSE);
        if (n == 3u) s->flags = (uint8_t)(one > 0 ? s->flags | SPF_33 : s->flags & ~SPF_33);
        break;
    case PG_OUT:
        if (n == 0u) {                          /* CHOP group --, 1-8: its pads cut each other; the pads held take it too */
            uint32_t g = (uint32_t)sp_clamp((int32_t)SPF_CHOP(s->flags) + one, 0, 8), k, cnt = 1;
            char m[24], *p;
            s->flags = SPF_CHOP_SET(s->flags, g);
            for (k = 0; k < 27u; k++) {
                uint32_t pd = (ui.keys >> k) & 1u ? key_pad(k) : 0xFFu;
                if (pd < SP_NSOUND && pd != ui.sel) {
                    sp_sound[pd].flags = SPF_CHOP_SET(sp_sound[pd].flags, g);
                    cnt++;
                }
            }
            p = cnt > 1u ? cat(num(m, (int32_t)cnt, cnt >= 10u ? 2u : 1u, 0), " PADS ") : m;   /* (13 fit the big line) */
            if (g) num(cat(p, "CHOP "), (int32_t)g, 1, 0);
            else cat(p, "NO CHOP");
            ui_say(m);
            break;
        }
        if (n == 1u) s->pan = (int8_t)sp_clamp(s->pan + dd, -64, 63);
        if (n >= 2u && SP_CH_OF(ui.sel) >= 2u) { ui_say("FILTER: PADS 1-2"); break; }   /* (3-8 have none to turn) */
        if (n == 2u) { s->cut = (uint8_t)sp_clamp(s->cut + dd, 0, 127); ui.turn_pad = ui.sel; ui.turn_ms = fm1_ms; }
        if (n == 3u) s->reso = (uint8_t)sp_clamp(s->reso + dd, 0, 127);
        break;
    case PG_SFX:
        if (n == 0u) s->drive = (uint8_t)sp_clamp(s->drive + dd, 0, 127);
        else s->send[n - 1u] = (uint8_t)sp_clamp(s->send[n - 1u] + dd, 0, 127);
        break;
    case PG_FILT:
        if (n == 0u) djf.v = (int8_t)sp_clamp(djf.v + d * 2, -64, 63);   /* (one turn's half: open to shut) */
        if (n == 1u) djf.res = (int8_t)sp_clamp(djf.res + dd, 0, 127);
        break;
    case PG_CHO:
        if (n == 0u) fxp.crate = (int16_t)sp_clamp(fxp.crate + dd, 0, 127);
        if (n == 1u) fxp.cdepth = (int16_t)sp_clamp(fxp.cdepth + dd, 0, 127);
        if (n == 2u) fxp.cmix = (int16_t)sp_clamp(fxp.cmix + dd, 0, 127);
        break;
    case PG_DLY:
        if (n == 0u) fxp.dtime = (int16_t)sp_clamp(fxp.dtime + one, 0, 5);
        if (n == 1u) fxp.fdbk = (int16_t)sp_clamp(fxp.fdbk + dd, 0, 120);
        if (n == 2u) fxp.colr = (int16_t)sp_clamp(fxp.colr + dd, 0, 127);
        if (n == 3u) fxp.dmix = (int16_t)sp_clamp(fxp.dmix + dd, 0, 127);
        break;
    case PG_REV:
        if (n == 0u) fxp.size = (int16_t)sp_clamp(fxp.size + dd, 0, 127);
        if (n == 1u) fxp.damp = (int16_t)sp_clamp(fxp.damp + dd, 0, 127);
        if (n == 2u) fxp.pre = (int16_t)sp_clamp(fxp.pre + d, 0, 90);
        break;
    case PG_SEG:
        if (n == 0u && !sq.playing) sq.seg = (uint8_t)sp_clamp(sq.seg + one, 0, SQ_NSEG - 1);
        if (n == 1u) {                              /* BARS: AUTO (0), 1..32 */
            sq_seg_t *g = &sq_seg[sq.seg % SQ_NSEG];
            g->bars = (uint8_t)sp_clamp(g->bars + one, 0, SQ_MAXBARS);
            sq.gen++;
        }
        if (n == 2u) sq.quant = (uint8_t)sp_clamp(sq.quant + one, 0, 6);
        if (n == 3u) sq.swing = (uint8_t)sp_clamp(sq.swing + one, 0, 5);
        break;
    case PG_SEG2:
        if (n == 0u && d > 0 && again(PG_SEG2 + 1u)) { sq_post(RQ_CLEAR, 0, 0); ui_say("SEGMENT CLEARED"); }
        if (n == 1u) ui.copy_to = (uint8_t)sp_clamp(ui.copy_to + one, 0, SQ_NSEG - 1);
        if (n == 2u && d > 0 && again(PG_SEG2 + 2u)) { sq_post(RQ_COPY, 0, ui.copy_to); ui_say("COPIED"); }
        break;
    case PG_SONG:
        if (n == 0u) ui.song_cur = (uint8_t)sp_clamp(ui.song_cur + one, 0, SQ_SONG_N < SQ_NSONG ? SQ_SONG_N : SQ_NSONG - 1);
        if (n == 1u && ui.song_cur < SQ_NSONG && !sq.playing) {
            uint32_t i = ui.song_cur;
            if (i >= SQ_SONG_N) {                     /* past the end: a new step with the segment edited */
                sq_song[i].seg = sq.seg;
                sq_song[i].rep = 1;
                SQ_SONG_N = (uint8_t)(i + 1u);
            } else {
                sq_song[i].seg = (uint8_t)sp_clamp(sq_song[i].seg + one, 0, SQ_NSEG - 1);
            }
        }
        if (n == 2u && ui.song_cur < SQ_SONG_N && !sq.playing) {   /* REPEAT 0: the song ends here */
            int32_t r = sp_clamp(sq_song[ui.song_cur].rep + one, 0, 99);
            if (r == 0) SQ_SONG_N = ui.song_cur;
            else sq_song[ui.song_cur].rep = (uint8_t)r;
        }
        if (n == 3u && !sq.playing) {              /* SONG: OFF (the loops) or 1-4, the one played and edited */
            int32_t v = sp_clamp((sq.song_mode ? sq.song_sel + 1 : 0) + one, 0, (int32_t)SQ_SONGS);
            sq.song_mode = (uint8_t)(v > 0);
            if (v > 0) { sq.song_sel = (uint8_t)(v - 1); ui.song_cur = 0; }
        }
        break;
    case PG_SETUP:
        if (n == 0u) sq.bpm10 = (uint16_t)sp_clamp(sq.bpm10 + dd * 3, 400, 2400);
        if (n == 1u) ui.back = (uint8_t)sp_clamp(ui.back + one, 0, 4);
        if (n == 3u && d > 0 && !sq.playing && again(PG_SETUP + 1u))
            ui.factory_req = 1;                    /* (zp12.c: everything to the factory state, saved) */
        break;
    case PG_CLICK:
        if (n == 0u) sq.click = (uint8_t)sp_clamp(sq.click + one, 0, 2);
        if (n == 1u) sq.cin_bars = (uint8_t)sp_clamp(sq.cin_bars + one, 0, 2);
        if (n == 2u) sq.dub_bar = one > 0;
        break;
    case PG_OUTPUT:
        if (n == 0u) sp_smooth = one > 0;         /* (right: on, left: off) */
        if (n == 1u) sq_clk_out = one > 0;
        break;
#if FELUCCA_UAI
    case PG_SAMPLE:
        if (n == 3u) { smp.mon = (uint8_t)sp_clamp(smp.mon + dd, 0, 127); break; }
        if (smp.state != SM_IDLE) break;         /* (not while a take runs) */
        if (n == 0u) smp.rate = (uint8_t)sp_clamp(smp.rate + one, 0, 2);
        if (n == 2u) smp.thr = (uint8_t)sp_clamp(smp.thr + one, 0, 6);
        if (n <= 1u) {                           /* TIME: up to what the largest free run holds at this rate */
            int32_t mx = (int32_t)(smp_room_samples() * 10u / SMP_RATE[smp.rate % 3u]);
            if (mx < 1) mx = 1;
            if (n == 1u) smp.len10 = (uint16_t)sp_clamp(smp.len10 + dd, 1, mx);
            else if (smp.len10 > mx) smp.len10 = (uint16_t)mx;
        }
        break;
#endif
    default:
        break;
    }
}

static void page(uint32_t p) { ui.page = (uint8_t)p; ui.touch_ms = fm1_ms; ui.arm = 0; }

/* SELECT: every menu's every page in one order (MIX, EDIT, FX, SEQ, GLO), round */
static void ui_select(int32_t d)
{
    uint32_t n = PG_N - (FELUCCA_UAI ? 0u : 1u), p = ui.page % n;
    p = d > 0 ? (p + 1u) % n : (p + n - 1u) % n;
    page(p);
}

static void tap_tempo(void)                      /* ENV: the tempo of the last taps (two at least, < 2 s apart) */
{
    uint32_t i, n = 0, sum = 0;
    for (i = 3; i; i--) ui.tap_ms[i] = ui.tap_ms[i - 1u];
    ui.tap_ms[0] = fm1_ms;
    for (i = 0; i < 3u && ui.tap_ms[i + 1u] && ui.tap_ms[i] - ui.tap_ms[i + 1u] < 2000u; i++, n++)
        sum += ui.tap_ms[i] - ui.tap_ms[i + 1u];
    if (n && sum)
        sq.bpm10 = (uint16_t)sp_clamp((int32_t)(600000u * n / sum), 400, 2400);
}

/* button b pressed (seq: SEQ was held at the time: OCT pages the step grid's bars) */
static void button(uint32_t b)
{
    if ((b == B_OCTDN || b == B_OCTUP) && (ui.held >> B_FX) & 1u) {   /* FX held: REV / ECHO THROW while held (zp12.c) */
        ui.fx_used = 1;
        ui_say(b == B_OCTDN ? "REV THROW" : "ECHO THROW");
        return;
    }
    switch (b) {
    case B_OCTUP:
        if (ui.steps) {
            const sq_seg_t *g = &sq_seg[sq.seg % SQ_NSEG];
            if (ui.step_bar + 1u < (g->bars ? g->bars : 1u)) ui.step_bar++;
        } else {
            ui.pair = 1;
        }
        break;
    case B_OCTDN:
        if ((ui.held >> B_EDIT) & 1u) {          /* EDIT + OCT-: undo the last CLEAR / ERASE / COPY (again: redo) */
            sq_post(RQ_UNDO, 0, 0);
            page(ui.prev_page);
            ui_say(sq_undo_of < SQ_NSEG ? "UNDO" : "NO UNDO");
        } else if (ui.steps) { if (ui.step_bar) ui.step_bar--; }
        else ui.pair = 0;
        break;
    case B_ARP: ui.multi ^= 1u; break;
    case B_HOME: page(PG_HOME); break;
    case B_SEL:                                  /* the faders 5-8 (lit) or 1-4, until pressed again; from a page: the faders */
        ui.shift ^= 1u;
        page(PG_HOME);
        ui_say(ui.shift ? "FADERS 5-8" : "FADERS 1-4");
        break;
    case B_EDIT: ui.prev_page = ui.page; page(ui.page >= PG_PAD && ui.page < PG_SFX ? ui.page + 1u : PG_PAD); break;
    case B_FX: page(ui.page >= PG_FXMIX && ui.page < PG_REV ? ui.page + 1u : PG_FXMIX); break;
    case B_SEQ: page(ui.page == PG_SEG ? PG_SEG2 : ui.page == PG_SEG2 ? PG_SONG : PG_SEG); break;
    case B_SAVE: ui.save_req = 1; ui_say("SAVED"); break;
    case B_GLO: page(ui.page >= PG_SETUP && ui.page < PG_SETUP + FAM_N[4] - 1u ? ui.page + 1u : PG_SETUP); break;
#if FELUCCA_UAI
    case B_LFO: page(PG_SAMPLE); break;          /* LFO tapped (zp12.c): the sampler */
#endif
    case B_ENV: tap_tempo(); break;
    case B_PLAY: sq_post(sq.playing ? RQ_STOP : RQ_PLAY, 0, 0); break;
    case B_REC: sq_post(RQ_REC, 0, 0); break;
    default: break;
    }
}

/* REC and SAVE, as on sloopDX. REC acts on the press (no lag); held 0.7 s a bar fills: held to its end (2 s in
 * all), the press is undone and the loop cleared (EDIT + OCT- brings it back); let go before, it was a tap. SAVE tapped saves
 * now; held, a black key saves the loop into that loop (key_down). held: the buttons held now; returns pressed
 * without the two. */
static uint32_t ui_holds(uint32_t held, uint32_t pressed, uint32_t released)
{
    ui.held = held;
#if FELUCCA_UAI
    if (pressed & (1u << B_REC) && ui.page == PG_SAMPLE) {   /* the sampler's REC, not the loop's */
        smp_press();
        ui.touch_ms = fm1_ms;
        pressed &= ~(1u << B_REC);
        ui.rec_on = 0;
    }
#endif
    if (pressed & (1u << B_REC)) {
        ui.rec_prev = (uint8_t)(sq.recording | sq.rec_arm << 1 | sq.dub_wait << 2);
        ui.rec_t0 = fm1_ms;
        ui.rec_on = 1;
        sq_post(RQ_REC, 0, 0);
    }
    if ((held >> B_REC) & 1u) {
        if (ui.rec_on && !ui.holding && (uint32_t)(fm1_ms - ui.rec_t0) >= 700u) {
            ui.holding = 1;                      /* a hold: the bar fills (the press stands unless it ends) */
            ui.hold_t0 = fm1_ms;
        }
        if (ui.holding && (uint32_t)(fm1_ms - ui.hold_t0) >= UI_HOLD_MS) {
            char m[24], *p = cat(m, "LOOP ");
            sq_post(RQ_RECSET, 0, ui.rec_prev);  /* the press undone, the loop cleared */
            sq_post(RQ_CLEAR, 0, 0);
            p = num(p, sq.seg + 1, sq.seg + 1u >= 10u ? 2u : 1u, 0);
            cat(p, " CLEARED");
            ui_say(m);
            ui.holding = 0;
            ui.rec_on = 0;                       /* (until REC is up: nothing more) */
        }
    } else {
        ui.holding = 0;                          /* let go before the end: nothing */
        ui.rec_on = 0;
    }
    if (pressed & (1u << B_SAVE))
        ui.save_used = 0;
    if (released & (1u << B_SAVE) && !ui.save_used) {
        ui.save_req = 1;
        ui_say("SAVED");
    }
    return pressed & ~((1u << B_REC) | (1u << B_SAVE));
}

/* what lights on the panel: buttons (bit = matrix id), white and black keys (bit = key 0..26, F3 = 0), the
 * keys' glow (the playhead in the step grid) */
static void ui_leds(uint32_t *btn, uint32_t *keys, uint32_t *glow)
{
    uint32_t b = 0, k = 0, g = 0, i;
    int blink = (sq.beat & 1u) == 0u, fam;
    if (sq.playing) b |= 1u << B_PLAY;
    if (sq.recording || ((sq.rec_arm || sq.dub_wait) && (fm1_ms / 250u) & 1u)) b |= 1u << B_REC;
#if FELUCCA_UAI
    if (smp.state == SM_REC || (smp.state != SM_IDLE && (fm1_ms / 150u) & 1u)) b |= 1u << B_REC;
    else if (smp.state == SM_IDLE && ui.page == PG_SAMPLE) b &= ~(1u << B_REC);
#endif
    if (ui.multi) b |= 1u << B_ARP;
    if (ui.shift) b |= 1u << B_SEL;
    if (sq.erase) b |= 1u << B_LFO;
    if (sq.playing && blink) b |= 1u << B_ENV;   /* (the beat: TAP's guide) */
    if (sq.song_mode) b |= 1u << B_SAVE;
    fam = ui.page;
    b |= 1u << (fam == PG_HOME ? B_HOME : fam <= PG_SFX ? B_EDIT : fam <= PG_REV ? B_FX : fam <= PG_SONG ? B_SEQ : B_GLO);
    if ((ui.held >> B_FX) & 1u)                  /* FX held: OCT- / OCT+ lit while REV / ECHO THROW */
        b |= (uint32_t)(sp_thr_req & 1u) << B_OCTDN | (uint32_t)(sp_thr_req >> 1 & 1u) << B_OCTUP;
    else
        b |= 1u << (ui.pair ? B_OCTUP : B_OCTDN);
    if ((ui.held >> B_GLO) & 1u) {               /* GLO held: white keys 1-8 lit while their channel is heard (dark:
                                                  * muted, or another soloed), 9-16 lit while soloed */
        for (i = 0; i < 27u; i++) {
            uint32_t w = white_of(i), c, heard;
            if (w == 0xFFu) continue;
            c = w % 8u;
            heard = !((sp_mute >> c) & 1u) && (!sp_solo || ((sp_solo >> c) & 1u));
            if (w < 8u ? heard : (sp_solo >> c) & 1u) k |= 1u << i;
            g |= 1u << i;
        }
    } else if (ui.steps) {                       /* the selected pad's steps of the bar on the white keys */
        const sq_seg_t *s = &sq_seg[sq.seg % SQ_NSEG];
        uint32_t t0 = ui.step_bar * SQ_BAR, row = 0, len = s->bars ? s->bars * SQ_BAR : SQ_BAR;
        for (i = 0; i < s->n; i++)
            if (s->ev[i].pad == ui.sel && s->ev[i].t >= t0 && s->ev[i].t < t0 + SQ_BAR)
                row |= 1u << ((s->ev[i].t - t0) / 24u);
        for (i = 0; i < 27u; i++) {
            uint32_t w = white_of(i);
            if (w != 0xFFu && (row >> w) & 1u) k |= 1u << i;
            if (w != 0xFFu && sq.playing && sq.countin <= 0 && ((sq.pos >> 16) % len) / 24u == ui.step_bar * 16u + w)
                g |= 1u << i;
        }
        b |= 1u << B_SEQ;
    } else {                                     /* the pads heard just now: their keys flash */
        uint32_t nb = 0;
        for (i = 0; i < 27u; i++) {
            uint32_t p = ui.multi ? 0xFFu : key_pad(i);
            if (p < 32u && (int32_t)(fm1_ms - ui.hit_ms[p]) < 90)
                k |= 1u << i;
            if (white_of(i) == 0xFFu && !ui.multi) {   /* the loops: the one playing lit, one waiting blinks */
                if (nb == sq.seg) k |= 1u << i;
                if (nb == sq.next_seg && blink) k |= 1u << i;
                g |= 1u << i;                    /* (the others glow: there is a loop under every black key) */
                nb++;
            }
        }
    }
    *btn = b;
    *keys = k;
    *glow = g;
}

/* ---- the start: a kick is sampled into the LCD (the 12-bit steps drawn as they come in), then the name is
 * typed in big LCD pixels, each letter a drum: ba (tom), dum (tom), tss (kick + crash: "12"). t: ms since the
 * start; zp12.c draws it and plays SPLASH_HIT: the pads A5, C1, A1 + A6 as they are (own sounds there: an own start;
 * 2.1: TOM H, TOM L, KICK + CRASH's places in the kit by position, where KIT_MOVED takes 2.0's A8, A7, A1, B1) */
#define SPLASH_MS 3000u
static const struct { uint16_t t; uint8_t pad, vel; } SPLASH_HIT[4] = {{1350, 4, 84}, {1550, 16, 84}, {1850, 0, 100}, {1850, 5, 70}};

static void cv_big(int32_t x, int32_t y, const char *s, int32_t k, uint16_t c)   /* FONT_S, k x k pixels a dot */
{
    for (; *s; s++) {
        uint32_t gi = glyph(&FONT_S, (uint8_t)*s), gx, gy, w = FONT_S.bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = FONT_S.data + FONT_S.off[gi];
        for (gy = 0; gy < FONT_S.h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                if (((gx & 1u) ? (v & 15u) : (v >> 4)) >= 8u)
                    cv_rect(x + ((int32_t)gx - FONT_S.pad) * k, y + (int32_t)gy * k, k, k, c);
            }
        x += FONT_S.adv[gi] * k;
    }
}

static int32_t splash_kick(uint32_t i)            /* the sampled kick at x = i: its pitch falling, dying away, held in steps */
{
    int32_t x = (int32_t)(i / 3u * 3u), ph = (85 * x - x * x / 8) & 1023, e = 42 - x * 34 / 232, v;
    v = ph < 512 ? ph - 256 : 767 - ph;          /* (a triangle for the sine: no tables) */
    return v * e / 256;
}

static void ui_splash(uint32_t t)
{
    uint32_t i, lit = 0, n = t < 1300u ? t * 232u / 1300u : 232u, flash = t >= 1850u && t < 1990u;
    char b[32];                                  /* the longest: "12-bit sampling drums  " + the version */
    cv_begin(240, 20, P_NAVY);                   /* the top: SAMPLING and its LED, the rate */
    if (t < 1300u) {
        cv_text_on(8, 2, &FONT_S, "SAMPLING", P_FRAME, P_NAVY);
        if ((t / 250u) & 1u) cv_rect(84, 7, 7, 7, P_LED);
        num(b, (int32_t)(n * 26040u / 232u), 5, 0);   /* (the samples taken: one second's worth) */
        cv_text_on(232 - text_w(&FONT_S, b), 2, &FONT_S, b, P_RULE, P_NAVY);
    } else {
        cv_text_on(8, 2, &FONT_S, "12 BIT  26.04 KHZ", P_RULE, P_NAVY);
    }
    cv_blit(0, 0);
    cv_begin(232, 112, flash ? P_LCDINK : P_LCD);   /* the LCD: the wave coming in, then the name over it */
    cv_rect(0, 0, 232, 3, RGB(70, 76, 66));
    for (i = 0; i < n; i++) {                     /* the wave as stairs: each step, and the rise to it */
        int32_t v = splash_kick(i), u = i ? splash_kick(i - 1u) : v, lo = v < u ? v : u, hi = v < u ? u : v;
        cv_rect((int32_t)i, 56 - hi, 2, hi - lo + 2, t < 1300u ? P_LCDINK : RGB(150, 162, 124));
    }
    if (t < 1300u)
        cv_rect((int32_t)n, 8, 2, 96, P_RED);    /* the write head */
    {
        static const char NAME[] = "PICO";
        uint32_t k = t < 1350u ? 0u : t < 1550u ? 1u : t < 1850u ? 2u : 4u;
        char nm[5];
        for (i = 0; i < k; i++) nm[i] = NAME[i];
        nm[k] = 0;
        if (k) {
            int32_t sh = flash ? (int32_t)((t / 30u) & 1u) * 4 - 2 : 0;   /* (the crash shakes it) */
            cv_big(116 - 80 + sh, 16, nm, 5, flash ? P_LCD : P_LCDINK);
        }
    }
    cv_blit(4, 22);
    for (i = 0; i < 4u; i++)                      /* the pads: a chase while sampling, the hits after */
        if (t >= SPLASH_HIT[i].t && t < SPLASH_HIT[i].t + 160u) lit |= 1u << (SPLASH_HIT[i].pad & 7u);
    if (t < 1300u) lit = 1u << (t / 90u % 8u);
    if (flash) lit = 0xFF;
    cv_begin(232, 46, P_NAVY);
    for (i = 0; i < 8u; i++) {
        int32_t cx = 14 + (int32_t)i * 29;
        cv_rect(cx - 11, 2, 23, 23, RGB(6, 6, 8));
        cv_rect(cx - 10, 3, 21, 21, (lit >> i) & 1u ? P_LED : P_PAD);
        cv_rect(cx - 8, 5, 17, 2, (lit >> i) & 1u ? RGB(255, 140, 120) : RGB(48, 48, 52));
    }
    if (t >= 1900u) {
        (void)sizeof(char[sizeof "12-bit sampling drums  " + sizeof ZP12_VER - 1u <= sizeof b ? 1 : -1]);   /* (fits, or no build) */
        cat(cat(b, "12-bit sampling drums  "), ZP12_VERSION);
        cv_text_on(116 - text_w(&FONT_S, b) / 2, 28, &FONT_S, b, P_FRAME, P_NAVY);
    }
    cv_blit(4, 136);
    cv_begin(240, 58, P_NAVY);
    if (t >= 2100u)
        cv_text_on(120 - text_w(&FONT_S, "based on SLOOP + Felucca") / 2, 30, &FONT_S, "based on SLOOP + Felucca", P_RULE, P_NAVY);
    cv_blit(0, 182);
    lcd_sync();
}

static void ui_init(void)
{
    uint32_t i;
    for (i = 0; i < SP_NCH; i++)
        ui.mix[i] = sp_mix[i];
    ui.force = 1;
}
