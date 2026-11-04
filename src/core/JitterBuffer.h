#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <vector>
namespace ensayo
{
// Sequence-indexed ring of fixed-size blocks. One producer (network thread) calls push, one consumer
// (audio/tick thread) calls pop. Auto mode re-targets the size once per second from arrival jitter.
class JitterBuffer
{
public:
    static constexpr int minBlocks = 2, maxBlocks = 20, initialBlocks = 4;
    // Ring slots. A power of two so `seq & (numSlots - 1)` stays contiguous across the 16-bit wrap (65536 % 20 = 16
    // would alias seqs 16 apart near the wrap). Must exceed maxBlocks: the live window never spans more than that.
    static constexpr int numSlots = 32;
    static_assert ((numSlots & (numSlots - 1)) == 0 && numSlots > maxBlocks && 65536 % numSlots == 0);
    static constexpr int lateResyncThreshold = 8;   // consecutive late packets that re-anchor the window on the stream
    JitterBuffer (int blockBytes, double blockDurationMs);
    void setAutoSize (bool enabled) noexcept;      // default true
    void setSizeBlocks (int blocks) noexcept;      // manual; clamps; resets window
    int getSizeBlocks() const noexcept;
    // Producer. Late (behind the read position) → counted as late and dropped. More than maxBlocks ahead → window resync.
    void push (uint16_t seq, const uint8_t* data, int len, double arrivalMs) noexcept;
    // Consumer. Copies one block into out and returns true; false → caller does PLC. Advances the read sequence.
    bool pop (uint8_t* out, int& len) noexcept;
    void reset() noexcept;                         // forgets everything; next push defines the window
    struct Stats { int sizeBlocks = 0; uint32_t underruns = 0, late = 0, resyncs = 0; double jitterMs = 0.0; bool primed = false; };
    Stats getStats() const noexcept;
private:
    // storage: numSlots slots of blockBytes + len + valid + seq; readSeq; primed flag that waits for sizeBlocks
    // packets before the first pop; jitter estimator: EWMA of |arrival interval - blockDurationMs| and its
    // max over the last second; target = clamp(ceil((mean + 3*stddev + max) / blockDurationMs) + 1).

    // A slot tag is 0 (empty) or validTag | seq. Written by the producer after the payload (release) and
    // cleared by the consumer after a successful copy. A seq handed from producer to consumer uses the same
    // encoding so that 0 can mean "nothing pending".
    static constexpr uint32_t validTag = 1u << 16;
    static constexpr uint32_t taggedSeq (uint16_t seq) noexcept { return validTag | seq; }
    static constexpr uint16_t seqOf (uint32_t tag) noexcept { return (uint16_t) (tag & 0xFFFFu); }
    static constexpr int seqDistance (uint16_t from, uint16_t to) noexcept { return (int16_t) (uint16_t) (to - from); }
    static constexpr size_t slotOf (uint16_t seq) noexcept { return (size_t) (seq & (uint16_t) (numSlots - 1)); }

    void evaluateTarget() noexcept;
    void clearSlots() noexcept;
    void resetEstimator() noexcept;

    const int blockBytes;
    const double blockDurationMs;
    std::vector<uint8_t> storage;                                  // numSlots * blockBytes, sized once
    std::array<std::atomic<uint32_t>, numSlots> tags;
    std::array<std::atomic<int>, numSlots> lengths;

    // Shared configuration / statistics.
    std::atomic<bool> autoSize { true };
    std::atomic<int> sizeBlocks { initialBlocks };
    std::atomic<bool> windowResetRequest { false };               // control → producer: restart the estimator window
    std::atomic<uint32_t> underruns { 0 }, late { 0 }, resyncs { 0 };
    std::atomic<double> jitterMs { 0.0 };
    std::atomic<bool> primed { false };

    // Producer → consumer hand-offs.
    std::atomic<bool> hasWindow { false };                         // set by the producer on the first push
    std::atomic<uint32_t> pendingResync { 0 };                     // taggedSeq of the new window base, 0 = none
    std::atomic<uint32_t> highestSeq { 0 };                        // taggedSeq of the newest pushed packet

    // Consumer → producer.
    std::atomic<uint16_t> readSeq { 0 };

    // Consumer-only state.
    int appliedSize = initialBlocks;

    // Producer-only state (jitter estimator).
    struct Estimator
    {
        bool hasLast = false, windowStarted = false;
        double lastArrivalMs = 0.0, windowStartMs = 0.0;
        uint16_t lastSeq = 0;
        double mean = 0.0, variance = 0.0, windowMax = 0.0;
        int downCount = 0;
        int consecutiveLate = 0;
    };
    Estimator estimator;
};
}
