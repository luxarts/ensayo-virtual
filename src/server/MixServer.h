#pragma once
#include "core/Protocol.h"
#include "core/OpusCodec.h"
#include "core/JitterBuffer.h"
#include "core/MixMath.h"
#include "BackingTrack.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
namespace ensayo
{
struct ParticipantInfo { uint32_t id = 0; juce::String name; Role role = Role::both; };
struct ParticipantStats { uint32_t id = 0; int jitterBlocks = 0; uint32_t underruns = 0, late = 0; float voicePeak = 0, instrumentPeak = 0; };

// One session. All public methods except tick()/pushAudio() are "control" calls (any thread, serialised by
// the caller through a lock); tick() runs on the clock thread; pushAudio() on the network thread.
class MixServer
{
public:
    using Sender = std::function<void (uint32_t clientId, const uint8_t* data, size_t len)>;
    explicit MixServer (FrameSize);
    ~MixServer();
    FrameSize frameSize() const noexcept;

    uint32_t addParticipant (const juce::String& name, Role);   // ids start at 1 and never repeat
    void removeParticipant (uint32_t id);
    std::vector<ParticipantInfo> getParticipants() const;
    bool isEmpty() const;

    // target 0 = backing track (its level/mute come from ChannelGain::voiceDb / muted). Unknown ids ignored.
    void setGain (uint32_t client, uint32_t target, const ChannelGain&);
    BackingTrack& track() noexcept;

    void pushAudio (const AudioPacketView&, double arrivalMs) noexcept;   // network thread
    // Clock thread. Pops one block per participant, decodes (PLC on gaps), renders the track, mixes one stereo
    // block per client excluding that client, encodes and calls send. Returns the number of packets sent.
    int tick (const Sender& send) noexcept;
    std::vector<ParticipantStats> getStats() const;
    uint32_t getTrackPosition() const noexcept;

    // Extra (not part of the plan contract): the id the next addParticipant() hands out, so a host that
    // multiplexes several sessions on one UDP port can keep ids unique across sessions. Only raises the counter.
    void setNextParticipantId (uint32_t id);

private:
    struct Participant;

    // Real-time paths (tick, pushAudio) enter as "readers" without blocking: they back off when a control
    // thread is restructuring. Control threads wait until no reader is inside before mutating.
    bool tryEnterRealtime() noexcept;
    void leaveRealtime() noexcept;
    struct ControlMutation;

    Participant* findUnlocked (uint32_t id) const noexcept;

    const FrameSize size;
    const int frame;
    OpusMode mode;
    BackingTrack backingTrack;

    mutable std::mutex controlMutex;                 // serialises control calls between themselves
    std::atomic<bool> writerActive { false };
    std::atomic<int> readers { 0 };

    std::vector<std::unique_ptr<Participant>> participants;
    uint32_t nextId = 1;

    std::vector<float> trackL, trackR;               // one block, rendered once per tick
    std::vector<float> mixL, mixR, interleaved;      // scratch for one client's mix
    std::vector<uint8_t> coded, packet;              // scratch for one client's packet
    std::atomic<uint32_t> trackPosition { 0 };
    uint32_t sampleTime = 0;

    JUCE_DECLARE_NON_COPYABLE (MixServer)
};
}
