/* SPDX-License-Identifier: MPL-2.0 */
/* SYRUP LINK (app.h). Three pages, all on the panel, nothing hidden:
 *   LINK (HOME)  the bridge: Wi-Fi, where Syrup is, what went out. The keys play Syrup, OCT- / OCT+ move them,
 *                K1..K4 send cutoff, resonance, WT position and macro 1, PRESET steps Syrup's patches,
 *                PLAY starts / stops Syrup's sequencer, ARP asks for a new acid line.
 *   WI-FI (SEL)  the networks around: SELECT picks, SAVE joins (a kept one at once, else the password page).
 *   PASSWORD     SELECT picks a character, PRESET jumps a group, SAVE adds it, FX deletes, PLAY joins, HOME leaves.
 *   ADDRESS (EDIT)  AUTO finds Syrup by itself (REC switches to a set address); K1..K4 set the four numbers, SAVE keeps.
 * Part of the unity build (after the platform: gfx.c is in scope). */

#define SET_MAGIC 0x4C4E4B31u            /* "LNK1" */
static struct {
    uint32_t magic;
    uint32_t ip;                         /* the set address, first byte lowest */
    int8_t manual, octave;
    uint8_t cc[4];
} set;

enum { PG_LINK, PG_WIFI, PG_PASS, PG_ADDR };
static const uint8_t CC_NUM[4] = {74, 71, 70, 1};
static const char *const CC_NAME[4] = {"CUTOFF", "RES", "WT POS", "MACRO 1"};
static const char *const STATE[] = {"OFF", "STARTING", "ON", "JOINING", "GETTING ADDRESS", "CONNECTED", "FAILED"};
static const char CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()-_=+.,?/:;'\"[]{}<>~`|\\ ";
#define NCHARS ((int)sizeof(CHARS) - 1)

static struct {
    int page, pick, ch;
    uint32_t btn, keys, msg_until, saved_dirty;
    char ssid[33], pass[64];
    char msg[2][24];
    uint8_t redraw, playing;
    int8_t held_note[27];
} ui;

static volatile uint32_t in_count, in_ago_ms;

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC) {
        uint32_t i;
        set.magic = SET_MAGIC;
        set.ip = 0;
        set.manual = 0;
        set.octave = -1;                 /* the keys start at F2: bass */
        for (i = 0; i < 4u; i++)
            set.cc[i] = 64;
    }
    link_target(set.ip, set.manual);
}

/* USB and the TRS jack in (audio interrupt): everything on to Syrup, active sensing left out */
static int msg_len(uint32_t st)
{
    if (st >= 0xF8u)
        return 1;
    switch (st & 0xF0u) {
    case 0xC0u: case 0xD0u: return 2;
    case 0xF0u: return st == 0xF2u ? 3 : (st == 0xF1u || st == 0xF3u) ? 2 : 1;
    default: return 3;
    }
}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, pkt;
    int port;
    (void)master_q12;
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;
    while (plat_midi_in(&pkt, &port)) {
        uint8_t b[3];
        uint32_t st = (pkt >> 8) & 0xFFu;
        if (st < 0x80u || st == 0xFEu || st == 0xF0u || st == 0xF7u)
            continue;                    /* (sysex is not passed on) */
        b[0] = (uint8_t)st;
        b[1] = (uint8_t)((pkt >> 16) & 0x7Fu);
        b[2] = (uint8_t)((pkt >> 24) & 0x7Fu);
        link_push(b, (uint32_t)msg_len(st), 1);
        if (st != 0xF8u) {
            in_count++;
            in_ago_ms = plat_ms();
        }
    }
}

static void send3(uint8_t a, uint8_t b, uint8_t c) { uint8_t m[3] = {a, b, c}; link_push(m, (uint32_t)msg_len(a), 0); }

void app_panic(void)
{
    send3(0xB0, 123, 0);
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1500u;
    ui.redraw = 1;
}

static char *put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}
static char *put_int(char *d, int32_t v)
{
    char t[12];
    int k = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    if (v < 0)
        *d++ = '-';
    do {
        t[k++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    while (k)
        *d++ = t[--k];
    *d = 0;
    return d;
}
static char *put_ip(char *d, uint32_t ip)
{
    int i;
    for (i = 0; i < 4; i++) {
        d = put_int(d, (int32_t)(ip >> (8 * i) & 255u));
        if (i < 3)
            d = put(d, ".");
    }
    return d;
}

static void save_settings(void)
{
    if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
        app_say("SAVED", "");
    link_target(set.ip, set.manual);
}

static void all_notes_off(void)
{
    int k;
    for (k = 0; k < 27; k++)
        if (ui.held_note[k] >= 0) {
            send3(0x80, (uint8_t)ui.held_note[k], 0);
            ui.held_note[k] = -1;
        }
}

static void join_picked(void)
{
    if (plat_wifi_kept(ui.ssid)) {
        plat_wifi_join(ui.ssid, 0);
        app_say("JOINING", ui.ssid);
        ui.page = PG_LINK;
    } else {
        ui.pass[0] = 0;
        ui.page = PG_PASS;
    }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), down = btn & ~ui.btn, keys = plat_keys(), kchg = keys ^ ui.keys;
    int32_t esel = plat_enc(EN_SELECT), epre = plat_enc(EN_PRESET), ek[4];
    int i, k;
    ui.btn = btn;
    ui.keys = keys;
    for (i = 0; i < 4; i++)
        ek[i] = plat_enc(EN_K1 + i);
    (void)plat_enc(EN_ALGO);

    /* the keys always play Syrup, whatever the page */
    for (k = 0; k < 27; k++)
        if (kchg >> k & 1u) {
            if (keys >> k & 1u) {
                int note = 53 + k + 12 * set.octave;
                if (note < 0) note = 0;
                if (note > 127) note = 127;
                ui.held_note[k] = (int8_t)note;
                send3(0x90, (uint8_t)note, 100);
            } else if (ui.held_note[k] >= 0) {
                send3(0x80, (uint8_t)ui.held_note[k], 0);
                ui.held_note[k] = -1;
            }
        }
    if (down >> B_OCTDN & 1u && set.octave > -3) { all_notes_off(); set.octave--; ui.saved_dirty = 1; ui.redraw = 1; }
    if (down >> B_OCTUP & 1u && set.octave < 3) { all_notes_off(); set.octave++; ui.saved_dirty = 1; ui.redraw = 1; }

    /* the pages */
    if (down >> B_HOME & 1u) { ui.page = PG_LINK; ui.redraw = 1; }
    if (down >> B_SEL & 1u && ui.page != PG_PASS) {
        ui.page = PG_WIFI;
        ui.pick = 0;
        if (plat_wifi_state() == WIFI_OFF)
            plat_wifi_on(1);
        plat_wifi_scan();
        ui.redraw = 1;
    }
    if (down >> B_EDIT & 1u && ui.page != PG_PASS) { ui.page = PG_ADDR; ui.redraw = 1; }

    switch (ui.page) {
    case PG_LINK:
        for (i = 0; i < 4; i++)
            if (ek[i]) {
                int v = set.cc[i] + 2 * ek[i];
                set.cc[i] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
                send3(0xB0, CC_NUM[i], set.cc[i]);
                ui.redraw = 1;
            }
        if (epre) { send3(0xB0, epre > 0 ? 103 : 102, 127); app_say("PATCH", epre > 0 ? "NEXT" : "BACK"); }
        if (down >> B_PLAY & 1u) {
            ui.playing = !ui.playing;
            send3(ui.playing ? 0xFA : 0xFC, 0, 0);
            app_say("SEQUENCER", ui.playing ? "START" : "STOP");
        }
        if (down >> B_ARP & 1u) { send3(0xB0, 104, 127); app_say("ACID", "NEW LINE"); }
        if (down >> B_FX & 1u) { send3(0xB0, 123, 0); all_notes_off(); app_say("ALL NOTES", "OFF"); }
        if (down >> B_REC & 1u) {
            int on = plat_wifi_state() == WIFI_OFF;
            plat_wifi_on(on);
            app_say("WI-FI", on ? "ON" : "OFF");
        }
        if (down >> B_SAVE & 1u && ui.saved_dirty) { ui.saved_dirty = 0; save_settings(); }
        break;
    case PG_WIFI:
        if (esel) {
            ui.pick += esel;
            if (ui.pick < 0) ui.pick = 0;
            while (ui.pick > 0 && !plat_wifi_found(ui.pick, 0, 0, 0))
                ui.pick--;
            ui.redraw = 1;
        }
        if (down >> B_SAVE & 1u) {
            if (plat_wifi_found(ui.pick, ui.ssid, 0, 0))
                join_picked();
            else
                app_say("NO NETWORK", "SEL LOOKS AGAIN");
            ui.redraw = 1;
        }
        break;
    case PG_PASS: {
        int n = 0;
        while (ui.pass[n]) n++;
        if (esel || epre) {
            ui.ch = (ui.ch + esel + 26 * epre) % NCHARS;
            if (ui.ch < 0) ui.ch += NCHARS;
            ui.redraw = 1;
        }
        if (down >> B_SAVE & 1u && n < 63) { ui.pass[n] = CHARS[ui.ch]; ui.pass[n + 1] = 0; ui.redraw = 1; }
        if (down >> B_FX & 1u && n > 0) { ui.pass[n - 1] = 0; ui.redraw = 1; }
        if (down >> B_PLAY & 1u) {
            plat_wifi_join(ui.ssid, ui.pass);
            app_say("JOINING", ui.ssid);
            ui.page = PG_LINK;
        }
        if (down >> B_HOME & 1u) ui.page = PG_LINK;
        break;
    }
    case PG_ADDR:
        if (!set.manual && (ek[0] | ek[1] | ek[2] | ek[3]) && link_found())
            set.ip = link_found();       /* start from the address heard */
        for (i = 0; i < 4; i++)
            if (ek[i]) {
                int v = (int)(set.ip >> (8 * i) & 255u) + ek[i];
                v = v < 0 ? 0 : v > 255 ? 255 : v;
                set.ip = (set.ip & ~(255u << (8 * i))) | ((uint32_t)v << (8 * i));
                set.manual = 1;
                ui.saved_dirty = 1;
                ui.redraw = 1;
            }
        if (down >> B_REC & 1u) {
            set.manual = !set.manual;
            if (set.manual && !set.ip && link_found())
                set.ip = link_found();
            ui.saved_dirty = 1;
            ui.redraw = 1;
        }
        if (down >> B_SAVE & 1u) { ui.saved_dirty = 0; save_settings(); }
        link_target(set.ip, set.manual);
        break;
    }
}
void app_input_only(void) { input(); }

/* ---- drawing: strips of the 240-wide screen */
static void title(const char *page)
{
    cv_begin(240, 32, C_BLACK);
    if (plat_ms() < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, "SYRUP", C_HI);
        x = cv_text(x + 6, 6, &FONT_B, "LINK", C_WHITE);
        cv_text(230 - text_w(&FONT_XS, page), 11, &FONT_XS, page, C_GRAY);
    }
    cv_rect(0, 31, 240, 1, C_LINE);
    cv_blit(0, 0);
}

static void row(int32_t y, const char *label, const char *value, uint16_t vc, int lit)
{
    cv_begin(240, 18, C_BLACK);
    if (lit)
        cv_rect(4, 0, 232, 17, C_LINE);
    cv_text(10, 2, &FONT_XS, label, C_GRAY);
    if (value)
        cv_text(88, 2, &FONT_XS, value, vc);
    cv_blit(0, (uint32_t)y);
}

static void hint(int32_t y, const char *t)
{
    cv_begin(240, 16, C_BLACK);
    cv_text(10, 1, &FONT_XS, t, C_DIM);
    cv_blit(0, (uint32_t)y);
}

static void draw_link(void)
{
    char t[48], *q;
    int st = plat_wifi_state(), i;
    uint32_t ip = set.manual ? set.ip : link_found(), ago = link_found_ago();
    title("HOME");
    q = put(t, st >= 0 && st <= WIFI_FAILED ? STATE[st] : "?");
    if (st == WIFI_CONNECTED) { q = put(q, "  "); put(q, plat_wifi_ssid()); }
    else if (st == WIFI_FAILED) { q = put(q, "  "); put(q, plat_wifi_why()); }
    row(38, "WI-FI", t, st == WIFI_CONNECTED ? C_HI : C_AMB, 0);
    if (ip) {
        q = put_ip(t, ip);
        put(q, set.manual ? "  SET" : ago < 6000u ? "  FOUND" : "  QUIET");
    } else
        put(t, set.manual ? "NO ADDRESS SET" : "LOOKING (WI-FI MIDI ON?)");
    row(56, "SYRUP", t, ip && (set.manual || ago < 6000u) ? C_HI : C_AMB, 0);
    q = put_int(t, (int32_t)link_sent());
    q = put(q, " SENT");
    if (link_dropped()) { q = put(q, "  "); q = put_int(q, (int32_t)link_dropped()); put(q, " LOST"); }
    row(74, "OUT", t, C_WHITE, 0);
    q = put_int(t, (int32_t)in_count);
    put(q, in_count && plat_ms() - in_ago_ms < 300u ? " IN  *" : " IN");
    row(92, "USB / TRS", t, C_WHITE, 0);
    q = put(t, "F");
    put_int(q, (int32_t)(3 + set.octave));
    row(110, "KEYS", t, C_HI, 0);
    cv_begin(240, 46, C_BLACK);
    for (i = 0; i < 4; i++) {
        int32_t x = 6 + i * 58, w = 52, v = set.cc[i] * w / 127;
        cv_text(x, 0, &FONT_XS, CC_NAME[i], C_GRAY);
        cv_rect(x, 18, w, 8, C_LINE);
        cv_rect(x, 18, v, 8, C_HI);
        put_int(t, set.cc[i]);
        cv_text(x, 28, &FONT_XS, t, C_WHITE);
    }
    cv_blit(0, 132);
    hint(182, "PRESET patch  PLAY seq  ARP acid  FX panic");
    hint(198, "SEL wi-fi  EDIT address  REC wi-fi on/off");
    hint(214, ui.saved_dirty ? "SAVE keeps octave and knobs" : "OCT- OCT+ move the keys");
}

static void draw_wifi(void)
{
    char t[40], ssid[33];
    int i;
    title("SEL  WI-FI");
    for (i = 0; i < 9; i++) {
        int rssi = 0, sec = 0, have = plat_wifi_found(i, ssid, &rssi, &sec);
        char *q = t;
        t[0] = 0;
        if (have) {
            q = put_int(q, rssi);
            put(q, sec ? (plat_wifi_kept(ssid) ? " kept" : " lock") : "");
        }
        cv_begin(240, 18, C_BLACK);
        if (have && i == ui.pick)
            cv_rect(4, 0, 232, 17, C_LINE);
        cv_text(10, 2, &FONT_XS, have ? ssid : (i == 0 ? "LOOKING..." : ""), i == ui.pick ? C_WHITE : C_GRAY);
        if (have)
            cv_text(230 - text_w(&FONT_XS, t), 2, &FONT_XS, t, C_DIM);
        cv_blit(0, 36u + (uint32_t)i * 18u);
    }
    hint(206, "SELECT picks   SAVE joins");
    hint(222, "SEL looks again   HOME back");
}

static void draw_pass(void)
{
    char t[72], c[2];
    int n = 0, i;
    title("PASSWORD");
    row(38, "NETWORK", ui.ssid, C_WHITE, 0);
    while (ui.pass[n]) n++;
    put(t, n > 26 ? ui.pass + n - 26 : ui.pass);
    row(60, "PASSWORD", t, C_HI, 1);
    cv_begin(240, 46, C_BLACK);
    for (i = -4; i <= 4; i++) {
        int k = ((ui.ch + i) % NCHARS + NCHARS) % NCHARS;
        c[0] = CHARS[k] == ' ' ? '_' : CHARS[k];
        c[1] = 0;
        if (i == 0) {
            cv_rect(104, 2, 32, 40, C_LINE);
            cv_text(120 - text_w(&FONT_B, c) / 2, 10, &FONT_B, c, C_WHITE);
        } else
            cv_text(120 + i * 24 - text_w(&FONT_XS, c) / 2 + (i > 0 ? 8 : -8), 16, &FONT_XS, c, C_DIM);
    }
    cv_blit(0, 86);
    hint(150, "SELECT picks a character");
    hint(166, "PRESET jumps: a-z  A-Z  0-9  !?");
    hint(182, "SAVE adds it   FX deletes");
    hint(198, "PLAY joins   HOME leaves");
}

static void draw_addr(void)
{
    char t[40];
    int i;
    uint32_t f = link_found();
    title("EDIT  ADDRESS");
    row(38, "MODE", set.manual ? "SET BY HAND" : "AUTO (SYRUP ANNOUNCES ITSELF)", C_HI, 0);
    if (f) put_ip(t, f); else put(t, "not heard yet");
    row(56, "HEARD", t, f ? C_WHITE : C_DIM, 0);
    cv_begin(240, 48, C_BLACK);
    for (i = 0; i < 4; i++) {
        int32_t x = 10 + i * 56;
        put_int(t, (int32_t)(set.ip >> (8 * i) & 255u));
        cv_text(x, 0, &FONT_XS, i == 0 ? "K1" : i == 1 ? "K2" : i == 2 ? "K3" : "K4", C_DIM);
        cv_text(x, 16, &FONT_B, t, set.manual ? C_WHITE : C_DIM);
    }
    cv_blit(0, 82);
    hint(150, "K1..K4 set the address (turns MODE to SET)");
    hint(166, "REC switches AUTO / SET");
    hint(182, ui.saved_dirty ? "SAVE keeps it" : "Syrup: HARDWARE > WI-FI MIDI ON");
    hint(198, "HOME back");
}

static void draw(void)
{
    lcd_fill(0, 230, 240, 10, C_BLACK);
    switch (ui.page) {
    case PG_WIFI: draw_wifi(); break;
    case PG_PASS: draw_pass(); break;
    case PG_ADDR: draw_addr(); break;
    default: draw_link(); break;
    }
}

void app_ui_init(void)
{
    int k;
    for (k = 0; k < 27; k++)
        ui.held_note[k] = -1;
    palette_set(1);                      /* amber, like Syrup */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    plat_wifi_on(1);                     /* it joins the networks kept */
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last, last_draw, last_page = 99;
    uint32_t now = plat_ms(), h;
    input();
    if (plat_wifi_state() == WIFI_CONNECTED)
        link_start();
    if ((uint32_t)ui.page != last_page) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        last_page = (uint32_t)ui.page;
        ui.redraw = 1;
    }
    h = (uint32_t)plat_wifi_state() ^ plat_wifi_ip() * 7u ^ link_found() * 13u ^ link_sent() * 31u ^ in_count * 17u ^
        plat_wifi_scans() * 3u;
    if (h != last || (ui.msg_until && now >= ui.msg_until) || now - last_draw > 1000u) {
        last = h;
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw && now - last_draw > 30u) {
        ui.redraw = 0;
        last_draw = now;
        draw();
    }
    plat_leds((ui.playing ? 1u << B_PLAY : 0u) | (ui.saved_dirty ? 1u << B_SAVE : 0u) | (1u << (ui.page == PG_WIFI ? B_SEL : ui.page == PG_ADDR ? B_EDIT : B_HOME)), ui.keys);
}

#ifdef PLAT_HOST
/* the host simulator has no lwIP (net.c is the device's): a stand-in that "finds" Syrup and counts what it sends */
static uint32_t h_sent, h_target, h_manual;
void link_start(void) {}
int link_running(void) { return 1; }
void link_push(const uint8_t *b, uint32_t n, int from_render) { (void)b, (void)n, (void)from_render; h_sent++; }
void link_target(uint32_t ip, int manual) { h_target = ip; h_manual = (uint32_t)manual; }
uint32_t link_found(void) { return 0xB701A8C0u; }   /* 192.168.1.183 */
uint32_t link_found_ago(void) { return 500; }
uint32_t link_sent(void) { return h_sent; }
uint32_t link_dropped(void) { return 0; }
#endif
