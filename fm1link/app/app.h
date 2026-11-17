/* SPDX-License-Identifier: MPL-2.0 */
/* SYRUP LINK: the FM-1 as a Wi-Fi MIDI bridge for Syrup (a wavetable synth for the Nintendo 2DS / 3DS).
 * Everything that comes in over USB MIDI or the TRS jack goes on over Wi-Fi to Syrup (UDP port 5008, plain MIDI
 * bytes), and the FM-1's own keys, knobs and buttons play and control it. Syrup announces itself on UDP 5009, so the
 * FM-1 finds the 2DS by itself; an address can also be set by hand. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "LINK"
#define APP_SPLASH_RGB RGB(255, 176, 46)
#define APP_BLOCK 256u
#define APP_CANVAS_PX (240u * 48u)       /* strips 48 rows high: Wi-Fi's buffers need the pool */

enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_WIFI, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u, 0xDA000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

/* net.c (built apart, with lwIP's headers) */
void link_start(void);                   /* starts the network task once (call when Wi-Fi is connected) */
int link_running(void);
void link_push(const uint8_t *b, uint32_t n, uint32_t t24, int from_render);   /* one message and when it came in
                                         * (plat_ticks24's clock); two queues: render, main loop */
void link_target(uint32_t ip, int manual);   /* manual: use ip; else the address the beacon gave */
uint32_t link_found(void);               /* the address Syrup announced itself from (0 = not heard yet) */
uint32_t link_found_ago(void);           /* ms since its last announcement */
uint32_t link_sent(void);                /* datagrams sent */
uint32_t link_dropped(void);             /* bytes dropped (a queue full) */
