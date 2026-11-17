# DS-1000

A TR-style drum machine for Nintendo 2DS / 3DS (homebrew), after Roland's TR-1000 workflow. Lives in the Syrup repo; built by `.github/workflows/ds1000.yml` into the `ds1000` release.

**Install:** FBI > Remote Install > Scan QR Code > `install-qr.png`. Or copy `ds1000.3dsx` to `/3ds/`.

## Sound

16 tracks: BD, SD, LT, MT, HT, RS, CP, CH, OH, CC, RC, CB (808 style), BD9, SD9 (909 style), CL, MA. All synthesised; CH chokes OH.

Hardware tricks (same approach as Syrup):
- **Core 1 renders, the DSP plays.** Each track's hit is rendered into a sample on the second CPU core only when its sound changes; the DSP chip plays the hits on 16 channels. TUNE is the DSP channel's playback rate, LEVEL / PAN / DELAY are its mix, and the track FILTER is the channel's own hardware biquad.
- **DSP effect buses** (Syrup's sound-driver copy, `source/ndsp/`): bus 0 runs the master chain (DJ filter + drive), bus 1 a tempo-synced ping-pong delay. If the DSP never returns bus audio, it falls back to direct out.
- **Sequencer clock** in the DSP frame callback (~4.9 ms).
- **Mic sampling** (HW page): records 2 s into the selected track, trimmed to the first sound, normalised. SOURCE switches the track between SYNTH and MIC.
- **Motion** (HW page): tilt left/right sweeps the master filter, tilt forward throws more into the delay, twist bends every hit's pitch, shake rolls the selected track.
- **Beat light**, **OUTPUT** STEREO / SURROUND / AUTO (surround on headphones), 804 MHz on New 3DS / New 2DS.
- **LINK** with Syrup or another DS-1000: WIFI HOST / WIFI JOIN (local wireless) or IR SEND / IR RECEIVE, same protocol as Syrup.

From PICO / zp12: 8 velocity levels per step, unquantized recording (QUANTIZE OFF keeps your timing as nudge), count-in and click, ERASE as the playhead passes, UNDO / REDO, TAP TEMPO, CHAIN, note-repeat ROLL (1/8 to 1/64) on held pads, filter types per track (LP, HP, BP, NOTCH, PEAK), SP-style BITS (16 to 4) and RATE crush, START and REVERSE.

## Velocity, nudge, mic velocity

- **Circle pad up / down:** the selected step's velocity (8 levels; the bar on the step, the cell height on the top screen). On the pads (TAP / ERASE): the pad velocity.
- **Circle pad left / right:** the selected step's nudge, up to half a step early or late in 12ths (the dot on the step; the cell shifts on the top screen). Touching an empty step with the circle pad turns it on.
- **MIC VELOCITY** (HW page): **PADS** reads how hard you tap a pad from the sound of the tap; **TAP MIC** plays the selected track when you tap the console near the mic, louder taps = higher velocity (records in TAP mode). It scales itself to your hardest tap. Headphones keep the speaker from triggering it.

## Screens

Top: pattern bank A-P, all 16 tracks x 16 steps with per-track playheads, status.

Bottom:
- **Instrument row** (16): select a track. In MUTE mode, mute it. In TAP mode, play it.
- **Steps** (16): STEP toggles hits; ACC accents; PROB cycles 100/75/50/25%; SUB cycles ratchets x1-x4. In TAP mode they become 16 pads (play + record while running; hold for ROLL).
- **Tabs:** STEP, ACC, PROB, SUB, TAP, ERASE (hold a pad: its hits vanish as the playhead passes; stopped: clears the track), MUTE, and the page switch (TRK > FLT > MST > SEQ > HW).
- **TRK:** TUNE, DECAY, TONE, LEVEL, PAN, DELAY, DRIVE, LAST (per-track pattern length, for polymeters).
- **FLT:** FILTER type, CUTOFF, RESO, BITS, RATE, START, REVERSE, SOURCE.
- **MST:** BPM, SWING (50-75%), ACCENT, ROLL, DLY TIME, DLY FDBK, FILTER (LP <- OFF -> HP), DRIVE.
- **SEQ:** QUANTIZE, CLICK, COUNT-IN (1 bar), CHAIN (plays the used patterns in turn), TAP TEMPO, UNDO (again: redo), SHIFT < / > (rotate the track).
- **HW:** MIC SAMPLE, MOTION (turning it on re-centres the tilt), BEAT LIGHT, LINK, OUTPUT, MIC VELOCITY, SAVE, CLEAR PATTERN.

## Buttons

A play/stop, B audition track, X clear track, Y randomize track, D-pad up/down track, left/right BPM, L/R pattern (switches at the end of the bar while playing), SELECT copy pattern to the next slot, START save and quit.

Saves to `/3ds/ds1000/ds1000.sav` (patterns, kit, mic samples, settings); older saves load with full velocity and no nudge.
