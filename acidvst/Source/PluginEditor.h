#pragma once
#include "PluginProcessor.h"

// Step grid: click a step to switch it on/off, drag up/down to change its pitch,
// and use the ACC / SLD / OCT strips underneath.
class StepGrid : public juce::Component
{
public:
    explicit StepGrid (AcidProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
private:
    struct Hit { int step = -1; int zone = 0; };   // zone 0 = note, 1 = accent, 2 = slide, 3 = octave
    Hit hitTest (juce::Point<int>) const;
    juce::Rectangle<float> cell (int i) const;
    AcidProcessor& proc;
    int dragStep = -1, dragStartSemis = 0, dragStartY = 0, paintMode = -1; bool dragged = false;
};

class AcidEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AcidEditor (AcidProcessor&);
    ~AcidEditor() override { setLookAndFeel (nullptr); }
    void paint (juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override { proc.checkRegen(); grid.repaint(); repaint (seedArea); }
    struct Ctl { std::unique_ptr<juce::Component> c; std::unique_ptr<juce::Label> l;
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sa;
                 std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> ca;
                 std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ba; };
    Ctl& add (const juce::String& id);
    void setParam (const char* id, float plain);
    AcidProcessor& proc;
    juce::LookAndFeel_V4 lnf;
    StepGrid grid;
    juce::TextButton newBtn { "NEW LINE" }, mutBtn { "MUTATE" }, prevBtn { "<" }, nextBtn { ">" }, clearBtn { "CLEAR" }, fillBtn { "REGEN" };
    std::map<juce::String, Ctl> ctls;
    juce::Rectangle<int> seedArea, groupGen, groupPitch, groupPlay;
};
