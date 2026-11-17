/* SPDX-License-Identifier: MPL-2.0 */
/* SYRUP LINK's network side (built as the SDK builds code that talks lwIP, apart from the app's unity build).
 * One task: it sends what the two MIDI queues hold to Syrup as UDP datagrams (port 5008, plain MIDI bytes, complete
 * messages), and listens on port 5009 for Syrup's "SYRUP" announcement to learn the 2DS's address. */
#include <stdint.h>
#include <string.h>
#include "lwip/sockets.h"
#include "plat.h"
#include "app.h"

extern void os_time_dly(int ticks);   /* the SDK's OS: wait this many ticks (fm1-radio does the same) */

#define QN 2048u                         /* bytes per queue (a power of two) */
struct q { uint8_t b[QN]; volatile uint32_t w, r; };
static struct q qr, qm;                  /* filled by the audio interrupt (USB / TRS in) and by the main loop */
static volatile int running;
static volatile uint32_t target_ip, found_ip, found_ms, sent, dropped;
static volatile int manual;

static void qpush(struct q *q, const uint8_t *b, uint32_t n)
{
    uint32_t w = q->w, i;
    if (QN - (w - q->r) < n) {           /* never half a message */
        dropped += n;
        return;
    }
    for (i = 0; i < n; i++)
        q->b[(w + i) & (QN - 1u)] = b[i];
    __sync_synchronize();
    q->w = w + n;
}
/* a record: the time it came in (24 MHz ticks, plat_ticks24's clock) then the message; Syrup plays each at that time
 * plus a fixed buffer, so the network's wobble becomes one steady delay */
void link_push(const uint8_t *b, uint32_t n, uint32_t t24, int from_render)
{
    uint8_t r[8];
    uint32_t i;
    if (n > 3u)
        return;
    r[0] = (uint8_t)t24;
    r[1] = (uint8_t)(t24 >> 8);
    r[2] = (uint8_t)(t24 >> 16);
    r[3] = (uint8_t)(t24 >> 24);
    for (i = 0; i < n; i++)
        r[4 + i] = b[i];
    qpush(from_render ? &qr : &qm, r, 4u + n);
}

static uint32_t rec_len(uint8_t st)
{
    if (st >= 0xF8u) return 5u;
    switch (st & 0xF0u) {
    case 0xC0u: case 0xD0u: return 6u;
    case 0xF0u: return st == 0xF2u ? 7u : (st == 0xF1u || st == 0xF3u) ? 6u : 5u;
    default: return 7u;
    }
}
static uint32_t qtake(struct q *q, uint8_t *d, uint32_t room)
{
    uint32_t r = q->r, w = q->w, n = 0, i;
    while (w - r >= 5u) {                 /* whole records only */
        uint32_t k = rec_len(q->b[(r + 4u) & (QN - 1u)]);
        if (w - r < k || n + k > room)
            break;
        for (i = 0; i < k; i++)
            d[n + i] = q->b[(r + i) & (QN - 1u)];
        n += k;
        r += k;
    }
    __sync_synchronize();
    q->r = r;
    return n;
}

void link_target(uint32_t ip, int man) { target_ip = ip; manual = man; }
uint32_t link_found(void) { return found_ip; }
uint32_t link_found_ago(void) { return found_ip ? plat_ms() - found_ms : 0xFFFFFFFFu; }
uint32_t link_sent(void) { return sent; }
uint32_t link_dropped(void) { return dropped; }
int link_running(void) { return running; }

static int udp_bound(int port, int timeout_ms)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        closesocket(s);
        return -1;
    }
    (void)timeout_ms;
    return s;
}

static void task(void *p)
{
    static uint8_t buf[512], in[64];
    int tx = -1, rx = -1;
    (void)p;
    for (;;) {
        uint32_t ip, n;
        if (plat_wifi_state() != WIFI_CONNECTED) {
            if (tx >= 0) { closesocket(tx); tx = -1; }
            if (rx >= 0) { closesocket(rx); rx = -1; }
            os_time_dly(5);
            continue;
        }
        if (tx < 0)
            tx = socket(AF_INET, SOCK_DGRAM, 0);
        if (rx < 0)
            rx = udp_bound(5009, 0);
        if (rx >= 0) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int k = recvfrom(rx, in, sizeof in, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if (k >= 5 && !memcmp(in, "SYRUP", 5)) {
                found_ip = from.sin_addr.s_addr;
                found_ms = plat_ms();
            }
        }
        ip = manual ? target_ip : found_ip;
        buf[0] = 'S';                     /* "SL" 1: timed records */
        buf[1] = 'L';
        buf[2] = 1;
        n = 3u + qtake(&qr, buf + 3, sizeof buf - 3u);
        n += qtake(&qm, buf + n, sizeof buf - n);
        if (n > 3u && ip && tx >= 0) {
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_len = sizeof a;
            a.sin_family = AF_INET;
            a.sin_port = htons(5008);
            a.sin_addr.s_addr = ip;
            if (sendto(tx, buf, n, 0, (struct sockaddr *)&a, sizeof a) == (int)n)
                sent++;
            else {
                closesocket(tx);         /* (a network gone and back: a new socket) */
                tx = -1;
            }
        }
        if (qr.w == qr.r && qm.w == qm.r)
            os_time_dly(1);              /* nothing waiting: one OS tick, then look again */
    }
}

void link_start(void)
{
    if (running)
        return;
    if (plat_net_task(task, 0, "link") == 0)
        running = 1;
}
