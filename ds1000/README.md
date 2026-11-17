# DS-1000

A drum machine for Nintendo 2DS / 3DS (homebrew), with the workflow and look of PICO (Matt's SP-1200-style FM-1 firmware). It's a drum machine, not a sampler: 16 synthesised 808/909-style voices. Lives in the Syrup repo; built by `.github/workflows/ds1000.yml` into the `ds1000` release.

**Install:** FBI > Remote Install > Scan QR Code > `install-qr.png`. Or copy `ds1000.3dsx` to `/3ds/`.

## Layout (PICO style)

- **Top screen:** header (menu, bank, loop + bar.beat, tempo, PLAY / REC / ARMED / COUNT, battery), message line (selected pad + its last velocity, `A3 TOM L  V96`, or the last action), the loop (every pad, one bar at a time: cell height = velocity, shift = nudge, yellow dot = condition, blue dot = ratchet), the hint line. NOTES shows a piano roll instead (in-key rows lit, root red, selected row blue). PUNCHES / FX show the punch lane.
- **Bottom screen:** six menu tabs (MIX, SND, ENV, FX, SEQ, GLO; tap again to step through its pages), 16 keys, then the page list on the left (a page's second entry indented under it) and the light knob panel: 2x2 knobs, each with its number chip, name and value. Drag a knob right or up to turn it; D-pad left / right nudges the selected one. Knobs that do something (CLEAR, WRITE, COPY, MUTATE, RESET) need two taps (AGAIN).
- **No hidden menus:** every control is on a page. SELECT steps through every page in order (on PUNCHES it picks the punch, on NOTES the next note).

## Keys

PADS view: keys 1-8 are the bank's pads (L switches bank A / B), keys 9-16 are one-press jobs: REC, ROLL (hold a pad for note repeat), ERASE (hold a pad: its hits vanish as they pass), PUNCH (hold), QUANT, SWING (50 / 54 / 58 / 62 / 66%), UNDO (again: redo), TAP tempo.
STEPS view (R, or the STEP / DETAIL / NOTES pages): the selected pad's steps for the bar in view; tap to add / remove.

Buttons: A play / stop, B REC (stopped: armed, A counts in), X hold = punch, Y hold = note repeat on the selected pad, L bank, R pads / steps, D-pad up / down pad, left / right knob, circle pad up / down velocity and left / right nudge of the selected step, SELECT next page, START save and quit.

## Menus

- **MIX:** HOME (SP-style faders for the bank: LEVEL, TUNE, DECAY or CUTOFF), BUS FX (filter LP / HP, drive, echo, echo time) > PAN (bus auto-pan rate, depth, volume), OUT (pad level, pan, drive, echo send) > FADER.
- **SND:** PAD (voice: any of the 16 sounds, tune, fine, tone) > TRIM (start, direction, choke group), CRUSH (bits 16-4, rate), COPY (pad sound to another pad).
- **ENV:** FILTER (cutoff, reso, envelope sweep, type LP / HP / BP / NOTCH / PEAK on the DSP's own filter) > LOCUT, AMP (attack, decay).
- **FX:** PUNCH (REPEAT, STUTTER, TAPE STOP, FADE, REVERSE; rate, decay, level), MOTION (tilt: filter or echo, twist: pitch, shake: roll or punch).
- **SEQ:** STEP (pad, bar, roll rate, clear pad), DETAIL (velocity 1-127, nudge in 12ths, condition: 75/50/25%, 1ST, 1:2 ... 4:4, NOT 1ST; ratchet), PUNCHES (four per loop: type, bar, move, length), NOTES (step, note, velocity, scale) > KEY, TEMPO (tempo, quantize, loop A-P, bars 1-4) > METER (swing, 16 or 12 steps per bar), SONG (on, step, loop, repeat) > TRANSP, PATTERN (Euclidean or preset bars written onto the pad) > ROTATE (rotate, shift, mutate), PAD TIME (the pad's own rate X1 / 1/2 / X2 / 3/4 / 3/2 / 1/4 / 1/8 and length for polymeter), LOOP TOOLS (clear, copy to).
- **GLO:** SETUP (save, reset, version), CLICK (off / rec / on, count-in, tick, level), LINK (with Syrup or another DS-1000: wifi host / join, IR send / receive), OUTPUT (stereo / surround / auto on headphones, beat light), TOUCH (velocity: fixed, position on the pad, mic tap loudness, tap the mic to play; PICO's curve; last; fixed value).

## Hardware

The core-1 worker renders each pad's hit only when its sound changes; the DSP plays the hits on 16 channels (rate = tune, hardware biquad = filter, one-pole = low cut). DSP effect bus 0 runs punch, bus filter, drive and auto-pan; bus 1 the ping-pong echo (Syrup's sound-driver copy in `source/ndsp/`). Sequencer in the DSP frame callback. 804 MHz on New 3DS / New 2DS.

Saves to `/3ds/ds1000/ds1000.sav`.
