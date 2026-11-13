#pragma once
#include "core/Render.h"
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
juce::Result exportMix (std::shared_ptr<const Project>, const SampleSource&, const juce::File& wav,
                        std::function<bool (float)> progress = {});
}
