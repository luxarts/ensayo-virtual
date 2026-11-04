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

            const uint16_t jump = (uint16_t) (2 + JitterBuffer::maxBlocks + 5);

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
            expect (jitterySize <= JitterBuffer::maxBlocks);
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

        beginTest ("6c. manual size clamps and disables auto changes");
        {
            JitterBuffer buffer (blockBytes, blockMs);
            buffer.setAutoSize (false);
            buffer.setSizeBlocks (100);
            expectEquals (buffer.getSizeBlocks(), JitterBuffer::maxBlocks);
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
