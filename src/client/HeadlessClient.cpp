#include "HeadlessClient.h"
#include "client/AudioEngine.h"
#include "client/SessionClient.h"
#include "client/SessionLink.h"
#include "server/ServerHost.h"
#include "core/MessagePump.h"

#include <juce_events/juce_events.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace ensayo
{
//==============================================================================
// Stdout from a GUI-subsystem exe: the CRT's std::cout is bound to nothing unless the parent redirected the
// standard handles, so write straight to the inherited handle. GetStdHandle returns the pipe/file the parent
// gave us even for a /SUBSYSTEM:WINDOWS process; when there is none, try attaching to the parent's console.
void headlessPrint (const juce::String& line)
{
    const auto text = (line + "\n").toStdString();

   #if JUCE_WINDOWS
    static HANDLE handle = []
    {
        HANDLE h = GetStdHandle (STD_OUTPUT_HANDLE);

        if (h == nullptr || h == INVALID_HANDLE_VALUE)
        {
            if (AttachConsole (ATTACH_PARENT_PROCESS))
                h = GetStdHandle (STD_OUTPUT_HANDLE);
        }

        return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
    }();

    if (handle != nullptr)
    {
        DWORD written = 0;
        WriteFile (handle, text.data(), (DWORD) text.size(), &written, nullptr);
        FlushFileBuffers (handle);
        return;
    }
   #endif

    std::cout << text << std::flush;
}

//==============================================================================
namespace
{
bool parseInt (const juce::String& text, int& out)
{
    const auto trimmed = text.trim();

    if (trimmed.isEmpty() || ! trimmed.containsOnly ("0123456789-"))
        return false;

    out = trimmed.getIntValue();
    return true;
}

// A link whose id is the literal NEW means "create a session on that host:port". parseSessionLink needs a
// six-character id, so NEW is swapped for a placeholder and restored afterwards.
std::optional<SessionLink> parseHeadlessLink (const juce::String& text)
{
    const auto trimmed = text.trim().unquoted().trimCharactersAtEnd ("/");

    if (trimmed.endsWithIgnoreCase ("/NEW"))
    {
        if (auto link = parseSessionLink (trimmed.dropLastCharacters (3) + "AAAAAA"))
        {
            link->sessionId = "NEW";
            return link;
        }

        return std::nullopt;
    }

    return parseSessionLink (trimmed);
}

juce::File fileArgument (const juce::String& text)
{
    const auto trimmed = text.trim().unquoted();

    if (juce::File::isAbsolutePath (trimmed))
        return juce::File (trimmed);

    return juce::File::getCurrentWorkingDirectory().getChildFile (trimmed);
}
}

std::optional<HeadlessOptions> parseHeadlessOptions (const juce::StringArray& args, juce::String& error)
{
    HeadlessOptions o;
    error.clear();

    for (int i = 0; i < args.size(); ++i)
    {
        const auto arg = args[i].trim();
        const bool hasValue = i + 1 < args.size();
        const auto value = hasValue ? args[i + 1].trim() : juce::String();

        const auto needValue = [&] (const char* name) -> bool
        {
            if (! hasValue || value.isEmpty())
            {
                error = juce::String (name) + " requires a value";
                return false;
            }

            ++i;
            return true;
        };

        if (arg == "--headless")
        {
            // The dispatcher's own flag; ignored here so the full command line can be passed through.
        }
        else if (arg == "--host")
        {
            o.host = true;
        }
        else if (arg == "--port")
        {
            if (! needValue ("--port")) return std::nullopt;

            if (! parseInt (value, o.hostPort) || o.hostPort <= 0 || o.hostPort > 65535)
            {
                error = "--port must be 1..65535";
                return std::nullopt;
            }
        }
        else if (arg == "--join")
        {
            if (! needValue ("--join")) return std::nullopt;

            if (! parseHeadlessLink (value))
            {
                error = "--join: invalid session link: " + value;
                return std::nullopt;
            }

            o.link = value;
        }
        else if (arg == "--name")
        {
            if (! needValue ("--name")) return std::nullopt;
            o.name = value;
        }
        else if (arg == "--role")
        {
            if (! needValue ("--role")) return std::nullopt;

            if (auto role = roleFromString (value))
                o.role = *role;
            else
            {
                error = "--role must be voice, instrument or both";
                return std::nullopt;
            }
        }
        else if (arg == "--block")
        {
            if (! needValue ("--block")) return std::nullopt;

            if (! parseInt (value, o.blockSize) || (o.blockSize != 64 && o.blockSize != 128 && o.blockSize != 256))
            {
                error = "--block must be 64, 128 or 256";
                return std::nullopt;
            }
        }
        else if (arg == "--fake-audio")
        {
            if (! needValue ("--fake-audio")) return std::nullopt;

            if (value == "silence")      o.signal = TestSignal::silence;
            else if (value == "sine")    o.signal = TestSignal::sine440;
            else if (value == "click")   o.signal = TestSignal::click;
            else
            {
                error = "--fake-audio must be silence, sine or click";
                return std::nullopt;
            }
        }
        else if (arg == "--probe")
        {
            if (! needValue ("--probe")) return std::nullopt;
            o.probeLog = fileArgument (value);
        }
        else if (arg == "--emit-log")
        {
            if (! needValue ("--emit-log")) return std::nullopt;
            o.emitLog = fileArgument (value);
        }
        else if (arg == "--compare")
        {
            if (! needValue ("--compare")) return std::nullopt;
            o.compareLog = fileArgument (value);
        }
        else if (arg == "--track")
        {
            if (! needValue ("--track")) return std::nullopt;
            o.trackFile = fileArgument (value);
        }
        else if (arg == "--play")
        {
            o.playTrack = true;
        }
        else if (arg == "--simulate-loss")
        {
            if (! needValue ("--simulate-loss")) return std::nullopt;

            if (! parseInt (value, o.lossPercent) || o.lossPercent < 0 || o.lossPercent > 100)
            {
                error = "--simulate-loss must be 0..100";
                return std::nullopt;
            }
        }
        else if (arg == "--duration")
        {
            if (! needValue ("--duration")) return std::nullopt;

            if (! value.containsOnly ("0123456789.") || value.getDoubleValue() <= 0.0)
            {
                error = "--duration must be a positive number of seconds";
                return std::nullopt;
            }

            o.durationSeconds = value.getDoubleValue();
        }
        else if (arg == "--stats")
        {
            if (! needValue ("--stats")) return std::nullopt;
            o.statsLog = fileArgument (value);
        }
        else
        {
            error = "unknown argument: " + arg;
            return std::nullopt;
        }
    }

    if (o.host && o.link.isNotEmpty())
    {
        error = "--host and --join are mutually exclusive";
        return std::nullopt;
    }

    if (! o.host && o.link.isEmpty())
    {
        error = "--headless needs --host or --join <link>";
        return std::nullopt;
    }

    if (o.playTrack && o.trackFile == juce::File())
    {
        error = "--play requires --track <file>";
        return std::nullopt;
    }

    return o;
}

//==============================================================================
std::vector<ClickEvent> readClickLog (const juce::File& file)
{
    std::vector<ClickEvent> events;
    juce::StringArray lines;
    file.readLines (lines);

    for (const auto& raw : lines)
    {
        const auto line = raw.trim();

        if (line.isEmpty() || ! juce::CharacterFunctions::isDigit (line[0]))
            continue;

        auto parts = juce::StringArray::fromTokens (line, ",", "");

        if (parts.size() < 2)
            continue;

        events.push_back ({ (int64_t) parts[1].trim().getLargeIntValue(), parts[0].trim().getDoubleValue() });
    }

    std::stable_sort (events.begin(), events.end(), [] (const ClickEvent& a, const ClickEvent& b) { return a.qpcMs < b.qpcMs; });
    return events;
}

std::vector<double> pairClickLatencies (const std::vector<ClickEvent>& emittedIn, const std::vector<ClickEvent>& detected,
                                        double maxLatencyMs)
{
    auto emitted = emittedIn;
    std::stable_sort (emitted.begin(), emitted.end(), [] (const ClickEvent& a, const ClickEvent& b) { return a.qpcMs < b.qpcMs; });

    std::vector<double> latencies;

    for (const auto& d : detected)
    {
        // Latest emitted click at or before the detection.
        auto it = std::upper_bound (emitted.begin(), emitted.end(), d.qpcMs,
                                    [] (double t, const ClickEvent& e) { return t < e.qpcMs; });

        if (it == emitted.begin())
            continue;

        --it;
        const double latency = d.qpcMs - it->qpcMs;

        if (latency >= 0.0 && latency <= maxLatencyMs)
            latencies.push_back (latency);
    }

    return latencies;
}

juce::String formatLatencyLine (std::vector<double> latencies)
{
    if (latencies.empty())
        return "latency n 0";

    std::sort (latencies.begin(), latencies.end());
    const auto at = [&] (double fraction)
    {
        const auto index = (size_t) juce::jlimit (0, (int) latencies.size() - 1, (int) std::floor (fraction * (double) (latencies.size() - 1) + 0.5));
        return latencies[index];
    };

    return "latency median " + juce::String (at (0.5), 2) + " p95 " + juce::String (at (0.95), 2)
         + " n " + juce::String ((int) latencies.size());
}

//==============================================================================
namespace
{
// Click events collected from the device thread and flushed to a CSV from the message thread.
class ClickLog
{
public:
    void setFile (const juce::File& f)
    {
        file = f;

        if (file != juce::File())
        {
            file.getParentDirectory().createDirectory();
            file.replaceWithText ("");
        }
    }

    void add (ClickEvent e)
    {
        const juce::ScopedLock sl (lock);
        pending.push_back (e);
    }

    void flush()
    {
        std::vector<ClickEvent> batch;
        {
            const juce::ScopedLock sl (lock);
            batch.swap (pending);
        }

        for (const auto& e : batch)
        {
            all.push_back (e);

            if (file != juce::File())
                file.appendText (juce::String (e.qpcMs, 3) + "," + juce::String ((juce::int64) e.sampleIndex) + "\n");
        }
    }

    const std::vector<ClickEvent>& events() const noexcept { return all; }

private:
    juce::CriticalSection lock;
    std::vector<ClickEvent> pending, all;
    juce::File file;
};

juce::String jsonBool (bool b) { return b ? "true" : "false"; }
}

int runHeadless (const HeadlessOptions& o)
{
    const auto fail = [] (const juce::String& message)
    {
        headlessPrint ("error " + message);
        return 1;
    };

    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr || ! juce::MessageManager::getInstance()->isThisTheMessageThread())
        return fail ("runHeadless must be called on the message thread with JUCE initialised");

    const auto frameSize = frameSizeForSamples (o.blockSize).value_or (FrameSize::f128);

    std::unique_ptr<ServerHost> host;

    if (o.host)
    {
        ServerConfig config;
        config.tcpPort = o.hostPort;
        config.udpPort = o.hostPort;
        config.frameSize = frameSize;
        host = std::make_unique<ServerHost> (config);

        const auto result = host->start();

        if (result.failed())
            return fail ("could not start the embedded server on port " + juce::String (o.hostPort) + ": " + result.getErrorMessage());
    }

    // The engine is destroyed (device closed) before the host and the client thanks to declaration order.
    struct Guard
    {
        std::unique_ptr<ServerHost>& host;
        AudioEngine engine;
        SessionClient client;

        ~Guard()
        {
            engine.setTransport (nullptr);
            engine.getDeviceManager().closeAudioDevice();   // stops the frame hook and the probe before host/client go away
            client.leave();

            if (host != nullptr)
                host->stop();
        }
    } g { host };

    AudioEngine& engine = g.engine;
    SessionClient& client = g.client;

    engine.setRole (o.role);
    engine.setChannels (0, 1, 0);
    engine.setSimulatedLossPercent (o.lossPercent);

    // The probe listens to the device output; local monitoring would put our own input clicks there too.
    if (o.probeLog != juce::File())
        engine.setMonitor (ChannelGain { 0.0f, 0.0f, true });

    if (host != nullptr)
        engine.setFrameHook ([&] { host->tick(); });

    engine.useFakeDevice (o.blockSize, o.signal);

    if (const auto deviceError = engine.takeDeviceError(); deviceError.isNotEmpty())
        return fail ("fake audio device: " + deviceError);

    auto* fake = engine.getFakeDevice();

    if (fake == nullptr)
        return fail ("fake audio device not available");

    ClickLog emitted, detected;
    ClickProbe probe;

    emitted.setFile (o.emitLog);
    detected.setFile (o.probeLog);

    fake->onClickEmitted = [&] (ClickEvent e) { emitted.add (e); };

    if (o.probeLog != juce::File())
    {
        probe.onClickDetected = [&] (ClickEvent e) { detected.add (e); };
        fake->onOutputBlock = [&] (const float* L, const float* R, int n, int64_t first, double qpcMs)
        {
            probe.process (L, R, n, first, qpcMs);
        };
    }

    // Session.
    bool joined = false, failed = false, playSent = false, uploadStarted = false;
    juce::String failure;
    int lastParticipants = -1;
    juce::String sessionLinkText;

    client.onJoined = [&]
    {
        joined = true;
        const auto& s = client.getState();

        SessionLink link;
        link.host = s.host;
        link.port = s.port;
        link.sessionId = s.sessionId;
        sessionLinkText = link.toString();

        headlessPrint ("link " + sessionLinkText);
        headlessPrint ("joined " + s.sessionId + " client " + juce::String ((juce::int64) s.clientId));
        lastParticipants = (int) s.participants.size();
        headlessPrint ("participants " + juce::String (lastParticipants));

        engine.setTransport (&client);

        if (o.trackFile != juce::File() && ! uploadStarted)
        {
            uploadStarted = true;

            if (! o.trackFile.existsAsFile())
                headlessPrint ("error track file not found: " + o.trackFile.getFullPathName());
            else
                client.uploadTrack (o.trackFile);
        }
    };

    client.onStateChanged = [&] (const SessionState& s)
    {
        if (joined && s.connected && (int) s.participants.size() != lastParticipants)
        {
            lastParticipants = (int) s.participants.size();
            headlessPrint ("participants " + juce::String (lastParticipants));
        }

        if (s.connected && o.playTrack && s.transport.loaded && ! playSent)
        {
            playSent = true;
            client.sendTransport ("play");
        }
    };

    client.onError = [&] (const juce::String& message)
    {
        if (! failed)
        {
            failed = true;
            failure = message;
            headlessPrint ("error " + message);
        }
    };

    if (host != nullptr)
    {
        client.create ("127.0.0.1", o.hostPort, o.name, o.role);
    }
    else
    {
        const auto link = parseHeadlessLink (o.link);

        if (! link)
            return fail ("invalid link: " + o.link);

        if (link->sessionId == "NEW")
            client.create (link->host, link->port, o.name, o.role);
        else
            client.join (*link, o.name, o.role);
    }

    // Wait for the join, then stay for the requested duration while writing stats once per second.
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    const double joinDeadlineMs = startMs + 10000.0;

    while (! joined && ! failed && juce::Time::getMillisecondCounterHiRes() < joinDeadlineMs)
        pumpMessages (10);

    if (failed)
        return 1;

    if (! joined)
        return fail ("timed out waiting to join");

    if (o.statsLog != juce::File())
    {
        o.statsLog.getParentDirectory().createDirectory();
        o.statsLog.replaceWithText ("");
    }

    const double sessionStartMs = juce::Time::getMillisecondCounterHiRes();
    const double endMs = sessionStartMs + o.durationSeconds * 1000.0;
    double nextStatsMs = sessionStartMs + 1000.0;

    const auto writeStats = [&]
    {
        emitted.flush();
        detected.flush();

        if (o.statsLog == juce::File())
            return;

        const auto& s = client.getState();
        const auto stats = client.receiveBuffer().getStats();
        const double t = (juce::Time::getMillisecondCounterHiRes() - sessionStartMs) / 1000.0;

        juce::String line;
        line << "{\"t\":" << juce::String (t, 3)
             << ",\"pingMs\":" << s.pingMs
             << ",\"jitterBlocks\":" << stats.sizeBlocks
             << ",\"underruns\":" << (juce::int64) stats.underruns
             << ",\"late\":" << (juce::int64) stats.late
             << ",\"participants\":" << (int) s.participants.size()
             << ",\"trackPlaying\":" << jsonBool (s.transport.playing)
             << ",\"trackPosition\":" << (juce::int64) client.getTrackPosition()
             << "}\n";
        o.statsLog.appendText (line);
    };

    while (! failed)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();

        if (now >= endMs)
            break;

        if (now >= nextStatsMs)
        {
            writeStats();
            nextStatsMs += 1000.0;
        }

        pumpMessages ((int) juce::jlimit (1.0, 50.0, std::min (endMs, nextStatsMs) - now));
    }

    engine.setTransport (nullptr);
    client.leave();
    pumpMessages (20);

    emitted.flush();
    detected.flush();

    if (o.compareLog != juce::File())
    {
        const auto otherEmitted = readClickLog (o.compareLog);
        headlessPrint (formatLatencyLine (pairClickLatencies (otherEmitted, detected.events())));
    }

    return failed ? 1 : 0;
}
}
