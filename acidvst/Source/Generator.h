#pragma once
#include <cmath>

// Same generator as the Drambo Code module (acid_bassline.txt): every step comes from a hash of
// (step, seed), so a seed number is a pattern and matches between the plugin and the Code module.
namespace acid
{
struct Settings { int seed = 0; float mutate = 0, density = 0.7f, accent = 0.3f, slide = 0.2f, jumps = 0.2f; int scale = 0; };
struct Step { bool on = false, accent = false, slide = false; int semis = 0; };

static inline float fr (float x) { return x - std::floor (x); }
static inline float hsh (float s, float a, float sd, float b, float k) { return fr (std::sin (s * a + sd * b) * k); }

static const char* const scaleNames[] = { "Minor", "Phrygian", "Dorian", "Minor Pent", "Harmonic Min", "Mixolydian", "Phrygian Dom" };
static const int scales[7][7] = {
    { 0, 2, 3, 5, 7, 8, 10 }, { 0, 1, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 3, 5, 7, 10, 12, 15 },
    { 0, 2, 3, 5, 7, 8, 11 }, { 0, 2, 4, 5, 7, 9, 10 }, { 0, 1, 4, 5, 7, 8, 10 } };

// semis is relative to the root, before octave/root offsets
static inline Step makeStep (int step, const Settings& st)
{
    const float s = (float) step, seedQ = (float) st.seed;
    const float hm = fr (std::sin (s * 7.31f + seedQ * 3.17f + 0.5f) * 43758.5453f);
    const float sd = hm < st.mutate * 0.999f ? seedQ + 101.0f : seedQ;
    const float h1 = hsh (s, 12.9898f, sd, 78.233f, 43758.5453f);
    const float h2 = hsh (s, 39.3468f, sd, 11.135f, 24634.6345f);
    const float h3 = hsh (s, 73.156f, sd, 52.235f, 37165.2715f);
    const float h4 = hsh (s, 26.651f, sd, 4.983f, 51842.8761f);
    const float h5 = hsh (s, 91.733f, sd, 17.411f, 29411.3317f);
    const bool beat = step % 4 == 0;
    Step o;
    o.on = beat ? h2 < 0.92f : h2 < st.density;
    if (step == 0) o.on = true;
    static const int degTab[8] = { 0, 0, 0, 4, 2, 6, 1, 0 };
    int deg = degTab[(int) (h1 * 8.0f) & 7];
    if (beat && h1 < 0.6f) deg = 0;
    int jump = h4 < st.jumps ? (h4 < st.jumps * 0.8f ? 12 : -12) : 0;
    if (deg > 5 && jump > 0) jump = 0;
    o.semis = scales[st.scale < 0 || st.scale > 6 ? 0 : st.scale][deg] + jump;
    o.slide = o.on && h5 < st.slide;
    o.accent = h3 < st.accent + (beat ? 0.15f : 0.0f);
    return o;
}
}
