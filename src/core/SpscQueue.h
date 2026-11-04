#pragma once
#include "Protocol.h"
#include <atomic>
#include <array>
#include <cstring>
#include <vector>
namespace ensayo
{
// Fixed-capacity single-producer / single-consumer queue of byte packets (max maxPacketBytes each).
// All storage is allocated in the constructor; push and pop are lock-free and allocation-free.
class PacketQueue
{
public:
    explicit PacketQueue (int capacity = 64)
        : slots ((size_t) (capacity < 1 ? 1 : capacity) + 1)   // one spare slot distinguishes full from empty
    {
    }

    bool push (const uint8_t* data, size_t len) noexcept       // false when full or len > maxPacketBytes
    {
        if (data == nullptr || len == 0 || len > maxPacketBytes)
            return false;

        const int n = (int) slots.size();
        const int t = tail.load (std::memory_order_relaxed);
        const int next = (t + 1) % n;

        if (next == head.load (std::memory_order_acquire))
            return false;

        auto& slot = slots[(size_t) t];
        std::memcpy (slot.bytes.data(), data, len);
        slot.len = (uint16_t) len;
        tail.store (next, std::memory_order_release);
        return true;
    }

    bool pop (uint8_t* out, size_t& len) noexcept              // false when empty
    {
        len = 0;
        const int h = head.load (std::memory_order_relaxed);

        if (h == tail.load (std::memory_order_acquire))
            return false;

        if (out == nullptr)
            return false;

        const auto& slot = slots[(size_t) h];
        len = slot.len;
        std::memcpy (out, slot.bytes.data(), len);
        head.store ((h + 1) % (int) slots.size(), std::memory_order_release);
        return true;
    }

    int size() const noexcept
    {
        const int n = (int) slots.size();
        const int h = head.load (std::memory_order_acquire);
        const int t = tail.load (std::memory_order_acquire);
        return (t - h + n) % n;
    }

private:
    struct Slot { std::array<uint8_t, maxPacketBytes> bytes; uint16_t len = 0; };
    std::vector<Slot> slots; std::atomic<int> head { 0 }, tail { 0 };

    PacketQueue (const PacketQueue&) = delete;
    PacketQueue& operator= (const PacketQueue&) = delete;
};
}
