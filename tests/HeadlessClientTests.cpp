#include "client/HeadlessClient.h"
#include "client/AudioEngine.h"
#include "client/SessionClient.h"
#include "server/ServerHost.h"
#include "MessagePump.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <vector>

namespace ensayo
{
class HeadlessClientTests : public juce::UnitTest
{
public:
    HeadlessClientTests() : juce::UnitTest ("HeadlessClient", "EnsayoVirtual") {}

    static constexpr int smokePort = 22180, latencyPort = 22181;

    static void say (const juce::String& message)
    {
        std::cout << message << std::endl;   // UnitTest::logMessage only reaches the debugger
    }

    static bool waitFor (const std::function<bool()>& predicate, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (predicate())
                return true;

            pumpMessages (10);
        }

        return predicate();
    }

    static juce::File tempDir()
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("headless");
        dir.createDirectory();
        return dir;
    }

    void testParsing()
    {
        beginTest ("parseHeadlessOptions: full argument set");
        {
            juce::String error;
            const auto o = parseHeadlessOptions (juce::StringArray { "--headless", "--join", "ensayo://127.0.0.1:22150/NEW", "--name", "Ana",
                                                                     "--role", "voice", "--fake-audio", "click", "--block", "64",
                                                                     "--emit-log", "a.csv", "--probe", "b.csv", "--compare", "c.csv",
                                                                     "--track", "t.wav", "--play", "--simulate-loss", "7",
                                                                     "--duration", "2.5", "--stats", "s.json" }, error);
            expect (o.has_value(), "parse failed: " + error);

            if (o)
            {
                expect (! o->host);
                expectEquals (o->link, juce::String ("ensayo://127.0.0.1:22150/NEW"));
                expectEquals (o->name, juce::String ("Ana"));
                expect (o->role == Role::voice);
                expect (o->signal == TestSignal::click);
                expectEquals (o->blockSize, 64);
                expectEquals (o->emitLog.getFileName(), juce::String ("a.csv"));
                expect (o->emitLog.isAbsolutePath (o->emitLog.getFullPathName()));
                expectEquals (o->probeLog.getFileName(), juce::String ("b.csv"));
                expectEquals (o->compareLog.getFileName(), juce::String ("c.csv"));
                expectEquals (o->trackFile.getFileName(), juce::String ("t.wav"));
                expect (o->playTrack);
                expectEquals (o->lossPercent, 7);
                expectEquals (o->durationSeconds, 2.5);
                expectEquals (o->statsLog.getFileName(), juce::String ("s.json"));
            }
        }

        beginTest ("parseHeadlessOptions: host mode and defaults");
        {
            juce::String error;
            const auto o = parseHeadlessOptions (juce::StringArray { "--host", "--port", "22151" }, error);
            expect (o.has_value(), error);

            if (o)
            {
                expect (o->host);
                expectEquals (o->hostPort, 22151);
                expectEquals (o->name, juce::String ("Headless"));
                expect (o->role == Role::both);
                expect (o->signal == TestSignal::silence);
                expectEquals (o->blockSize, 128);
                expectEquals (o->durationSeconds, 10.0);
                expectEquals (o->lossPercent, 0);
                expect (o->probeLog == juce::File());
                expect (o->statsLog == juce::File());
                expect (! o->playTrack);
            }

            const auto d = parseHeadlessOptions (juce::StringArray { "--host" }, error);
            expect (d.has_value() && d->hostPort == defaultPort);
        }

        beginTest ("parseHeadlessOptions: errors");
        {
            const auto rejects = [&] (const juce::StringArray& args, const juce::String& why)
            {
                juce::String error;
                const auto o = parseHeadlessOptions (args, error);
                expect (! o.has_value(), "should reject: " + why);
                expect (error.isNotEmpty(), "no error text for: " + why);
            };

            rejects ({ "--host", "--role", "x" }, "bad role");
            rejects ({ "--host", "--block", "100" }, "bad block");
            rejects ({ "--host", "--fake-audio", "noise" }, "bad signal");
            rejects ({ "--host", "--port", "70000" }, "bad port");
            rejects ({ "--host", "--port", "abc" }, "non-numeric port");
            rejects ({ "--host", "--simulate-loss", "101" }, "loss out of range");
            rejects ({ "--host", "--duration", "0" }, "zero duration");
            rejects ({ "--host", "--duration" }, "missing value");
            rejects ({ "--join", "not a link" }, "bad link");
            rejects ({ "--join", "ensayo://127.0.0.1:22150/NEWER" }, "bad NEW link");

            juce::String error;
            const auto n = parseHeadlessOptions ({ "--join", "127.0.0.1:22150/new" }, error);
            expect (n.has_value() && n->link == "127.0.0.1:22150/new", "NEW link without scheme: " + error);
            rejects ({ "--join", "ensayo://1.2.3.4:22140/ABCDEF", "--host" }, "host and join together");
            rejects ({ "--name", "Solo" }, "neither host nor join");
            rejects ({ "--host", "--play" }, "play without track");
            rejects ({ "--host", "--bogus" }, "unknown argument");
        }
    }

    void testLatencyHelpers()
    {
        beginTest ("pairClickLatencies and formatLatencyLine");
        {
            const std::vector<ClickEvent> emitted { { 0, 1000.0 }, { 12000, 1250.0 }, { 24000, 1500.0 } };
            const std::vector<ClickEvent> detected { { 100, 1030.0 }, { 300, 1280.0 }, { 500, 1900.0 }, { 700, 990.0 } };

            const auto latencies = pairClickLatencies (emitted, detected);
            expectEquals ((int) latencies.size(), 2);   // 1900 is > 250 ms after the last emit; 990 precedes every emit

            if (latencies.size() == 2)
            {
                expectWithinAbsoluteError (latencies[0], 30.0, 1e-9);
                expectWithinAbsoluteError (latencies[1], 30.0, 1e-9);
            }

            expectEquals (formatLatencyLine ({}), juce::String ("latency n 0"));
            expectEquals (formatLatencyLine ({ 30.0, 10.0, 20.0 }), juce::String ("latency median 20.00 p95 30.00 n 3"));
        }

        beginTest ("readClickLog");
        {
            const auto file = tempDir().getChildFile ("read-click-log.csv");
            file.replaceWithText ("# comment\n1500.250,128\n\n1250.5,64\nbad line\n");
            const auto events = readClickLog (file);
            expectEquals ((int) events.size(), 2);

            if (events.size() == 2)
            {
                expectWithinAbsoluteError (events[0].qpcMs, 1250.5, 1e-9);   // sorted by time
                expectEquals ((juce::int64) events[0].sampleIndex, (juce::int64) 64);
                expectWithinAbsoluteError (events[1].qpcMs, 1500.25, 1e-9);
            }

            file.deleteFile();
        }
    }

    void testSmoke()
    {
        beginTest ("runHeadless --host smoke");
        {
            juce::String error;
            const auto stats = tempDir().getChildFile ("smoke-stats.jsonl");
            stats.deleteFile();

            const auto o = parseHeadlessOptions (juce::StringArray { "--host", "--port", juce::String (smokePort), "--duration", "1.2",
                                                                     "--fake-audio", "silence", "--stats", stats.getFullPathName() }, error);
            expect (o.has_value(), error);

            if (o)
            {
                const auto t0 = juce::Time::getMillisecondCounter();
                expectEquals (runHeadless (*o), 0);
                const auto elapsed = juce::Time::getMillisecondCounter() - t0;
                expect (elapsed >= 1000 && elapsed < 6000, "unexpected run time " + juce::String (elapsed) + " ms");

                juce::StringArray lines;
                stats.readLines (lines);
                lines.removeEmptyStrings();
                expect (lines.size() >= 1, "no stats line written");

                if (lines.size() >= 1)
                {
                    const auto parsed = juce::JSON::parse (lines[0]);
                    expect (parsed.isObject(), "stats line is not JSON: " + lines[0]);
                    expectEquals ((int) parsed["participants"], 1);
                    expect (parsed.hasProperty ("pingMs") && parsed.hasProperty ("jitterBlocks") && parsed.hasProperty ("underruns")
                             && parsed.hasProperty ("late") && parsed.hasProperty ("trackPlaying") && parsed.hasProperty ("trackPosition"));
                }
            }

            stats.deleteFile();
        }

        beginTest ("runHeadless --join to a dead server fails cleanly");
        {
            juce::String error;
            const auto o = parseHeadlessOptions (juce::StringArray { "--join", "ensayo://127.0.0.1:22189/ABCDEF", "--duration", "1" }, error);
            expect (o.has_value(), error);

            if (o)
                expectEquals (runHeadless (*o), 1);
        }
    }

    // The core measurement of the project: A emits clicks on its fake inputs, both are joined to one in-process
    // server, and B's probe sees the clicks in its device output. Both devices stamp QPC time, so detection time
    // minus emission time is the real one-way path: A block → UDP → server jitter buffer → mix → UDP → B jitter
    // buffer → B output block.
    void testTwoClientLatency()
    {
        beginTest ("two in-process clients: click latency through the server");

        ServerConfig config;
        config.tcpPort = latencyPort;
        config.udpPort = latencyPort;
        config.frameSize = FrameSize::f128;
        config.uploadDirectory = tempDir().getChildFile ("uploads");

        ServerHost host (config);
        const auto started = host.start();
        expect (started.wasOk(), "server did not start: " + started.getErrorMessage());

        if (started.failed())
            return;

        host.startInternalClock();

        AudioEngine engineA, engineB;
        SessionClient a, b;

        std::vector<ClickEvent> emitted, detected;
        juce::CriticalSection lock;
        ClickProbe probe;

        engineA.setRole (Role::both);
        engineB.setRole (Role::both);
        engineB.setMonitor (ChannelGain { 0.0f, 0.0f, true });   // B's output must only contain the server mix

        engineA.useFakeDevice (128, TestSignal::click);
        engineB.useFakeDevice (128, TestSignal::silence);
        expect (engineA.getFakeDevice() != nullptr && engineB.getFakeDevice() != nullptr);

        if (engineA.getFakeDevice() == nullptr || engineB.getFakeDevice() == nullptr)
            return;

        engineA.getFakeDevice()->onClickEmitted = [&] (ClickEvent e) { const juce::ScopedLock sl (lock); emitted.push_back (e); };
        probe.onClickDetected = [&] (ClickEvent e) { const juce::ScopedLock sl (lock); detected.push_back (e); };
        engineB.getFakeDevice()->onOutputBlock = [&] (const float* L, const float* R, int n, int64_t first, double qpc)
        {
            probe.process (L, R, n, first, qpc);
        };

        bool aJoined = false, bJoined = false;
        juce::String errors;
        a.onJoined = [&] { aJoined = true; engineA.setTransport (&a); };
        b.onJoined = [&] { bJoined = true; engineB.setTransport (&b); };
        a.onError = [&] (const juce::String& e) { errors << "A: " << e << " "; };
        b.onError = [&] (const juce::String& e) { errors << "B: " << e << " "; };

        a.create ("127.0.0.1", latencyPort, "A", Role::both);
        expect (waitFor ([&] { return aJoined; }, 5000), "A did not join: " + errors);

        SessionLink link;
        link.host = "127.0.0.1";
        link.port = latencyPort;
        link.sessionId = a.getState().sessionId;
        b.join (link, "B", Role::both);
        expect (waitFor ([&] { return bJoined; }, 5000), "B did not join: " + errors);
        expect (waitFor ([&] { return a.getState().participants.size() == 2 && b.getState().participants.size() == 2; }, 5000));

        // Let the clicks flow for 3 s.
        const double runStart = juce::Time::getMillisecondCounterHiRes();

        {
            const juce::ScopedLock sl (lock);
            emitted.clear();
            detected.clear();
        }

        while (juce::Time::getMillisecondCounterHiRes() < runStart + 3000.0)
            pumpMessages (20);

        std::vector<ClickEvent> emittedCopy, detectedCopy;
        {
            const juce::ScopedLock sl (lock);
            emittedCopy = emitted;
            detectedCopy = detected;
        }

        engineA.setTransport (nullptr);
        engineB.setTransport (nullptr);
        engineA.getDeviceManager().closeAudioDevice();
        engineB.getDeviceManager().closeAudioDevice();
        a.leave();
        b.leave();
        pumpMessages (50);
        host.stop();

        const auto bufferStats = b.receiveBuffer().getStats();
        expect (errors.isEmpty(), "session errors: " + errors);
        expect (emittedCopy.size() >= 10, "A emitted only " + juce::String ((int) emittedCopy.size()) + " clicks");
        expect (detectedCopy.size() >= 5, "B detected only " + juce::String ((int) detectedCopy.size()) + " clicks");

        const auto latencies = pairClickLatencies (emittedCopy, detectedCopy);
        const auto line = formatLatencyLine (latencies);
        say ("  " + line + "  (emitted " + juce::String ((int) emittedCopy.size()) + ", detected " + juce::String ((int) detectedCopy.size())
             + "; B buffer size " + juce::String (bufferStats.sizeBlocks) + " underruns " + juce::String ((juce::int64) bufferStats.underruns)
             + " late " + juce::String ((juce::int64) bufferStats.late) + " resyncs " + juce::String ((juce::int64) bufferStats.resyncs) + ")");

        expect (latencies.size() >= 5, "fewer than 5 clicks paired: " + line);

        if (! latencies.empty())
        {
            auto sorted = latencies;
            std::sort (sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            expect (median < 60.0, "median one-way latency too high: " + line);
            expect (median > 0.0, "non-positive latency: " + line);
        }
    }

    void runTest() override
    {
        testParsing();
        testLatencyHelpers();
        testSmoke();
        testTwoClientLatency();
    }
};

static HeadlessClientTests headlessClientTests;
}
