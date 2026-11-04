#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
namespace ensayo
{
// Peak meter with a slow fall. Vertical when taller than wide, horizontal otherwise.
class LevelMeter : public juce::Component
{
public:
    void setLevel (float peak)
    {
        const float next = juce::jmax (juce::jlimit (0.0f, 2.0f, peak), level * 0.85f);

        if (std::abs (next - level) > 0.001f)
        {
            level = next;
            repaint();
        }
    }

    float getLevel() const noexcept { return level; }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff101418));
        g.fillRoundedRectangle (bounds, 2.0f);

        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        const float fraction = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (level >= 0.99f ? juce::Colours::red : (db > -6.0f ? juce::Colours::orange : juce::Colour (0xff4caf50)));

        auto inner = bounds.reduced (1.0f);

        if (getHeight() >= getWidth())
            g.fillRoundedRectangle (inner.removeFromBottom (inner.getHeight() * fraction), 2.0f);
        else
            g.fillRoundedRectangle (inner.removeFromLeft (inner.getWidth() * fraction), 2.0f);

        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawRoundedRectangle (bounds, 2.0f, 1.0f);
    }

private:
    float level = 0.0f;
};
}
