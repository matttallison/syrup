#include "PluginEditor.h"

namespace
{
const juce::Colour bg (0xff14110d), panel (0xff1f1a14), panel2 (0xff2a231b), line (0xff3a3127), txt (0xfff1e3c8),
                   dim (0xff8d7d63), hot (0xffffb02e), accC (0xffff5a36), sldC (0xff53c6b8), octC (0xff9b8cff);
const char* nn[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
constexpr int stripH = 18;
}

// ---------------- step grid
juce::Rectangle<float> StepGrid::cell (int i) const
{
    const int len = proc.length(), cols = juce::jmin (16, len), rows = len > 16 ? 2 : 1;
    const float cw = getWidth() / (float) cols, rh = getHeight() / (float) rows;
    return { (i % cols) * cw, (i / cols) * rh, cw, rh };
}

StepGrid::Hit StepGrid::hitTest (juce::Point<int> p) const
{
    for (int i = 0; i < proc.length(); ++i)
    {
        auto r = cell (i).reduced (2);
        if (! r.contains (p.toFloat())) continue;
        const float y = p.y - r.getY(), noteH = r.getHeight() - 3 * stripH;
        return { i, y < noteH ? 0 : 1 + juce::jmin (2, (int) ((y - noteH) / stripH)) };
    }
    return {};
}

void StepGrid::paint (juce::Graphics& g)
{
    const auto p = proc.getPattern();
    const int len = proc.length(), root = proc.rootNote(), cur = proc.playStep.load();
    int lo = 99, hi = -99;
    for (int i = 0; i < len; ++i) if (p[(size_t) i].on) { lo = juce::jmin (lo, p[(size_t) i].semis); hi = juce::jmax (hi, p[(size_t) i].semis); }
    if (lo > hi) { lo = 0; hi = 12; }
    if (hi - lo < 12) hi = lo + 12;

    for (int i = 0; i < len; ++i)
    {
        const auto& s = p[(size_t) i];
        auto r = cell (i).reduced (2);
        g.setColour (i % 4 == 0 ? panel2 : panel); g.fillRoundedRectangle (r, 3);
        if (i == cur) { g.setColour (hot); g.drawRoundedRectangle (r, 3, 2); }
        auto strips = r.removeFromBottom (3.0f * stripH);
        auto noteR = r.reduced (3);
        auto nameR = noteR.removeFromBottom (16);
        if (s.on)
        {
            const float t = (s.semis - lo) / (float) (hi - lo);
            const float barH = 6 + t * (noteR.getHeight() - 10);
            auto bar = noteR.withTrimmedTop (noteR.getHeight() - barH).reduced (noteR.getWidth() * 0.2f, 0);
            g.setColour (s.acc ? accC : hot); g.fillRoundedRectangle (bar, 2);
            if (s.slide) { g.setColour (sldC); g.fillRect (bar.getRight(), bar.getY(), cell (i).getWidth() * 0.3f, 3.0f); }
            const int note = root + s.semis;
            g.setColour (txt); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (juce::String (nn[((note % 12) + 12) % 12]) + juce::String (note / 12 - 1), nameR, juce::Justification::centred);
        }
        else { g.setColour (line); g.setFont (juce::FontOptions (14.0f)); g.drawText ("-", noteR, juce::Justification::centred); }

        const char* lab[3] = { "ACC", "SLD", "OCT" };
        const bool st3[3] = { s.acc, s.slide, s.semis >= 12 };
        const juce::Colour col[3] = { accC, sldC, octC };
        for (int k = 0; k < 3; ++k)
        {
            auto sr = strips.removeFromTop ((float) stripH).reduced (2, 1);
            g.setColour (st3[k] && s.on ? col[k] : bg); g.fillRoundedRectangle (sr, 2);
            g.setColour (st3[k] && s.on ? bg : dim); g.setFont (juce::FontOptions (9.5f));
            g.drawText (lab[k], sr, juce::Justification::centred);
        }
    }
}

void StepGrid::mouseDown (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.getPosition());
    dragged = false; dragStep = -1; paintMode = -1;
    if (h.step < 0) return;
    if (h.zone == 0)
    {
        dragStep = h.step; dragStartY = e.y;
        dragStartSemis = proc.getPattern()[(size_t) h.step].semis;
    }
    else
    {
        proc.editStep (h.step, [&] (AcidProcessor::PStep& s) {
            if (h.zone == 1) { s.acc = ! s.acc; paintMode = s.acc; }
            if (h.zone == 2) { s.slide = ! s.slide; paintMode = s.slide; }
            if (h.zone == 3) { s.semis += s.semis >= 12 ? -12 : 12; }
            s.on = true;
        });
    }
    repaint();
}

void StepGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStep >= 0)
    {
        const int d = (dragStartY - e.y) / 8;
        if (d != 0) dragged = true;
        if (dragged) proc.editStep (dragStep, [&] (AcidProcessor::PStep& s) { s.on = true; s.semis = juce::jlimit (-24, 36, dragStartSemis + d); });
        repaint(); return;
    }
    // drag across ACC / SLD strips to paint them
    const auto h = hitTest (e.getPosition());
    if (h.step >= 0 && paintMode >= 0 && (h.zone == 1 || h.zone == 2))
        proc.editStep (h.step, [&] (AcidProcessor::PStep& s) { if (s.on) { if (h.zone == 1) s.acc = paintMode; else s.slide = paintMode; } });
    repaint();
}

void StepGrid::mouseUp (const juce::MouseEvent&)
{
    if (dragStep >= 0 && ! dragged) proc.editStep (dragStep, [] (AcidProcessor::PStep& s) { s.on = ! s.on; if (! s.on) s.slide = false; });
    dragStep = -1; repaint();
}

// ---------------- editor
AcidEditor::AcidEditor (AcidProcessor& p) : AudioProcessorEditor (p), proc (p), grid (p)
{
    lnf.setColour (juce::Slider::rotarySliderFillColourId, hot);
    lnf.setColour (juce::Slider::rotarySliderOutlineColourId, line);
    lnf.setColour (juce::Slider::thumbColourId, txt);
    lnf.setColour (juce::Slider::textBoxTextColourId, txt);
    lnf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::ComboBox::backgroundColourId, bg);
    lnf.setColour (juce::ComboBox::outlineColourId, line);
    lnf.setColour (juce::ComboBox::textColourId, txt);
    lnf.setColour (juce::ComboBox::arrowColourId, hot);
    lnf.setColour (juce::TextButton::buttonColourId, panel2);
    lnf.setColour (juce::TextButton::textColourOffId, txt);
    lnf.setColour (juce::PopupMenu::backgroundColourId, panel);
    lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, hot);
    lnf.setColour (juce::ToggleButton::textColourId, txt);
    lnf.setColour (juce::ToggleButton::tickColourId, hot);
    setLookAndFeel (&lnf);

    addAndMakeVisible (grid);
    for (auto* b : { &newBtn, &mutBtn, &prevBtn, &nextBtn, &clearBtn, &fillBtn }) addAndMakeVisible (*b);
    newBtn.setColour (juce::TextButton::buttonColourId, hot); newBtn.setColour (juce::TextButton::textColourOffId, bg);
    newBtn.onClick = [this] { setParam ("seed", (float) juce::Random::getSystemRandom().nextInt (64)); };
    prevBtn.onClick = [this] { setParam ("seed", (float) ((proc.settings().seed + 63) % 64)); };
    nextBtn.onClick = [this] { setParam ("seed", (float) ((proc.settings().seed + 1) % 64)); };
    mutBtn.onClick = [this] { auto m = proc.settings().mutate; setParam ("mutate", m >= 0.95f ? 0.0f : juce::jmin (1.0f, m + 0.15f)); };
    fillBtn.onClick = [this] { proc.regenerate(); grid.repaint(); };
    clearBtn.onClick = [this] { for (int i = 0; i < AcidProcessor::maxSteps; ++i) proc.editStep (i, [] (AcidProcessor::PStep& s) { s = {}; }); grid.repaint(); };
    prevBtn.setTooltip ("Previous seed"); fillBtn.setTooltip ("Throw away your edits and rebuild the line from the knobs");

    for (auto id : { "mutate", "density", "accent", "slide", "jumps", "root", "scale", "octave", "length", "gate", "velNorm", "velAcc", "channel", "keyMode", "freeRun" })
        add (id);
    setSize (900, 520);
    startTimerHz (30);
}

AcidEditor::Ctl& AcidEditor::add (const juce::String& id)
{
    auto* pr = proc.apvts.getParameter (id);
    Ctl c;
    if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (pr))
    {
        auto* cb = new juce::ComboBox(); cb->addItemList (ch->choices, 1);
        c.c.reset (cb); c.ca = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id, *cb);
    }
    else if (dynamic_cast<juce::AudioParameterBool*> (pr) != nullptr)
    {
        auto* tb = new juce::ToggleButton ("FREE RUN");
        c.c.reset (tb); c.ba = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, id, *tb);
    }
    else
    {
        auto* s = new juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::TextBoxBelow);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 15);
        s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        c.c.reset (s); c.sa = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, *s);
        if (dynamic_cast<juce::AudioParameterFloat*> (pr) != nullptr)
            s->textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100)) + "%"; };
        s->updateText();
    }
    juce::String name = pr->getName (20).toUpperCase();
    if (id == "velNorm") name = "VELOCITY"; if (id == "velAcc") name = "ACCENT VEL"; if (id == "channel") name = "MIDI CH"; if (id == "keyMode") name = "KEYS";
    c.l = std::make_unique<juce::Label> ("", name);
    c.l->setJustificationType (juce::Justification::centred);
    c.l->setColour (juce::Label::textColourId, dim);
    c.l->setFont (juce::FontOptions (10.5f));
    addAndMakeVisible (*c.c);
    if (id != "freeRun") addAndMakeVisible (*c.l);
    return ctls[id] = std::move (c);
}

void AcidEditor::setParam (const char* id, float plain)
{
    if (auto* p = proc.apvts.getParameter (id))
    { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (plain)); p->endChangeGesture(); }
    proc.checkRegen(); grid.repaint(); repaint();
}

void AcidEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (hot); g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("SYRUP ACID", 16, 10, 200, 30, juce::Justification::centredLeft);
    g.setColour (panel); g.fillRoundedRectangle (seedArea.toFloat(), 4);
    g.setColour (dim); g.setFont (juce::FontOptions (10.0f));
    g.drawText ("SEED", seedArea.withHeight (12).translated (0, 2), juce::Justification::centred);
    g.setColour (txt); g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText (juce::String (proc.settings().seed), seedArea.withTrimmedTop (10), juce::Justification::centred);

    const std::pair<juce::Rectangle<int>, const char*> groups[] = { { groupGen, "GENERATE" }, { groupPitch, "PITCH" }, { groupPlay, "PLAY" } };
    for (auto& [r, name] : groups)
    {
        g.setColour (panel); g.fillRoundedRectangle (r.toFloat(), 5);
        g.setColour (hot); g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (name, r.withHeight (18).reduced (10, 0), juce::Justification::centredLeft);
    }
    g.setColour (dim); g.setFont (juce::FontOptions (10.0f));
    g.drawText ("Click a step: on/off.  Drag a note up/down: pitch.  ACC / SLD / OCT: tap, or drag across to paint.",
                grid.getBounds().withY (grid.getBottom() + 2).withHeight (14), juce::Justification::centredLeft);
}

void AcidEditor::resized()
{
    auto a = getLocalBounds().reduced (12);
    auto top = a.removeFromTop (34);
    top.removeFromLeft (190);
    prevBtn.setBounds (top.removeFromLeft (30).reduced (0, 3));
    seedArea = top.removeFromLeft (56).reduced (3, 0);
    nextBtn.setBounds (top.removeFromLeft (30).reduced (0, 3));
    top.removeFromLeft (8);
    newBtn.setBounds (top.removeFromLeft (100).reduced (2, 3));
    mutBtn.setBounds (top.removeFromLeft (90).reduced (2, 3));
    clearBtn.setBounds (top.removeFromRight (80).reduced (2, 3));
    fillBtn.setBounds (top.removeFromRight (80).reduced (2, 3));
    a.removeFromTop (8);
    grid.setBounds (a.removeFromTop (230));
    a.removeFromTop (20);

    auto row = a;
    groupGen = row.removeFromLeft (row.getWidth() * 5 / 12).reduced (3, 0);
    groupPitch = row.removeFromLeft (row.getWidth() * 3 / 7).reduced (3, 0);
    groupPlay = row.reduced (3, 0);

    auto knob = [this] (const char* id, juce::Rectangle<int> r)
    {
        auto& c = ctls[id];
        c.l->setBounds (r.removeFromTop (14));
        c.c->setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 76), juce::jmin (r.getHeight(), 82)));
    };
    auto combo = [this] (const char* id, juce::Rectangle<int> r)
    {
        auto& c = ctls[id];
        c.l->setBounds (r.removeFromTop (14));
        c.c->setBounds (r.removeFromTop (26).reduced (2, 0));
    };
    auto cols = [] (juce::Rectangle<int> r, int n, int i, int span = 1)
    {
        const int w = r.getWidth() / n;
        return juce::Rectangle<int> (r.getX() + i * w, r.getY(), w * span, r.getHeight()).reduced (3, 0);
    };

    auto g = groupGen.withTrimmedTop (22).reduced (6, 6);
    const char* gen[] = { "mutate", "density", "accent", "slide", "jumps" };
    for (int i = 0; i < 5; ++i) knob (gen[i], cols (g, 5, i).withSizeKeepingCentre (g.getWidth() / 5, 100));

    auto pch = groupPitch.withTrimmedTop (22).reduced (6, 6);
    auto pr1 = pch.removeFromTop (46);
    combo ("scale", pr1.reduced (3, 0));
    pch.removeFromTop (6);
    combo ("root", cols (pch, 2, 0).withHeight (46));
    knob ("octave", cols (pch, 2, 1).withHeight (juce::jmin (pch.getHeight(), 100)));

    auto pl = groupPlay.withTrimmedTop (22).reduced (6, 6);
    auto r1 = pl.removeFromTop (pl.getHeight() / 2), r2 = pl;
    combo ("length", cols (r1, 4, 0).withHeight (46));
    knob ("gate", cols (r1, 4, 1));
    knob ("velNorm", cols (r1, 4, 2));
    knob ("velAcc", cols (r1, 4, 3));
    combo ("keyMode", cols (r2, 4, 0, 2).withHeight (46));
    knob ("channel", cols (r2, 4, 2));
    ctls["freeRun"].c->setBounds (cols (r2, 4, 3).withSizeKeepingCentre (cols (r2, 4, 3).getWidth(), 30));
}
