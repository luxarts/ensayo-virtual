#pragma once
#include "core/Protocol.h"
#include <juce_core/juce_core.h>
namespace ensayo
{
// Every non-ASCII UI literal goes through here so the source encoding never leaks into the String.
inline juce::String ES (const char* utf8) { return juce::String (juce::CharPointer_UTF8 (utf8)); }

inline juce::String formatTime (int64_t samples)
{
    const auto totalSeconds = (int) (juce::jmax ((int64_t) 0, samples) / sampleRate);
    return juce::String::formatted ("%d:%02d", totalSeconds / 60, totalSeconds % 60);
}

inline juce::String roleToSpanish (Role role)
{
    switch (role)
    {
        case Role::voice:      return "Voz";
        case Role::instrument: return "Instrumento";
        case Role::both:       break;
    }

    return "Instrumento + Voz";
}
}
