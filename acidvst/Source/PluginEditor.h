#pragma once
#include "PluginProcessor.h"

class AcidEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AcidEditor (AcidProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override { repaint (strip); }
    AcidProcessor& proc;
    juce::Rectangle<int> strip;
    juce::TextButton newBtn { "NEW" }, mutBtn { "MUTATE" }, prevBtn { "<" }, nextBtn { ">" };
    struct Ctl { std::unique_ptr<juce::Component> c; std::unique_ptr<juce::Label> l;
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sa;
                 std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> ca;
                 std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ba; };
    std::vector<Ctl> ctls;
    void setParam (const char* id, float plain);
};
