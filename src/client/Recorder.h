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
    // Audio thread. When the FIFO is full the block is dropped and counted; the writer later puts exactly that many
    // samples of silence in its place, so the take keeps every device sample position (alignment) and onError says
    // how much was lost. The take goes on.
    void push (const float* const* channels, int n) noexcept;
    void stop();                                                    // message thread; flushes, joins, fires onFinished
    bool isRecording() const noexcept;
    uint32_t overflows() const noexcept;
    int64_t samplesCaptured() const noexcept;    // samples pushed into the FIFO (dropped ones excluded)
    int64_t samplesDropped() const noexcept;     // samples dropped on overflow (written as silence)

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
    // Writer thread: a local WAV failure (the take goes on), a segment encode failure (the recording stops) or an
    // overflow filled with silence (the take goes on).
    std::function<void (const juce::String&)> onError;

private:
    struct Writer;
    void writerLoop();

    std::unique_ptr<Writer> writer;
    std::thread thread;
    std::atomic<bool> capturing { false }, stopRequested { false }, paused { false }, failWav { false }, failEncode { false };
    std::atomic<uint32_t> overflowCount { 0 };
    std::atomic<int64_t> captured { 0 }, dropped { 0 };
    juce::WaitableEvent wake;
    int numChannels = 1;
    std::unique_ptr<juce::AbstractFifo> fifo;
    std::vector<float> ring;            // numChannels x ringSize
    int ringSize = 0;
    juce::File wavFile;

    // Overflow gaps: the audio thread accumulates dropped samples in openGap and publishes {position in the FIFO
    // stream, length} right before the next block it does write, so the writer sees a gap before the samples after it.
    static constexpr int maxGaps = 256;
    std::unique_ptr<juce::AbstractFifo> gapFifo;
    std::vector<int64_t> gapPosition, gapLength;
    int64_t fifoWritten = 0;            // audio thread: samples written into the FIFO
    int64_t openGap = 0;                // audio thread: samples dropped since the last write
    void publishGap() noexcept;
};
}
