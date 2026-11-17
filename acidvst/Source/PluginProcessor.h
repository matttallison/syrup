#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Generator.h"

class AcidProcessor : public juce::AudioProcessor
{
public:
    AcidProcessor();
    void prepareToPlay (double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Syrup Acid"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    acid::Settings settings() const;
    int length() const;
    int rootNote() const;   // MIDI note of the pattern root (key-follow included)
    std::atomic<int> playStep { -1 };

    // The playing pattern. Generated from the knobs, then editable in the step grid; edits stay until a
    // generator knob (seed, mutate, density, accent, slide, jumps, scale) changes.
    struct PStep { bool on = false, acc = false, slide = false; int semis = 0; };
    static constexpr int maxSteps = 32;
    std::array<PStep, maxSteps> getPattern();
    void editStep (int i, std::function<void (PStep&)> fn);
    void regenerate();
    void checkRegen();   // regenerates if a generator knob moved since the last generate

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    void noteOn (juce::MidiBuffer&, int note, int vel, int pos);
    void noteOff (juce::MidiBuffer&, int pos);
    double sampleRate = 44100;
    int sounding = -1, pendingOffSamples = -1, keyNote = -1, heldKeys = 0;
    bool wasPlaying = false, lastSlide = false;
    double freePpq = 0;
    std::array<PStep, maxSteps> pat, patAudio;
    juce::SpinLock patLock;
    acid::Settings lastGen; bool generatedOnce = false;
    static bool sameGen (const acid::Settings& a, const acid::Settings& b);   // free-running position when the host transport isn't running
};
