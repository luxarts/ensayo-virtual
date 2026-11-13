#include "client/HeadlessClient.h"
#include "client/SessionController.h"
#include "client/ProjectClient.h"
#include "client/Offline.h"
#include "server/ProjectHost.h"
#include "core/Segments.h"
#include "MessagePump.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <functional>
#include <set>

namespace ensayo
{
class HeadlessClientTests : public juce::UnitTest
{
public:
    HeadlessClientTests() : juce::UnitTest ("HeadlessClient", "EnsayoVirtual") {}

    static constexpr int recordPort = 22252, resumePort = 22253, hostPort = 22254, deadPort = 22255, growPort = 22260;

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

    static juce::File tempDir (const juce::String& name)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("headless").getChildFile (name);
        dir.deleteRecursively();
        dir.createDirectory();
        return dir;
    }

    std::optional<HeadlessOptions> parse (const juce::StringArray& args)
    {
        juce::String error;
        auto o = parseHeadlessOptions (args, error);
        expect (o.has_value(), "parse failed: " + error);
        return o;
    }

    void rejects (const juce::StringArray& args, const juce::String& why)
    {
        juce::String error;
        const auto o = parseHeadlessOptions (args, error);
        expect (! o.has_value(), "should reject: " + why);
        expect (error.isNotEmpty(), "no error text for: " + why);
    }

    void testParsing()
    {
        beginTest ("participant mixes map to the current clientIds; a reused clientId gets the new user's level");
        {
            const Id ana = newId(), beto = newId(), carla = newId();
            std::map<Id, SessionController::ParticipantMix> mixes { { ana, { 0.5f, false } }, { beto, { 2.0f, true } } };
            const std::vector<Presence> presence { { ana, "Ana", true, 7 }, { beto, "Beto", false, 0 }, { carla, "Carla", true, 9 },
                                                  { newId(), "Sin UDP", true, 0 } };
            const auto gains = SessionController::clientGains (presence, mixes);
            expectEquals ((int) gains.size(), 2);
            expect (gains[0].clientId == 7 && gains[0].gain == 0.5f && ! gains[0].muted);
            expect (gains[1].clientId == 9 && gains[1].gain == 1.0f && ! gains[1].muted, "no entry: unity, so 9 forgets any older user");

            // Beto reconnects with Ana's old clientId: the level follows the user, not the number.
            const auto after = SessionController::clientGains ({ { beto, "Beto", true, 7 } }, mixes);
            expect (after.size() == 1 && after[0].clientId == 7 && after[0].gain == 2.0f && after[0].muted);
        }

        beginTest ("parseHeadlessOptions: every v2 option");
        {
            const auto user = newId();
            const auto o = parse ({ "--headless", "--join", "ensayo://127.0.0.1:22150/ABCDEF", "--name", "Ana", "--user", user,
                                    "--cache", "cache-dir", "--fake-audio", "loopback", "--block", "512", "--latency-in", "200",
                                    "--latency-out", "264", "--add-track", "voice", "Voz", "--add-track", "instrument", "Guitarra",
                                    "--record", "Voz", "--at", "1.5", "--for", "2", "--import", "a.wav", "--ops", "ops.jsonl",
                                    "--wait-takes", "3", "--export", "mix.wav", "--dump-project", "p.json", "--duration", "2.5",
                                    "--calibrate", "--resume", "--mute-monitor" });

            if (o)
            {
                expect (o->hostProject == juce::File());
                expectEquals (o->link, juce::String ("ensayo://127.0.0.1:22150/ABCDEF"));
                expectEquals (o->name, juce::String ("Ana"));
                expectEquals (o->user, user);
                expectEquals (o->cacheDir.getFileName(), juce::String ("cache-dir"));
                expect (o->cacheDir.isAbsolutePath (o->cacheDir.getFullPathName()));
                expect (o->signal == TestSignal::loopback);
                expectEquals (o->blockSize, 512);
                expectEquals (o->latencyIn, 200);
                expectEquals (o->latencyOut, 264);
                expectEquals ((int) o->addTracks.size(), 2);

                if (o->addTracks.size() == 2)
                {
                    expect (o->addTracks[0].kind == TrackKind::voice && o->addTracks[0].name == "Voz");
                    expect (o->addTracks[1].kind == TrackKind::instrument && o->addTracks[1].name == "Guitarra");
                }

                expectEquals (o->recordTrack, juce::String ("Voz"));
                expectEquals (o->recordAt, 1.5);
                expectEquals (o->recordFor, 2.0);
                expectEquals (o->importFile.getFileName(), juce::String ("a.wav"));
                expectEquals (o->opsFile.getFileName(), juce::String ("ops.jsonl"));
                expectEquals (o->waitTakes, 3);
                expectEquals (o->exportFile.getFileName(), juce::String ("mix.wav"));
                expectEquals (o->dumpProject.getFileName(), juce::String ("p.json"));
                expectEquals (o->durationSeconds, 2.5);
                expect (o->durationGiven && o->calibrate && o->resume && o->muteMonitor);
            }
        }

        beginTest ("parseHeadlessOptions: host mode and defaults");
        {
            const auto o = parse ({ "--host", "proj", "--port", "22151" });

            if (o)
            {
                expectEquals (o->hostProject.getFileName(), juce::String ("proj"));
                expectEquals (o->hostPort, 22151);
                expectEquals (o->name, juce::String ("Headless"));
                expect (o->user.isEmpty() && o->cacheDir == juce::File());
                expect (o->signal == TestSignal::silence);
                expectEquals (o->blockSize, 256);
                expectEquals (o->durationSeconds, 10.0);
                expect (! o->durationGiven && ! o->calibrate && ! o->resume && o->waitTakes == 0 && o->addTracks.empty());
            }

            const auto d = parse ({ "--host", "proj" });
            expect (d.has_value() && d->hostPort == defaultPort);

            const auto y = parse ({ "--host", "proj", "--import-url", "https://www.youtube.com/watch?v=jNQXAC9IVRw" });
            expect (y.has_value() && y->importUrl == "https://www.youtube.com/watch?v=jNQXAC9IVRw" && y->importFile == juce::File());

            const auto c = parse ({ "--calibrate", "--fake-audio", "loopback", "--latency-in", "200", "--latency-out", "264" });
            expect (c.has_value() && c->calibrate && ! c->hasSession(), "--calibrate runs without a session");
        }

        beginTest ("parseHeadlessOptions: real ASIO device options are kept");
        {
            const auto o = parse ({ "--host", "p", "--asio", "MiniFuse ASIO Driver", "--out", "1", "--mic", "2", "--inst", "3",
                                    "--mute-monitor", "--block", "128" });

            if (o)
            {
                expectEquals (o->asioDevice, juce::String ("MiniFuse ASIO Driver"));
                expectEquals (o->outputPair, 1);
                expectEquals (o->micChannel, 2);
                expectEquals (o->instrumentChannel, 3);
                expect (o->muteMonitor);
            }
        }

        beginTest ("parseHeadlessOptions: errors and removed live-mix flags");
        {
            rejects ({ "--host" }, "host without a project dir");
            rejects ({ "--host", "p", "--block", "8" }, "block too small");
            rejects ({ "--host", "p", "--fake-audio", "noise" }, "bad signal");
            rejects ({ "--host", "p", "--port", "70000" }, "bad port");
            rejects ({ "--host", "p", "--port", "abc" }, "non-numeric port");
            rejects ({ "--host", "p", "--duration", "0" }, "zero duration");
            rejects ({ "--host", "p", "--duration" }, "missing value");
            rejects ({ "--join", "not a link" }, "bad link");
            rejects ({ "--join", "ensayo://1.2.3.4:22140/ABCDEF", "--host", "p" }, "host and join together");
            rejects ({ "--name", "Solo" }, "neither host nor join");
            rejects ({ "--host", "p", "--user", "xyz" }, "bad user id");
            rejects ({ "--host", "p", "--add-track", "drums", "x" }, "bad track kind");
            rejects ({ "--host", "p", "--add-track", "imported", "x" }, "imported tracks come from --import");
            rejects ({ "--host", "p", "--add-track", "voice" }, "track without a name");
            rejects ({ "--host", "p", "--add-track", "voice", "--record" }, "a flag is not a track name");
            rejects ({ "--host", "p", "--record", "Voz" }, "record without --for");
            rejects ({ "--host", "p", "--record", "Voz", "--for", "0" }, "zero-length take");
            rejects ({ "--host", "p", "--at", "2" }, "--at without --record");
            rejects ({ "--host", "p", "--wait-takes", "0" }, "wait for zero takes");
            rejects ({ "--host", "p", "--asio" }, "asio without driver name");
            rejects ({ "--host", "p", "--asio", "X", "--fake-audio", "sine" }, "asio and fake audio together");
            rejects ({ "--host", "p", "--mic", "64" }, "mic channel out of range");
            rejects ({ "--host", "p", "--out", "-1" }, "negative output pair");
            rejects ({ "--calibrate", "--resume" }, "resume needs a session");
            rejects ({ "--host", "p", "--import-url", "https://example.com/v.m4a" }, "--import-url accepts only YouTube URLs");
            rejects ({ "--host", "p", "--import-url" }, "--import-url without a URL");
            rejects ({ "--calibrate", "--import-url", "https://youtu.be/jNQXAC9IVRw" }, "--import-url needs a session");

            for (const auto* removed : { "--simulate-loss", "--role", "--probe", "--emit-log", "--compare", "--track", "--stats" })
                rejects ({ "--host", "p", removed, "1" }, juce::String ("removed flag ") + removed);

            rejects ({ "--host", "p", "--play" }, "removed flag --play");
        }
    }

    static juce::MemoryBlock sineSegment (int samples, float freq)
    {
        juce::AudioBuffer<float> audio (1, samples);

        for (int i = 0; i < samples; ++i)
            audio.setSample (0, i, 0.4f * std::sin (juce::MathConstants<float>::twoPi * freq * (float) i / (float) sampleRate));

        return encodeSegment (audio, samples);
    }

    static juce::File writeWav (const juce::File& file, double rate, int seconds)
    {
        juce::AudioBuffer<float> audio (2, (int) rate * seconds);

        for (int i = 0; i < audio.getNumSamples(); ++i)
            for (int c = 0; c < 2; ++c)
                audio.setSample (c, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 330.0f * (float) i / (float) rate));

        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (16);

        if (auto writer = stream != nullptr ? wav.createWriterFor (stream, options) : nullptr)
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());

        return file;
    }

    void testRuns()
    {
        beginTest ("runHeadless --join records a take on its own track, aligned at --at, and waits until it is complete");
        {
            const auto dir = tempDir ("record");
            ProjectHost host ({ recordPort, 8 });
            expect (host.start (dir.getChildFile ("project"), newId(), "Host", "Grabar").wasOk());

            const auto dump = dir.getChildFile ("dump.json");
            const auto user = newId();
            const auto o = parse ({ "--join", SessionLink { "127.0.0.1", recordPort, host.getSessionId() }.toString(), "--name", "Bea",
                                    "--user", user, "--cache", dir.getChildFile ("cache").getFullPathName(), "--fake-audio", "sine",
                                    "--add-track", "voice", "Voz", "--record", "Voz", "--at", "1", "--for", "2", "--duration", "0.1",
                                    "--dump-project", dump.getFullPathName() });

            if (o)
            {
                expectEquals (runHeadless (*o), 0);
                const auto p = host.snapshot();
                const auto* track = p->tracks.empty() ? nullptr : &p->tracks.front();
                expect (track != nullptr && track->name == "Voz" && track->author == user && track->regions.size() == 1);

                if (track != nullptr && track->regions.size() == 1)
                {
                    const auto& r = track->regions.front();
                    const auto* take = p->findTake (r.take);
                    expectEquals ((juce::int64) r.start, (juce::int64) 48000);
                    expectEquals ((juce::int64) r.offset, (juce::int64) 0);   // no count-in, fake latencies 0
                    expect (r.length >= 96000 && r.length <= 96000 + 2 * 256, "length " + juce::String ((juce::int64) r.length));
                    expect (take != nullptr && take->complete && take->author == user && take->channels == 1);
                }

                juce::String error;
                const auto dumped = projectFromJson (dump.loadFileAsString(), error);
                expect (dumped.has_value(), "dump-project: " + error);
                expect (dumped && contentHash (*dumped) == contentHash (*p), "the dump matches the host");
                expect (dir.getChildFile ("cache").getChildFile (p->id).getChildFile ("recordings").findChildFiles (juce::File::findFiles, false, "*.wav").size() == 1,
                        "local WAV copy of the take");
            }

            host.stop();
        }

        beginTest ("runHeadless --resume finishes a take left without a finish intent (aligned with its offset)");
        {
            const auto dir = tempDir ("resume");
            const auto hostUser = newId(), user = newId();
            ProjectHost host ({ resumePort, 8 });
            expect (host.start (dir.getChildFile ("project"), hostUser, "Host", "Retomar").wasOk());
            const auto cache = dir.getChildFile ("cache");
            const auto track = newId(), take = newId();

            {
                // The "crashed" client: begins a take, queues 1.5 s and disappears without finishTake.
                ProjectClient client ({ cache, user, "Ceci" });
                bool connected = false;
                client.onConnected = [&] { connected = true; };
                client.connect ({ "127.0.0.1", resumePort, host.getSessionId() });
                expect (waitFor ([&] { return connected; }, 8000));
                client.sendOp (Ops::addTrack (track, "Voz", TrackKind::voice));
                expect (waitFor ([&] { return client.project()->findTrack (track) != nullptr; }, 5000));
                client.beginTake (take, 1, track, 96000, 1000);
                client.queueSegment (take, 0, sineSegment (48000, 440.0f));
                client.queueSegment (take, 1, sineSegment (24000, 440.0f));
                waitFor ([&] { return client.transfers().pendingUploads == 0; }, 5000);
                client.disconnect();
            }

            expect (! host.snapshot()->findTake (take)->complete);

            const auto o = parse ({ "--join", SessionLink { "127.0.0.1", resumePort, host.getSessionId() }.toString(), "--user", user,
                                    "--name", "Ceci", "--cache", cache.getFullPathName(), "--resume" });

            if (o)
            {
                const auto t0 = juce::Time::getMillisecondCounter();
                expectEquals (runHeadless (*o), 0);
                expect (juce::Time::getMillisecondCounter() - t0 < 9000, "--resume must exit once flushed, not wait --duration");

                const auto p = host.snapshot();
                const auto* t = p->findTake (take);
                expect (t != nullptr && t->complete && t->samples == 72000);
                const auto* tr = p->findTrack (track);
                expect (tr != nullptr && tr->regions.size() == 1);

                if (tr != nullptr && tr->regions.size() == 1)
                {
                    expectEquals ((juce::int64) tr->regions[0].start, (juce::int64) 96000);
                    expectEquals ((juce::int64) tr->regions[0].offset, (juce::int64) 1000);
                    expectEquals ((juce::int64) tr->regions[0].length, (juce::int64) 71000);
                }

                expect (cache.getChildFile (p->id).getChildFile ("pending").findChildFiles (juce::File::findDirectories, false).isEmpty(),
                        "the pending take is gone");
            }

            host.stop();
        }

        beginTest ("runHeadless --host imports a file, sends --ops in order, exports and dumps the project");
        {
            const auto dir = tempDir ("host");
            const auto wav = writeWav (dir.getChildFile ("Bateria.wav"), 44100.0, 3);
            const auto ops = dir.getChildFile ("ops.jsonl");
            ops.replaceWithText ("{\"op\":\"rename_project\",\"name\":\"Renombrado\"}\n\n"
                                 "{\"op\":\"delete_track\",\"track\":\"00000000000000000000000000000000\"}\n"
                                 "{\"op\":\"set_tempo\",\"bpm\":100,\"beatsPerBar\":3}\n");
            const auto mix = dir.getChildFile ("mix.wav");
            const auto dump = dir.getChildFile ("dump.json");
            const auto projectDir = dir.getChildFile ("project");

            const auto o = parse ({ "--host", projectDir.getFullPathName(), "--port", juce::String (hostPort), "--user", newId(),
                                    "--cache", dir.getChildFile ("cache").getFullPathName(), "--import", wav.getFullPathName(),
                                    "--ops", ops.getFullPathName(), "--export", mix.getFullPathName(), "--dump-project",
                                    dump.getFullPathName(), "--duration", "0.5" });

            if (o)
            {
                expectEquals (runHeadless (*o), 0);
                juce::String error;
                const auto p = projectFromJson (projectDir.getChildFile ("project.json").loadFileAsString(), error);
                expect (p.has_value(), error);

                if (p)
                {
                    expectEquals (p->name, juce::String ("Renombrado"));
                    expectEquals (p->bpm, 100.0);
                    expectEquals (p->beatsPerBar, 3);
                    expect (p->tracks.size() == 1 && p->tracks[0].kind == TrackKind::imported && p->tracks[0].name == "Bateria");

                    if (p->tracks.size() == 1 && p->tracks[0].regions.size() == 1)
                    {
                        const auto& r = p->tracks[0].regions[0];
                        const auto* take = p->findTake (r.take);
                        expect (r.start == 0 && r.offset == 0 && take != nullptr && take->complete && take->channels == 2);
                        expectEquals ((juce::int64) r.length, (juce::int64) 144000);
                        expect (take != nullptr && r.length == take->samples);
                    }
                    else
                    {
                        expect (false, "the imported track has no region");
                    }

                    const auto dumped = projectFromJson (dump.loadFileAsString(), error);
                    expect (dumped && contentHash (*dumped) == contentHash (*p), "dump == saved project");
                }

                std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (mix.createInputStream().release(), true));
                expect (reader != nullptr, "exported WAV missing");

                if (reader != nullptr)
                {
                    expectEquals ((int) reader->bitsPerSample, 24);
                    expectEquals ((juce::int64) reader->lengthInSamples, (juce::int64) 144000);
                }
            }
        }

        beginTest ("leaving during an import cancels it: no track is created and leave() returns promptly");
        {
            const auto dir = tempDir ("import-cancel");
            const auto wav = writeWav (dir.getChildFile ("Larga.wav"), 44100.0, 120);
            const auto projectDir = dir.getChildFile ("project");
            StudioEngine engine;
            engine.useFakeDevice (256, TestSignal::silence);

            {
                SessionController session (engine);
                bool connected = false;
                session.onConnected = [&] { connected = true; };
                expect (session.host (projectDir, recordPort, { newId(), "Dani", dir.getChildFile ("cache") }, "Cancelar").wasOk());
                expect (waitFor ([&] { return connected; }, 8000));
                expect (session.importFile (wav).wasOk());
                expect (session.isImporting());

                const auto t0 = juce::Time::getMillisecondCounter();
                session.leave();
                const auto elapsed = juce::Time::getMillisecondCounter() - t0;
                expect (elapsed < 15000, "leave took " + juce::String ((int) elapsed) + " ms");
                expect (! session.isImporting() && ! session.isActive());
                pumpMessages (200);   // stale import posts must be dropped
            }

            juce::String error;
            const auto p = projectFromJson (projectDir.getChildFile ("project.json").loadFileAsString(), error);
            expect (p.has_value() && p->tracks.empty(), "a cancelled import left a track");
            engine.getDeviceManager().closeAudioDevice();
        }

        beginTest ("leaving once an import's track exists finishes it: the next connection completes the take and its region");
        {
            const auto dir = tempDir ("import-leave");
            const auto wav = writeWav (dir.getChildFile ("Guia.wav"), 44100.0, 60);
            const auto projectDir = dir.getChildFile ("project");
            const SessionIdentity identity { newId(), "Eli", dir.getChildFile ("cache") };
            StudioEngine engine;
            engine.useFakeDevice (256, TestSignal::silence);

            {
                SessionController session (engine);
                bool connected = false;
                float progress = 0.0f;
                session.onConnected = [&] { connected = true; };
                session.onImportProgress = [&] (const juce::String&, float f) { progress = f; };
                expect (session.host (projectDir, recordPort, identity, "Importar").wasOk());
                expect (waitFor ([&] { return connected; }, 8000));
                expect (session.importFile (wav).wasOk());
                expect (waitFor ([&] { return progress > 0.0f; }, 30000), "no segment was queued");
                logMessage ("left at " + juce::String (progress * 100.0f, 1) + " % queued");
                session.leave();   // the host stops too: the uploads must wait for the next connection
                expect (! session.isImporting());
            }

            {
                SessionController session (engine);
                expect (session.host (projectDir, recordPort, identity, "Importar").wasOk());
                expect (waitFor ([&]
                {
                    const auto p = session.getHost() != nullptr ? session.getHost()->snapshot() : nullptr;

                    if (p == nullptr || p->tracks.size() != 1 || p->tracks[0].regions.size() != 1)
                        return false;

                    const auto* take = p->findTake (p->tracks[0].regions[0].take);
                    return take != nullptr && take->complete;
                }, 60000), "the import never completed after reconnecting");

                const auto p = session.getHost()->snapshot();
                expect (p->tracks.size() == 1 && p->tracks[0].kind == TrackKind::imported && p->tracks[0].name == "Guia");

                if (p->tracks.size() == 1 && p->tracks[0].regions.size() == 1)
                {
                    const auto& r = p->tracks[0].regions[0];
                    expect (r.start == 0 && r.offset == 0 && r.length == 60 * 48000 && p->findTake (r.take)->samples == r.length);
                }

                session.leave();
            }

            engine.getDeviceManager().closeAudioDevice();
        }

        beginTest ("no import while recording; an import is never shown as a live recording");
        {
            const auto dir = tempDir ("import-live");
            const auto wav = writeWav (dir.getChildFile ("Corta.wav"), 48000.0, 3);
            StudioEngine engine;
            engine.useFakeDevice (256, TestSignal::sine440);
            SessionController session (engine);
            bool connected = false;
            session.onConnected = [&] { connected = true; };
            session.setMetronome (false, false);
            expect (session.host (dir.getChildFile ("project"), recordPort, { newId(), "Fede", dir.getChildFile ("cache") }, "Vivo").wasOk());
            expect (waitFor ([&] { return connected; }, 8000));

            // A guest watches.
            ProjectClient guest ({ dir.getChildFile ("guest"), newId(), "Gabi" });
            bool guestConnected = false;
            guest.onConnected = [&] { guestConnected = true; };
            guest.connect (session.getConnectLink());
            expect (waitFor ([&] { return guestConnected; }, 8000));

            bool sawLive = false, finished = false;
            session.onImportFinished = [&] (const juce::String&, const juce::Result& r) { expect (r.wasOk(), r.getErrorMessage()); finished = true; };
            expect (session.importFile (wav).wasOk());
            expect (waitFor ([&] { sawLive = sawLive || ! guest.liveRecordings().empty(); return finished; }, 15000));

            for (int i = 0; i < 30; ++i)
            {
                pumpMessages (10);
                sawLive = sawLive || ! guest.liveRecordings().empty();
            }

            expect (! sawLive, "the guest saw the import as a live recording");

            const Id track = newId();
            session.getClient()->sendOp (Ops::addTrack (track, "Voz", TrackKind::voice));
            expect (waitFor ([&] { return session.project()->findTrack (track) != nullptr; }, 5000));
            expect (session.startTake (track).wasOk());
            expect (waitFor ([&] { return guest.liveRecordings().size() == 1; }, 5000), "the guest does not see the take");

            const auto refused = session.importFile (wav);
            expect (refused.failed() && refused.getErrorMessage().contains ("mientras grab"), refused.getErrorMessage());
            expect (! session.isImporting());

            expect (waitFor ([&] { const auto l = guest.liveRecordings(); return l.size() == 1 && l[0].peaks.size() > 50; }, 5000),
                    "the take's live peaks stopped reaching the guest");
            expect (session.stopTake().has_value());
            expect (waitFor ([&] { return guest.liveRecordings().empty(); }, 8000));

            guest.disconnect();
            session.leave();
            engine.getDeviceManager().closeAudioDevice();
        }

        beginTest ("an import with a long name gets a 64-character track; a rejected track ends the import with no take");
        {
            const auto dir = tempDir ("import-name");
            const auto wav = writeWav (dir.getChildFile ("Corta.wav"), 48000.0, 2);
            StudioEngine engine;
            engine.useFakeDevice (256, TestSignal::silence);
            SessionController session (engine);
            bool connected = false;
            int finishedOk = 0, finishedFailed = 0;
            juce::String failure;
            session.onConnected = [&] { connected = true; };
            session.onImportFinished = [&] (const juce::String&, const juce::Result& r)
            {
                if (r.wasOk()) ++finishedOk; else { ++finishedFailed; failure = r.getErrorMessage(); }
            };
            expect (session.host (dir.getChildFile ("project"), recordPort, { newId(), "Hana", dir.getChildFile ("cache") }, "Nombres").wasOk());
            expect (waitFor ([&] { return connected; }, 8000));

            // A YouTube-like title well over 64 characters (with multibyte characters).
            const auto longName = juce::String (juce::CharPointer_UTF8 ("  Canci\xc3\xb3n ")) + juce::String::repeatedString (juce::String (juce::CharPointer_UTF8 ("largu\xc3\xadsima ")), 12);
            expect (session.importFile (wav, longName).wasOk());
            expect (waitFor ([&] { return finishedOk + finishedFailed == 1; }, 15000));
            expectEquals (finishedOk, 1, failure);
            expect (waitFor ([&]
            {
                const auto p = session.getHost()->snapshot();
                return p->tracks.size() == 1 && p->tracks[0].regions.size() == 1;
            }, 15000), "the long-named import never got its region");
            expectEquals (session.getHost()->snapshot()->tracks[0].name, clampName (longName));
            expectEquals (session.getHost()->snapshot()->tracks[0].name.length(), 64);

            // An add_track the host rejects (track id in use): an error, no take, nothing pending.
            const auto existing = session.getHost()->snapshot()->tracks[0].id;
            const auto takesBefore = session.getHost()->snapshot()->takes.size();
            session.importTrackIdForTest = existing;
            expect (session.importFile (wav, "Otra").wasOk());
            expect (waitFor ([&] { return finishedOk + finishedFailed == 2; }, 15000));
            expectEquals (finishedFailed, 1);
            expect (failure.startsWith ("No se pudo crear la pista de Otra"), failure);
            expect (! session.isImporting());
            pumpMessages (300);
            expectEquals (session.getHost()->snapshot()->takes.size(), takesBefore);
            expectEquals (session.getClient()->transfers().pendingTakes, 0);
            expectEquals ((int) session.getHost()->snapshot()->tracks.size(), 1);

            // The session keeps importing normally afterwards.
            expect (session.importFile (wav, "Tercera").wasOk());
            expect (waitFor ([&] { return finishedOk == 2; }, 15000));

            session.leave();
            engine.getDeviceManager().closeAudioDevice();
        }

        beginTest ("runHeadless --calibrate on a fake loopback finds the hidden latency and exits");
        {
            const auto o = parse ({ "--calibrate", "--fake-audio", "loopback", "--latency-in", "200", "--latency-out", "264" });

            if (o)
                expectEquals (runHeadless (*o), 0);
        }

        beginTest ("runHeadless prints cached <take> <k> while the segments of a take still being recorded arrive");
        {
            const auto dir = tempDir ("growing");
            ProjectHost host ({ growPort, 8 });
            expect (host.start (dir.getChildFile ("project"), newId(), "Host", "Crecer").wasOk());
            const SessionLink link { "127.0.0.1", growPort, host.getSessionId() };

            ProjectClient rec ({ dir.getChildFile ("rec-cache"), newId(), "Rita" });
            bool connected = false;
            rec.onConnected = [&] { connected = true; };
            rec.connect (link);
            expect (waitFor ([&] { return connected; }, 8000));
            const auto track = newId(), take = newId();
            rec.sendOp (Ops::addTrack (track, "Voz", TrackKind::voice));
            expect (waitFor ([&] { return rec.project()->findTrack (track) != nullptr; }, 5000));
            rec.beginTake (take, 1, track, 0);

            // Once the observer is in, one 1 s segment every 400 ms, then the take is finished.
            struct Feeder : juce::Timer { std::function<void()> tick; void timerCallback() override { tick(); } } feeder;
            int next = 0;
            feeder.tick = [&]
            {
                if (next < 4)
                {
                    rec.queueSegment (take, next, sineSegment (48000, 440.0f));
                    ++next;
                    return;
                }

                feeder.stopTimer();
                rec.finishTake (take, track, { newId(), take, 0, 0, 4 * 48000 });
            };

            juce::StringArray lines;
            setHeadlessOutputCopyForTest ([&] (const juce::String& line)
            {
                lines.add (line);

                if (line.startsWith ("welcome "))
                    feeder.startTimer (400);
            });

            const auto o = parse ({ "--join", link.toString(), "--cache", dir.getChildFile ("cache").getFullPathName(),
                                    "--wait-takes", "1", "--duration", "30" });

            if (o)
                expectEquals (runHeadless (*o), 0);

            setHeadlessOutputCopyForTest (nullptr);
            feeder.stopTimer();

            const int completeAt = lines.indexOf ("take_complete " + take + " " + juce::String (4 * 48000));
            expect (completeAt >= 0, "no take_complete: " + lines.joinIntoString (" | "));
            std::set<juce::String> partial;

            for (int i = 0; i < completeAt; ++i)
                if (lines[i].startsWith ("cached " + take + " "))
                    partial.insert (lines[i].fromLastOccurrenceOf (" ", false, false));

            expect (partial.size() >= 3, "segment counts seen before the take was complete: " + lines.joinIntoString (" | "));
            // The complete take's own line comes after take_complete (a partial "cached <take> 4" may precede it).
            bool completeCached = false;

            for (int i = completeAt + 1; i < lines.size(); ++i)
                completeCached = completeCached || lines[i] == "cached " + take + " 4";

            expect (completeAt >= 0 && completeCached, "the complete take is reported as cached after take_complete: " + lines.joinIntoString (" | "));
            host.stop();
        }

        beginTest ("runHeadless --join to a dead host fails cleanly");
        {
            const auto o = parse ({ "--join", SessionLink { "127.0.0.1", deadPort, "ABCDEF" }.toString(), "--duration", "1",
                                    "--cache", tempDir ("dead").getFullPathName() });

            if (o)
                expectEquals (runHeadless (*o), 1);
        }
    }

    void runTest() override
    {
        testParsing();
        testRuns();
    }
};

static HeadlessClientTests headlessClientTests;
}
