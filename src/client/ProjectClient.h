#pragma once
#include "core/Project.h"
#include "core/JitterBuffer.h"
#include "client/TakeCache.h"
#include "client/SessionLink.h"
#include <functional>
#include <memory>
namespace ensayo
{
struct ClientConfig { juce::File cacheRoot; Id user; juce::String userName; };   // cacheRoot = %LOCALAPPDATA%\EnsayoVirtual\cache
juce::File defaultCacheRoot();

struct LiveRecording { Id user, take, track; int64_t start = 0; std::vector<Peak> peaks; };
struct Presence { Id user; juce::String name; bool online = false; };
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
    void disconnect();
    bool isConnected() const noexcept;
    bool isHost() const noexcept;
    const Id& user() const noexcept;
    std::shared_ptr<const Project> project() const;          // latest replica (message thread)
    TakeCache* cache() noexcept;                              // created at `welcome`, per project id; nullptr before
    void sendOp (const juce::var& op);                         // queued; result arrives as onProjectChanged or onRejected
    // Recording plumbing (message thread unless noted)
    // add_take op + rec_start. takeOffset (persisted, not sent) = take samples before the record point (count-in and
    // latency); used only to finish the take if the app stops before finishTake (see unfinishedTakes).
    void beginTake (const Id& take, int channels, const Id& track, int64_t timelineStart, int64_t takeOffset = 0);
    void sendPeaks (const Id& take, int64_t fromPeak, const std::vector<Peak>&);           // any thread
    // Any thread: appends to the local cache right away, persists to <cacheRoot>/pending/<take>/<index>.flac,
    // uploads in order; survives disconnects (resent on the next connect, starting at the host's take.segments).
    void queueSegment (const Id& take, int index, const juce::MemoryBlock& flac);
    // When every segment of `take` is acknowledged: take_complete, then add_region `region` on `track`, then rec_stop.
    void finishTake (const Id& take, const Id& track, const Region& region);
    // Message thread, after onConnected: pending takes without a finish intent. samples = what the local cache holds.
    std::vector<UnfinishedTake> unfinishedTakes() const;
    void discardTake (const Id& take);                         // message thread: forgets a pending take and its files
    void setListening (bool);
    void sendPreview (const uint8_t* packet, size_t len) noexcept;   // any thread (UDP)
    JitterBuffer* previewBuffer() noexcept;                           // mix-free: datagrams from whoever records
    std::vector<Presence> presence() const;
    std::vector<LiveRecording> liveRecordings() const;
    Transfers transfers() const;
    int pingMs() const noexcept;

    std::function<void()> onConnected, onProjectChanged, onPresenceChanged, onTransfersChanged;
    std::function<void (const juce::String&)> onRejected, onError, onDisconnected;   // message thread

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
