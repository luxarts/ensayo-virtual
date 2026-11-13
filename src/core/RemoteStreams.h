#pragma once
#include "core/JitterBuffer.h"
#include "core/Protocol.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace ensayo
{
// Live audio received from the other participants, one jitter buffer per (sender, stream kind).
//
// Threads:
//   producer - one thread (ProjectClient's UDP receiver): push, sweep. It alone claims and releases slots.
//   message  - releaseClient (presence says the sender left); clear only while no producer runs.
//   consumer - one thread (the audio callback): slots(), then per slot the protocol below.
//
// Consumer protocol, per slot and per block:
//   const auto state = slot.state.load (std::memory_order_acquire);    // key and generation in one load
//   if (Slot::keyOf (state) == freeKey)  -> nothing there (fade out what was sounding)
//   if (state != the state this slot decoded last) -> a new stream: fresh decoder, fade in (the previous one, if it
//   was sounding, is concealed and faded out like any other stop);
//   slot.pop (state, out, len)  -> false: nothing due, or the slot changed hands meanwhile (conceal).
//
// Claiming a slot resets its JitterBuffer (non-atomic storage), so claim and pop are kept apart with a Dekker
// hand-off: pop raises `reading` and then checks `state`; the producer only claims a free slot whose `reading` is
// down (a free slot cannot be entered, since pop sees it free and leaves), and otherwise takes another free slot or
// drops the packet.
class RemoteStreamTable
{
public:
    static constexpr int capacity = 16;
    static constexpr double idleReleaseMs = 5000.0;     // a stream with no packet for this long frees its slot
    static constexpr uint32_t freeKey = 0;

    // clientIds are 1 .. 0x7fffffff (the host's range), so a key is never freeKey and never overflows.
    static constexpr uint32_t keyFor (uint32_t clientId, StreamKind kind) noexcept { return (clientId << 1) | (uint32_t) kind; }
    static constexpr uint32_t clientOf (uint32_t key) noexcept   { return key >> 1; }
    static constexpr StreamKind kindOf (uint32_t key) noexcept   { return (StreamKind) (key & 1u); }

    struct Slot
    {
        // generation << 32 | key. Written by the producer only (seq_cst). The generation grows on every claim and is
        // kept on release, so a state value names one stream instance.
        std::atomic<uint64_t> state { 0 };
        static constexpr uint32_t keyOf (uint64_t s) noexcept         { return (uint32_t) s; }
        static constexpr uint32_t generationOf (uint64_t s) noexcept  { return (uint32_t) (s >> 32); }
        uint32_t key() const noexcept                                 { return keyOf (state.load (std::memory_order_acquire)); }

        // Sized for the largest payload (128-sample stereo, 96 B); voice blocks are 48 B.
        JitterBuffer buffer { codedBytesStereo (FrameSize::f128), frameSamples (FrameSize::f128) * 1000.0 / sampleRate };

        // Consumer: one block of the stream whose state was `expectedState`. False when nothing is due, or the slot
        // was released / claimed again (then len = 0).
        bool pop (uint64_t expectedState, uint8_t* out, int& len) noexcept;

        // Consumer-owned (raised only inside pop); public so tests can hold a slot as if mid-pop.
        std::atomic<bool> reading { false };

    private:
        friend class RemoteStreamTable;
        std::atomic<uint64_t> releaseState { 0 };       // message thread -> producer: release if state still equals it
        double lastPacketMs = 0.0;                      // producer only
    };

    RemoteStreamTable() = default;

    // Producer. Routes one payload to the slot of (clientId, kind), claiming a free slot on a stream's first packet.
    // Returns the slot index, or -1 when clientId is out of range or no slot can take a new stream right now.
    int push (uint32_t clientId, StreamKind kind, uint16_t seq, const uint8_t* payload, int len, double nowMs) noexcept;
    // Producer, at least every few hundred ms: releases idle streams and the ones requested by releaseClient.
    void sweep (double nowMs) noexcept;

    // Message thread: every stream of clientId is released at the producer's next sweep.
    void releaseClient (uint32_t clientId) noexcept;
    // Only while no producer runs (e.g. between connections): frees every slot.
    void clear() noexcept;

    // Consumer (any thread for reading states).
    std::span<Slot, capacity> slots() noexcept           { return table; }
    int activeCount() const noexcept;

private:
    static void release (Slot&) noexcept;

    std::array<Slot, capacity> table;
};
}
