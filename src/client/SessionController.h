#pragma once
#include "client/ProjectClient.h"
#include "client/StudioEngine.h"
#include "client/Offline.h"
#include "server/ProjectHost.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
namespace ensayo
{
struct SessionIdentity { Id user; juce::String name; juce::File cacheRoot; };

// One session of the app or the headless client: owns the ProjectClient (and the embedded ProjectHost when hosting)
// and wires them to a StudioEngine owned by the caller (sample source, project, voice chat, monitor, recording, import,
// export). While a session is active the engine's voice chat runs: the mic goes out through the client and the other
// participants' streams are mixed into the output.
//
// Threads: every method and callback runs on the message thread. Recorder callbacks run on the recorder's writer thread
// and only call ProjectClient's any-thread methods; import and export run on their own threads and report back through
// posts that are dropped once the session is left.
class SessionController
{
public:
    explicit SessionController (StudioEngine&);
    ~SessionController();                                            // leave()

    // Hosts projectDir (created as newProjectName when it has no project.json) on `port` and connects to it over
    // loopback, as the host user must. Fails when the port cannot be bound or the project belongs to another host.
    juce::Result host (const juce::File& projectDir, int port, const SessionIdentity&, const juce::String& newProjectName);
    void join (const SessionLink&, const SessionIdentity&);
    void reconnect();                       // manual retry after onDisconnected / onError: same client, same link
    // Ends a running take (finishing it), waits for import/export threads, disconnects and stops the embedded host.
    void leave();

    bool isActive() const noexcept          { return client != nullptr; }
    bool isConnected() const noexcept;
    bool hasConnected() const noexcept      { return everConnected; }   // at least one welcome on this session
    bool isHosting() const noexcept         { return projectHost != nullptr; }
    ProjectClient* getClient() noexcept     { return client.get(); }
    ProjectHost* getHost() noexcept         { return projectHost.get(); }
    std::shared_ptr<const Project> project() const;
    SessionLink getConnectLink() const      { return link; }
    SessionLink getShareLink() const;       // hosting: the LAN address other people can reach; guest: the link joined

    // Engine settings that recording depends on.
    void setCalibration (int samples);
    void setMetronome (bool click, bool countIn);

    // Recording. startTake needs a connected session, a running device and a track of mine that is not imported.
    juce::Result startTake (const Id& track);
    // stopRecording + finishTake. nullopt when nothing was recording or the take was shorter than its offset (then
    // getTakeError() says why and the take is discarded).
    std::optional<RecordingResult> stopTake();
    juce::String getTakeError() const       { return takeError; }
    bool isRecording() const noexcept;
    Id recordingTrack() const               { return takeTrack; }
    std::optional<LiveRecording> ownLiveRecording() const;          // peaks aligned to the record point
    // Finishes the takes left in pending/ without a finish intent (the app stopped mid-take): a region over the whole
    // take after its recording offset, at the persisted record position. Returns how many were finished.
    int finishUnfinishedTakes();            // also runs by itself at every onConnected
    const std::vector<Id>& getResumedTakes() const noexcept   { return resumedTakes; }   // finished by the call above

    // The armed track (empty = none). The engine monitors the input of its kind when it is one of my own voice or
    // instrument tracks; re-checked on every project change (a deleted track disarms the monitor).
    void setArmedTrack (const Id& track);

    // Import: decode + resample + segments on a thread, then a new `imported` track with one region at 0. The track name
    // (trackName, else the file name) is trimmed and cut to 64 characters. The take starts only once the host applied
    // the track: a rejected add_track or a lost connection ends the import with an error and nothing recorded.
    juce::Result importFile (const juce::File&, const juce::String& trackName = {});
    bool isImporting() const noexcept       { return importBusy; }
    // Export: offline render of the whole project to a 24-bit WAV on a thread. Refused while a take used by a region
    // is not fully downloaded (it would be written as silence).
    juce::Result exportMix (const juce::File& wav);
    bool isExporting() const noexcept       { return exportBusy; }
    ExportInfo lastExportInfo() const       { return exportInfo; }   // peak / clipping of the last successful export

    std::function<void()> onConnected, onProjectChanged, onTransfersChanged, onPresenceChanged;
    std::function<void (const juce::String&)> onDisconnected, onError, onRejected;
    // Message thread: a non-fatal problem to show (a take the host cannot send completely, recorded audio lost to an
    // overflow and filled with silence). The session and the take go on.
    std::function<void (const juce::String&)> onWarning;
    std::function<void (const juce::String& name, float fraction)> onImportProgress;     // 0..1 while segments are queued
    std::function<void (const juce::String& name, const juce::Result&)> onImportFinished; // ok = queued and finished
    std::function<void (float fraction)> onExportProgress;
    std::function<void (const juce::File&, const juce::Result&)> onExportFinished;
    std::function<void (const juce::String& reason)> onTakeAborted;   // the take stopped by itself (write failure)

    static juce::String lanAddress();       // first private IPv4, else any routable IPv4, else 127.0.0.1

    Id importTrackIdForTest;                // test hook: the next import uses this track id (an id in use is rejected)

private:
    struct ImportFinish { Id take, track; juce::String name; int64_t samples = 0; };
    // Phase 2 of an import: add_track sent (op seq), waiting for the host to apply or reject it.
    struct ImportWait { int seq = 0; Id track; juce::String name; int channels = 0; int64_t samples = 0;
                        std::shared_ptr<std::vector<juce::MemoryBlock>> segments; };

    void createClient (const SessionIdentity&);
    void refreshEngine();
    void applyArmed();
    void attachLiveAudio();                         // engine <-> client: chat sink, remote streams, chat on
    void detachLiveAudio();                         // chat off and remote streams gone before the client changes
    void post (std::function<void()>);
    void joinImportThread();
    void completeImport();
    void checkImportWait();                         // starts the take and the queueing once the track exists
    void failImport (const juce::String& message);  // ends a waiting import with an error

    StudioEngine& engine;
    std::unique_ptr<ProjectHost> projectHost;
    std::unique_ptr<ProjectClient> client;
    SessionLink link;
    juce::File cacheRoot;
    const SampleSource* engineSource = nullptr;
    bool everConnected = false;
    Id armedTrack;
    std::vector<Id> resumedTakes;

    // Take being recorded here.
    Id takeId, takeTrack;
    int64_t takePoint = 0;
    juce::String takeError;
    std::atomic<bool> stopping { false };
    mutable std::mutex liveLock;
    std::vector<Peak> livePeaks;

    // Import / export.
    std::thread importThread, exportThread;
    std::atomic<bool> importBusy { false }, importCancel { false }, exportBusy { false }, exportCancel { false };
    ExportInfo exportInfo;                      // message thread
    std::optional<ImportWait> importWait;       // message thread: the import's add_track is not applied yet
    std::optional<ImportFinish> importFinish;   // message thread: set before the queueing thread starts
    std::atomic<bool> importQueued { false };   // the queueing thread queued every segment

    std::shared_ptr<std::atomic<int>> generation;   // bumped by leave(): posts of an older session are dropped

    JUCE_DECLARE_NON_COPYABLE (SessionController)
};
}
