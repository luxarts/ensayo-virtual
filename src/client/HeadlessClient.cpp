#include "HeadlessClient.h"
#include "client/SessionController.h"
#include "client/Offline.h"
#include "client/SessionLink.h"
#include "core/MessagePump.h"
#include "ui/Text.h"

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

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>

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

    if (trimmed.isEmpty() || ! trimmed.containsOnly ("0123456789-") || trimmed.length() > 9)
        return false;

    out = trimmed.getIntValue();
    return true;
}

bool parseSeconds (const juce::String& text, double& out)
{
    const auto trimmed = text.trim();

    if (trimmed.isEmpty() || ! trimmed.containsOnly ("0123456789.") || trimmed.indexOfChar ('.') != trimmed.lastIndexOfChar ('.'))
        return false;

    out = trimmed.getDoubleValue();
    return std::isfinite (out);
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
    bool atGiven = false, forGiven = false;

    for (int i = 0; i < args.size(); ++i)
    {
        const auto arg = args[i].trim();
        const bool hasValue = i + 1 < args.size();
        const auto value = hasValue ? args[i + 1].trim() : juce::String();

        const auto needValue = [&] (const juce::String& flag) -> bool
        {
            if (! hasValue || value.isEmpty())
            {
                error = flag + " requires a value";
                return false;
            }

            ++i;
            return true;
        };

        const auto needInt = [&] (const juce::String& flag, int low, int high, int& out) -> bool
        {
            if (! needValue (flag))
                return false;

            if (! parseInt (value, out) || out < low || out > high)
            {
                error = flag + " must be " + juce::String (low) + ".." + juce::String (high);
                return false;
            }

            return true;
        };

        if (arg == "--headless")
        {
            // The dispatcher's own flag; ignored here so the full command line can be passed through.
        }
        else if (arg == "--host")
        {
            if (! needValue (arg)) return std::nullopt;
            o.hostProject = fileArgument (value);
        }
        else if (arg == "--port")
        {
            if (! needInt (arg, 1, 65535, o.hostPort)) return std::nullopt;
        }
        else if (arg == "--join")
        {
            if (! needValue (arg)) return std::nullopt;

            if (! parseSessionLink (value))
            {
                error = "--join: invalid session link: " + value;
                return std::nullopt;
            }

            o.link = value;
        }
        else if (arg == "--name")
        {
            if (! needValue (arg)) return std::nullopt;
            o.name = value;
        }
        else if (arg == "--user")
        {
            if (! needValue (arg)) return std::nullopt;

            if (! isValidId (value.toLowerCase()))
            {
                error = "--user must be 32 lowercase hex characters";
                return std::nullopt;
            }

            o.user = value.toLowerCase();
        }
        else if (arg == "--cache")
        {
            if (! needValue (arg)) return std::nullopt;
            o.cacheDir = fileArgument (value);
        }
        else if (arg == "--fake-audio")
        {
            if (! needValue (arg)) return std::nullopt;

            if (value == "silence")       o.signal = TestSignal::silence;
            else if (value == "sine")     o.signal = TestSignal::sine440;
            else if (value == "click")    o.signal = TestSignal::click;
            else if (value == "loopback") o.signal = TestSignal::loopback;
            else
            {
                error = "--fake-audio must be silence, sine, click or loopback";
                return std::nullopt;
            }
        }
        else if (arg == "--block")
        {
            if (! needInt (arg, 16, 4096, o.blockSize)) return std::nullopt;
        }
        else if (arg == "--latency-in")
        {
            if (! needInt (arg, 0, 48000, o.latencyIn)) return std::nullopt;
        }
        else if (arg == "--latency-out")
        {
            if (! needInt (arg, 0, 48000, o.latencyOut)) return std::nullopt;
        }
        else if (arg == "--asio")
        {
            if (! needValue (arg)) return std::nullopt;
            o.asioDevice = value;
        }
        else if (arg == "--mic")
        {
            if (! needInt (arg, 0, 63, o.micChannel)) return std::nullopt;
        }
        else if (arg == "--inst")
        {
            if (! needInt (arg, 0, 63, o.instrumentChannel)) return std::nullopt;
        }
        else if (arg == "--out")
        {
            if (! needInt (arg, 0, 63, o.outputPair)) return std::nullopt;
        }
        else if (arg == "--mute-monitor")
        {
            o.muteMonitor = true;
        }
        else if (arg == "--calibrate")
        {
            o.calibrate = true;
        }
        else if (arg == "--resume")
        {
            o.resume = true;
        }
        else if (arg == "--add-track")
        {
            if (! needValue (arg)) return std::nullopt;
            const auto kind = trackKindFromString (value);

            if (! kind || *kind == TrackKind::imported)
            {
                error = "--add-track kind must be voice or instrument";
                return std::nullopt;
            }

            const bool hasName = i + 1 < args.size() && args[i + 1].trim().isNotEmpty() && ! args[i + 1].trim().startsWith ("--");

            if (! hasName)
            {
                error = "--add-track requires a kind and a name";
                return std::nullopt;
            }

            o.addTracks.push_back ({ *kind, args[++i].trim() });
        }
        else if (arg == "--record")
        {
            if (! needValue (arg)) return std::nullopt;
            o.recordTrack = value;
        }
        else if (arg == "--at" || arg == "--for")
        {
            if (! needValue (arg)) return std::nullopt;
            double seconds = 0.0;

            if (! parseSeconds (value, seconds) || seconds > 4 * 3600.0)
            {
                error = arg + " must be a number of seconds";
                return std::nullopt;
            }

            if (arg == "--at") { o.recordAt = seconds; atGiven = true; }
            else               { o.recordFor = seconds; forGiven = true; }
        }
        else if (arg == "--import")
        {
            if (! needValue (arg)) return std::nullopt;
            o.importFile = fileArgument (value);
        }
        else if (arg == "--ops")
        {
            if (! needValue (arg)) return std::nullopt;
            o.opsFile = fileArgument (value);
        }
        else if (arg == "--wait-takes")
        {
            if (! needInt (arg, 1, 100000, o.waitTakes)) return std::nullopt;
        }
        else if (arg == "--export")
        {
            if (! needValue (arg)) return std::nullopt;
            o.exportFile = fileArgument (value);
        }
        else if (arg == "--dump-project")
        {
            if (! needValue (arg)) return std::nullopt;
            o.dumpProject = fileArgument (value);
        }
        else if (arg == "--duration")
        {
            if (! needValue (arg)) return std::nullopt;

            if (! parseSeconds (value, o.durationSeconds) || o.durationSeconds <= 0.0)
            {
                error = "--duration must be a positive number of seconds";
                return std::nullopt;
            }

            o.durationGiven = true;
        }
        else
        {
            error = "unknown argument: " + arg;
            return std::nullopt;
        }
    }

    if (o.hostProject != juce::File() && o.link.isNotEmpty())
    {
        error = "--host and --join are mutually exclusive";
        return std::nullopt;
    }

    if (! o.hasSession() && ! o.calibrate)
    {
        error = "--headless needs --host <projectDir> or --join <link> (or --calibrate alone)";
        return std::nullopt;
    }

    if (! o.hasSession() && (! o.addTracks.empty() || o.recordTrack.isNotEmpty() || o.importFile != juce::File()
                             || o.opsFile != juce::File() || o.waitTakes > 0 || o.resume || o.exportFile != juce::File()
                             || o.dumpProject != juce::File()))
    {
        error = "session options need --host or --join";
        return std::nullopt;
    }

    if (o.recordTrack.isNotEmpty() && (! forGiven || o.recordFor <= 0.0))
    {
        error = "--record needs --for <seconds> greater than 0";
        return std::nullopt;
    }

    if (o.recordTrack.isEmpty() && (atGiven || forGiven))
    {
        error = "--at and --for need --record <trackName>";
        return std::nullopt;
    }

    if (o.asioDevice.isNotEmpty() && (o.signal != TestSignal::silence || o.latencyIn != 0 || o.latencyOut != 0))
    {
        error = "--asio cannot be combined with --fake-audio or the fake latencies";
        return std::nullopt;
    }

    return o;
}

//==============================================================================
namespace
{
// Owns the engine and the session; tears them down in the safe order on every exit path.
struct HeadlessRun
{
    StudioEngine engine;
    SessionController session { engine };

    ~HeadlessRun()
    {
        // The callbacks capture locals of runHeadless that may already be gone.
        session.onConnected = nullptr;
        session.onProjectChanged = nullptr;
        session.onTransfersChanged = nullptr;
        session.onPresenceChanged = nullptr;
        session.onDisconnected = nullptr;
        session.onError = nullptr;
        session.onRejected = nullptr;
        session.onImportProgress = nullptr;
        session.onImportFinished = nullptr;
        session.onExportProgress = nullptr;
        session.onExportFinished = nullptr;
        session.onTakeAborted = nullptr;
        session.leave();
        engine.getDeviceManager().closeAudioDevice();
    }
};

bool waitUntil (const std::function<bool()>& done, double timeoutMs, const std::function<bool()>& failed)
{
    const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

    while (! done())
    {
        if (failed() || juce::Time::getMillisecondCounterHiRes() >= deadline)
            return done();

        pumpMessages (10);
    }

    return true;
}
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

    auto run = std::make_unique<HeadlessRun>();
    auto& engine = run->engine;
    auto& session = run->session;

    // Device.
    if (o.asioDevice.isNotEmpty())
    {
        if (! engine.getAsioDeviceNames().contains (o.asioDevice))
            return fail ("ASIO driver not found: " + o.asioDevice + " (available: " + engine.getAsioDeviceNames().joinIntoString (", ") + ")");

        if (const auto openError = engine.openDevice ("ASIO", o.asioDevice, (double) sampleRate, o.blockSize); openError.isNotEmpty())
            return fail ("ASIO device: " + openError);

        auto* device = engine.getDeviceManager().getCurrentAudioDevice();

        if (device == nullptr)
            return fail ("ASIO device did not open");

        const auto inputs = device->getInputChannelNames();
        const auto outputs = device->getOutputChannelNames();
        juce::StringArray sizes, rates;

        for (const auto size : device->getAvailableBufferSizes())
            sizes.add (juce::String (size));

        for (const auto rate : device->getAvailableSampleRates())
            rates.add (juce::String (rate, 0));

        headlessPrint ("device " + device->getName()
                       + " rate " + juce::String (device->getCurrentSampleRate(), 0)
                       + " block " + juce::String (device->getCurrentBufferSizeSamples())
                       + " inputs " + juce::String (inputs.size())
                       + " outputs " + juce::String (outputs.size())
                       + " latencyIn " + juce::String (device->getInputLatencyInSamples())
                       + " latencyOut " + juce::String (device->getOutputLatencyInSamples()));
        headlessPrint ("blocks " + sizes.joinIntoString (" "));
        headlessPrint ("rates " + rates.joinIntoString (" "));

        for (int i = 0; i < inputs.size(); ++i)
            headlessPrint ("input " + juce::String (i) + " " + inputs[i]);

        for (int i = 0; i < outputs.size(); ++i)
            headlessPrint ("output " + juce::String (i) + " " + outputs[i]);
    }
    else
    {
        engine.useFakeDevice (o.blockSize, o.signal, FakeLatency { o.latencyIn, o.latencyOut });

        if (engine.getFakeDevice() == nullptr)
            return fail ("fake audio device: " + engine.takeDeviceError());
    }

    // Give the device a moment to run its first callbacks (a stopped device makes the source swap wait).
    pumpMessages (50);

    if (const auto deviceError = engine.takeDeviceError(); deviceError.isNotEmpty())
        return fail ("audio device: " + deviceError);

    engine.setChannels (o.micChannel, o.instrumentChannel, o.outputPair);

    // A looped-back output must not carry the own inputs, or the monitor feeds back on itself.
    if (o.muteMonitor || o.signal == TestSignal::loopback)
        engine.setMonitor (0.0f, 0.0f);

    session.setMetronome (false, false);   // --record: count-in off; no click in the output either

    if (o.calibrate)
    {
        engine.startCalibration();

        if (! waitUntil ([&] { return ! engine.isCalibrating(); }, 10000.0, [] { return false; }))
            return fail ("calibration did not finish");

        auto* device = engine.getDeviceManager().getCurrentAudioDevice();
        const auto roundTrip = engine.calibrationRoundTrip();

        if (! roundTrip || device == nullptr)
            return fail ("calibration failed: fewer than 5 of 8 clicks came back to the input");

        const int value = LatencyCalibrator::calibrationFor (*roundTrip, device->getInputLatencyInSamples(), device->getOutputLatencyInSamples());
        headlessPrint ("calibration " + juce::String (value));
        session.setCalibration (value);

        if (! o.hasSession())
            return 0;
    }

    // Session.
    const Id user = o.user.isNotEmpty() ? o.user : newId();
    const auto cacheDir = o.cacheDir != juce::File() ? o.cacheDir
                                                     : defaultCacheRoot().getChildFile ("headless").getChildFile (user);
    const SessionIdentity identity { user, o.name, cacheDir };

    bool connected = false, failed = false;
    int rejections = 0;
    int64_t lastVersion = -1;
    juce::String failure;
    std::set<Id> completeSeen, completePrinted, cachedPrinted;
    bool takesKnown = false;
    std::optional<juce::Result> importResult;

    const auto markFailed = [&] (const juce::String& message)
    {
        headlessPrint ("error " + message);

        if (! failed)
        {
            failed = true;
            failure = message;
        }
    };

    // Prints take_complete for transitions seen during the run and cached once a complete take is fully local.
    const auto scanTakes = [&]
    {
        auto* client = session.getClient();
        const auto p = session.project();

        if (client == nullptr || p == nullptr)
            return;

        auto* cache = client->cache();

        for (const auto& take : p->takes)
        {
            if (! take.complete)
                continue;

            if (takesKnown && completeSeen.count (take.id) == 0 && completePrinted.insert (take.id).second)
                headlessPrint ("take_complete " + take.id + " " + juce::String ((juce::int64) take.samples));

            completeSeen.insert (take.id);

            if (cache != nullptr && cache->segmentCount (take.id) >= take.segments && cachedPrinted.insert (take.id).second)
                headlessPrint ("cached " + take.id + " " + juce::String (take.segments));
        }

        takesKnown = true;
    };

    session.onConnected = [&]
    {
        if (connected)
            return;

        connected = true;
        const auto p = session.project();
        lastVersion = p != nullptr ? p->version : 0;
        headlessPrint ("link " + session.getConnectLink().toString());
        headlessPrint ("welcome " + session.getConnectLink().sessionId + (session.getClient()->isHost() ? " host" : " guest"));
        scanTakes();
    };

    session.onProjectChanged = [&]
    {
        if (! connected)
            return;

        if (const auto p = session.project(); p != nullptr && p->version > lastVersion)
        {
            for (auto v = lastVersion + 1; v <= p->version; ++v)
                headlessPrint ("applied " + juce::String ((juce::int64) v));

            lastVersion = p->version;
        }

        scanTakes();
    };

    session.onTransfersChanged = [&] { if (connected) scanTakes(); };
    session.onRejected = [&] (const juce::String& reason) { ++rejections; headlessPrint ("rejected " + reason); };
    session.onDisconnected = [&] (const juce::String& reason) { markFailed ("disconnected: " + reason); };
    session.onError = [&] (const juce::String& message) { markFailed (message); };
    session.onImportFinished = [&] (const juce::String&, const juce::Result& r) { importResult = r; };
    session.onTakeAborted = [&] (const juce::String& reason) { markFailed (reason); };

    if (o.hostProject != juce::File())
    {
        const auto started = session.host (o.hostProject, o.hostPort, identity, o.hostProject.getFileName());

        if (started.failed())
            return fail ("could not host " + o.hostProject.getFullPathName() + " on port " + juce::String (o.hostPort) + ": "
                         + started.getErrorMessage());
    }
    else
    {
        session.join (*parseSessionLink (o.link), identity);
    }

    const auto isFailed = [&] { return failed; };

    if (! waitUntil ([&] { return connected; }, 15000.0, isFailed))
        return failed ? 1 : fail ("timed out waiting for the welcome");

    const double welcomeMs = juce::Time::getMillisecondCounterHiRes();
    const auto project = [&] { return session.project(); };
    const auto myTrack = [&] (const juce::String& trackName) -> Id
    {
        if (const auto p = project())
            for (const auto& t : p->tracks)
                if (t.name == trackName && t.author == user)
                    return t.id;

        return {};
    };

    // Sends one op and waits until a new version arrives or the host rejects it.
    const auto sendAndWait = [&] (const juce::var& op, const juce::String& what) -> bool
    {
        const auto before = project() != nullptr ? project()->version : 0;
        const int rejectedBefore = rejections;
        session.getClient()->sendOp (op);

        if (! waitUntil ([&] { return (project() != nullptr && project()->version > before) || rejections > rejectedBefore; },
                         10000.0, isFailed))
        {
            if (! failed)
                markFailed ("timed out waiting for " + what);

            return false;
        }

        return true;
    };

    // Waits until every take this client began is complete on the host with its region (pending list empty).
    const auto waitOwnTakes = [&] (double timeoutMs) -> bool
    {
        if (! waitUntil ([&] { return session.getClient()->transfers().pendingTakes == 0 && ! session.isImporting(); }, timeoutMs, isFailed))
        {
            if (! failed)
                markFailed ("timed out flushing my takes (" + juce::String (session.getClient()->transfers().pendingUploads)
                            + " segments still to upload)");

            return false;
        }

        scanTakes();
        return true;
    };

    bool ownTakes = false;

    if (o.resume && ! failed)
    {
        ownTakes = true;
        waitOwnTakes (o.durationGiven ? o.durationSeconds * 1000.0 : 120000.0);

        for (const auto& take : session.getResumedTakes())
        {
            const auto* t = project() != nullptr ? project()->findTake (take) : nullptr;

            if (t != nullptr && t->complete && completePrinted.insert (take).second)
                headlessPrint ("take_complete " + take + " " + juce::String ((juce::int64) t->samples));
        }
    }

    for (const auto& track : o.addTracks)
    {
        if (failed)
            break;

        sendAndWait (Ops::addTrack (newId(), track.name, track.kind), "add_track " + track.name);
    }

    if (o.importFile != juce::File() && ! failed)
    {
        ownTakes = true;

        if (const auto started = session.importFile (o.importFile); started.failed())
            markFailed (started.getErrorMessage());
        else if (! waitUntil ([&] { return importResult.has_value(); }, 600000.0, isFailed))
            markFailed ("timed out importing " + o.importFile.getFullPathName());
        else if (importResult->failed())
            markFailed (importResult->getErrorMessage());
    }

    if (o.recordTrack.isNotEmpty() && ! failed)
    {
        ownTakes = true;
        const Id trackId = myTrack (o.recordTrack);

        if (trackId.isEmpty())
        {
            markFailed ("no track of mine named " + o.recordTrack);
        }
        else
        {
            const auto target = (int64_t) std::llround (o.recordAt * sampleRate);
            engine.setPosition (target);

            if (! waitUntil ([&] { return engine.getPosition() == target; }, 3000.0, isFailed))
                markFailed ("the transport did not move to " + juce::String ((juce::int64) target));
            else if (const auto started = session.startTake (trackId); started.failed())
                markFailed ("record: " + started.getErrorMessage());
            else
            {
                // Device time: the take lasts recordFor seconds of audio callbacks, not of wall clock.
                const auto endPosition = target + (int64_t) std::llround (o.recordFor * sampleRate);
                waitUntil ([&] { return engine.getPosition() >= endPosition; }, o.recordFor * 1000.0 + 30000.0, isFailed);

                if (! session.stopTake())
                    markFailed ("record: " + (session.getTakeError().isNotEmpty() ? session.getTakeError()
                                                                                  : ES ("la toma no se guard\xC3\xB3 (demasiado corta)")));
            }
        }
    }

    if (o.opsFile != juce::File() && ! failed)
    {
        juce::StringArray lines;

        if (! o.opsFile.existsAsFile())
            markFailed ("ops file not found: " + o.opsFile.getFullPathName());
        else
            o.opsFile.readLines (lines);

        for (const auto& raw : lines)
        {
            if (failed)
                break;

            const auto line = raw.trim();

            if (line.isEmpty())
                continue;

            const auto op = juce::JSON::parse (line);

            if (! op.isObject() || ! op.hasProperty ("op"))
            {
                markFailed ("invalid op line: " + line);
                break;
            }

            sendAndWait (op, "op " + op["op"].toString());
        }
    }

    if (ownTakes && ! failed && ! o.resume)
        waitOwnTakes (120000.0);

    if (! failed && ! o.resume)
    {
        if (o.waitTakes > 0)
        {
            const auto countReady = [&]
            {
                scanTakes();
                return (int) cachedPrinted.size();
            };

            if (! waitUntil ([&] { return countReady() >= o.waitTakes; }, o.durationGiven ? o.durationSeconds * 1000.0 : 120000.0, isFailed)
                 && ! failed)
                markFailed ("timed out waiting for " + juce::String (o.waitTakes) + " complete takes (have "
                            + juce::String ((int) cachedPrinted.size()) + ")");
        }
        else
        {
            const double endMs = welcomeMs + o.durationSeconds * 1000.0;
            waitUntil ([&] { return juce::Time::getMillisecondCounterHiRes() >= endMs; }, o.durationSeconds * 1000.0 + 1000.0, isFailed);
        }
    }

    const auto p = project();

    if (o.exportFile != juce::File() && p != nullptr && ! failed)
    {
        auto* cache = session.getClient()->cache();

        if (cache == nullptr)
            markFailed ("no local cache to export from");
        else if (const auto exported = exportMix (p, *cache, o.exportFile); exported.failed())
            markFailed ("export: " + exported.getErrorMessage());
        else
            headlessPrint ("exported " + o.exportFile.getFullPathName());
    }

    if (o.dumpProject != juce::File() && p != nullptr)
    {
        o.dumpProject.getParentDirectory().createDirectory();

        if (! o.dumpProject.replaceWithText (toJson (*p)))
            markFailed ("could not write " + o.dumpProject.getFullPathName());
    }

    if (p != nullptr)
        headlessPrint ("project_hash " + contentHash (*p));

    if (o.asioDevice.isNotEmpty())
        headlessPrint ("callbacks " + juce::String ((juce::int64) engine.getCallbackCount()));

    run = nullptr;
    pumpMessages (20);
    return failed ? 1 : 0;
}
}
