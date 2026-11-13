#pragma once
#include "core/Render.h"
#include "client/TakeCache.h"
#include <functional>
namespace ensayo
{
// Decodes wav/flac/ogg/mp3 (JUCE) and m4a/mp4/aac (Media Foundation), keeps 1 or 2 channels (more → first two),
// resamples to 48 kHz with juce::WindowedSincInterpolator when needed (no anti-alias filter when downsampling, so
// content above 24 kHz in a 96/192 kHz source can alias). Fails on > 4 h.
juce::Result loadAndResample (const juce::File&, juce::AudioBuffer<float>& out);
// Cuts audio into segmentSamples-long FLAC segments (last one shorter).
std::vector<juce::MemoryBlock> makeSegments (const juce::AudioBuffer<float>& audio);
// Renders [0, project.endSample()) with a fresh Renderer (no solo, no metronome) in 4096-sample blocks and writes
// a 24-bit stereo WAV at 48 kHz with TPDF dither (±1 LSB triangular, juce::Random seeded 1). progress(0..1) may
// return false to cancel (file deleted).
// What the mix did before it was quantised (filled when the export succeeds). The audio is not changed: samples
// beyond full scale are hard clipped as always; this only lets the caller say so.
struct ExportInfo
{
    float peak = 0.0f;              // largest |sample| over both channels, linear (1.0 = 0 dBFS)
    int64_t clippedSamples = 0;     // samples (per channel) beyond full scale
    float peakDb() const noexcept   { return juce::Decibels::gainToDecibels (peak, -200.0f); }
};
juce::Result exportMix (std::shared_ptr<const Project>, const SampleSource&, const juce::File& wav,
                        std::function<bool (float)> progress = {}, ExportInfo* info = nullptr);
// Ok when every take used by a region is fully in `cache` (its sampleCount reaches the take's samples); otherwise
// fails with a Spanish message, because exportMix would write silence for the missing part.
juce::Result checkTakesCached (const Project&, const TakeCache&);
}
