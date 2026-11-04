#pragma once
#include "core/Protocol.h"
#include "core/JitterBuffer.h"
#include "core/SpscQueue.h"
#include "core/MixMath.h"
#include "core/Participants.h"                // ParticipantInfo, ParticipantStats (shared with server/MixServer.h)
#include "client/AudioTransportInterface.h"   // AudioTransport (shared with client/AudioEngine.h)
#include "client/SessionLink.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
namespace ensayo
{
struct SessionState
{
    bool connected = false; juce::String sessionId; uint32_t clientId = 0; juce::String host; int port = 0;
    std::vector<ParticipantInfo> participants; std::vector<ParticipantStats> stats;
    TransportState transport; int pingMs = -1; FrameSize frameSize = FrameSize::f128;
    juce::String trackUploadStatus; float trackUploadProgress = 0;   // 0..1
};

class SessionClient : public AudioTransport
{
public:
    SessionClient(); ~SessionClient() override;
    // Message thread. Callbacks are delivered on the message thread (juce::MessageManager::callAsync).
    void create (const juce::String& host, int port, const juce::String& name, Role);
    void join (const SessionLink&, const juce::String& name, Role);
    void leave();
    void sendGain (uint32_t target, const ChannelGain&);
    void sendTransport (const juce::String& cmd, int64_t position = 0, bool loop = false);
    void uploadTrack (const juce::File&);          // background thread; progress via onStateChanged
    const SessionState& getState() const noexcept;
    std::function<void (const SessionState&)> onStateChanged;
    std::function<void (const juce::String&)> onError;        // also fires on disconnect
    std::function<void()> onJoined;
    // AudioTransport
    void sendAudio (const uint8_t*, size_t) noexcept override;
    JitterBuffer& receiveBuffer() noexcept override;
    uint32_t getClientId() const noexcept override;
    uint32_t getTrackPosition() const noexcept override;
    // UDP thread: sends queued packets, receives mix packets → receiveBuffer().push (seq, payload, arrival), stores trackPosition.
    // Control thread: StreamingSocket read loop; ping every 1 s; reconnect is not attempted.

private:
    class ControlThread;
    class UdpThread;
    class UploadThread;
    friend class ControlThread;
    friend class UdpThread;
    friend class UploadThread;

    void connect (const juce::String& host, int port, const juce::var& hello);
    void stopThreads();
    void sendLine (const juce::var& message);                    // any thread; serialised by writeLock
    bool trySendLine (const juce::var& message);                 // skipped while an upload holds the socket
    void post (std::function<void()> fn);                        // message thread, dropped after leave()
    void notifyStateChanged();

    // Control thread → message thread.
    void handleLine (const juce::String& line);
    void handleJoined (const juce::var&);
    void handleDisconnected (const juce::String& reason);
    static std::vector<ParticipantInfo> parseParticipants (const juce::var& list);
    static std::vector<ParticipantStats> parseStats (const juce::var& list);

    SessionState state;
    std::unique_ptr<JitterBuffer> buffer;
    PacketQueue outgoing;
    std::atomic<uint32_t> clientId { 0 }, trackPosition { 0 };

    juce::StreamingSocket socket;
    juce::CriticalSection writeLock;
    std::unique_ptr<ControlThread> control;
    std::unique_ptr<UdpThread> udp;
    std::unique_ptr<UploadThread> upload;

    std::shared_ptr<std::atomic<int>> generation;   // bumped by leave(); queued callbacks for older generations are dropped
    juce::String serverHost;
    int serverUdpPort = 0;

    JUCE_DECLARE_NON_COPYABLE (SessionClient)
};
}
