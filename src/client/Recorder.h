#pragma once
#include "core/Project.h"
#include "core/Segments.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
namespace ensayo
{
// Samples to drop from the start of a take so its first kept sample is what was played over the record point:
// count-in + output latency + input latency + calibration (never negative).
int64_t recordingOffset (int64_t countInSamples, int inputLatency, int outputLatency, int calibration) noexcept;

class Recorder
{
public:
    Recorder();
    ~Recorder();
    // Message thread. channels 1 or 2; maxBlockSize sizes nothing on the audio thread: the FIFO holds 8 s.
    // localWav receives the full take as 32-bit float (crash-safe copy, flushed every segment).
    juce::Result start (const Id& take, int channels, const juce::File& localWav);
    void push (const float* const* channels, int n) noexcept;      // audio thread; drops and counts on overflow
    void stop();                                                    // message thread; flushes, joins, fires onFinished
    bool isRecording() const noexcept;
    uint32_t overflows() const noexcept;
    int64_t samplesCaptured() const noexcept;

    // Caller contract: push() must not race start() or stop(). The caller stops feeding (the engine stops capture in
    // its callback) before stop(); if it does not, samplesCaptured() can exceed the finished sample count by at most
    // one block. A local WAV failure is reported once through onError but the take continues (the WAV is only a backup);
    // a segment encoding failure stops the recording (further pushes are dropped) and fires onFinished.

    // Test hooks (no effect unless called): pause the writer; force WAV writes / segment encoding to fail.
    void setFailWavWritesForTest (bool fail) noexcept;
    void setFailEncodeForTest (bool fail) noexcept;

    // Test hook: while true the writer thread does not drain the FIFO (ignored once stop() was called).
    void setWriterPausedForTest (bool paused) noexcept;

    // Writer thread callbacks:
    std::function<void (int index, const juce::MemoryBlock& flac, int samples)> onSegment;
    std::function<void (int64_t fromPeak, const std::vector<Peak>&)> onPeaks;          // about every 100 ms
    std::function<void (const float* const* channels, int numChannels, int n)> onLiveAudio;   // every chunk drained
    std::function<void (int segments, int64_t samples)> onFinished;
    std::function<void (const juce::String&)> onError;                                  // write failure; recording stops

private:
    struct Writer;
    void writerLoop();

    std::unique_ptr<Writer> writer;
    std::thread thread;
    std::atomic<bool> capturing { false }, stopRequested { false }, paused { false }, failWav { false }, failEncode { false };
    std::atomic<uint32_t> overflowCount { 0 };
    std::atomic<int64_t> captured { 0 };
    juce::WaitableEvent wake;
    int numChannels = 1;
    std::unique_ptr<juce::AbstractFifo> fifo;
    std::vector<float> ring;            // numChannels x ringSize
    int ringSize = 0;
    juce::File wavFile;
};
}
