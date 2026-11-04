#pragma once
#include "core/Protocol.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
namespace ensayo
{
class BackingTrack
{
public:
    static constexpr int64_t maxLengthSamples = 30LL * 60 * sampleRate;
    BackingTrack();
    // Worker/message thread. Decodes to 48 kHz stereo float (LagrangeInterpolator when needed), then swaps in atomically.
    juce::Result load (const juce::File&);
    void unload();
    void play() noexcept; void pause() noexcept; void stop() noexcept;   // stop = pause + seek 0
    void seek (int64_t samples) noexcept; void setLoop (bool) noexcept;
    TransportState getState() const noexcept;
    // Extra (not in the plan contract): lock-free position/playing reads for the audio thread.
    int64_t getPosition() const noexcept { return position.load (std::memory_order_relaxed); }
    bool isPlaying() const noexcept { return playing.load (std::memory_order_relaxed); }
    // Audio thread. Adds nothing when not playing. Advances position. At the end: loop or pause at length.
    void render (float* L, float* R, int numSamples) noexcept;
private:
    juce::AudioFormatManager formats; // registerBasicFormats

    struct Audio
    {
        juce::AudioBuffer<float> buffer;   // 2 channels @ 48 kHz
        juce::String name;
    };

    void swapIn (std::shared_ptr<const Audio> newAudio);

    // The audio thread try-locks `lock` and keeps a raw pointer to the current Audio while rendering;
    // control threads replace `audio` only while holding `lock`, so the pointer stays valid during a render.
    mutable juce::SpinLock lock;
    std::shared_ptr<const Audio> audio;

    std::atomic<int64_t> position { 0 };
    std::atomic<int64_t> length { 0 };
    std::atomic<bool> playing { false }, loop { false }, loaded { false };

    JUCE_DECLARE_NON_COPYABLE (BackingTrack)
};
}
