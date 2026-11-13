#include "core/RemoteStreams.h"

#include <juce_core/juce_core.h>
#include <array>
#include <vector>

namespace ensayo
{
class RemoteStreamsTests : public juce::UnitTest
{
public:
    RemoteStreamsTests() : juce::UnitTest ("RemoteStreams", "EnsayoVirtual") {}

    static constexpr double blockMs = 128.0 / 48.0;

    // A 48-byte voice block whose first two bytes identify the sender and the sequence.
    static std::array<uint8_t, 48> block (uint8_t sender, uint16_t seq)
    {
        std::array<uint8_t, 48> b {};
        b.fill ((uint8_t) (seq * 3));
        b[0] = sender;
        b[1] = (uint8_t) seq;
        return b;
    }

    int slotOf (RemoteStreamTable& table, uint32_t clientId, StreamKind kind)
    {
        const auto key = RemoteStreamTable::keyFor (clientId, kind);
        int found = -1;

        for (int i = 0; i < RemoteStreamTable::capacity; ++i)
            if (table.slots()[(size_t) i].key() == key)
                found = i;

        return found;
    }

    void runTest() override
    {
        beginTest ("keys pack clientId and kind");
        {
            expectEquals (RemoteStreamTable::keyFor (5, StreamKind::voice), (uint32_t) 10);
            expectEquals (RemoteStreamTable::keyFor (5, StreamKind::instrument), (uint32_t) 11);
            expectEquals (RemoteStreamTable::clientOf (11), (uint32_t) 5);
            expect (RemoteStreamTable::kindOf (11) == StreamKind::instrument);
            expect (RemoteStreamTable::kindOf (10) == StreamKind::voice);
            RemoteStreamTable table;
            expectEquals ((int) table.slots().size(), RemoteStreamTable::capacity);
        }

        beginTest ("two senders interleaved fill two independent buffers in sequence order");
        {
            RemoteStreamTable table;
            const uint32_t a = 0x12345678, b = 7;
            double now = 1000.0;

            for (uint16_t seq = 100; seq < 120; ++seq)   // within one buffer's window (80 ms)
            {
                const auto ba = block (1, seq);
                const auto bb = block (2, (uint16_t) (seq + 5000));
                expect (table.push (a, StreamKind::voice, seq, ba.data(), 48, now) >= 0);
                expect (table.push (b, StreamKind::voice, (uint16_t) (seq + 5000), bb.data(), 48, now) >= 0);
                now += blockMs;
            }

            const int sa = slotOf (table, a, StreamKind::voice), sb = slotOf (table, b, StreamKind::voice);
            expect (sa >= 0 && sb >= 0 && sa != sb);
            expectEquals (table.activeCount(), 2);

            for (auto [index, sender, first] : { std::tuple { sa, 1, 100 }, std::tuple { sb, 2, 5100 } })
            {
                auto& slot = table.slots()[(size_t) index];
                const auto state = slot.state.load();
                std::array<uint8_t, 96> out {};
                int len = 0, popped = 0, expected = first;
                bool ordered = true;

                for (int i = 0; i < 20; ++i)
                {
                    if (! slot.pop (state, out.data(), len))
                        continue;

                    ordered = ordered && len == 48 && out[0] == sender && out[1] == (uint8_t) expected;
                    ++expected;
                    ++popped;
                }

                expectEquals (popped, 20);
                expect (ordered, "each buffer holds only its sender's blocks, in order");
            }
        }

        beginTest ("the same sender's voice and instrument are separate streams");
        {
            RemoteStreamTable table;
            const auto v = block (1, 0);
            std::array<uint8_t, 96> stereo {};
            const int sv = table.push (9, StreamKind::voice, 0, v.data(), 48, 0.0);
            const int si = table.push (9, StreamKind::instrument, 0, stereo.data(), 96, 0.0);
            expect (sv >= 0 && si >= 0 && sv != si);
            expect (RemoteStreamTable::kindOf (table.slots()[(size_t) si].key()) == StreamKind::instrument);
        }

        beginTest ("invalid client ids are refused; a full table drops new streams and keeps the old ones");
        {
            RemoteStreamTable table;
            const auto b0 = block (1, 0);
            expectEquals (table.push (0, StreamKind::voice, 0, b0.data(), 48, 0.0), -1);
            expectEquals (table.push (0x80000000u, StreamKind::voice, 0, b0.data(), 48, 0.0), -1);

            for (uint32_t id = 1; id <= (uint32_t) RemoteStreamTable::capacity; ++id)
                expect (table.push (id, StreamKind::voice, 0, b0.data(), 48, 0.0) >= 0);

            expectEquals (table.push (1000, StreamKind::voice, 0, b0.data(), 48, 0.0), -1);
            expectEquals (table.activeCount(), RemoteStreamTable::capacity);
            expect (table.push (3, StreamKind::voice, 1, b0.data(), 48, 1.0) >= 0);
        }

        beginTest ("a stream silent for 5 s is released; the slot is claimed again with a new generation");
        {
            RemoteStreamTable table;
            const auto b0 = block (1, 0);
            const int s = table.push (4, StreamKind::voice, 0, b0.data(), 48, 0.0);
            table.push (8, StreamKind::voice, 0, b0.data(), 48, 0.0);
            auto& slot = table.slots()[(size_t) s];
            const auto first = slot.state.load();

            table.push (8, StreamKind::voice, 1, b0.data(), 48, 3000.0);   // only client 8 keeps talking
            table.sweep (4999.0);
            expectEquals (table.activeCount(), 2);
            table.sweep (5001.0);
            expectEquals (slot.key(), RemoteStreamTable::freeKey);
            expectEquals (RemoteStreamTable::Slot::generationOf (slot.state.load()),
                          RemoteStreamTable::Slot::generationOf (first), "release keeps the generation");
            expectEquals (slotOf (table, 8, StreamKind::voice) >= 0 ? 1 : 0, 1);

            const int again = table.push (12, StreamKind::voice, 77, b0.data(), 48, 6000.0);
            expectEquals (again, s);
            expect (RemoteStreamTable::Slot::generationOf (slot.state.load())
                    == RemoteStreamTable::Slot::generationOf (first) + 1);
            expectEquals (slot.key(), RemoteStreamTable::keyFor (12, StreamKind::voice));

            // A pop with the old state fails even though the new stream has data; with the new one it works.
            for (uint16_t seq = 78; seq < 90; ++seq)
                table.push (12, StreamKind::voice, seq, b0.data(), 48, 6000.0 + seq);

            std::array<uint8_t, 96> out {};
            int len = 0;
            expect (! slot.pop (first, out.data(), len));
            expectEquals (len, 0);
            expect (slot.pop (slot.state.load(), out.data(), len));
            expectEquals (len, 48);
            expect (! slot.reading.load(), "pop lowers reading on every path");
        }

        beginTest ("a free slot still inside a pop is not claimed: another free slot is, or the packet is dropped");
        {
            RemoteStreamTable table;
            const auto b0 = block (1, 0);

            for (uint32_t id = 1; id <= (uint32_t) RemoteStreamTable::capacity; ++id)
                table.push (id, StreamKind::voice, 0, b0.data(), 48, id == 1 || id == 2 ? 0.0 : 9000.0);

            table.sweep (9000.0);   // frees slots 0 and 1 (clients 1 and 2 idle)
            expectEquals (table.activeCount(), RemoteStreamTable::capacity - 2);

            table.slots()[0].reading.store (true);   // the audio thread is still inside slot 0
            expectEquals (table.push (100, StreamKind::voice, 0, b0.data(), 48, 9001.0), 1);

            table.slots()[0].reading.store (true);
            expectEquals (table.push (101, StreamKind::voice, 0, b0.data(), 48, 9002.0), -1, "dropped, not claimed");
            expectEquals (table.slots()[0].key(), RemoteStreamTable::freeKey);

            table.slots()[0].reading.store (false);
            expectEquals (table.push (101, StreamKind::voice, 1, b0.data(), 48, 9003.0), 0);

            // A pop on a free slot leaves without touching the buffer.
            RemoteStreamTable empty;
            std::array<uint8_t, 96> out {};
            int len = 7;
            expect (! empty.slots()[3].pop (empty.slots()[3].state.load(), out.data(), len));
            expectEquals (len, 0);
            expect (! empty.slots()[3].reading.load());
        }

        beginTest ("releaseClient frees every stream of that client on the next sweep, not a reclaimed slot");
        {
            RemoteStreamTable table;
            const auto b0 = block (1, 0);
            table.push (21, StreamKind::voice, 0, b0.data(), 48, 0.0);
            table.push (21, StreamKind::instrument, 0, b0.data(), 48, 0.0);
            table.push (22, StreamKind::voice, 0, b0.data(), 48, 0.0);

            table.releaseClient (21);
            expectEquals (table.activeCount(), 3);   // the producer does the release
            table.sweep (1.0);
            expectEquals (table.activeCount(), 1);
            expect (slotOf (table, 22, StreamKind::voice) >= 0);

            // A request for a key whose slot changed hands meanwhile is ignored.
            RemoteStreamTable fresh;
            expectEquals (fresh.push (22, StreamKind::voice, 0, b0.data(), 48, 0.0), 0);
            fresh.releaseClient (22);
            auto& s0 = fresh.slots()[0];
            s0.state.store (s0.state.load() & ~(uint64_t) 0xffffffffu);   // as if released by silence first
            expectEquals (fresh.push (23, StreamKind::voice, 0, b0.data(), 48, 2.0), 0);
            fresh.sweep (3.0);
            expect (slotOf (fresh, 23, StreamKind::voice) == 0);

            table.clear();
            expectEquals (table.activeCount(), 0);
        }
    }
};

static RemoteStreamsTests remoteStreamsTests;
}
