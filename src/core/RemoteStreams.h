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
//   const auto key = slot.key.load (std::memory_order_acquire);          // freeKey = nothing there
//   const auto generation = slot.generation.load (std::memory_order_acquire);
//   if (key or generation differ from what this slot decoded last) -> a new stream: fresh decoder, fade in
//   (and the previous one, if it was sounding, is concealed and faded out like any other stop);
//   slot.pop (generation, out, len)  -> false: nothing due, or the slot changed hands meanwhile (conceal).
// A slot's JitterBuffer is reset by the producer when it is claimed, so a stream always starts on a clean window.
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
        std::atomic<uint32_t> key { freeKey };          // written by the producer only (release)
        std::atomic<uint32_t> generation { 0 };         // +1 on every claim, before `key` is published
        // Sized for the largest payload (128-sample stereo, 96 B); voice blocks are 48 B.
        JitterBuffer buffer { codedBytesStereo (FrameSize::f128), frameSamples (FrameSize::f128) * 1000.0 / sampleRate };

        // Consumer: one block of the stream claimed as `expectedGeneration`. False when nothing is due or the slot
        // was released / claimed again meanwhile (then len = 0 and the block, if any, is not this stream's).
        bool pop (uint32_t expectedGeneration, uint8_t* out, int& len) noexcept;

    private:
        friend class RemoteStreamTable;
        std::atomic<uint32_t> releaseKey { freeKey };   // message thread -> producer: release if key still equals it
        double lastPacketMs = 0.0;                      // producer only
    };

    RemoteStreamTable() = default;

    // Producer. Routes one payload to the slot of (clientId, kind), claiming a free slot on a stream's first packet.
    // Returns the slot index, or -1 when clientId is out of range or every slot is taken by another stream.
    int push (uint32_t clientId, StreamKind kind, uint16_t seq, const uint8_t* payload, int len, double nowMs) noexcept;
    // Producer, at least every few hundred ms: releases idle streams and the ones requested by releaseClient.
    void sweep (double nowMs) noexcept;

    // Message thread: every stream of clientId is released at the producer's next sweep.
    void releaseClient (uint32_t clientId) noexcept;
    // Only while no producer runs (e.g. between connections): frees every slot.
    void clear() noexcept;

    // Consumer (any thread for reading keys).
    std::span<Slot, capacity> slots() noexcept           { return table; }
    int activeCount() const noexcept;

private:
    void release (Slot&) noexcept;

    std::array<Slot, capacity> table;
};
}
