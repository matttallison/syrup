#include "PluginProcessor.h"
#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;

static juce::StringArray noteNames() { return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; }

APVTS::ParameterLayout AcidProcessor::layout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "seed", 1 }, "Seed", 0, 63, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "mutate", 1 }, "Mutate", 0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "root", 1 }, "Root", noteNames(), 9));
    StringArray sc; for (auto* n : acid::scaleNames) sc.add (n);
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", sc, 0));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "octave", 1 }, "Octave", -2, 2, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "length", 1 }, "Length", StringArray { "8", "16", "32" }, 1));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "density", 1 }, "Density", 0.0f, 1.0f, 0.7f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "accent", 1 }, "Accent", 0.0f, 1.0f, 0.3f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "slide", 1 }, "Slide", 0.0f, 1.0f, 0.2f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "jumps", 1 }, "Jumps", 0.0f, 1.0f, 0.2f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gate", 1 }, "Gate", 0.0f, 1.0f, 0.55f));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "velNorm", 1 }, "Velocity", 1, 127, 90));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "velAcc", 1 }, "Accent Velocity", 1, 127, 127));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "channel", 1 }, "MIDI Channel", 1, 16, 1));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "keyMode", 1 }, "Keys", StringArray { "Ignore", "Transpose", "Hold to play" }, 1));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "freeRun", 1 }, "Run when stopped", false));
    return { p.begin(), p.end() };
}

AcidProcessor::AcidProcessor()
    : AudioProcessor (BusesProperties()), apvts (*this, nullptr, "STATE", layout()) {}

static int ival (const APVTS& a, const char* id) { return (int) std::lround (a.getRawParameterValue (id)->load()); }
static float fval (const APVTS& a, const char* id) { return a.getRawParameterValue (id)->load(); }

acid::Settings AcidProcessor::settings() const
{
    acid::Settings s;
    s.seed = ival (apvts, "seed"); s.mutate = fval (apvts, "mutate"); s.density = fval (apvts, "density");
    s.accent = fval (apvts, "accent"); s.slide = fval (apvts, "slide"); s.jumps = fval (apvts, "jumps"); s.scale = ival (apvts, "scale");
    return s;
}
int AcidProcessor::length() const { const int l = ival (apvts, "length"); return l == 0 ? 8 : l == 1 ? 16 : 32; }
int AcidProcessor::rootNote() const
{
    int r = 36 + ival (apvts, "root") + 12 * ival (apvts, "octave");   // default A1 (bass register)
    if (ival (apvts, "keyMode") >= 1 && keyNote >= 0) r += (keyNote - 48);   // C3 = no transpose
    return juce::jlimit (0, 115, r);
}

void AcidProcessor::noteOn (juce::MidiBuffer& m, int note, int vel, int pos)
{
    m.addEvent (juce::MidiMessage::noteOn (ival (apvts, "channel"), note, (juce::uint8) vel), pos);
    sounding = note;
}
void AcidProcessor::noteOff (juce::MidiBuffer& m, int pos)
{
    if (sounding >= 0) m.addEvent (juce::MidiMessage::noteOff (ival (apvts, "channel"), sounding), pos);
    sounding = -1; pendingOffSamples = -1;
}

void AcidProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    audio.clear();
    const int n = audio.getNumSamples() > 0 ? audio.getNumSamples() : 512;
    const int keyMode = ival (apvts, "keyMode");

    // keys: transpose and/or hold-to-play; everything else passes through
    juce::MidiBuffer out;
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (keyMode >= 1 && msg.isNoteOn()) { keyNote = msg.getNoteNumber(); heldKeys++; continue; }
        if (keyMode >= 1 && msg.isNoteOff()) { heldKeys = juce::jmax (0, heldKeys - 1); continue; }
        out.addEvent (msg, meta.samplePosition);
    }

    bool playing = false; double ppq = freePpq, bpm = 120.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = *b;
            playing = pos->getIsPlaying();
            if (playing) if (auto p = pos->getPpqPosition()) ppq = *p;
        }
    const bool freeRun = ival (apvts, "freeRun") != 0;
    bool run = playing || freeRun;
    if (keyMode == 2 && heldKeys == 0) run = false;
    if (! playing) ppq = freePpq;

    if (! run)
    {
        if (wasPlaying || sounding >= 0) { noteOff (out, 0); lastSlide = false; }
        wasPlaying = false; playStep = -1;
        if (keyMode == 2) freePpq = 0;   // hold-to-play restarts the line from step 1
        midi.swapWith (out);
        return;
    }
    if (! wasPlaying && ! playing) freePpq = ppq = 0;
    wasPlaying = true;

    const double ppqPerSample = bpm / 60.0 / sampleRate, stepSamples = sampleRate * 60.0 / bpm / 4.0;
    const double end = ppq + n * ppqPerSample;
    const int len = length(), root = rootNote();
    const auto st = settings();
    const float gate = fval (apvts, "gate");

    int k = (int) std::ceil (ppq * 4.0 - 1e-6);
    auto flushOff = [&] (int upTo) {
        if (pendingOffSamples >= 0 && pendingOffSamples < upTo) noteOff (out, pendingOffSamples);
    };
    for (; k < end * 4.0; ++k)
    {
        const int pos = juce::jlimit (0, n - 1, (int) std::lround ((k / 4.0 - ppq) / ppqPerSample));
        flushOff (pos + 1);
        const int idx = ((k % len) + len) % len;
        const auto s = acid::makeStep (idx, st);
        playStep = idx;
        if (s.on)
        {
            const int note = juce::jlimit (0, 127, root + s.semis);
            const int vel = s.accent ? ival (apvts, "velAcc") : ival (apvts, "velNorm");
            if (lastSlide && sounding >= 0)
            {
                if (note != sounding) { const int old = sounding; noteOn (out, note, vel, pos);   // legato: new note before the old one ends
                    out.addEvent (juce::MidiMessage::noteOff (ival (apvts, "channel"), old), pos); }
            }
            else
            {
                if (sounding >= 0) noteOff (out, pos);
                noteOn (out, note, vel, pos);
            }
            pendingOffSamples = s.slide ? -1 : pos + (int) juce::jmax (32.0, stepSamples * (0.1 + gate * 0.85));
            lastSlide = s.slide;
        }
        else
        {
            if (lastSlide && sounding >= 0) noteOff (out, pos);   // a slide into a rest ends the note
            lastSlide = false;
        }
    }
    flushOff (n);
    if (pendingOffSamples >= 0) pendingOffSamples -= n;   // carries into the next block
    if (pendingOffSamples < -1) pendingOffSamples = 0;
    freePpq = end;
    midi.swapWith (out);
}

void AcidProcessor::getStateInformation (juce::MemoryBlock& d)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, d);
}
void AcidProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size)) apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* AcidProcessor::createEditor() { return new AcidEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AcidProcessor(); }
