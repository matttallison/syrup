# PICO: the spec

What the first PICO (FM-1 builds up to t127, Nov 2026) did, decided with Matt. The rebuild brings these back in stages.

## Principles (decided, do not reopen)

- The SP-1200 workflow first: sample, chop, assign to pads, sequence, mix. Clean and functional beats decorative.
- No hidden menus: every control is in a visible menu. A page with more than four controls gets a second entry in the
  page list, named for what is on it (TRIM, then "> CUT" under it). Holding SEL may still reach it, never only that.
- SELECT steps through every menu entry, second entries included, in one order across all menus. Only NOTES (moves
  between notes) and PUNCHES (picks the punch) borrow it.
- White keys play pads; black keys are fixed functions; white 9-16 are one-press jobs. Black 1-2 only switch fader banks.
- Look and bank handling after Matt's SPCTL firmware. Note repeat (rolls) is essential. Several filter styles per pad.
- Readable: bold Terminus, white on slate RGB(80,90,108), dark labels RGB(42,50,66) on the light knob panel.

## Screen

- Header: menu name, bank, loop / bar, tempo, play state, battery.
- Message line: the pad and its last hit's velocity (`A3 HAT  V110`), or the last action ("CUT: OLD KEPT", "GAPS 3");
  the page name on the right.
- Menu view: the menu's pages listed down the left (the current one highlighted, its second entry indented under it);
  on the right a light panel: a waveform on sample pages, then the knobs 2 x 2 (1 2 / 3 4): blue number chip, name,
  rule, big value. The knob values are drawn in the same pass as the panel (no blank panel between: that flickered).
- Hint line at the bottom: one short instruction for the page.

## Menus (each button opens its menu; pressing it again steps through the pages, second entries included)

- MIX: HOME (faders); BUS FX (filter, drive, reverb, echo; > PAN: pan, level, auto-pan, depth); OUT (> FADER).
- EDIT: PAD (> AUTO); TRIM (start, end, direction, SILENCE; > CUT: start, ZERO, direction, CUT); CHOP; PLAY; LOOP
  (mode, loop start, end, crossfade); STRETCH (by % / tempo, amount, grain, run); COPY (> MERGE); REC (quality, time,
  threshold, monitor; > INPUT: gain, zero, input L+R / LEFT / RIGHT); RESAMP (quality, time, source LOOP / MIX / PAD).
- ENV: envelope and FILTER (cutoff, reso, filter env, type; > LOCUT).
- FX: PUNCH (punch, rate, decay, level; > BUS).
- SEQ: STEP; DETAIL (> BAR); PUNCHES (> RATCH); NOTES (step, velocity, scale, length; > KEY); TEMPO (> METER);
  SONG (> TRANSP); PATTERN (> ROTATE); PAD TIME.
- GLO: SETUP, CLICK (> LEVEL), MIDI, OUTPUT, TOUCH (velocity on / off, curve, last), SYRUP LINK (new: see below).

## Pads and engine

- 32 pads, banks A-D of 8 on the white keys. Factory kit only on bank A (KICK SNARE HAT CLAP TOM-H CRASH BASS PNO);
  B-D empty for own samples (the kit's room became sample room: ~13 s at 12/26K).
- Per pad: wave, slice, tune, fine; start / end, FWD / REV; loop OFF / LOOP / PING / FRZ (needs NOTE ON), loop start,
  crossfade 0-8 ms; attack, decay, sustain, release; cutoff, reso, filter env, filter type, low cut; level, pan, drive,
  crush, sends, bus, choke; chord, inversion, key, scale.
- START / END snap to zero crossings (ZERO, per pad; OFF = exact). Kit 12-bit plays stair-stepped (the grit), own
  samples interpolated. A new recording resets its pad to neutral (decay full, level 110, filter open, one-shot, no
  choke / sends / chord); pan, mix level, bus and pattern stay.

## Sampler

- Quality 12/32K, 12/26K (default), 8/22K, ADPCM 32K / 22K / 11K. Mono.
- USB audio in (44.1 kHz). The host's volume slider sets the level with GAIN. INPUT L+R / LEFT / RIGHT.
- Path: gain, 8th-order Chebyshev anti-alias, linear resampler, soft knee from -2 dB, 12 / 8 / 4-bit writer.
- THRESH waits for the level (about 30 ms pre-roll). Meter shows peaks as the take gets them; CLIP = the take clipped.
- Never lose USB packets while the flash programs a page: poll the USB input from RAM during the page program
  (PIGEON's fix); hold the last frame for a missed packet; GAPS n after the take.
- SILENCE trim; CUT (the trim made a new sample, kit sounds too; turn twice); MERGE; RESAMP (loop, mix, or one pad
  printed with its settings).
- STRETCH (WSOLA): grains 25 / 50 / 90 ms, half overlap, triangular fades; each grain's source searched within
  +-1/4 hop for the best cross-correlation with what the last grain goes on into (decimated, <= 128 points); the
  first grain whole; 25-250 % or 1 beat - 8 bars at the tempo; a new sample, the original kept.

## Sequencer

- Loop recording, overdub, count-in, QUANT and SWING as one-press macros. Step entry; ERASE = LFO held + pad.
- Per-hit DETAIL: length, nudge, condition, bar. PAD TIME: RATE (X1 1/2 X2 3/4 3/2 1/4 1/8) and LEN (polymeter).
- PATTERN: euclidean (hits / steps / rotate) or preset bars. SHIFT a pad's pattern; MUTATE only the selected pad.
- PUNCHES: a sequence of punch effects, drawn like NOTES; long tape stop and fade.
- NOTES: the pad chromatic while there (MULTI PITCH, restored on leaving); every semitone a row, in-key rows lit, root
  red; BAR view and ALL view (lowest to highest note, >= 12 rows); SEL + white key adds / removes the row's note at
  that step, LFO + white key clears the step; knobs STEP VEL SCALE LENGTH; chords, named ("Cm9").
- Song mode (repeats, tempo, transpose per step); undo / redo.

## Keys and velocity

- White 9: held inverts the screen (a warning); a 0.5 s press toggles MACROS; with MACROS on white 10-16 need a
  0.5 s hold (SWING, QUANT one press); a panel shows the held key and its neighbours.
- Velocity from the keys (charlesvestal/fm1-base touch + sense, MPL-2.0, in fixed point): vel = 1 + 126 (1 - t)^c,
  t the log-scaled key reading between firm and light; c = 0.70 x 2^(CURVE / 5), CURVE -5..+5. TOUCH off: all 110.

## SYRUP LINK (new in the rebuild)

PICO is the clock: over Wi-Fi it sends MIDI clock and start / stop to Syrup on the 2DS / 3DS (UDP 5008, plain MIDI
bytes; Syrup announces itself on UDP 5009, as fm1link does), so Syrup plays in time with PICO.
