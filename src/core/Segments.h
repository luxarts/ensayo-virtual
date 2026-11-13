#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
namespace ensayo
{
constexpr int segmentSamples = 48000;
constexpr int peakBlock = 256;
struct Peak { float min = 0.0f, max = 0.0f; };

// FLAC, 24 bit, 48 kHz, audio.getNumChannels() channels (1 or 2), the first numSamples samples.
// Samples are clamped to [-1, 1). Returns an empty block on failure.
juce::MemoryBlock encodeSegment (const juce::AudioBuffer<float>& audio, int numSamples);
// Resizes `out` to channels × samples. False on anything that is not a 48 kHz FLAC of 1–2 channels.
bool decodeSegment (const void* data, size_t size, juce::AudioBuffer<float>& out);
// Appends one Peak per peakBlock samples (the last may cover fewer), min/max across all channels.
void appendPeaks (const juce::AudioBuffer<float>& audio, int numSamples, std::vector<Peak>& out);
// What a sample becomes after a round trip through 24-bit FLAC (for exact comparisons in tests).
float quantize24 (float sample) noexcept;
}
