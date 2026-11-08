#include "core/JitterBuffer.h"

#include <juce_core/juce_core.h>
#include <array>
#include <cstring>

namespace ensayo
{
class JitterBufferTests : public juce::UnitTest
{
public:
    JitterBufferTests() : juce::UnitTest ("JitterBuffer", "EnsayoVirtual") {}

    static constexpr int blockBytes = 48;
    static constexpr double blockMs = 128.0 / 48.0;   // 2.6667 ms

    static std::array<uint8_t, blockBytes> blockFor (uint16_t seq)
    {
        std::array<uint8_t, blockBytes> block {};

        for (size_t i = 0; i < block.size(); ++i)
            block[i] = (uint8_t) (seq * 7 + i);

        return block;
    }

    static double timeFor (uint16_t seq) { return seq * blockMs; }

    void pushSeq (JitterBuffer& buffer, uint16_t seq, double arrivalMs)
    {
        const auto block = blockFor (seq);
        buffer.push (seq, block.data(), (int) block.size(), arrivalMs);
    }

    void pushSeq (JitterBuffer& buffer, uint16_t seq) { pushSeq (buffer, seq, timeFor (seq)); }

    bool popExpecting (JitterBuffer& buffer, uint16_t seq)
    {
        std::array<uint8_t, blockBytes> out {};
        int len = 0;
        const bool ok = buffer.pop (out.data(), len);

        if (ok)
        {
            expectEquals (len, blockBytes);
            expect (out == blockFor (seq), "wrong bytes for seq " + juce::String (seq));
        }

        return ok;
    }

    bool popDiscarding (JitterBuffer& buffer)
    {
        std::array<uint8_t, blockBytes> out {};
        int len = 0;
        return buffer.pop (out.data(), len);
    }

    void runTest() override
    {
        beginTest ("1. in-order push/pop, priming waits for initialBlocks");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);
            expectEquals (buffer.getSizeBlocks(), JitterBuffer::initialBlocks);
            expect (! buffer.getStats().primed);

            expect (! popDiscarding (buffer), "pop before any push must fail");
            expectEquals ((int) buffer.getStats().underruns, 0);

            for (uint16_t seq = 0; seq < JitterBuffer::initialBlocks - 1; ++seq)
            {
                pushSeq (buffer, seq);
                expect (! popDiscarding (buffer), "pop must fail until primed");
            }

            expectEquals ((int) buffer.getStats().underruns, 0);
            expect (! buffer.getStats().primed);

            pushSeq (buffer, (uint16_t) (JitterBuffer::initialBlocks - 1));

            for (uint16_t seq = 0; seq < JitterBuffer::initialBlocks; ++seq)
                expect (popExpecting (buffer, seq), "pop " + juce::String (seq));

            expect (buffer.getStats().primed);

            for (uint16_t seq = JitterBuffer::initialBlocks; seq < 40; ++seq)
            {
                pushSeq (buffer, seq);
                expect (popExpecting (buffer, seq), "steady state pop " + juce::String (seq));
            }

            const auto stats = buffer.getStats();
            expectEquals ((int) stats.underruns, 0);
            expectEquals ((int) stats.late, 0);
            expectEquals ((int) stats.resyncs, 0);
        }

        beginTest ("2. missing sequence yields exactly one failed pop");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);

            for (uint16_t seq = 0; seq < 4; ++seq)
                pushSeq (buffer, seq);

            expect (popExpecting (buffer, 0));
            pushSeq (buffer, 4);
            expect (popExpecting (buffer, 1));
            pushSeq (buffer, 6);   // 5 never arrives
            expect (popExpecting (buffer, 2));
            pushSeq (buffer, 7);
            expect (popExpecting (buffer, 3));
            expect (popExpecting (buffer, 4));
            expect (! popDiscarding (buffer), "missing seq 5 must fail once");
            expectEquals ((int) buffer.getStats().underruns, 1);
            expect (popExpecting (buffer, 6));
            expect (popExpecting (buffer, 7));
            expectEquals ((int) buffer.getStats().underruns, 1);
        }

        beginTest ("3. out-of-order arrival within the window is reordered");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);

            pushSeq (buffer, 0);
            pushSeq (buffer, 1);
            pushSeq (buffer, 3);
            pushSeq (buffer, 2);
            pushSeq (buffer, 5);
            pushSeq (buffer, 4);

            for (uint16_t seq = 0; seq < 6; ++seq)
                expect (popExpecting (buffer, seq), "reordered pop " + juce::String (seq));

            expectEquals ((int) buffer.getStats().underruns, 0);
            expectEquals ((int) buffer.getStats().late, 0);
        }

        beginTest ("4. packets behind the read position are late and ignored");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);

            for (uint16_t seq = 0; seq < 6; ++seq)
                pushSeq (buffer, seq);

            expect (popExpecting (buffer, 0));
            expect (popExpecting (buffer, 1));
            expect (popExpecting (buffer, 2));

            pushSeq (buffer, 1);   // late
            pushSeq (buffer, 2);   // late
            expectEquals ((int) buffer.getStats().late, 2);

            expect (popExpecting (buffer, 3));
            expect (popExpecting (buffer, 4));
            expect (popExpecting (buffer, 5));
            expectEquals ((int) buffer.getStats().underruns, 0);
        }

        beginTest ("5. jump ahead beyond maxBlocks resyncs the window");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);

            for (uint16_t seq = 0; seq < 8; ++seq)
                pushSeq (buffer, seq);

            expect (popExpecting (buffer, 0));
            expect (popExpecting (buffer, 1));

            const uint16_t jump = (uint16_t) (2 + buffer.getMaxBlocks() + 5);

            for (uint16_t i = 0; i < JitterBuffer::initialBlocks; ++i)
                pushSeq (buffer, (uint16_t) (jump + i));

            expectEquals ((int) buffer.getStats().resyncs, 1);

            for (uint16_t i = 0; i < JitterBuffer::initialBlocks; ++i)
                expect (popExpecting (buffer, (uint16_t) (jump + i)), "post-resync pop " + juce::String (i));

            expectEquals ((int) buffer.getStats().underruns, 0);
            expectEquals ((int) buffer.getStats().late, 0);
        }

        beginTest ("5b. sequence numbers wrap around 65535");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);
            uint16_t seq = 65530;

            for (int i = 0; i < 4; ++i)
                pushSeq (buffer, (uint16_t) (seq + i));

            for (int i = 0; i < 12; ++i)
            {
                pushSeq (buffer, (uint16_t) (seq + 4 + i));
                expect (popExpecting (buffer, (uint16_t) (seq + i)), "wrap pop " + juce::String (i));
            }

            expectEquals ((int) buffer.getStats().resyncs, 0);
            expectEquals ((int) buffer.getStats().underruns, 0);
        }

        beginTest ("5c. slots do not alias across the 16-bit wrap at the maximum size");
        {
            // With 20 slots indexed by seq % 20 the slot sequence breaks at the wrap (65535 % 20 = 15, then 0), so
            // packets 16 apart share a slot and a full 20-block window overwrites blocks it has not consumed yet.
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);
            buffer.setSizeBlocks (buffer.getMaxBlocks());
            const uint16_t start = 65500;

            for (int i = 0; i < buffer.getMaxBlocks(); ++i)
                pushSeq (buffer, (uint16_t) (start + i));

            // Steady state with the producer a full window ahead of the consumer, crossing 65535 -> 0.
            for (int i = 0; i < 100; ++i)
            {
                expect (popExpecting (buffer, (uint16_t) (start + i)), "wrap pop at full size, i = " + juce::String (i));
                pushSeq (buffer, (uint16_t) (start + buffer.getMaxBlocks() + i));
            }

            const auto stats = buffer.getStats();
            expectEquals ((int) stats.underruns, 0);
            expectEquals ((int) stats.late, 0);
            expectEquals ((int) stats.resyncs, 0);
        }

        beginTest ("6. auto size grows with jitter and shrinks with clean arrivals");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            expectEquals (buffer.getSizeBlocks(), JitterBuffer::initialBlocks);

            juce::Random random (42);
            uint16_t seq = 0;
            double ideal = 0.0;

            // 2 s of +-3 ms jitter, one consumer pop per push.
            const int jitteryPackets = (int) (2000.0 / blockMs);
            int sizeBefore1000 = -1;

            for (int i = 0; i < jitteryPackets; ++i, ++seq)
            {
                const double arrival = ideal + (random.nextDouble() * 6.0 - 3.0);
                pushSeq (buffer, seq, arrival);
                popDiscarding (buffer);

                if (ideal < 990.0)
                    sizeBefore1000 = buffer.getSizeBlocks();

                ideal += blockMs;
            }

            expectEquals (sizeBefore1000, JitterBuffer::initialBlocks, "size must not change within the first second");
            const int jitterySize = buffer.getSizeBlocks();
            logMessage ("size after 2 s of jitter: " + juce::String (jitterySize) + " blocks, jitter "
                        + juce::String (buffer.getStats().jitterMs, 2) + " ms");
            expect (jitterySize > JitterBuffer::initialBlocks, "jitter must raise the size above initialBlocks");
            expect (jitterySize <= buffer.getMaxBlocks());
            expect (buffer.getStats().jitterMs > 0.5);

            // 10 s of exact arrivals. Down moves need three consecutive lower evaluations (one per second),
            // so 1.5 s into the clean phase at most two evaluations have happened and the size must still be
            // at the jittery value; afterwards it comes down, never below minBlocks.
            const int cleanPackets = (int) (10000.0 / blockMs);
            int sizeAt1500 = -1;

            for (int i = 0; i < cleanPackets; ++i, ++seq)
            {
                pushSeq (buffer, seq, ideal);
                popDiscarding (buffer);

                if (i == (int) (1500.0 / blockMs))
                    sizeAt1500 = buffer.getSizeBlocks();

                ideal += blockMs;
            }

            expectEquals (sizeAt1500, jitterySize, "hysteresis must hold the size for three evaluations");
            const int cleanSize = buffer.getSizeBlocks();
            logMessage ("size after 10 s clean: " + juce::String (cleanSize) + " blocks");
            expect (cleanSize < jitterySize, "clean arrivals must shrink the size");
            expect (cleanSize >= JitterBuffer::minBlocks);
            expect (buffer.getStats().jitterMs < 0.1);
        }

        beginTest ("6b. up moves apply immediately at the one-second boundary");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            juce::Random random (7);
            uint16_t seq = 0;
            double ideal = 0.0;

            // The first arrival (window start) lies in (-3, 3) ms, so arrivals below 997 ms can never
            // complete a 1000 ms window and an arrival at 1003.5 ms always does.
            while (ideal < 994.0)
            {
                pushSeq (buffer, seq++, ideal + (random.nextDouble() * 6.0 - 3.0));
                popDiscarding (buffer);
                ideal += blockMs;
            }

            expectEquals (buffer.getSizeBlocks(), JitterBuffer::initialBlocks);

            pushSeq (buffer, seq++, 1003.5);
            expect (buffer.getSizeBlocks() > JitterBuffer::initialBlocks, "first evaluation must raise the size");
        }

        beginTest ("6d. the cap is a duration: 80 ms at 64 and at 128 samples");
        {
            JitterBuffer at64 (24, 64.0 / 48.0), at128 (blockBytes, blockMs);
            expectEquals (at64.getMaxBlocks(), 60);
            expectEquals (at128.getMaxBlocks(), 30);
            expect (at64.getMaxBlocks() * 64.0 / 48.0 >= 79.9 && at128.getMaxBlocks() * blockMs >= 79.9);
            expect (at64.getMaxBlocks() < JitterBuffer::numSlots / 2 + 1);
        }

        beginTest ("6e. 64-sample stream with 30 ms spikes (Tailscale, 2026-11-08) stops dropping once sized");
        {
            // Measured path: ~7 ms with spikes to 16 and 32 ms. A spike delays a run of packets that then arrive
            // together. With the old 20-block cap (27 ms at 64 samples) every 32 ms spike was an underrun.
            const double frameMs = 64.0 / 48.0;
            JitterBuffer buffer (24, frameMs);
            juce::Random random (42);
            const uint8_t payload[24] = {};
            uint8_t out[64];
            uint16_t seq = 0;
            int pops = 0;
            uint32_t underrunsAfterWarmup = 0, underrunsAtWarmup = 0;
            double spikeUntilMs = -1.0;

            // 20 s of 1.333 ms ticks; the producer pushes each packet when it "arrives", the consumer pops every tick.
            std::vector<double> arrivals;

            for (int i = 0; i < 15000; ++i)
            {
                const double sent = i * frameMs;

                if (spikeUntilMs < sent && random.nextInt (400) == 0)   // a spike roughly every 0.5 s
                    spikeUntilMs = sent + (random.nextBool() ? 16.0 : 32.0);

                const double delay = 7.0 + random.nextDouble() * 2.0;
                arrivals.push_back (std::max (sent + delay, spikeUntilMs > sent ? spikeUntilMs + delay : 0.0));
            }

            size_t next = 0;

            for (int tick = 0; tick < 15000 + 100; ++tick)
            {
                const double now = 10.0 + tick * frameMs;

                while (next < arrivals.size() && arrivals[next] <= now)
                    buffer.push (seq++, payload, sizeof (payload), arrivals[next++]);

                int len = 0;
                buffer.pop (out, len);
                ++pops;

                if (tick == 5 * 750)   // after 5 s the estimator has seen the spikes
                    underrunsAtWarmup = buffer.getStats().underruns;
            }

            underrunsAfterWarmup = buffer.getStats().underruns - underrunsAtWarmup;
            const double lossPercent = 100.0 * underrunsAfterWarmup / (15000.0 - 5 * 750);
            logMessage ("size " + juce::String (buffer.getSizeBlocks()) + " blocks ("
                        + juce::String (buffer.getSizeBlocks() * frameMs, 1) + " ms), loss after warm-up "
                        + juce::String (lossPercent, 2) + " %");
            expect (buffer.getSizeBlocks() > 20, "the size must grow past the old 20-block cap");
            expect (lossPercent < 1.0, "loss after warm-up must stay under 1 %, got " + juce::String (lossPercent, 2));
        }

        beginTest ("6c. manual size clamps and disables auto changes");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);
            buffer.setSizeBlocks (100);
            expectEquals (buffer.getSizeBlocks(), buffer.getMaxBlocks());
            buffer.setSizeBlocks (0);
            expectEquals (buffer.getSizeBlocks(), JitterBuffer::minBlocks);
            buffer.setSizeBlocks (6);
            expectEquals (buffer.getSizeBlocks(), 6);

            juce::Random random (3);
            double ideal = 0.0;

            for (uint16_t seq = 0; seq < 1500; ++seq)
            {
                pushSeq (buffer, seq, ideal + (random.nextDouble() * 6.0 - 3.0));
                popDiscarding (buffer);
                ideal += blockMs;
            }

            expectEquals (buffer.getSizeBlocks(), 6, "manual mode must not re-target");
            expectEquals (buffer.getStats().sizeBlocks, 6);
        }

        beginTest ("6d. a larger size is applied by re-buffering, a smaller one by skipping ahead");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);
            uint16_t next = 0;

            for (; next < 4; ++next)
                pushSeq (buffer, next);

            expect (popExpecting (buffer, 0));

            buffer.setSizeBlocks (6);
            pushSeq (buffer, next++);
            expect (! popDiscarding (buffer), "growing by two must stall once");
            pushSeq (buffer, next++);
            expect (! popDiscarding (buffer), "growing by two must stall twice");
            pushSeq (buffer, next++);
            expect (popExpecting (buffer, 1), "after the stall the read position is unchanged");
            expectEquals ((int) buffer.getStats().underruns, 0);

            buffer.setSizeBlocks (3);
            pushSeq (buffer, next++);
            expect (popExpecting (buffer, 5), "shrinking by three skips three blocks");
        }

        beginTest ("7. reset forgets the window");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);

            for (uint16_t seq = 0; seq < 6; ++seq)
                pushSeq (buffer, seq);

            expect (popExpecting (buffer, 0));
            buffer.reset();
            expect (! popDiscarding (buffer));
            expect (! buffer.getStats().primed);
            expectEquals ((int) buffer.getStats().underruns, 0);

            for (uint16_t seq = 1000; seq < 1004; ++seq)
                pushSeq (buffer, seq);

            expectEquals ((int) buffer.getStats().late, 0);
            expectEquals ((int) buffer.getStats().resyncs, 0);
            expect (popExpecting (buffer, 1000));
        }

        beginTest ("4b. a run of late packets re-anchors the window instead of dropping the stream");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);

            for (uint16_t seq = 0; seq < 4; ++seq)
                pushSeq (buffer, seq);

            for (uint16_t seq = 0; seq < 4; ++seq)
                expect (popExpecting (buffer, seq));

            // The producer stalls: the consumer keeps popping (underruns) and runs 10 blocks ahead.
            for (int i = 0; i < 10; ++i)
                expect (! popDiscarding (buffer));

            expectEquals ((int) buffer.getStats().underruns, 10);

            // The stream resumes where it left off: every packet is behind the read position.
            for (uint16_t seq = 4; seq < 4 + JitterBuffer::lateResyncThreshold; ++seq)
                pushSeq (buffer, seq);

            expectEquals ((int) buffer.getStats().late, JitterBuffer::lateResyncThreshold);
            expectEquals ((int) buffer.getStats().resyncs, 1);

            const uint16_t anchor = (uint16_t) (4 + JitterBuffer::lateResyncThreshold - 1);   // the packet that triggered it

            for (uint16_t seq = (uint16_t) (anchor + 1); seq < anchor + 4; ++seq)
                pushSeq (buffer, seq);

            for (uint16_t seq = anchor; seq < anchor + 4; ++seq)
                expect (popExpecting (buffer, seq), "post-late-resync pop " + juce::String (seq));

            expectEquals ((int) buffer.getStats().underruns, 10);
            expectEquals ((int) buffer.getStats().late, JitterBuffer::lateResyncThreshold);

            // Isolated reordering is still just "late".
            pushSeq (buffer, (uint16_t) (anchor + 4));
            pushSeq (buffer, 1);
            pushSeq (buffer, (uint16_t) (anchor + 5));
            pushSeq (buffer, 2);
            expectEquals ((int) buffer.getStats().late, JitterBuffer::lateResyncThreshold + 2);
            expectEquals ((int) buffer.getStats().resyncs, 1);
        }

        beginTest ("oversized and empty blocks are ignored");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);
            std::array<uint8_t, blockBytes + 1> big {};

            for (uint16_t seq = 0; seq < 4; ++seq)
                buffer.push (seq, big.data(), (int) big.size(), timeFor (seq));

            expect (! popDiscarding (buffer));
            buffer.push (0, big.data(), 0, 0.0);
            expect (! popDiscarding (buffer));
        }
    }
};

static JitterBufferTests jitterBufferTests;
}
