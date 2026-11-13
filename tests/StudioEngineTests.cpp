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

// Counts reads made through a source after it was destroyed (the swap test destroys sources while playing).
std::atomic<int> readsAfterDestruction { 0 };

// sign -1 serves the phase-inverted sine: swapping between the two without a fade would step by up to 1.0.
class PoisonedSineTakes : public SineTakes
{
public:
    explicit PoisonedSineTakes (float signToUse = 1.0f) : sign (signToUse) {}
    ~PoisonedSineTakes() override { alive.store (false); }

    void read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept override
    {
        if (! alive.load())
            ++readsAfterDestruction;

        SineTakes::read (take, channel, offset, n, dest);

        for (int i = 0; i < n; ++i)
            dest[i] *= sign;
    }

private:
    const float sign;
    std::atomic<bool> alive { true };
};

// One imported track, hard left (the left channel carries the full 0.5 sine), one region over the whole take.
struct SineProject
{
    static constexpr int64_t length = 10 * 48000;
    Id host = newId(), take = newId(), track = newId();
    Project project = makeProject ("Song", host, "Lucas");
    bool ok = true;

    SineProject()
    {
        ok = apply (Ops::addTake (take, 1))
          && apply (Ops::updateTake (take, 10, length, true))
          && apply (Ops::addTrack (track, "Sine", TrackKind::imported))
          && apply (Ops::setMix (track, std::nullopt, -1.0f, std::nullopt))
          && apply (Ops::addRegion (track, { newId(), take, 0, 0, length }));
    }

    bool apply (const juce::var& op) { return applyOp (project, op, host).ok; }
    std::shared_ptr<const Project> snapshot() const { return std::make_shared<const Project> (project); }
};

float peakOfVector (const std::vector<float>& x)
{
    float m = 0.0f;

    for (auto v : x)
        m = std::max (m, std::abs (v));

    return m;
}

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

    struct AlignCase
    {
        juce::String name;
        int block = 256;
        FakeLatency latency;
        int calibration = 0;
        int64_t expectedOffset = 0;      // hard-coded by the caller, never recomputed here
        bool fromPlaying = false;        // record while the transport is already playing (capture starts mid-block)
        bool interfere = false;          // call stop(), play() and setPosition() during the take
    };

    // Loopback alignment: records the click track coming back through the fake device and checks every recorded click
    // sits exactly (0 samples) where the timeline put it.
    void alignment (const AlignCase& t)
    {
        const auto dir = studioTestDir().getChildFile ("align-" + t.name);
        dir.deleteRecursively();
        ClickProject p (dir.getChildFile ("cache"));
        expect (p.ok, "fixture");

        StudioEngine engine;
        engine.setMonitor (0.0f, 0.0f);          // no feedback through the loopback
        engine.setMetronome (false, true);
        engine.setCalibration (t.calibration);
        engine.setSampleSource (&p.cache);
        engine.setProject (p.snapshot());
        engine.useFakeDevice (t.block, TestSignal::loopback, t.latency);
        expectEquals (engine.takeDeviceError(), juce::String());
        expect (engine.getFakeDevice() != nullptr);
        engine.setChannels (0, 1, 0);

        if (t.fromPlaying)
        {
            engine.setPosition (10000);
            engine.play();
            pumpMessages (300);
            expect (engine.isPlaying() && engine.getPosition() > 10000, "playing before the take");
        }
        else
        {
            engine.setPosition (48000);
            pumpMessages (30);
        }

        Collected c;
        c.attach (engine);
        const Id take = newId();
        const auto pointBefore = engine.getPosition();
        const auto result = engine.startRecording (p.voice(), take, dir.getChildFile ("take.wav"));
        expect (result.wasOk(), result.getErrorMessage());
        expect (engine.isRecording());

        if (t.interfere)
        {
            expect (waitFor ([&] { return engine.getPosition() >= pointBefore + 48000; }, 6000));
            const auto before = engine.getPosition();
            engine.stop();
            pumpMessages (100);
            expect (engine.isPlaying(), "stop() is a no-op during a take");
            engine.play();
            engine.setPosition (0);
            pumpMessages (100);
            const auto after = engine.getPosition();
            logMessage ("during the take: position " + juce::String (before) + " -> " + juce::String (after));
            expect (after > before + 4800, "the timeline kept running through stop(), play() and setPosition()");
        }

        expect (waitFor ([&] { return engine.getPosition() >= pointBefore + 3 * 48000; }, 10000), "recording did not advance");
        const auto rec = engine.stopRecording();
        expect (rec.has_value(), "no recording result");
        expect (! engine.isRecording());
        expectEquals (c.errors.load(), 0);

        if (! rec.has_value())
            return;

        const auto start = rec->region.start;

        if (t.fromPlaying)
            expect (start > 10000, "record point taken from the playing transport");
        else
            expectEquals (start, (int64_t) 48000);

        expectEquals (rec->region.offset, t.expectedOffset);
        expect (rec->region.take == take && rec->take == take && rec->channels == 1);
        expectEquals (rec->region.length, rec->samples - t.expectedOffset);

        const auto samples = c.decodeTake();
        expectEquals ((int64_t) samples.size(), rec->samples);

        // Reference: timeline click T lands at take sample offset + (T - start).
        std::vector<int64_t> reference;
        const int64_t period = ClickProject::clickPeriod;

        for (int64_t at = ((start + period - 1) / period) * period; at < start + rec->region.length; at += period)
            reference.push_back (rec->region.offset + at - start);

        std::vector<int64_t> inRegion;

        for (auto at : detectClicks (samples))
            if (at >= rec->region.offset && at < rec->region.offset + rec->region.length)
                inRegion.push_back (at);

        juce::String errors;
        int64_t worst = 0;

        for (auto at : inRegion)
        {
            int64_t best = INT64_MAX;

            for (auto r : reference)
                best = std::min (best, std::abs (at - r));

            worst = std::max (worst, best);
            errors << " " << juce::String (best);
        }

        logMessage (t.name + ": block " + juce::String (t.block) + " latency " + juce::String (t.latency.input) + "+"
                    + juce::String (t.latency.output) + "+" + juce::String (t.latency.hidden) + " calibration "
                    + juce::String (t.calibration) + ", start " + juce::String (start) + ", offset "
                    + juce::String (rec->region.offset) + ": " + juce::String ((int) inRegion.size()) + " clicks of "
                    + juce::String ((int) reference.size()) + ", errors:" + errors);
        expect (! reference.empty());
        expectEquals ((int) inRegion.size(), (int) reference.size(), "clicks detected in the region");
        expect (worst == 0, "worst alignment error " + juce::String (worst));
    }

    // Streams a stereo 440 Hz sine (amplitude 0.5) as 128-sample Opus packets into the preview jitter buffer at the
    // device's pace and checks what reaches the output: the level of the source (within 15 %) and no step above
    // 1.5 x the source's own largest step, including the stream's start, an optional lost packet mid-stream and the
    // end of the stream (concealment, then a faded cut).
    // cutMidStream: listening is turned off (setPreviewBuffer (nullptr)) while packets still arrive.
    // offAndOn: listening is turned off and, one device block later (while the faded tail still plays), on again;
    // the stream goes on.
    void previewReceive (bool dropOne, bool cutMidStream = false, bool offAndOn = false, int blockSize = 256)
    {
        StudioEngine engine;
        engine.setMonitor (0.0f, 0.0f);
        engine.useFakeDevice (blockSize, TestSignal::silence);
        OutputCapture out (48000 * 3);
        out.attach (*engine.getFakeDevice());

        const double blockMs = 128000.0 / sampleRate;
        JitterBuffer rx (codedBytesStereo (FrameSize::f128), blockMs);
        rx.setAutoSize (false);
        rx.setSizeBlocks (8);

        OpusMode mode (FrameSize::f128);
        OpusEncoder encoder (mode, 2);
        std::vector<float> pcm (256);
        std::vector<uint8_t> coded (128);
        constexpr int blocks = 300, prefill = 8, dropped = 150, cutAt = 200;

        const auto push = [&] (int b)
        {
            for (int i = 0; i < 128; ++i)
                pcm[(size_t) (2 * i)] = pcm[(size_t) (2 * i + 1)] = sineAt (b * 128 + i);

            const int n = encoder.encode (pcm.data(), coded.data(), (int) coded.size());

            if (! (dropOne && b == dropped))
                rx.push ((uint16_t) b, coded.data(), n, juce::Time::getMillisecondCounterHiRes());
        };

        for (int b = 0; b < prefill; ++b)
            push (b);

        engine.setPreviewBuffer (&rx);
        const double startMs = juce::Time::getMillisecondCounterHiRes();

        // offAndOn: toggled from the device thread between two callbacks, so the second switch lands exactly one
        // block after the first.
        std::atomic<int> toggle { 0 };   // 1: turn off at the next block end, 2: turn on at the next one, 3: done

        if (offAndOn)
        {
            engine.getFakeDevice()->onOutputBlock = [&] (const float* l, const float* r, int n, int64_t, double)
            {
                {
                    std::lock_guard<std::mutex> g (out.lock);

                    for (int i = 0; i < n && out.L.size() < out.L.capacity(); ++i)
                    {
                        out.L.push_back (l[i]);
                        out.R.push_back (r[i]);
                    }
                }

                if (toggle.load() == 1)
                {
                    engine.setPreviewBuffer (nullptr);
                    toggle.store (2);
                }
                else if (toggle.load() == 2)
                {
                    engine.setPreviewBuffer (&rx);
                    toggle.store (3);
                }
            };
        }

        for (int b = prefill; b < blocks; ++b)
        {
            while (juce::Time::getMillisecondCounterHiRes() < startMs + (b - prefill) * blockMs)
                juce::Thread::sleep (1);

            if (offAndOn && b == cutAt)
                toggle.store (1);

            if (cutMidStream && b == cutAt)
            {
                engine.setPreviewBuffer (nullptr);   // the sine is playing right now: it must fade, not step
                break;
            }

            push (b);
        }

        if (offAndOn)
            expectEquals (toggle.load(), 3, "listening was turned off and on again");

        const auto stats = rx.getStats();
        pumpMessages (150);                          // the stream ends: concealment, then a faded cut
        engine.setPreviewBuffer (nullptr);
        engine.getFakeDevice()->onOutputBlock = nullptr;

        std::vector<float> pure (48000);

        for (size_t i = 0; i < pure.size(); ++i)
            pure[i] = sineAt ((int64_t) i);

        const auto bound = 1.5f * maxStepOf (pure);
        std::lock_guard<std::mutex> g (out.lock);
        const auto peak = peakOfVector (out.L), step = maxStepOf (out.L);
        logMessage (juce::String (offAndOn ? "off and on" : cutMidStream ? "cut mid-stream" : dropOne ? "one packet lost" : "no packet lost") + ": underruns while streaming "
                    + juce::String ((int) stats.underruns) + ", output peak " + juce::String (peak, 4) + ", max step "
                    + juce::String (step, 6) + " (bound " + juce::String (bound, 6) + ")");

        if (! dropOne && ! offAndOn)
            expectEquals ((int) stats.underruns, 0, "the stream was paced without gaps");

        expect (peak >= 0.425f && peak <= 0.575f, "output peak " + juce::String (peak, 4));
        expect (step <= bound, "click in the preview: step " + juce::String (step, 6));
        expect (out.L.back() == 0.0f, "silent after the stream ended");
    }

    void runTest() override
    {
        studioTestDir().deleteRecursively();

        beginTest ("loopback alignment, block 256");
        alignment ({ "b256", 256, { 200, 264 }, 0, 96000 + 200 + 264 });

        beginTest ("loopback alignment, blocks 64 and 1024");
        alignment ({ "b64", 64, { 200, 264 }, 0, 96000 + 200 + 264 });
        // A real round trip spans at least one block: at 1024 the fake device needs latencies that add up to >= 1024.
        alignment ({ "b1024", 1024, { 1224, 1288 }, 0, 96000 + 1224 + 1288 });

        beginTest ("loopback alignment with a calibration of 37 samples");
        // The device hides 37 samples of its round trip; the calibration supplies them.
        alignment ({ "cal37", 256, { 200, 264, 37 }, 37, 96000 + 200 + 264 + 37 });

        beginTest ("loopback alignment when recording from a playing transport");
        alignment ({ "playing", 256, { 200, 264 }, 0, 96000 + 200 + 264, true });

        beginTest ("stop, play and setPosition during a take change neither the position nor the alignment");
        alignment ({ "interfere", 256, { 200, 264 }, 0, 96000 + 200 + 264, false, true });

        beginTest ("a take shorter than the offset yields no region and an error");
        {
            const auto dir = studioTestDir().getChildFile ("short");
            ClickProject p (dir.getChildFile ("cache"));
            StudioEngine engine;
            engine.setMetronome (false, true);           // count-in: 96000 samples at 120 BPM
            engine.setProject (p.snapshot());
            engine.useFakeDevice (256, TestSignal::silence);
            pumpMessages (20);

            Collected c;
            c.attach (engine);
            expect (engine.startRecording (p.voice(), newId(), dir.getChildFile ("take.wav")).wasOk());
            pumpMessages (200);
            const auto rec = engine.stopRecording();
            expect (! rec.has_value(), "no region for a take shorter than the offset");
            expectEquals (c.errors.load(), 1);
            expect (! engine.isRecording());
        }

        beginTest ("a device stop and restart during a take is reported; stopping keeps what was recorded");
        {
            const auto dir = studioTestDir().getChildFile ("restart");
            const Id host = newId(), track = newId();
            auto project = makeProject ("Song", host, "Lucas");
            expect (applyOp (project, Ops::addTrack (track, "Voice", TrackKind::voice), host).ok);

            StudioEngine engine;
            engine.setMetronome (false, false);
            engine.setProject (std::make_shared<const Project> (project));
            engine.useFakeDevice (256, TestSignal::sine440);
            pumpMessages (20);
            expect (engine.takeDeviceError().isEmpty());

            Collected c;
            c.attach (engine);
            expect (engine.startRecording (*project.findTrack (track), newId(), dir.getChildFile ("take.wav")).wasOk());
            pumpMessages (400);

            auto& dm = engine.getDeviceManager();
            dm.closeAudioDevice();
            dm.restartLastAudioDevice();
            pumpMessages (100);

            const auto error = engine.takeDeviceError();
            expect (error.contains (juce::String (juce::CharPointer_UTF8 ("reinici\xc3\xb3"))), "no restart error: " + error);
            expect (engine.isRecording(), "the engine leaves stopping the take to its owner");

            const auto rec = engine.stopRecording();
            expect (rec.has_value(), "what was recorded before the restart is kept");

            if (rec.has_value())
                expect (rec->samples >= 48000 / 4, "only " + juce::String (rec->samples) + " samples kept");

            // Outside a take, a restart is not an error.
            dm.closeAudioDevice();
            dm.restartLastAudioDevice();
            pumpMessages (50);
            expect (engine.takeDeviceError().isEmpty());
        }

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

        beginTest ("calibration finds latency the device does not report");
        {
            StudioEngine engine;
            engine.setMonitor (0.0f, 0.0f);
            engine.useFakeDevice (256, TestSignal::loopback, { 200, 264, 37 });
            engine.setChannels (0, 1, 0);
            pumpMessages (20);
            engine.startCalibration();
            expect (waitFor ([&] { return ! engine.isCalibrating(); }, 9000), "calibration did not finish");
            const auto trip = engine.calibrationRoundTrip();
            logMessage ("calibration round trip with 37 hidden: " + (trip.has_value() ? juce::String (*trip) : juce::String ("none")));
            expect (trip.has_value() && *trip == 501, "round trip");
            expectEquals (trip.has_value() ? LatencyCalibrator::calibrationFor (*trip, 200, 264) : -1, 37);
        }

        beginTest ("play, seek and stop are click-free at the output");
        {
            SineProject sp;
            expect (sp.ok);
            SineTakes sines;
            sines.add (sp.take, sp.length);

            StudioEngine engine;
            engine.setMonitor (0.0f, 0.0f);
            engine.setMetronome (false, false);
            engine.setSampleSource (&sines);
            engine.setProject (sp.snapshot());
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

        beginTest ("swapping the sample source while playing is safe and click-free");
        {
            SineProject sp;
            expect (sp.ok);
            readsAfterDestruction = 0;
            float sign = 1.0f;
            auto current = std::make_unique<PoisonedSineTakes> (sign);
            current->add (sp.take, sp.length);

            StudioEngine engine;
            engine.setMonitor (0.0f, 0.0f);
            engine.setMetronome (false, false);
            engine.setSampleSource (current.get());
            engine.setProject (sp.snapshot());
            engine.useFakeDevice (64, TestSignal::silence);
            OutputCapture out (48000 * 6);
            out.attach (*engine.getFakeDevice());

            // Swaps to the other phase; the previous source is destroyed right after the call returns.
            const auto swap = [&]
            {
                sign = -sign;
                auto next = std::make_unique<PoisonedSineTakes> (sign);
                next->add (sp.take, sp.length);
                engine.setSampleSource (next.get());
                current = std::move (next);
            };

            engine.play();
            pumpMessages (100);

            for (int i = 0; i < 50; ++i)                 // while playing
            {
                swap();
                pumpMessages (i % 2 == 0 ? 3 : 15);
            }

            for (int i = 0; i < 10; ++i)                 // play() and a swap within the same block
            {
                engine.stop();
                pumpMessages (40);
                engine.play();
                swap();
                pumpMessages (40);
            }

            engine.stop();
            pumpMessages (50);
            engine.getFakeDevice()->onOutputBlock = nullptr;
            engine.setSampleSource (nullptr);

            std::lock_guard<std::mutex> g (out.lock);
            const auto step = maxStepOf (out.L), bound = noClickBound();
            logMessage ("60 phase-inverting swaps: reads after destruction " + juce::String (readsAfterDestruction.load())
                        + ", peak " + juce::String (peakOfVector (out.L), 4) + ", max step " + juce::String (step, 6)
                        + " (bound " + juce::String (bound, 6) + ")");
            expectEquals (readsAfterDestruction.load(), 0);
            expect (peakOfVector (out.L) > 0.45f, "the sine played");
            expect (step <= bound, "click at a swap: step " + juce::String (step, 6));
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

        beginTest ("live preview: received packets are added to the output without clicks");
        previewReceive (false);

        beginTest ("live preview: a lost packet is concealed without a click");
        previewReceive (true);

        beginTest ("live preview: turning listening off mid-stream fades out without a click");
        previewReceive (false, true);

        beginTest ("live preview: listening off and on again during the faded tail (64-sample blocks) plays the tail out");
        previewReceive (false, false, true, 64);
    }
};

static StudioEngineTests studioEngineTests;
}
