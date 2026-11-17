#include "PluginEditor.h"

namespace { const juce::Colour bg (0xff14110d), panel (0xff1f1a14), line (0xff3a3127), txt (0xfff1e3c8), dim (0xff8d7d63), hot (0xffffb02e), acc (0xffff5a36), sld (0xff53c6b8); }

AcidEditor::AcidEditor (AcidProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    getLookAndFeel().setColour (juce::Slider::rotarySliderFillColourId, hot);
    getLookAndFeel().setColour (juce::Slider::rotarySliderOutlineColourId, line);
    getLookAndFeel().setColour (juce::Slider::thumbColourId, txt);
    getLookAndFeel().setColour (juce::Slider::textBoxTextColourId, txt);
    getLookAndFeel().setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    getLookAndFeel().setColour (juce::ComboBox::backgroundColourId, panel);
    getLookAndFeel().setColour (juce::ComboBox::outlineColourId, line);
    getLookAndFeel().setColour (juce::ComboBox::textColourId, txt);
    getLookAndFeel().setColour (juce::TextButton::buttonColourId, panel);
    getLookAndFeel().setColour (juce::TextButton::textColourOffId, txt);
    getLookAndFeel().setColour (juce::PopupMenu::backgroundColourId, panel);
    getLookAndFeel().setColour (juce::ToggleButton::textColourId, txt);
    getLookAndFeel().setColour (juce::ToggleButton::tickColourId, hot);

    for (auto* b : { &newBtn, &mutBtn, &prevBtn, &nextBtn }) addAndMakeVisible (*b);
    newBtn.onClick = [this] { setParam ("seed", (float) juce::Random::getSystemRandom().nextInt (64)); };
    prevBtn.onClick = [this] { setParam ("seed", (float) ((proc.settings().seed + 63) % 64)); };
    nextBtn.onClick = [this] { setParam ("seed", (float) ((proc.settings().seed + 1) % 64)); };
    mutBtn.onClick = [this] { auto m = proc.settings().mutate; setParam ("mutate", m >= 0.95f ? 0.0f : juce::jmin (1.0f, m + 0.15f)); };

    for (auto* rp : proc.getParameters())
    {
        auto* pr = dynamic_cast<juce::RangedAudioParameter*> (rp);
        if (pr == nullptr) continue;
        Ctl c;
        const auto id = pr->getParameterID();
        if (dynamic_cast<juce::AudioParameterChoice*> (pr) != nullptr)
        {
            auto* cb = new juce::ComboBox();
            cb->addItemList (dynamic_cast<juce::AudioParameterChoice*> (pr)->choices, 1);
            c.c.reset (cb); c.ca = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id, *cb);
        }
        else if (dynamic_cast<juce::AudioParameterBool*> (pr) != nullptr)
        {
            auto* tb = new juce::ToggleButton (pr->getName (32));
            c.c.reset (tb); c.ba = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, id, *tb);
        }
        else
        {
            auto* s = new juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::TextBoxBelow);
            s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 16);
            c.c.reset (s); c.sa = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, *s);
        }
        c.l = std::make_unique<juce::Label> ("", pr->getName (16).toUpperCase());
        c.l->setJustificationType (juce::Justification::centred);
        c.l->setColour (juce::Label::textColourId, dim);
        c.l->setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (*c.c); addAndMakeVisible (*c.l);
        ctls.push_back (std::move (c));
    }
    setSize (760, 420);
    startTimerHz (30);
}

void AcidEditor::setParam (const char* id, float plain)
{
    if (auto* p = proc.apvts.getParameter (id))
    { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (plain)); p->endChangeGesture(); }
    repaint();
}

void AcidEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (hot); g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("SYRUP ACID", 14, 8, 200, 22, juce::Justification::centredLeft);
    const auto st = proc.settings();
    g.setColour (txt); g.setFont (juce::FontOptions (13.0f));
    g.drawText ("SEED " + juce::String (st.seed), 200, 8, 120, 22, juce::Justification::centredLeft);

    const int len = proc.length(), root = proc.rootNote(), cur = proc.playStep.load();
    const int cols = juce::jmin (16, len), rows = len > 16 ? 2 : 1;
    const float cw = strip.getWidth() / (float) cols, rh = strip.getHeight() / (float) rows;
    static const char* nn[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < len; ++i)
    {
        const auto s = acid::makeStep (i, st);
        auto r = juce::Rectangle<float> (strip.getX() + (i % cols) * cw, strip.getY() + (i / cols) * rh, cw, rh).reduced (2);
        g.setColour (i == cur ? panel.brighter (0.25f) : panel); g.fillRect (r);
        g.setColour (i % 4 == 0 ? dim : line); g.drawRect (r, i == cur ? 2.0f : 1.0f);
        if (i == cur) { g.setColour (hot); g.drawRect (r, 2.0f); }
        if (! s.on) { g.setColour (line); g.drawText ("-", r, juce::Justification::centred); continue; }
        const int note = root + s.semis;
        const float h = juce::jmap ((float) s.semis, -12.0f, 27.0f, 4.0f, r.getHeight() - 34.0f);
        g.setColour (s.accent ? acc : hot);
        g.fillRect (r.withTrimmedTop (r.getHeight() - 18 - h).withTrimmedBottom (18).reduced (r.getWidth() * 0.22f, 0));
        if (s.slide) { g.setColour (sld); g.fillRect (r.getRight() - 6, r.getBottom() - 22 - h * 0.5f, 8.0f, 3.0f); }
        g.setColour (txt); g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (nn[note % 12]) + juce::String (note / 12 - 1), r.removeFromBottom (16), juce::Justification::centred);
    }
}

void AcidEditor::resized()
{
    auto a = getLocalBounds().reduced (10);
    auto top = a.removeFromTop (26);
    top.removeFromLeft (330);
    for (auto* b : { &prevBtn, &nextBtn }) b->setBounds (top.removeFromLeft (34).reduced (2, 0));
    newBtn.setBounds (top.removeFromLeft (80).reduced (2, 0));
    mutBtn.setBounds (top.removeFromLeft (90).reduced (2, 0));
    a.removeFromTop (6);
    strip = a.removeFromTop (150);
    a.removeFromTop (8);
    const int perRow = 8, cw = a.getWidth() / perRow, rh = a.getHeight() / 2;
    for (size_t i = 0; i < ctls.size(); ++i)
    {
        auto r = juce::Rectangle<int> (a.getX() + (int) (i % perRow) * cw, a.getY() + (int) (i / perRow) * rh, cw, rh).reduced (4);
        ctls[i].l->setBounds (r.removeFromTop (14));
        if (dynamic_cast<juce::Slider*> (ctls[i].c.get())) ctls[i].c->setBounds (r);
        else ctls[i].c->setBounds (r.withSizeKeepingCentre (r.getWidth(), 24));
    }
}
