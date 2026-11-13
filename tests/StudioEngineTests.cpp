#include "client/StudioEngine.h"
#include "client/TakeCache.h"
#include "core/Segments.h"
#include "MessagePump.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

namespace ensayo
{
namespace
{
juce::File studioTestDir()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EnsayoVirtual/tests/studioengine");
}

float sineAt (int64_t k) noexcept
{
    return (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) k / 48000.0));
}

// A 440 Hz sine (amplitude 0.5) for every known mono take, sample k = sineAt (k).
class SineTakes : public SampleSource
{
public:
    void add (const Id& take, int64_t length) { takes[take] = length; }

    int channels (const Id& take) const noexcept override { return takes.count (take) != 0 ? 1 : 0; }

    void read (const Id& take, int, int64_t offset, int n, float* dest) const noexcept override
    {
        const auto it = takes.find (take);

        for (int i = 0; i < n; ++i)
        {
            const auto k = offset + i;
            dest[i] = (it != takes.end() && k >= 0 && k < it->second) ? sineAt (k) : 0.0f;
        }
    }

private:
    std::map<Id, int64_t> takes;
};

float maxStepOf (const std::vector<float>& x)
{
    float m = 0.0f;

    for (size_t i = 1; i < x.size(); ++i)
        m = std::max (m, std::abs (x[i] - x[i - 1]));

    return m;
}

float noClickBound()
{
    std::vector<float> pure (48000);

    for (size_t i = 0; i < pure.size(); ++i)
        pure[i] = sineAt ((int64_t) i);

    return maxStepOf (pure) + 1.0e-3f;
}

// Everything the recorder reports, gathered on the writer thread.
struct Collected
{
    std::mutex lock;
    std::vector<juce::MemoryBlock> segments;
    std::vector<int> segmentSamples;
    std::vector<std::vector<uint8_t>> previewPackets;
    std::atomic<bool> finished { false };
    std::atomic<int> errors { 0 };

    void attach (StudioEngine& engine)
    {
        auto& r = engine.getRecorder();
        r.onSegment = [this] (int index, const juce::MemoryBlock& flac, int samples)
        {
            std::lock_guard<std::mutex> g (lock);
            if ((int) segments.size() != index) return;
            segments.push_back (flac);
            segmentSamples.push_back (samples);
        };
        r.onFinished = [this] (int, int64_t) { finished.store (true); };
        r.onError = [this] (const juce::String&) { ++errors; };
        engine.onPreviewPacket = [this] (const uint8_t* packet, size_t len)
        {
            std::lock_guard<std::mutex> g (lock);
            previewPackets.emplace_back (packet, packet + len);
        };
    }

    // Channel 0 of the whole take.
    std::vector<float> decodeTake()
    {
        std::lock_guard<std::mutex> g (lock);
        std::vector<float> out;

        for (const auto& s : segments)
        {
            juce::AudioBuffer<float> audio;

            if (! decodeSegment (s.getData(), s.getSize(), audio))
                return {};

            out.insert (out.end(), audio.getReadPointer (0), audio.getReadPointer (0) + audio.getNumSamples());
        }

        return out;
    }

    int64_t segmentSampleSum()
    {
        std::lock_guard<std::mutex> g (lock);
        int64_t sum = 0;

        for (auto s : segmentSamples)
            sum += s;

        return sum;
    }
};

// Captures the fake device's output L/R (device thread) into preallocated storage.
struct OutputCapture
{
    explicit OutputCapture (size_t capacity) { L.reserve (capacity); R.reserve (capacity); }

    void attach (FakeAudioDevice& device)
    {
        device.onOutputBlock = [this] (const float* l, const float* r, int n, int64_t, double)
        {
            std::lock_guard<std::mutex> g (lock);

            for (int i = 0; i < n && L.size() < L.capacity(); ++i)
            {
                L.push_back (l[i]);
                R.push_back (r[i]);
            }
        };
    }

    std::mutex lock;
    std::vector<float> L, R;
};

bool waitFor (const std::function<bool()>& condition, int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

    while (! condition())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;

        pumpMessages (5);
    }

    return true;
}

// A project with an imported click track (a click every 24000 samples from sample 0) and an empty voice track.
struct ClickProject
{
    static constexpr int clickPeriod = 24000, numSegments = 6;
    Id host = newId(), clickTake = newId(), clickTrack = newId(), voiceTrack = newId();
    Project project = makeProject ("Song", host, "Lucas");
    TakeCache cache;
    bool ok = true;

    explicit ClickProject (const juce::File& dir) : cache (dir)
    {
        juce::AudioBuffer<float> segment (1, segmentSamples);

        for (int s = 0; s < numSegments && ok; ++s)
        {
            segment.clear();

            for (int i = 0; i < segmentSamples; i += clickPeriod)
                segment.setSample (0, i, 1.0f);

            const auto flac = encodeSegment (segment, segmentSamples);
            ok = cache.appendSegment (clickTake, s, flac.getData(), flac.getSize());
        }

        const int64_t length = (int64_t) numSegments * segmentSamples;
        ok = ok
          && apply (Ops::addTake (clickTake, 1))
          && apply (Ops::updateTake (clickTake, numSegments, length, true))
          && apply (Ops::addTrack (clickTrack, "Clicks", TrackKind::imported))
          && apply (Ops::addRegion (clickTrack, { newId(), clickTake, 0, 0, length }))
          && apply (Ops::addTrack (voiceTrack, "Voice", TrackKind::voice));
    }

    bool apply (const juce::var& op) { return applyOp (project, op, host).ok; }
    std::shared_ptr<const Project> snapshot() const { return std::make_shared<const Project> (project); }
    const Track& voice() const { return *project.findTrack (voiceTrack); }
};

// Positions (take samples) of clicks: a sample above 0.3 after 20 ms of quiet.
std::vector<int64_t> detectClicks (const std::vector<float>& x)
{
    std::vector<int64_t> found;
    int64_t quiet = 960;

    for (size_t i = 0; i < x.size(); ++i)
    {
        if (std::abs (x[i]) > 0.3f)
        {
            if (quiet >= 960)
                found.push_back ((int64_t) i);

            quiet = 0;
        }
        else
        {
            ++quiet;
        }
    }

    return found;
}
}

class StudioEngineTests : public juce::UnitTest
{
public:
    StudioEngineTests() : juce::UnitTest ("StudioEngine", "EnsayoVirtual") {}

    // Loopback alignment: records the click track coming back through the fake device and checks every recorded click
    // sits where the timeline put it.
    void alignment (int blockSize, FakeLatency latency)
    {
        const auto dir = studioTestDir().getChildFile ("align" + juce::String (blockSize));
        dir.deleteRecursively();
        ClickProject p (dir.getChildFile ("cache"));
        expect (p.ok, "fixture");

        StudioEngine engine;
        engine.setMonitor (0.0f, 0.0f);          // no feedback through the loopback
        engine.setMetronome (false, true);
        engine.setCalibration (0);
        engine.setSampleSource (&p.cache);
        engine.setProject (p.snapshot());
        engine.useFakeDevice (blockSize, TestSignal::loopback, latency);
        expectEquals (engine.takeDeviceError(), juce::String());
        expect (engine.getFakeDevice() != nullptr);
        engine.setChannels (0, 1, 0);
        engine.setPosition (48000);
        pumpMessages (30);

        Collected c;
        c.attach (engine);
        const Id take = newId();
        const auto result = engine.startRecording (p.voice(), take, dir.getChildFile ("take.wav"));
        expect (result.wasOk(), result.getErrorMessage());
        expect (engine.isRecording());

        expect (waitFor ([&] { return engine.getPosition() >= 48000 + 3 * 48000; }, 10000), "recording did not advance");
        const auto rec = engine.stopRecording();
        expect (rec.has_value(), "no recording result");
        expect (! engine.isRecording());

        if (! rec.has_value())
            return;

        const auto expectedOffset = recordingOffset (96000, latency.input, latency.output, 0);
        expectEquals (rec->region.start, (int64_t) 48000);
        expectEquals (rec->region.offset, expectedOffset);
        expect (rec->region.take == take && rec->take == take && rec->channels == 1);
        expectEquals (rec->region.length, rec->samples - expectedOffset);

        const auto samples = c.decodeTake();
        expectEquals ((int64_t) samples.size(), rec->samples);

        // Reference: timeline click T lands at take sample offset + (T - 48000).
        std::vector<int64_t> reference;

        for (int64_t t = 48000; t < 48000 + rec->region.length; t += ClickProject::clickPeriod)
            reference.push_back (rec->region.offset + t - 48000);

        std::vector<int64_t> inRegion;

        for (auto at : detectClicks (samples))
            if (at >= rec->region.offset && at < rec->region.offset + rec->region.length)
                inRegion.push_back (at);

        juce::String errors;
        int worst = 0;

        for (auto at : inRegion)
        {
            int64_t best = INT64_MAX;

            for (auto r : reference)
                best = std::min (best, std::abs (at - r));

            worst = std::max (worst, (int) std::min<int64_t> (best, 1 << 30));
            errors << " " << juce::String (best);
        }

        logMessage ("block " + juce::String (blockSize) + " latency " + juce::String (latency.input) + "+"
                    + juce::String (latency.output) + ": " + juce::String ((int) inRegion.size()) + " clicks of "
                    + juce::String ((int) reference.size()) + ", errors:" + errors);
        expectEquals ((int) inRegion.size(), (int) reference.size(), "clicks detected in the region");
        expect (worst <= 1, "worst alignment error " + juce::String (worst));
    }

    void runTest() override
    {
        studioTestDir().deleteRecursively();

        beginTest ("loopback alignment, block 256");
        alignment (256, { 200, 264 });

        beginTest ("loopback alignment, blocks 64 and 1024");
        alignment (64, { 200, 264 });
        // A real round trip spans at least one block: at 1024 the fake device needs latencies that add up to >= 1024.
        alignment (1024, { 1224, 1288 });

        beginTest ("loopback refuses a round trip shorter than the block");
        {
            StudioEngine engine;
            engine.useFakeDevice (1024, TestSignal::loopback, { 200, 264 });
            expect (engine.takeDeviceError().isNotEmpty());
        }

        beginTest ("calibration measures the loopback round trip");
        {
            StudioEngine engine;
            engine.setMonitor (0.0f, 0.0f);
            engine.useFakeDevice (256, TestSignal::loopback, { 200, 264 });
            engine.setChannels (0, 1, 0);
            pumpMessages (20);
            engine.startCalibration();
            expect (engine.isCalibrating());
            expect (waitFor ([&] { return ! engine.isCalibrating(); }, 9000), "calibration did not finish");
            const auto trip = engine.calibrationRoundTrip();
            logMessage ("calibration round trip: " + (trip.has_value() ? juce::String (*trip) : juce::String ("none")));
            expect (trip.has_value() && std::abs (*trip - 464) <= 1, "round trip");
        }

        beginTest ("play, seek and stop are click-free at the output");
        {
            const Id host = newId(), take = newId(), track = newId();
            auto project = makeProject ("Song", host, "Lucas");
            const int64_t length = 10 * 48000;
            expect (applyOp (project, Ops::addTake (take, 1), host).ok
                    && applyOp (project, Ops::updateTake (take, 10, length, true), host).ok
                    && applyOp (project, Ops::addTrack (track, "Sine", TrackKind::imported), host).ok
                    && applyOp (project, Ops::setMix (track, std::nullopt, -1.0f, std::nullopt), host).ok   // hard left
                    && applyOp (project, Ops::addRegion (track, { newId(), take, 0, 0, length }), host).ok);
            SineTakes sines;
            sines.add (take, length);

            StudioEngine engine;
            engine.setMonitor (0.0f, 0.0f);
            engine.setMetronome (false, false);
            engine.setSampleSource (&sines);
            engine.setProject (std::make_shared<const Project> (project));
            engine.setPosition (12345);
            engine.useFakeDevice (256, TestSignal::silence);
            OutputCapture out (48000 * 4);
            out.attach (*engine.getFakeDevice());

            engine.play();
            expect (engine.isPlaying());
            pumpMessages (250);
            engine.setPosition (200000);
            pumpMessages (250);
            expect (engine.getPosition() > 200000);
            engine.stop();
            pumpMessages (60);
            expect (! engine.isPlaying(), "the stop fade ended");
            const auto stoppedAt = engine.getPosition();
            pumpMessages (60);
            expectEquals (engine.getPosition(), stoppedAt);
            engine.play();
            pumpMessages (200);
            engine.stop();
            pumpMessages (60);
            engine.getFakeDevice()->onOutputBlock = nullptr;

            std::lock_guard<std::mutex> g (out.lock);
            const auto peak = std::abs (*std::max_element (out.L.begin(), out.L.end(),
                                                           [] (float a, float b) { return std::abs (a) < std::abs (b); }));
            const auto step = maxStepOf (out.L), bound = noClickBound();
            logMessage ("output peak " + juce::String (peak, 4) + ", max step " + juce::String (step, 6)
                        + " (bound " + juce::String (bound, 6) + ")");
            expect (peak > 0.45f, "the sine played");
            expect (step <= bound, "click at the output: step " + juce::String (step, 6));
            expect (out.L.back() == 0.0f && out.R.back() == 0.0f, "silent after stop");
        }

        beginTest ("the metronome sounds only at the output, never in the take");
        {
            const auto dir = studioTestDir().getChildFile ("metronome");
            const Id host = newId(), track = newId();
            auto project = makeProject ("Song", host, "Lucas");
            expect (applyOp (project, Ops::setTempo (240.0, 4), host).ok
                    && applyOp (project, Ops::addTrack (track, "Voice", TrackKind::voice), host).ok);

            StudioEngine engine;
            engine.setMonitor (1.0f, 1.0f);
            engine.setMetronome (true, true);
            engine.setProject (std::make_shared<const Project> (project));
            engine.useFakeDevice (256, TestSignal::silence);
            OutputCapture out (48000 * 4);
            out.attach (*engine.getFakeDevice());
            pumpMessages (20);

            Collected c;
            c.attach (engine);
            expect (engine.startRecording (*project.findTrack (track), newId(), dir.getChildFile ("take.wav")).wasOk());
            expect (waitFor ([&] { return engine.getPosition() >= 48000; }, 6000));
            const auto rec = engine.stopRecording();
            pumpMessages (30);
            engine.getFakeDevice()->onOutputBlock = nullptr;

            expect (rec.has_value());
            expectEquals (rec.has_value() ? rec->region.offset : -1, (int64_t) 48000);   // one bar at 240 BPM
            const auto samples = c.decodeTake();
            expect (samples.size() > 48000);
            float recordedPeak = 0.0f;

            for (auto s : samples)
                recordedPeak = std::max (recordedPeak, std::abs (s));

            std::lock_guard<std::mutex> g (out.lock);
            float outputPeak = 0.0f;

            for (auto s : out.L)
                outputPeak = std::max (outputPeak, std::abs (s));

            logMessage ("metronome output peak " + juce::String (outputPeak, 4) + ", recorded peak " + juce::String (recordedPeak));
            expect (outputPeak > 0.3f, "the metronome sounded");
            expectEquals (recordedPeak, 0.0f);
        }

        beginTest ("stopRecording waits for the writer to drain");
        {
            const auto dir = studioTestDir().getChildFile ("drain");
            const Id host = newId(), track = newId();
            auto project = makeProject ("Song", host, "Lucas");
            expect (applyOp (project, Ops::addTrack (track, "Voice", TrackKind::voice), host).ok);

            StudioEngine engine;
            engine.setMetronome (false, false);
            engine.setProject (std::make_shared<const Project> (project));
            engine.useFakeDevice (256, TestSignal::sine440);
            engine.setChannels (0, 1, 0);
            pumpMessages (20);

            Collected c;
            c.attach (engine);
            engine.getRecorder().setWriterPausedForTest (true);
            expect (engine.startRecording (*project.findTrack (track), newId(), dir.getChildFile ("take.wav")).wasOk());
            pumpMessages (1500);
            expect (c.segmentSampleSum() == 0, "the writer is paused");
            const auto rec = engine.stopRecording();
            const bool finishedAtReturn = c.finished.load();
            engine.getRecorder().setWriterPausedForTest (false);

            expect (finishedAtReturn, "onFinished fired before stopRecording returned");
            expect (rec.has_value());

            if (rec.has_value())
            {
                logMessage ("drained " + juce::String (rec->samples) + " samples in " + juce::String (rec->segments) + " segments");
                expect (rec->samples > 48000);
                expectEquals (rec->samples, c.segmentSampleSum());
                expectEquals (rec->segments, (int) c.segmentSamples.size());
                expectEquals (rec->region.offset, (int64_t) 0);
                expectEquals (rec->region.length, rec->samples);
            }
        }

        beginTest ("live preview: the take is sent as 128-sample Opus packets");
        {
            const auto dir = studioTestDir().getChildFile ("preview");
            const Id host = newId(), track = newId();
            auto project = makeProject ("Song", host, "Lucas");
            expect (applyOp (project, Ops::addTrack (track, "Voice", TrackKind::voice), host).ok);

            StudioEngine engine;
            engine.setMetronome (false, false);
            engine.setProject (std::make_shared<const Project> (project));
            engine.useFakeDevice (256, TestSignal::sine440);
            pumpMessages (20);

            Collected c;
            c.attach (engine);
            expect (engine.startRecording (*project.findTrack (track), newId(), dir.getChildFile ("take.wav")).wasOk());
            pumpMessages (500);
            const auto rec = engine.stopRecording();
            expect (rec.has_value());

            std::lock_guard<std::mutex> g (c.lock);
            const auto expected = rec.has_value() ? (int) (rec->samples / 128) : 0;
            expectEquals ((int) c.previewPackets.size(), expected);
            bool wellFormed = ! c.previewPackets.empty();

            for (size_t i = 0; i < c.previewPackets.size(); ++i)
            {
                AudioPacketView view;
                const auto& packet = c.previewPackets[i];
                wellFormed = wellFormed && decodeAudioPacket (packet.data(), packet.size(), view)
                          && view.flags == AudioFlags::voice && view.frameSize == FrameSize::f128
                          && view.voiceLen == codedBytesMono (FrameSize::f128) && view.instrumentLen == 0
                          && view.seq == (uint16_t) i && view.sampleTime == (uint32_t) (i * 128) && view.clientId == 0;
            }

            expect (wellFormed, "preview packets");
        }

        beginTest ("live preview: received packets are added to the output");
        {
            StudioEngine engine;
            engine.setMonitor (0.0f, 0.0f);
            engine.useFakeDevice (256, TestSignal::silence);
            JitterBuffer rx (codedBytesStereo (FrameSize::f128), 128000.0 / sampleRate);
            rx.setAutoSize (false);
            engine.setPreviewBuffer (&rx);
            pumpMessages (20);
            engine.takeMeters();

            OpusMode mode (FrameSize::f128);
            OpusEncoder encoder (mode, 2);
            std::vector<float> pcm (256);
            std::vector<uint8_t> coded (128);
            const double startMs = juce::Time::getMillisecondCounterHiRes();

            for (int b = 0; b < 150; ++b)
            {
                for (int i = 0; i < 128; ++i)
                    pcm[(size_t) (2 * i)] = pcm[(size_t) (2 * i + 1)] = sineAt (b * 128 + i);

                const int n = encoder.encode (pcm.data(), coded.data(), (int) coded.size());
                rx.push ((uint16_t) b, coded.data(), n, startMs + b * 128000.0 / sampleRate);

                if ((b + 1) % 6 == 0)
                    juce::Thread::sleep (16);
            }

            pumpMessages (50);
            const auto meters = engine.takeMeters();
            engine.setPreviewBuffer (nullptr);
            logMessage ("preview output peak " + juce::String (meters.output, 4));
            expect (meters.output > 0.3f && meters.output < 1.0f, "preview output peak " + juce::String (meters.output));
        }
    }
};

static StudioEngineTests studioEngineTests;
}
