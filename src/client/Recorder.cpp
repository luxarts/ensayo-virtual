#include "client/Recorder.h"

#include <algorithm>

namespace ensayo
{
namespace
{
constexpr int fifoSeconds = 8;
constexpr int rate = 48000;
constexpr int chunkSamples = 4096;
constexpr int writerPollMs = 20;
constexpr int peakPublishMs = 100;
}

int64_t recordingOffset (int64_t countInSamples, int inputLatency, int outputLatency, int calibration) noexcept
{
    return std::max<int64_t> (0, countInSamples + inputLatency + outputLatency + calibration);
}

// State owned by the writer thread once it is running.
struct Recorder::Writer
{
    std::unique_ptr<juce::AudioFormatWriter> wav;
    juce::AudioBuffer<float> chunk, segment;
    int segmentFill = 0, segmentsCut = 0;
    int64_t samples = 0;                  // samples fully handled (WAV written, accumulated)
    std::vector<Peak> peaks;
    int64_t peaksSentFrom = 0;
    bool peaksDirty = false;
};

Recorder::Recorder() = default;

Recorder::~Recorder() { stop(); }

juce::Result Recorder::start (const Id&, int channels, const juce::File& localWav)
{
    if (capturing.load())
        return juce::Result::fail ("already recording");

    if (thread.joinable())
        thread.join();

    if (channels < 1 || channels > 2)
        return juce::Result::fail ("channels must be 1 or 2");

    localWav.getParentDirectory().createDirectory();
    localWav.deleteFile();                       // FileOutputStream appends to an existing file

    auto file = std::make_unique<juce::FileOutputStream> (localWav);

    if (file->failedToOpen())
        return juce::Result::fail ("cannot open " + localWav.getFullPathName() + ": " + file->getStatus().getErrorMessage());

    std::unique_ptr<juce::OutputStream> stream = std::move (file);
    auto wav = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                   .withSampleRate ((double) rate)
                                                                   .withNumChannels (channels)
                                                                   .withBitsPerSample (32));

    if (wav == nullptr)
        return juce::Result::fail ("cannot create the WAV writer");

    auto w = std::make_unique<Writer>();
    w->wav = std::move (wav);
    w->chunk.setSize (channels, chunkSamples);
    w->segment.setSize (channels, segmentSamples);
    w->peaks.reserve ((size_t) (fifoSeconds * rate / peakBlock) * 4);
    writer = std::move (w);

    numChannels = channels;
    ringSize = fifoSeconds * rate + 1;     // AbstractFifo keeps one slot free
    ring.assign ((size_t) ringSize * (size_t) channels, 0.0f);
    fifo = std::make_unique<juce::AbstractFifo> (ringSize);
    wavFile = localWav;
    overflowCount = 0;
    captured = 0;
    dropped = 0;
    gapFifo = std::make_unique<juce::AbstractFifo> (maxGaps + 1);
    gapPosition.assign (maxGaps + 1, 0);
    gapLength.assign (maxGaps + 1, 0);
    fifoWritten = 0;
    openGap = 0;
    stopRequested = false;
    wake.reset();
    thread = std::thread ([this] { writerLoop(); });
    capturing.store (true, std::memory_order_release);
    return juce::Result::ok();
}

void Recorder::push (const float* const* channels, int n) noexcept
{
    if (! capturing.load (std::memory_order_acquire) || n <= 0)
        return;

    // A full FIFO, or a gap that cannot be published (more than maxGaps pending): the block is dropped and becomes
    // silence, never shifting what follows.
    if (fifo->getFreeSpace() < n || (openGap > 0 && gapFifo->getFreeSpace() < 1))
    {
        overflowCount.fetch_add (1, std::memory_order_relaxed);
        dropped.fetch_add (n, std::memory_order_relaxed);
        openGap += n;
        return;
    }

    if (openGap > 0)
        publishGap();

    int s1, n1, s2, n2;
    fifo->prepareToWrite (n, s1, n1, s2, n2);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* dst = ring.data() + (size_t) ch * (size_t) ringSize;
        const float* src = channels[ch];
        std::copy (src, src + n1, dst + s1);

        if (n2 > 0)
            std::copy (src + n1, src + n1 + n2, dst + s2);
    }

    fifo->finishedWrite (n1 + n2);
    fifoWritten += n;
    captured.fetch_add (n, std::memory_order_relaxed);
    wake.signal();
}

void Recorder::publishGap() noexcept
{
    int s1, n1, s2, n2;
    gapFifo->prepareToWrite (1, s1, n1, s2, n2);

    if (n1 + n2 < 1)
        return;

    const int slot = n1 > 0 ? s1 : s2;
    gapPosition[(size_t) slot] = fifoWritten;
    gapLength[(size_t) slot] = openGap;
    gapFifo->finishedWrite (1);
    openGap = 0;
}

void Recorder::stop()
{
    if (! thread.joinable())
        return;

    capturing.store (false, std::memory_order_release);

    // Samples dropped at the very end: the push side is quiet now (caller contract), so publish them from here and
    // the take still ends at the device's last sample.
    if (openGap > 0 && gapFifo->getFreeSpace() > 0)
        publishGap();

    stopRequested.store (true, std::memory_order_release);
    wake.signal();
    thread.join();
}

bool Recorder::isRecording() const noexcept { return capturing.load (std::memory_order_acquire); }
uint32_t Recorder::overflows() const noexcept { return overflowCount.load (std::memory_order_relaxed); }
int64_t Recorder::samplesDropped() const noexcept { return dropped.load (std::memory_order_relaxed); }
int64_t Recorder::samplesCaptured() const noexcept { return captured.load (std::memory_order_relaxed); }

void Recorder::setWriterPausedForTest (bool p) noexcept
{
    paused.store (p);

    if (! p)
        wake.signal();
}

void Recorder::setFailWavWritesForTest (bool fail) noexcept { failWav.store (fail); }
void Recorder::setFailEncodeForTest (bool fail) noexcept { failEncode.store (fail); }

void Recorder::writerLoop()
{
    auto& w = *writer;
    auto lastPublish = juce::Time::getMillisecondCounter();
    int64_t emittedSamples = 0;
    bool encodeFailed = false, wavFailed = false;

    // The WAV is only a backup copy: a failure is reported once, the file is closed and the take carries on.
    auto failWavCopy = [&] (const juce::String& what)
    {
        if (wavFailed)
            return;

        wavFailed = true;
        w.wav.reset();

        if (onError)
            onError ("The local backup copy failed (" + what + " " + wavFile.getFullPathName() + "); the take is still being uploaded");
    };

    // The segments are what gets uploaded: if one cannot be encoded the recording stops.
    auto failEncoding = [&]
    {
        encodeFailed = true;
        capturing.store (false, std::memory_order_release);   // further pushes are dropped
        w.segmentFill = 0;                                    // the unencodable samples are discarded

        if (onError)
            onError ("Segment encoding failed; the recording was stopped");
    };

    auto cutSegment = [&] (int count)
    {
        if (count <= 0 || encodeFailed)
            return;

        const auto flac = failEncode.load() ? juce::MemoryBlock() : encodeSegment (w.segment, count);

        if (flac.getSize() == 0)
        {
            failEncoding();
            return;
        }

        if (onSegment)
            onSegment (w.segmentsCut, flac, count);

        ++w.segmentsCut;
        emittedSamples += count;
        w.segmentFill = 0;

        if (w.wav != nullptr && (failWav.load() || ! w.wav->flush()))
            failWavCopy ("cannot flush");
    };

    auto addPeaks = [&] (int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto index = (size_t) ((w.samples + i) / peakBlock);

            if (index >= w.peaks.size())
                w.peaks.push_back ({ w.chunk.getSample (0, i), w.chunk.getSample (0, i) });

            auto& p = w.peaks[index];

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto v = w.chunk.getSample (ch, i);
                p.min = std::min (p.min, v);
                p.max = std::max (p.max, v);
            }
        }

        w.peaksDirty = w.peaksDirty || n > 0;
    };

    auto publishPeaks = [&]
    {
        if (! w.peaksDirty)
            return;

        w.peaksDirty = false;
        const auto from = (size_t) w.peaksSentFrom;

        if (from < w.peaks.size() && onPeaks)
            onPeaks ((int64_t) from, std::vector<Peak> (w.peaks.begin() + (std::ptrdiff_t) from, w.peaks.end()));

        // Complete windows are final; the trailing partial one is re-sent when it grows.
        w.peaksSentFrom = w.samples / peakBlock;
    };

    // Runs w.chunk[0, ready) through the WAV, the live preview, the peaks and the segmenter.
    auto processChunk = [&] (int ready) -> bool
    {
        if (w.wav != nullptr && (failWav.load() || ! w.wav->writeFromAudioSampleBuffer (w.chunk, 0, ready)))
            failWavCopy ("cannot write");

        if (onLiveAudio)
            onLiveAudio (w.chunk.getArrayOfReadPointers(), numChannels, ready);

        addPeaks (ready);

        for (int done = 0; done < ready && ! encodeFailed;)
        {
            const int take = std::min (ready - done, segmentSamples - w.segmentFill);

            for (int ch = 0; ch < numChannels; ++ch)
                w.segment.copyFrom (ch, w.segmentFill, w.chunk, ch, done, take);

            w.segmentFill += take;
            done += take;

            if (w.segmentFill == segmentSamples)
                cutSegment (segmentSamples);
        }

        w.samples += ready;
        return ! encodeFailed;
    };

    int64_t fifoRead = 0;                 // samples read from the FIFO
    int64_t nextGap = -1, nextGapLength = 0, silenceLeft = 0;

    // Moves one chunk (from the FIFO, or silence for an overflow gap) through the WAV, the segmenter, the peaks and
    // the live preview.
    auto drainChunk = [&] () -> bool
    {
        // Read the FIFO count before the gaps: a gap is published before the samples that follow it, so every gap
        // inside what is ready here is visible below.
        const int available = fifo->getNumReady();

        if (nextGap < 0 && silenceLeft == 0 && gapFifo->getNumReady() > 0)
        {
            int s1, n1, s2, n2;
            gapFifo->prepareToRead (1, s1, n1, s2, n2);
            const int slot = n1 > 0 ? s1 : s2;
            nextGap = gapPosition[(size_t) slot];
            nextGapLength = gapLength[(size_t) slot];
            gapFifo->finishedRead (1);
        }

        if (nextGap >= 0 && nextGap == fifoRead)
        {
            silenceLeft = nextGapLength;
            nextGap = -1;

            if (onError)
                onError (juce::String (juce::CharPointer_UTF8 ("Se perdieron "))
                         + juce::String ((double) silenceLeft * 1000.0 / rate, 0)
                         + juce::String (juce::CharPointer_UTF8 (" ms de audio porque la grabaci\xc3\xb3n no dio abasto; "
                                                                 "se rellenaron con silencio y la toma sigue alineada")));
        }

        if (silenceLeft > 0)
        {
            const int n = (int) std::min<int64_t> (silenceLeft, chunkSamples);
            w.chunk.clear();
            silenceLeft -= n;
            return processChunk (n);
        }

        int ready = std::min (available, chunkSamples);

        if (nextGap >= 0)
            ready = (int) std::min<int64_t> (ready, nextGap - fifoRead);

        if (ready <= 0)
            return false;

        int s1, n1, s2, n2;
        fifo->prepareToRead (ready, s1, n1, s2, n2);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* src = ring.data() + (size_t) ch * (size_t) ringSize;
            auto* dst = w.chunk.getWritePointer (ch);
            std::copy (src + s1, src + s1 + n1, dst);

            if (n2 > 0)
                std::copy (src + s2, src + s2 + n2, dst + n1);
        }

        fifo->finishedRead (n1 + n2);
        fifoRead += ready;
        return processChunk (ready);
    };

    for (;;)
    {
        wake.wait (writerPollMs);
        const bool stopping = stopRequested.load (std::memory_order_acquire);   // read before draining: that drain is final

        if (stopping || ! paused.load())
            while (drainChunk()) {}

        if (encodeFailed)
            break;

        const auto now = juce::Time::getMillisecondCounter();

        if (stopping || now - lastPublish >= (juce::uint32) peakPublishMs)
        {
            publishPeaks();
            lastPublish = now;
        }

        if (stopping)
            break;
    }

    cutSegment (w.segmentFill);              // the partial last segment; never retried after an encode failure
    publishPeaks();
    w.wav.reset();                           // closes the file and finalises the header

    if (onFinished)
        onFinished (w.segmentsCut, emittedSamples);
}
}
