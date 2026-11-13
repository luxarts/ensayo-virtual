#include "core/RemoteStreams.h"

namespace ensayo
{
bool RemoteStreamTable::Slot::pop (uint64_t expectedState, uint8_t* out, int& len) noexcept
{
    len = 0;

    // Dekker with the producer's claim: raise `reading`, then look at `state` (both seq_cst). A claim either sees
    // `reading` up and leaves this slot alone, or happened before and this load sees its new state.
    reading.store (true);
    const auto current = state.load();

    if (current != expectedState || keyOf (current) == freeKey)
    {
        reading.store (false);
        return false;
    }

    const bool popped = buffer.pop (out, len);
    reading.store (false);

    if (! popped)
        len = 0;

    return popped;
}

int RemoteStreamTable::push (uint32_t clientId, StreamKind kind, uint16_t seq, const uint8_t* payload, int len,
                             double nowMs) noexcept
{
    if (clientId == 0 || clientId > 0x7fffffffu)
        return -1;

    const auto wanted = keyFor (clientId, kind);
    int index = -1, claimable = -1;

    for (int i = 0; i < capacity; ++i)
    {
        auto& slot = table[(size_t) i];
        const auto key = Slot::keyOf (slot.state.load (std::memory_order_relaxed));   // only this thread writes it

        if (key == wanted)
        {
            index = i;
            break;
        }

        // A free slot is claimable only while no pop is inside it (it may still be finishing a pop that began
        // before the release).
        if (key == freeKey && claimable < 0 && ! slot.reading.load())
            claimable = i;
    }

    if (index < 0)
    {
        if (claimable < 0)
            return -1;

        index = claimable;
        auto& slot = table[(size_t) index];
        const auto generation = Slot::generationOf (slot.state.load (std::memory_order_relaxed)) + 1;
        slot.buffer.resetForNewStream();
        slot.releaseState.store (0, std::memory_order_relaxed);
        slot.state.store (((uint64_t) generation << 32) | wanted);
    }

    auto& slot = table[(size_t) index];
    slot.lastPacketMs = nowMs;
    slot.buffer.push (seq, payload, len, nowMs);
    return index;
}

void RemoteStreamTable::sweep (double nowMs) noexcept
{
    for (auto& slot : table)
    {
        const auto state = slot.state.load (std::memory_order_relaxed);
        const auto requested = slot.releaseState.exchange (0, std::memory_order_acq_rel);

        if (Slot::keyOf (state) == freeKey)
            continue;

        if (requested == state || nowMs - slot.lastPacketMs >= idleReleaseMs)
            release (slot);
    }
}

void RemoteStreamTable::releaseClient (uint32_t clientId) noexcept
{
    for (auto& slot : table)
    {
        const auto state = slot.state.load (std::memory_order_acquire);
        const auto key = Slot::keyOf (state);

        if (key != freeKey && clientOf (key) == clientId)
            slot.releaseState.store (state, std::memory_order_release);
    }
}

void RemoteStreamTable::clear() noexcept
{
    for (auto& slot : table)
    {
        slot.releaseState.store (0, std::memory_order_relaxed);
        release (slot);
    }
}

int RemoteStreamTable::activeCount() const noexcept
{
    int count = 0;

    for (const auto& slot : table)
        if (Slot::keyOf (slot.state.load (std::memory_order_acquire)) != freeKey)
            ++count;

    return count;
}

void RemoteStreamTable::release (Slot& slot) noexcept
{
    const auto state = slot.state.load (std::memory_order_relaxed);
    slot.state.store (state & ~(uint64_t) 0xffffffffu);   // keeps the generation (seq_cst: see pop)
}
}
