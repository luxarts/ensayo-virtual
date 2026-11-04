#pragma once
#include "MixServer.h"
#include "HighResClock.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
namespace ensayo
{
struct ServerConfig
{
    int tcpPort = defaultPort, udpPort = defaultPort;
    FrameSize frameSize = FrameSize::f128;
    juce::File uploadDirectory;        // default %TEMP%\EnsayoVirtual\server
    int maxParticipantsPerSession = 8;
};

class ServerHost
{
public:
    explicit ServerHost (ServerConfig);
    ~ServerHost();
    juce::Result start();              // binds TCP + UDP, starts accept + UDP threads. No clock.
    void stop();
    void startInternalClock();         // headless: HighResClock at frame period calling tick()
    void tick() noexcept;              // embedded: call from the host's audio callback once per frame
    int getSessionCount() const;
    std::vector<juce::String> getSessionIds() const;
    int getUdpPort() const noexcept;
    std::function<void (const juce::String&)> onLog;   // message-thread-agnostic; called from any thread

private:
    struct Session;
    struct Connection;
    class AcceptThread;
    class HousekeepingThread;
    friend struct Connection;

    void log (const juce::String&);

    // TCP control (connection threads).
    void onConnectionAccepted (juce::StreamingSocket*);
    void handleMessage (Connection&, const juce::var&);
    void handleCreate (Connection&, const juce::var&);
    void handleJoin (Connection&, const juce::var&);
    void handleGain (Connection&, const juce::var&);
    void handleTransport (Connection&, const juce::var&);
    void handleUpload (Connection&, const juce::var&);
    void joinSession (Connection&, std::shared_ptr<Session>, const juce::String& name, Role);
    void leaveSession (Connection&);
    uint32_t allocateClientId();                            // random, unused on this host; caller holds sessionsLock
    bool isClientIdInUse (uint32_t) const noexcept;         // caller holds sessionsLock
    void broadcast (Session&, const juce::var&);
    void broadcastParticipants (Session&);
    void broadcastTransport (Session&);
    void housekeeping();
    std::shared_ptr<Session> findSession (const juce::String& id) const;   // caller holds sessionsLock

    // UDP audio.
    void udpLoop();
    void sendMix (Session&, uint32_t clientId, const uint8_t* data, size_t len) noexcept;   // tick thread

    ServerConfig config;
    std::atomic<bool> running { false };
    juce::Random random;                                    // seeded randomly; used under sessionsLock

    // Protects `sessions`, each session's connection list and endpoint table. tick() try-locks it.
    mutable juce::SpinLock sessionsLock;
    std::vector<std::shared_ptr<Session>> sessions;

    // All live connections (joined or not); reaped by housekeeping once their thread has finished.
    juce::CriticalSection connectionsLock;
    std::vector<std::shared_ptr<Connection>> connections;

    std::unique_ptr<juce::StreamingSocket> listener;
    std::unique_ptr<AcceptThread> acceptThread;
    std::unique_ptr<HousekeepingThread> housekeepingThread;
    std::unique_ptr<HighResClock> clock;
    static constexpr int maxCatchUpTicks = 4;   // frames reproduced after ticks skipped on lock contention
    std::atomic<int> missedTicks { 0 };

    uintptr_t udpSocket = ~(uintptr_t) 0;     // SOCKET
    int boundUdpPort = 0;
    std::thread udpThread;
    bool winsockStarted = false;

    JUCE_DECLARE_NON_COPYABLE (ServerHost)
};
}
