/* SPDX-License-Identifier: GPL-3.0-only */
/* PICO: the sample room in the app space the kit left free (the kit only on bank A). The firmware's slot ends
 * below it (tools/fm1pkg_make.py APP_SLOT, ROOM_LO / ROOM_HI: keep them equal), the update loader never writes it
 * (firmware/loader/ldr_core.c), the sampler, the editor and the link use it as one more room. 4 KiB aligned.
 * Back it up with PICO's backup page before flashing stock zp12 / SLOOP: their loaders write the whole app area. */
#ifndef PICO_ROOM_H
#define PICO_ROOM_H
#ifndef PICO_ROOM_LO                       /* (the host tests may set it: tests/sp_sampler_test.c) */
#define PICO_ROOM_LO 0x5C000u
#define PICO_ROOM_HI 0x93000u
#endif
#endif
