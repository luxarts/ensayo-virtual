#include "client/StudioEngine.h"
#include "client/TakeCache.h"
#include "core/Segments.h"
#include "core/RemoteStreams.h"
#include "core/OpusCodec.h"
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
// One remote sender: a sine (amplitude 0.5) as 128-sample Opus packets of one stream kind (voice mono, instrument
// stereo), pushed into a RemoteStreamTable as the ProjectClient's UDP thread would.
struct RemoteSender
{
    RemoteSender (const OpusMode& mode, uint32_t client, StreamKind streamKind, double frequency)
        : clientId (client), kind (streamKind), freq (frequency), channels (streamKind == StreamKind::voice ? 1 : 2),
          encoder (mode, channels), pcm ((size_t) (128 * channels)), coded (128) {}

    void send (RemoteStreamTable& table, bool drop = false)
    {
        for (int i = 0; i < 128; ++i)
        {
            const auto s = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) (sample + i) / 48000.0));

            for (int c = 0; c < channels; ++c)
                pcm[(size_t) (i * channels + c)] = s;
        }

        const int n = encoder.encode (pcm.data(), coded.data(), (int) coded.size());

        if (! drop)
            table.push (clientId, kind, seq, coded.data(), n, juce::Time::getMillisecondCounterHiRes());

        ++seq;
        sample += 128;
    }

    const uint32_t clientId;
    const StreamKind kind;
    const double freq;
    const int channels;
    OpusEncoder encoder;
    std::vector<float> pcm;
    std::vector<uint8_t> coded;
    uint16_t seq = 0;
    int64_t sample = 0;
};

// The chat packets the engine's encoder thread hands to onChatPacket, by kind.
struct ChatSink
{
    std::mutex lock;
    std::vector<std::vector<uint8_t>> voice, instrument;

    void attach (StudioEngine& engine)
    {
        engine.onChatPacket = [this] (StreamKind kind, const uint8_t* packet, size_t len)
        {
            std::lock_guard<std::mutex> g (lock);
            (kind == StreamKind::voice ? voice : instrument).emplace_back (packet, packet + len);
        };
    }

    int count (StreamKind kind)
    {
        std::lock_guard<std::mutex> g (lock);
        return (int) (kind == StreamKind::voice ? voice : instrument).size();
    }
};

// Amplitude of the `freq` component of x[from, to) (Goertzel); exact for a whole number of cycles.
float toneAmplitude (const std::vector<float>& x, size_t from, size_t to, double freq)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / 48000.0, c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    to = std::min (to, x.size());

    for (size_t i = from; i < to; ++i)
    {
        const double s = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }

    const double re = s1 - s2 * std::cos (w), im = s2 * std::sin (w);
    return to > from ? (float) (2.0 * std::sqrt (re * re + im * im) / (double) (to - from)) : 0.0f;
}

float peakBetween (const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;

    for (size_t i = from; i < std::min (to, x.size()); ++i)
        m = std::max (m, std::abs (x[i]));

    return m;
}

// A project with one voice and one instrument track of the host user.
struct TwoTrackProject
{
    Id host = newId(), voice = newId(), instrument = newId();
    Project project = makeProject ("Song", host, "Lucas");
    bool ok = true;

    TwoTrackProject()
    {
        ok = applyOp (project, Ops::addTrack (voice, "Voz", TrackKind::voice), host).ok
          && applyOp (project, Ops::addTrack (instrument, "Guitarra", TrackKind::instrument), host).ok;
    }

    std::shared_ptr<const Project> snapshot() const { return std::make_shared<const Project> (project); }
    const Track& track (const Id& id) const { return *project.findTrack (id); }
};
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

    // How a received stream ends in remoteReceive.
    enum class RemoteEnd
    {
        stops,          // the sender stops: concealment, then a faded cut
        dropOne,        // one lost packet mid-stream (concealed), then stops
        released,       // the slot is released mid-stream (presence says the sender left); the sender goes on, so the
                        // next packet starts a new stream instance in the table
        tableOff,       // setRemoteStreams (nullptr) while the stream sounds
        offAndOn        // setRemoteStreams (nullptr) and, one device block later (the faded tail still plays), back on
    };

    // Streams a mono 440 Hz sine (amplitude 0.5) as 128-sample Opus voice packets into a RemoteStreamTable at the
    // device's pace and checks what reaches the output: the level of the source (within 15 %) and no sample step
    // above 0.043 (1.5 x the source's own largest step), through the stream's start, its end and the given event.
    void remoteReceive (RemoteEnd scenario, int blockSize = 256)
    {
        StudioEngine engine;
        engine.setMonitor (0.0f, 0.0f);
        engine.useFakeDevice (blockSize, TestSignal::silence);
        OutputCapture out (48000 * 4);
        out.attach (*engine.getFakeDevice());

        RemoteStreamTable table;

        for (auto& slot : table.slots())
        {
            slot.buffer.setAutoSize (false);
            slot.buffer.setSizeBlocks (8);
        }

        OpusMode mode (FrameSize::f128);
        RemoteSender sender (mode, 7, StreamKind::voice, 440.0);
        const double blockMs = 128000.0 / sampleRate;
        constexpr int blocks = 300, prefill = 8, dropped = 150, cutAt = 200;

        for (int b = 0; b < prefill; ++b)
            sender.send (table);

        engine.setRemoteStreams (&table);
        const double startMs = juce::Time::getMillisecondCounterHiRes();
        std::atomic<int> toggle { 0 };   // offAndOn: 1 turn off at the next block end, 2 turn on at the next, 3 done

        if (scenario == RemoteEnd::offAndOn)
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
                    engine.setRemoteStreams (nullptr);
                    toggle.store (2);
                }
                else if (toggle.load() == 2)
                {
                    engine.setRemoteStreams (&table);
                    toggle.store (3);
                }
            };
        }

        for (int b = prefill; b < blocks; ++b)
        {
            while (juce::Time::getMillisecondCounterHiRes() < startMs + (b - prefill) * blockMs)
                juce::Thread::sleep (1);

            if (b == cutAt)
            {
                if (scenario == RemoteEnd::offAndOn)
                    toggle.store (1);

                if (scenario == RemoteEnd::tableOff)
                {
                    engine.setRemoteStreams (nullptr);   // the sine is playing right now: it must fade, not step
                    break;
                }

                if (scenario == RemoteEnd::released)
                {
                    table.releaseClient (7);
                    table.sweep (juce::Time::getMillisecondCounterHiRes());
                    expectEquals (table.activeCount(), 0, "released");
                }
            }

            sender.send (table, scenario == RemoteEnd::dropOne && b == dropped);
        }

        if (scenario == RemoteEnd::offAndOn)
            expectEquals (toggle.load(), 3, "the table was set off and on again");

        if (scenario == RemoteEnd::released)
            expectEquals (table.activeCount(), 1, "the sender's next packet claimed a slot again");

        pumpMessages (150);                          // the stream ends: concealment, then a faded cut
        engine.setRemoteStreams (nullptr);
        engine.getFakeDevice()->onOutputBlock = nullptr;

        const auto bound = 0.043f;
        std::lock_guard<std::mutex> g (out.lock);
        const auto peak = peakOfVector (out.L), step = maxStepOf (out.L), stepR = maxStepOf (out.R);
        const char* names[] = { "stops", "one packet lost", "released mid-stream", "table off mid-stream", "off and on" };
        logMessage (juce::String (names[(int) scenario]) + ", block " + juce::String (blockSize) + ": output peak "
                    + juce::String (peak, 4) + ", max step " + juce::String (step, 6) + " (bound " + juce::String (bound, 6) + ")");

        expect (peak >= 0.425f && peak <= 0.575f, "output peak " + juce::String (peak, 4));
        expect (step <= bound && stepR <= bound, "click in the received stream: step " + juce::String (std::max (step, stepR), 6));
        expect (out.L.back() == 0.0f && out.R.back() == 0.0f, "silent after the stream ended");
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
            const auto capturedAtStop = engine.getRecorder().samplesCaptured();
            dm.restartLastAudioDevice();
            pumpMessages (100);

            // The take ends at the last aligned sample: nothing captured after the stop (the latencies may differ).
            expectEquals (engine.getRecorder().samplesCaptured(), capturedAtStop, "samples captured after the device stopped");

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
            float trackPeak = 0.0f;

            for (const auto& [id, peak] : engine.takeTrackMeters())
                if (id == sp.track)
                    trackPeak = std::max (trackPeak, peak);

            expect (std::abs (trackPeak - 0.5f) < 0.01f, "track meter of the playing sine: " + juce::String (trackPeak, 4));
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

        beginTest ("monitor: only the armed kind's input sounds, faded on every change; send gain is pre monitor and meter");
        {
            StudioEngine engine;
            engine.setMonitor (1.0f, 0.25f);         // voice at full level, instrument at a quarter: they tell apart
            engine.setMetronome (false, false);
            engine.useFakeDevice (256, TestSignal::sine440);
            engine.setChannels (0, 1, 0);
            OutputCapture out (48000 * 4);
            out.attach (*engine.getFakeDevice());

            std::vector<size_t> marks;
            const auto mark = [&]
            {
                std::lock_guard<std::mutex> g (out.lock);
                marks.push_back (out.R.size());
            };

            pumpMessages (300);
            expectEquals (engine.takeArmedInputPeak(), 0.0f, "nothing armed, no armed input");
            mark();
            engine.setArmed (TrackKind::voice);
            pumpMessages (300);
            const auto armedVoicePeak = engine.takeArmedInputPeak();
            mark();
            engine.setSendGains (0.5f, 1.0f);
            pumpMessages (200);
            engine.takeArmedInputPeak();             // drops the peak of the ramp
            pumpMessages (100);
            const auto halfVoicePeak = engine.takeArmedInputPeak();
            mark();
            engine.setArmed (TrackKind::instrument);
            pumpMessages (300);
            mark();
            engine.setArmed (std::nullopt);
            pumpMessages (300);
            mark();
            engine.getFakeDevice()->onOutputBlock = nullptr;

            std::lock_guard<std::mutex> g (out.lock);
            const auto window = [&] (size_t i) { return peakBetween (out.R, marks[i] + 2048, marks[i + 1]); };
            const auto unarmed = peakBetween (out.R, 0, marks[0]), voice = window (0), halfVoice = window (1),
                       instrument = window (2), off = peakBetween (out.R, marks[3] + 2048, marks[4]);
            const auto step = std::max (maxStepOf (out.L), maxStepOf (out.R));
            logMessage ("monitor peaks: unarmed " + juce::String (unarmed, 4) + ", voice " + juce::String (voice, 4)
                        + ", voice at send 0.5 " + juce::String (halfVoice, 4) + ", instrument " + juce::String (instrument, 4)
                        + ", unarmed again " + juce::String (off, 4) + "; armed input " + juce::String (armedVoicePeak, 4)
                        + " / " + juce::String (halfVoicePeak, 4) + "; max step " + juce::String (step, 6));
            expectEquals (unarmed, 0.0f, "no monitor while nothing is armed");
            expect (std::abs (voice - 0.5f) < 0.01f, "armed voice: the mic at the voice monitor gain");
            expect (std::abs (halfVoice - 0.25f) < 0.01f, "the monitor hears the input after the send gain");
            expect (std::abs (instrument - 0.125f) < 0.01f, "armed instrument: only the instrument, at its monitor gain");
            expectEquals (off, 0.0f, "disarmed: silent again");
            expect (std::abs (armedVoicePeak - 0.5f) < 0.01f, "armed input peak");
            expect (std::abs (halfVoicePeak - 0.25f) < 0.01f, "armed input peak after the send gain");
            expect (step <= 0.043f, "click when the monitor changes: step " + juce::String (step, 6));
        }

        beginTest ("send gains are printed exactly into the take once the ramp has settled");
        {
            const auto dir = studioTestDir().getChildFile ("sendgain");
            TwoTrackProject p;
            expect (p.ok);

            StudioEngine engine;
            engine.setMetronome (false, false);
            engine.setSendGains (0.5f, 0.25f);
            engine.setProject (p.snapshot());
            engine.useFakeDevice (256, TestSignal::click);   // +1.0 impulses on both inputs
            engine.setChannels (0, 1, 0);
            pumpMessages (50);

            for (const auto& [trackId, gain] : { std::pair<Id, float> { p.voice, 0.5f }, { p.instrument, 0.25f } })
            {
                Collected c;
                c.attach (engine);
                expect (engine.startRecording (p.track (trackId), newId(), dir.getChildFile (trackId + ".wav")).wasOk());
                pumpMessages (1300);
                const auto rec = engine.stopRecording();
                expect (rec.has_value());

                int clicks = 0;
                bool exact = true;

                for (auto s : c.decodeTake())
                {
                    if (s == 0.0f)
                        continue;

                    ++clicks;
                    exact = exact && s == gain;
                }

                logMessage ("send gain " + juce::String (gain) + ": " + juce::String (clicks) + " clicks recorded");
                expect (clicks >= 3, "clicks recorded");
                expect (exact, "every recorded click is exactly gain x input");
            }
        }

        beginTest ("received streams: start, end and concealment are faded");
        remoteReceive (RemoteEnd::stops);

        beginTest ("received streams: a lost packet is concealed without a click");
        remoteReceive (RemoteEnd::dropOne);

        beginTest ("received streams: a slot released mid-stream fades out and the next stream fades in");
        remoteReceive (RemoteEnd::released);

        beginTest ("received streams: setRemoteStreams (nullptr) mid-stream fades out without a click");
        remoteReceive (RemoteEnd::tableOff);

        beginTest ("received streams: off and on again during the faded tail (64-sample blocks) plays the tail out");
        remoteReceive (RemoteEnd::offAndOn, 64);

        beginTest ("received streams: two senders are mixed with independent participant gains");
        {
            StudioEngine engine;
            engine.setMonitor (0.0f, 0.0f);
            engine.useFakeDevice (256, TestSignal::silence);
            OutputCapture out (48000 * 4);
            out.attach (*engine.getFakeDevice());

            RemoteStreamTable table;

            for (auto& slot : table.slots())
            {
                slot.buffer.setAutoSize (false);
                slot.buffer.setSizeBlocks (8);
            }

            OpusMode mode (FrameSize::f128);
            RemoteSender voice (mode, 1, StreamKind::voice, 440.0), guitar (mode, 2, StreamKind::instrument, 1000.0);
            engine.setParticipantGain (1, 1.0f, false);
            engine.setParticipantGain (2, 0.25f, false);

            for (int b = 0; b < 8; ++b)
            {
                voice.send (table);
                guitar.send (table);
            }

            engine.setRemoteStreams (&table);
            const double blockMs = 128000.0 / sampleRate;
            const double startMs = juce::Time::getMillisecondCounterHiRes();
            const int total = (int) (2.4 * sampleRate / 128);
            size_t both = 0, muted = 0;

            for (int b = 0; b < total; ++b)
            {
                while (juce::Time::getMillisecondCounterHiRes() < startMs + b * blockMs)
                    juce::Thread::sleep (1);

                const auto seconds = b * 128.0 / sampleRate;

                if (both == 0 && seconds >= 0.8)
                {
                    std::lock_guard<std::mutex> g (out.lock);
                    both = out.L.size();
                }

                if (muted == 0 && seconds >= 1.5)
                {
                    engine.setParticipantGain (1, 1.0f, true);
                    std::lock_guard<std::mutex> g (out.lock);
                    muted = out.L.size();
                }

                voice.send (table);
                guitar.send (table);
            }

            pumpMessages (150);
            engine.setRemoteStreams (nullptr);
            engine.getFakeDevice()->onOutputBlock = nullptr;

            std::lock_guard<std::mutex> g (out.lock);
            const auto v1 = toneAmplitude (out.L, both, both + 24000, 440.0);
            const auto g1 = toneAmplitude (out.L, both, both + 24000, 1000.0);
            const auto g1r = toneAmplitude (out.R, both, both + 24000, 1000.0);
            const auto v2 = toneAmplitude (out.L, muted + 4800, muted + 4800 + 24000, 440.0);
            const auto g2 = toneAmplitude (out.L, muted + 4800, muted + 4800 + 24000, 1000.0);
            const auto step = std::max (maxStepOf (out.L), maxStepOf (out.R));
            logMessage ("two senders: voice " + juce::String (v1, 4) + " guitar " + juce::String (g1, 4) + "/"
                        + juce::String (g1r, 4) + "; voice muted: voice " + juce::String (v2, 4) + " guitar "
                        + juce::String (g2, 4) + "; max step " + juce::String (step, 6));
            expect (both > 0 && muted > both + 24000, "windows");
            expect (std::abs (v1 - 0.5f) < 0.075f, "voice at gain 1");
            expect (std::abs (g1 - 0.125f) < 0.02f && std::abs (g1r - 0.125f) < 0.02f, "instrument (stereo) at gain 0.25");
            expect (v2 < 0.005f, "muted voice");
            expect (std::abs (g2 - 0.125f) < 0.02f, "the instrument is not affected by the voice sender's mute");
            expect (step <= 1.5f * (0.5f + 0.125f * 1000.0f / 440.0f) * 0.0576f, "click: step " + juce::String (step, 6));
        }

        beginTest ("chat: the mic is sent while enabled; muting it stops the voice packets");
        {
            StudioEngine engine;
            ChatSink sink;
            sink.attach (engine);
            engine.setMonitor (0.0f, 0.0f);
            engine.useFakeDevice (256, TestSignal::sine440);
            pumpMessages (200);
            expectEquals (sink.count (StreamKind::voice), 0, "nothing sent before setChatEnabled (true)");

            engine.setChatEnabled (true);
            expect (waitFor ([&] { return sink.count (StreamKind::voice) >= 60; }, 3000), "voice packets flow");
            expectEquals (sink.count (StreamKind::instrument), 0, "no instrument stream without an instrument take");

            {
                std::lock_guard<std::mutex> g (sink.lock);
                bool wellFormed = true;
                AudioPacketView first, view;
                wellFormed = decodeAudioPacket (sink.voice.front().data(), sink.voice.front().size(), first);

                for (size_t i = 0; i < sink.voice.size() && wellFormed; ++i)
                {
                    wellFormed = decodeAudioPacket (sink.voice[i].data(), sink.voice[i].size(), view)
                              && view.frameSize == FrameSize::f128 && view.flags == AudioFlags::voice
                              && view.voiceLen == codedBytesMono (FrameSize::f128) && view.instrumentLen == 0
                              && view.seq == (uint16_t) (first.seq + i)
                              && view.sampleTime == first.sampleTime + (uint32_t) (i * 128);
                }

                expect (wellFormed, "consecutive mono 128-sample voice packets");
            }

            engine.setMicMuted (true);
            pumpMessages (100);
            const int atMute = sink.count (StreamKind::voice);
            pumpMessages (400);
            expectEquals (sink.count (StreamKind::voice), atMute, "no voice packet while the mic is muted");

            engine.setMicMuted (false);
            expect (waitFor ([&] { return sink.count (StreamKind::voice) >= atMute + 40; }, 3000), "voice resumes");

            {
                // Muted time is skipped in the sequence, so a receiver places the next frame where it belongs.
                std::lock_guard<std::mutex> g (sink.lock);
                AudioPacketView before, after;
                decodeAudioPacket (sink.voice[(size_t) atMute - 1].data(), sink.voice[(size_t) atMute - 1].size(), before);
                decodeAudioPacket (sink.voice[(size_t) atMute].data(), sink.voice[(size_t) atMute].size(), after);
                const auto gap = (uint32_t) (after.sampleTime - before.sampleTime) / 128;
                logMessage ("muted for " + juce::String ((int) gap) + " frames");
                expect (gap > 100 && (uint16_t) (after.seq - before.seq) == (uint16_t) gap, "sequence follows the time muted");
            }

            engine.setChatEnabled (false);
            pumpMessages (50);
            const int atDisable = sink.count (StreamKind::voice);
            pumpMessages (200);
            expectEquals (sink.count (StreamKind::voice), atDisable, "nothing is sent once the chat is disabled");
        }

        beginTest ("chat: the instrument stream is sent only while recording an instrument track");
        {
            const auto dir = studioTestDir().getChildFile ("chat-instrument");
            TwoTrackProject p;
            expect (p.ok);

            StudioEngine engine;
            ChatSink sink;
            sink.attach (engine);
            engine.setMetronome (false, false);
            engine.setMonitor (0.0f, 0.0f);
            engine.setProject (p.snapshot());
            engine.useFakeDevice (256, TestSignal::sine440);
            engine.setChatEnabled (true);
            engine.play();
            pumpMessages (300);
            expect (sink.count (StreamKind::voice) > 50, "voice flows");
            expectEquals (sink.count (StreamKind::instrument), 0, "no instrument stream while only playing");
            engine.stop();
            pumpMessages (50);

            Collected voiceTake;
            voiceTake.attach (engine);
            expect (engine.startRecording (p.track (p.voice), newId(), dir.getChildFile ("voice.wav")).wasOk());
            pumpMessages (400);
            expect (engine.stopRecording().has_value());
            expectEquals (sink.count (StreamKind::instrument), 0, "no instrument stream while recording a voice track");

            Collected instrumentTake;
            instrumentTake.attach (engine);
            expect (engine.startRecording (p.track (p.instrument), newId(), dir.getChildFile ("instrument.wav")).wasOk());
            pumpMessages (500);
            const auto rec = engine.stopRecording();
            expect (rec.has_value());
            pumpMessages (100);
            const int afterTake = sink.count (StreamKind::instrument);
            pumpMessages (300);
            const int voiceNow = sink.count (StreamKind::voice);
            logMessage ("instrument packets during the take: " + juce::String (afterTake) + ", voice packets so far "
                        + juce::String (voiceNow));
            expect (afterTake > 100, "the instrument is sent during an instrument take");
            expectEquals (sink.count (StreamKind::instrument), afterTake, "and stops when the take ends");

            std::lock_guard<std::mutex> g (sink.lock);
            AudioPacketView view;
            expect (decodeAudioPacket (sink.instrument.front().data(), sink.instrument.front().size(), view)
                    && view.flags == AudioFlags::instrument && view.instrumentLen == codedBytesStereo (FrameSize::f128)
                    && view.voiceLen == 0, "stereo instrument packets");
            engine.setChatEnabled (false);
        }
    }
};

static StudioEngineTests studioEngineTests;
}
