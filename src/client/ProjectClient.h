#pragma once
#include "core/Project.h"
#include "core/RemoteStreams.h"
#include "client/TakeCache.h"
#include "client/SessionLink.h"
#include <functional>
#include <memory>
namespace ensayo
{
struct ClientConfig { juce::File cacheRoot; Id user; juce::String userName; };   // cacheRoot = %LOCALAPPDATA%\EnsayoVirtual\cache
juce::File defaultCacheRoot();

struct LiveRecording { Id user, take, track; int64_t start = 0; std::vector<Peak> peaks; };
struct Presence { Id user; juce::String name; bool online = false; uint32_t clientId = 0; };   // clientId: online only
struct Transfers { int pendingUploads = 0; int downloadingTakes = 0; int pendingTakes = 0; };   // pendingTakes: own takes not yet complete with their region
// A take of this user that was begun (beginTake) but never finished (the app stopped mid-recording).
struct UnfinishedTake { Id take, track; int64_t start = 0, offset = 0; int channels = 0; int64_t samples = 0; };

class ProjectClient
{
public:
    explicit ProjectClient (ClientConfig);
    ~ProjectClient();
    // Message thread. Async: onConnected after `welcome` and the data connection are up; onError otherwise.
    void connect (const SessionLink&);
    void disconnect();                                        // flushes queued control lines (at most 500 ms) first
    bool isConnected() const noexcept;
    bool isHost() const noexcept;
    const Id& user() const noexcept;
    std::shared_ptr<const Project> project() const;          // latest replica (message thread)
    TakeCache* cache() noexcept;                              // created at `welcome`, per project id; nullptr before
    // Queued; the result arrives as onProjectChanged or onRejected (+ onOpRejected with this seq). Returns the op's
    // seq, or 0 when not welcomed (then onRejected is called right away).
    int sendOp (const juce::var& op);
    // Recording plumbing (message thread unless noted)
    // add_take op + rec_start. takeOffset (persisted, not sent) = take samples before the record point (count-in and
    // latency); used only to finish the take if the app stops before finishTake (see unfinishedTakes).
    // announce = false (imports): no rec_start/rec_stop, so nobody sees it as a live recording, and it never becomes
    // the take re-announced after a reconnect.
    void beginTake (const Id& take, int channels, const Id& track, int64_t timelineStart, int64_t takeOffset = 0,
                    bool announce = true);
    // Any thread. Peaks of the take begun here are also kept: a reconnect during the take re-sends rec_start and them.
    void sendPeaks (const Id& take, int64_t fromPeak, const std::vector<Peak>&);
    // Any thread: appends to the local cache right away, persists to <cacheRoot>/pending/<take>/<index>.flac,
    // uploads in order; survives disconnects (resent on the next connect, starting at the host's take.segments).
    void queueSegment (const Id& take, int index, const juce::MemoryBlock& flac);
    // When every segment of `take` is acknowledged: take_complete, then add_region `region` on `track`, then rec_stop.
    void finishTake (const Id& take, const Id& track, const Region& region);
    // Message thread, after onConnected: pending takes without a finish intent. samples = what the local cache holds.
    std::vector<UnfinishedTake> unfinishedTakes() const;
    // Message thread: forgets a pending take and its files and sends rec_stop, so no one keeps showing it as recording.
    void discardTake (const Id& take);
    // Live audio (voice chat, live instrument). Sends one encoded datagram (encodeAudioPacket) of stream `kind`; the
    // client stamps its own clientId and the kind's flag bit. Real-time safe; at most one calling thread per kind.
    // Dropped while not connected or when the per-kind queue is full.
    void sendAudio (StreamKind kind, const uint8_t* packet, size_t len) noexcept;
    // What the others send, one jitter buffer per (sender clientId, kind); see RemoteStreamTable for the threading.
    // The table lives as long as this client; slots are freed after 5 s of silence, when presence says the sender
    // left, and on every connect / disconnect.
    RemoteStreamTable& remoteStreams() noexcept;
    // Message thread: the online user behind a UDP clientId, from the latest presence (empty if unknown).
    Id userForClient (uint32_t clientId) const;
    std::vector<Presence> presence() const;
    std::vector<LiveRecording> liveRecordings() const;
    Transfers transfers() const;
    int pingMs() const noexcept;

    std::function<void()> onConnected, onProjectChanged, onPresenceChanged, onTransfersChanged;
    std::function<void (const juce::String&)> onRejected, onError, onDisconnected;   // message thread
    std::function<void (int seq, const juce::String& reason)> onOpRejected;           // message thread, before onRejected
    // Message thread: something the user should know that does not stop the session (a take the host cannot send
    // completely: `take_damaged`).
    std::function<void (const juce::String&)> onWarning;

    // Test hooks.
    int64_t bytesDownloaded() const noexcept;      // any thread: segment payload bytes received since the last connect()
    void dropConnectionForTest();                  // message thread: shuts both sockets down without the clean path
    std::function<void()> onWelcomeForTest;        // message thread: inside `welcome`, before pending uploads reconcile

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (ProjectClient)
};
}
