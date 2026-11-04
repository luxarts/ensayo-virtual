#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
namespace ensayo
{
float dbToGain (float db) noexcept;                 // clamps to [minFaderDb, maxFaderDb]; -60 → 0 exactly
struct ChannelGain { float voiceDb = 0.0f, instrumentDb = 0.0f; bool muted = false;
                     float voiceGain() const noexcept; float instrumentGain() const noexcept; };  // 0 when muted
// out[L/R] += gain * mono (centred). Real-time safe.
void addMonoCentred (float* L, float* R, const float* mono, float gain, int n) noexcept;
void addStereo (float* L, float* R, const float* inL, const float* inR, float gain, int n) noexcept;
float peakOf (const float* data, int n) noexcept;
void interleave (const float* L, const float* R, float* out, int n) noexcept;
void deinterleave (const float* in, float* L, float* R, int n) noexcept;
}
