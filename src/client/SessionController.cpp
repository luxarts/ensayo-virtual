#include "client/SessionController.h"
#include "client/Offline.h"
#include "ui/Text.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <cmath>

namespace ensayo
{
namespace
{
    juce::String translateEngineError (const juce::String& error)
    {
        if (error.startsWith ("No audio device"))
            return ES ("No hay un dispositivo de audio funcionando: eleg\xC3\xAD uno en el lobby");

        if (error.startsWith ("Already recording"))
            return ES ("Ya se est\xC3\xA1 grabando");

        if (error.startsWith ("Cannot record while calibrating"))
            return ES ("No se puede grabar mientras se calibra la latencia");

        if (error.startsWith ("Imported tracks"))
            return ES ("En una pista importada no se graba");

        return error;
    }
}

SessionController::SessionController (StudioEngine& e)
    : engine (e), generation (std::make_shared<std::atomic<int>> (0))
{
}

SessionController::~SessionController()
{
    leave();
    ++(*generation);
}

//==============================================================================
void SessionController::post (std::function<void()> fn)
{
    juce::MessageManager::callAsync ([g = generation, gen = generation->load(), fn = std::move (fn)]
    {
        if (g->load() == gen)
            fn();
    });
}

juce::String SessionController::lanAddress()
{
    juce::String fallback;

    for (const auto& address : juce::IPAddress::getAllAddresses (false))
    {
        if (address.isIPv6 || address == juce::IPAddress::local())
            continue;

        const auto text = address.toString();
        const auto a = address.address[0], b = address.address[1];

        // Link-local 169.254.x.x comes from adapters without a network and is never useful.
        if (text == "0.0.0.0" || (a == 169 && b == 254))
            continue;

        if ((a == 10) || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31))
            return text;

        if (fallback.isEmpty())
            fallback = text;
    }

    return fallback.isNotEmpty() ? fallback : "127.0.0.1";
}

bool SessionController::isConnected() const noexcept   { return client != nullptr && client->isConnected(); }

std::shared_ptr<const Project> SessionController::project() const
{
    return client != nullptr ? client->project() : nullptr;
}

SessionLink SessionController::getShareLink() const
{
    if (projectHost != nullptr)
        return { lanAddress(), link.port, link.sessionId };

    return link;
}

void SessionController::setCalibration (int samples)
{
    engine.setCalibration (samples);
}

void SessionController::setMetronome (bool clickOn, bool countInOn)
{
    engine.setMetronome (clickOn, countInOn);
}

//==============================================================================
void SessionController::createClient (const SessionIdentity& identity)
{
    cacheRoot = identity.cacheRoot;
    client = std::make_unique<ProjectClient> (ClientConfig { identity.cacheRoot, identity.user, identity.name });
    everConnected = false;

    client->onConnected = [this]
    {
        everConnected = true;
        refreshEngine();
        finishUnfinishedTakes();

        if (onConnected) onConnected();
    };

    client->onProjectChanged = [this]
    {
        refreshEngine();
        checkImportWait();

        if (onProjectChanged) onProjectChanged();
    };

    client->onTransfersChanged = [this] { if (onTransfersChanged) onTransfersChanged(); };
    client->onPresenceChanged  = [this]
    {
        applyParticipantGains();

        if (onPresenceChanged) onPresenceChanged();
    };
    client->onDisconnected     = [this] (const juce::String& reason)
    {
        if (importWait)
            failImport (ES ("Se perdi\xC3\xB3 la conexi\xC3\xB3n con el anfitri\xC3\xB3n: no se import\xC3\xB3 ") + importWait->name);

        if (onDisconnected) onDisconnected (reason);
    };
    client->onError            = [this] (const juce::String& message) { if (onError) onError (message); };
    client->onRejected         = [this] (const juce::String& reason) { if (onRejected) onRejected (reason); };
    client->onWarning          = [this] (const juce::String& message) { if (onWarning) onWarning (message); };
    client->onOpRejected       = [this] (int seq, const juce::String& reason)
    {
        if (importWait && importWait->seq == seq)
            failImport ("No se pudo crear la pista de " + importWait->name + ": " + reason);
    };
}

// The engine plays the replica from the client's cache. The cache is created at the first welcome and kept across
// reconnects to the same project, so the (possibly blocking) source swap happens once per session.
void SessionController::refreshEngine()
{
    if (client == nullptr)
        return;

    engine.setProject (client->project());

    if (auto* cache = client->cache(); cache != engineSource && cache != nullptr)
    {
        engine.setSampleSource (cache);
        engineSource = cache;
    }

    applyArmed();
}

void SessionController::setArmedTrack (const Id& track)
{
    armedTrack = track;
    applyArmed();
}

void SessionController::applyArmed()
{
    std::optional<TrackKind> kind;
    const auto p = project();

    if (const auto* track = p != nullptr && client != nullptr ? p->findTrack (armedTrack) : nullptr;
        track != nullptr && track->author == client->user() && track->kind != TrackKind::imported)
        kind = track->kind;

    engine.setArmed (kind);
}

void SessionController::setParticipantMix (const Id& user, ParticipantMix mix)
{
    participantMixes[user] = mix;
    applyParticipantGains();
}

std::vector<SessionController::ClientGain> SessionController::clientGains (const std::vector<Presence>& presence,
                                                                           const std::map<Id, ParticipantMix>& mixes)
{
    std::vector<ClientGain> out;

    for (const auto& p : presence)
    {
        if (! p.online || p.clientId == 0)
            continue;

        const auto it = mixes.find (p.user);
        const auto mix = it != mixes.end() ? it->second : ParticipantMix();
        out.push_back ({ p.clientId, mix.gain, mix.muted });
    }

    return out;
}

// The engine's table follows the presence exactly: every online sender gets its user's level, then the clientIds
// that are gone (a user who left, an old clientId after a reconnect) are dropped. Setting before dropping never lets a
// muted sender play at 1.0 in between.
void SessionController::applyParticipantGains()
{
    if (client == nullptr)
        return;

    std::set<uint32_t> fresh;

    for (const auto& g : clientGains (client->presence(), participantMixes))
    {
        engine.setParticipantGain (g.clientId, g.gain, g.muted);
        fresh.insert (g.clientId);
    }

    for (const auto id : appliedClients)
        if (fresh.count (id) == 0)
            engine.removeParticipantGain (id);

    appliedClients = std::move (fresh);
}

void SessionController::attachLiveAudio()
{
    if (client == nullptr)
        return;

    auto* c = client.get();
    engine.clearParticipantGains();     // levels are per session: clientIds restart with the host
    appliedClients.clear();
    applyParticipantGains();
    engine.onChatPacket = [c] (StreamKind kind, const uint8_t* packet, size_t len) { c->sendAudio (kind, packet, len); };
    engine.setRemoteStreams (&c->remoteStreams());
    engine.setChatEnabled (true);
}

void SessionController::detachLiveAudio()
{
    engine.setChatEnabled (false);     // returns once the sink is no longer running
    engine.setRemoteStreams (nullptr); // returns once the callback no longer reads the client's table
    engine.onChatPacket = nullptr;
    engine.clearParticipantGains();
    appliedClients.clear();
}

juce::Result SessionController::host (const juce::File& projectDir, int port, const SessionIdentity& identity,
                                      const juce::String& newProjectName)
{
    leave();

    HostConfig config;
    config.port = port;
    projectHost = std::make_unique<ProjectHost> (config);

    if (const auto started = projectHost->start (projectDir, identity.user, identity.name, newProjectName); started.failed())
    {
        projectHost = nullptr;
        return started;
    }

    link = SessionLink { "127.0.0.1", projectHost->getPort(), projectHost->getSessionId() };
    createClient (identity);
    client->connect (link);
    attachLiveAudio();
    return juce::Result::ok();
}

void SessionController::join (const SessionLink& target, const SessionIdentity& identity)
{
    leave();
    link = target;
    createClient (identity);
    client->connect (link);
    attachLiveAudio();
}

void SessionController::reconnect()
{
    if (client == nullptr || client->isConnected())
        return;

    detachLiveAudio();
    client->disconnect();
    client->connect (link);
    attachLiveAudio();
}

void SessionController::leave()
{
    if (isRecording())
        stopTake();

    // An import is cancelled only while it decodes or cuts (before its track exists). Once the track is created the
    // queueing thread runs to the end (local appends, fast) and the take is finished: the finish intent persists and
    // the upload completes on the next connection, like a recording.
    importCancel = true;
    joinImportThread();

    if (importFinish && importQueued)
        completeImport();

    // The track was asked for but its take never started: remove it if the host applies it after all (ops are applied
    // in order, so a rejected add_track only makes this a rejected delete).
    if (importWait && client != nullptr && client->isConnected())
        client->sendOp (Ops::deleteTrack (importWait->track));

    importWait.reset();
    importFinish.reset();
    importBusy = false;
    importQueued = false;
    importCancel = false;

    exportCancel = true;

    if (exportThread.joinable())
        exportThread.join();

    exportBusy = false;
    exportCancel = false;

    ++(*generation);   // drops the posts of the threads just joined and of the client being destroyed

    detachLiveAudio();
    armedTrack.clear();
    engine.setArmed (std::nullopt);

    if (client != nullptr || engineSource != nullptr)
    {
        engine.stop();
        engine.setPosition (0);
    }

    if (engineSource != nullptr)
    {
        engine.setSampleSource (nullptr);
        engineSource = nullptr;
    }

    engine.setProject (nullptr);

    if (client != nullptr)
    {
        client->onConnected = nullptr;
        client->onProjectChanged = nullptr;
        client->onTransfersChanged = nullptr;
        client->onPresenceChanged = nullptr;
        client->onDisconnected = nullptr;
        client->onError = nullptr;
        client->onRejected = nullptr;
        client->onWarning = nullptr;
        client->onOpRejected = nullptr;
        client->disconnect();
        client = nullptr;
    }

    if (projectHost != nullptr)
    {
        projectHost->stop();
        projectHost = nullptr;
    }

    everConnected = false;
    resumedTakes.clear();
    link = {};
}

//==============================================================================
juce::Result SessionController::startTake (const Id& trackId)
{
    if (client == nullptr || ! client->isConnected())
        return juce::Result::fail (ES ("Sin conexi\xC3\xB3n con el anfitri\xC3\xB3n: no se puede grabar"));

    if (isRecording())
        return juce::Result::fail (ES ("Ya se est\xC3\xA1 grabando"));

    const auto p = client->project();
    const auto* track = p != nullptr ? p->findTrack (trackId) : nullptr;

    if (track == nullptr)
        return juce::Result::fail (ES ("Arm\xC3\xA1 una pista propia (\xE2\x97\x8FR) para grabar"));

    if (track->author != client->user())
        return juce::Result::fail ("Solo se graba en pistas propias");

    if (track->kind == TrackKind::imported)
        return juce::Result::fail (ES ("En una pista importada no se graba"));

    auto* device = engine.getDeviceManager().getCurrentAudioDevice();

    if (device == nullptr || ! device->isPlaying())
        return juce::Result::fail (ES ("No hay un dispositivo de audio funcionando: eleg\xC3\xAD uno en el lobby"));

    const Id take = newId();
    const int channels = track->kind == TrackKind::voice ? 1 : 2;

    const auto wav = cacheRoot.getChildFile (p->id).getChildFile ("recordings").getChildFile (take + ".wav");
    wav.getParentDirectory().createDirectory();

    {
        const std::lock_guard<std::mutex> scoped (liveLock);
        livePeaks.clear();
    }

    takeId = take;
    takeTrack = trackId;
    takePoint = engine.getPosition();   // replaced by the engine's own record point once the take starts
    takeError.clear();
    stopping = false;

    auto* c = client.get();
    auto& recorder = engine.getRecorder();
    recorder.onSegment = [c, take] (int index, const juce::MemoryBlock& flac, int) { c->queueSegment (take, index, flac); };

    // Peaks are re-based on the record point (the first `offset` samples are count-in and latency), so the growing
    // region lines up with the timeline here and on the other machines.
    recorder.onPeaks = [this, c, take] (int64_t fromPeak, const std::vector<Peak>& peaks)
    {
        // Set by startRecording before the writer starts.
        const int64_t skipPeaks = engine.getRecordOffset() / peakBlock;
        const int64_t skip = std::max<int64_t> (0, skipPeaks - fromPeak);

        if (skip >= (int64_t) peaks.size())
            return;

        const std::vector<Peak> aligned (peaks.begin() + (std::ptrdiff_t) skip, peaks.end());
        const int64_t from = fromPeak + skip - skipPeaks;
        c->sendPeaks (take, from, aligned);

        const std::lock_guard<std::mutex> scoped (liveLock);

        if ((int64_t) livePeaks.size() < from + (int64_t) aligned.size())
            livePeaks.resize ((size_t) (from + (int64_t) aligned.size()));

        std::copy (aligned.begin(), aligned.end(), livePeaks.begin() + (std::ptrdiff_t) from);
    };

    recorder.onLiveAudio = nullptr;
    // On the message thread (a take too short, inside stopRecording) the message is kept for stopTake's caller, so
    // only one message is shown; writer-thread failures are posted.
    recorder.onError = [this] (const juce::String& message)
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
            takeError = message;
        else
            post ([this, message] { if (onError) onError ("Error grabando: " + message); });
    };
    recorder.onWarning = [this] (const juce::String& message) { post ([this, message] { if (onWarning) onWarning (message); }); };

    // Fires on the writer thread when the take ends, normally inside stopRecording; by itself only when a segment
    // could not be encoded (the recorder stops) - then the take is stopped and finished from the message thread.
    recorder.onFinished = [this] (int, int64_t)
    {
        if (! stopping.load())
            post ([this]
            {
                if (isRecording())
                {
                    stopTake();

                    if (onTakeAborted) onTakeAborted (ES ("La grabaci\xC3\xB3n se detuvo por un error de escritura; se conserva lo grabado"));
                }
            });
    };

    const auto started = engine.startRecording (*track, take, wav);

    if (started.failed())
    {
        takeId.clear();
        takeTrack.clear();
        return juce::Result::fail (translateEngineError (started.getErrorMessage()));
    }

    // Right after the start: the first segment comes a second later, and one queued earlier would be kept anyway.
    takePoint = engine.getRecordPoint();
    client->beginTake (take, channels, trackId, takePoint, engine.getRecordOffset());
    return juce::Result::ok();
}

std::optional<RecordingResult> SessionController::stopTake()
{
    if (! engine.isRecording())
        return std::nullopt;

    stopping = true;
    const auto result = engine.stopRecording();
    const auto take = std::exchange (takeId, Id());
    const auto track = std::exchange (takeTrack, Id());

    {
        const std::lock_guard<std::mutex> scoped (liveLock);
        livePeaks.clear();
    }

    if (client != nullptr)
    {
        if (result)
            client->finishTake (result->take, track, result->region);
        else
            client->discardTake (take);   // shorter than the count-in and latency: nothing to keep
    }

    return result;
}

bool SessionController::isRecording() const noexcept   { return engine.isRecording(); }

std::optional<LiveRecording> SessionController::ownLiveRecording() const
{
    if (takeId.isEmpty() || client == nullptr)
        return std::nullopt;

    LiveRecording live;
    live.user = client->user();
    live.take = takeId;
    live.track = takeTrack;
    live.start = takePoint;

    const std::lock_guard<std::mutex> scoped (liveLock);
    live.peaks = livePeaks;
    return live;
}

int SessionController::finishUnfinishedTakes()
{
    if (client == nullptr)
        return 0;

    int finished = 0;

    for (const auto& u : client->unfinishedTakes())
    {
        if (u.take == takeId || (importFinish && importFinish->take == u.take))
            continue;   // still being recorded or imported here

        const int64_t length = u.samples - u.offset;

        if (length < 1 || ! isValidId (u.track))
        {
            client->discardTake (u.take);
            continue;
        }

        client->finishTake (u.take, u.track, Region { newId(), u.take, u.start, u.offset, length });
        resumedTakes.push_back (u.take);
        ++finished;
    }

    return finished;
}

//==============================================================================
void SessionController::joinImportThread()
{
    if (importThread.joinable())
        importThread.join();
}

void SessionController::completeImport()
{
    if (! importFinish || client == nullptr)
        return;

    const auto f = *importFinish;
    importFinish.reset();
    importQueued = false;
    client->finishTake (f.take, f.track, Region { newId(), f.take, 0, 0, f.samples });
}

void SessionController::failImport (const juce::String& message)
{
    if (! importWait)
        return;

    const auto name = importWait->name;
    importWait.reset();
    importBusy = false;

    if (onImportFinished) onImportFinished (name, juce::Result::fail (message));
}

void SessionController::checkImportWait()
{
    if (! importWait || client == nullptr)
        return;

    const auto p = client->project();

    if (p == nullptr || p->findTrack (importWait->track) == nullptr)
        return;

    const auto w = std::move (*importWait);
    importWait.reset();
    joinImportThread();

    const Id take = newId();
    client->beginTake (take, w.channels, w.track, 0, 0, false);   // not a live recording: nothing to announce
    importFinish = ImportFinish { take, w.track, w.name, w.samples };

    auto* c = client.get();
    const auto segments = w.segments;
    const auto name = w.name;

    importThread = std::thread ([this, c, take, segments, name]
    {
        const auto total = (int) segments->size();

        for (int i = 0; i < total; ++i)
        {
            c->queueSegment (take, i, (*segments)[(size_t) i]);
            const float fraction = (float) (i + 1) / (float) total;
            post ([this, name, fraction] { if (onImportProgress) onImportProgress (name, fraction); });
        }

        importQueued = true;

        post ([this, name]
        {
            joinImportThread();
            completeImport();
            importBusy = false;

            if (onImportFinished) onImportFinished (name, juce::Result::ok());
        });
    });
}

juce::Result SessionController::importFile (const juce::File& file, const juce::String& trackName)
{
    if (client == nullptr || ! client->isConnected() || client->project() == nullptr)
        return juce::Result::fail (ES ("Sin conexi\xC3\xB3n con el anfitri\xC3\xB3n: no se puede importar"));

    if (importBusy)
        return juce::Result::fail (ES ("Ya hay una importaci\xC3\xB3n en curso"));

    if (isRecording())
        return juce::Result::fail (ES ("No se puede importar mientras grab\xC3\xA1s"));

    if (! file.existsAsFile())
        return juce::Result::fail ("No existe el archivo " + file.getFullPathName());

    joinImportThread();
    importBusy = true;
    importQueued = false;
    auto name = clampName (trackName.isNotEmpty() ? trackName : file.getFileNameWithoutExtension());

    if (name.isEmpty())
        name = ES ("Pista importada");

    if (onImportProgress) onImportProgress (name, 0.0f);

    // Phase 1 (thread): decode, resample and cut. Phase 2 (message thread): track + take. Phase 3 (thread): queue the
    // segments. Phase 4 (message thread): finish the take with one region at 0. No thread ever waits for the message
    // thread, so leave() can join them.
    importThread = std::thread ([this, file, name]
    {
        auto audio = std::make_shared<juce::AudioBuffer<float>>();
        const auto loaded = loadAndResample (file, *audio);
        auto segments = std::make_shared<std::vector<juce::MemoryBlock>>();

        if (importCancel)
            return;

        if (loaded.wasOk())
            *segments = makeSegments (*audio);

        if (importCancel)
            return;

        const int channels = audio->getNumChannels();
        const int64_t samples = audio->getNumSamples();
        audio.reset();

        post ([this, loaded, segments, channels, samples, name]
        {
            joinImportThread();

            if (importCancel)
                return;

            if (loaded.failed() || segments->empty() || samples < 1 || client == nullptr)
            {
                importBusy = false;
                const auto why = loaded.failed() ? loaded.getErrorMessage() : juce::String ("el archivo no tiene audio");

                if (onImportFinished) onImportFinished (name, juce::Result::fail ("No se pudo importar " + name + ": " + why));

                return;
            }

            // The take starts only once the host has applied the track (checkImportWait): a rejected track would
            // otherwise leave an uploaded take whose region can never be added.
            const Id track = importTrackIdForTest.isNotEmpty() ? std::exchange (importTrackIdForTest, Id()) : newId();
            const int seq = client->isConnected() ? client->sendOp (Ops::addTrack (track, name, TrackKind::imported)) : 0;

            if (seq <= 0)
            {
                importBusy = false;

                if (onImportFinished)
                    onImportFinished (name, juce::Result::fail (ES ("Sin conexi\xC3\xB3n con el anfitri\xC3\xB3n: no se import\xC3\xB3 ") + name));

                return;
            }

            importWait = ImportWait { seq, track, name, channels, samples, segments };
        });
    });

    return juce::Result::ok();
}

juce::Result SessionController::exportMix (const juce::File& wav)
{
    if (client == nullptr || client->project() == nullptr || client->cache() == nullptr)
        return juce::Result::fail ("No hay un proyecto abierto para exportar");

    if (exportBusy)
        return juce::Result::fail (ES ("Ya hay una exportaci\xC3\xB3n en curso"));

    if (const auto cached = checkTakesCached (*client->project(), *client->cache()); cached.failed())
        return cached;

    if (exportThread.joinable())
        exportThread.join();

    exportBusy = true;
    exportCancel = false;
    auto snapshot = client->project();
    const SampleSource* source = client->cache();

    exportThread = std::thread ([this, snapshot, source, wav]
    {
        int lastPercent = -1;
        ExportInfo info;
        const auto result = ensayo::exportMix (snapshot, *source, wav, [this, &lastPercent] (float fraction)
        {
            if (const int percent = (int) (fraction * 100.0f); percent != lastPercent)
            {
                lastPercent = percent;
                post ([this, fraction] { if (onExportProgress) onExportProgress (fraction); });
            }

            return ! exportCancel.load();
        }, &info);

        post ([this, wav, result, info]
        {
            if (exportThread.joinable())
                exportThread.join();

            exportBusy = false;

            if (result.wasOk())
                exportInfo = info;

            if (onExportFinished) onExportFinished (wav, result);
        });
    });

    return juce::Result::ok();
}
}
