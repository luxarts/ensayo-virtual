#include "core/SpscQueue.h"

#include <array>
#include <thread>

namespace ensayo
{
class SpscQueueTests : public juce::UnitTest
{
public:
    SpscQueueTests() : juce::UnitTest ("SpscQueue", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("push and pop preserve order and lengths");
        {
            PacketQueue queue (8);
            expectEquals (queue.size(), 0);

            for (uint8_t i = 0; i < 5; ++i)
            {
                std::array<uint8_t, 10> packet {};
                packet.fill (i);
                expect (queue.push (packet.data(), (size_t) (i + 1)));
            }

            expectEquals (queue.size(), 5);

            for (uint8_t i = 0; i < 5; ++i)
            {
                std::array<uint8_t, maxPacketBytes> out {};
                size_t len = 0;
                expect (queue.pop (out.data(), len));
                expectEquals (len, (size_t) (i + 1));

                for (size_t k = 0; k < len; ++k)
                    expectEquals ((int) out[k], (int) i);
            }

            expectEquals (queue.size(), 0);
        }

        beginTest ("empty pop fails and full push fails");
        {
            PacketQueue queue (4);
            std::array<uint8_t, maxPacketBytes> out {};
            size_t len = 99;
            expect (! queue.pop (out.data(), len));

            const std::array<uint8_t, 4> packet { 1, 2, 3, 4 };

            for (int i = 0; i < 4; ++i)
                expect (queue.push (packet.data(), packet.size()), "push " + juce::String (i));

            expect (! queue.push (packet.data(), packet.size()), "fifth push must fail when full");
            expectEquals (queue.size(), 4);

            expect (queue.pop (out.data(), len));
            expectEquals (len, (size_t) 4);
            expect (queue.push (packet.data(), packet.size()), "push after pop must succeed again");
            expectEquals (queue.size(), 4);
        }

        beginTest ("oversized and null packets are rejected");
        {
            PacketQueue queue (4);
            std::array<uint8_t, maxPacketBytes + 1> big {};
            expect (! queue.push (big.data(), big.size()));
            expect (queue.push (big.data(), maxPacketBytes));
            expect (! queue.push (nullptr, 4));
            expect (! queue.push (big.data(), 0));
            expectEquals (queue.size(), 1);

            std::array<uint8_t, maxPacketBytes> out {};
            size_t len = 0;
            expect (queue.pop (out.data(), len));
            expectEquals (len, maxPacketBytes);
        }

        beginTest ("wraps around many times");
        {
            PacketQueue queue (3);

            for (uint32_t i = 0; i < 1000; ++i)
            {
                const std::array<uint8_t, 4> packet { (uint8_t) i, (uint8_t) (i >> 8), (uint8_t) (i >> 16), 0 };
                expect (queue.push (packet.data(), packet.size()));

                std::array<uint8_t, maxPacketBytes> out {};
                size_t len = 0;
                expect (queue.pop (out.data(), len));
                expectEquals ((uint32_t) (out[0] | (out[1] << 8) | (out[2] << 16)), i);
            }
        }

        beginTest ("single producer and single consumer threads agree");
        {
            PacketQueue queue (16);
            constexpr uint32_t count = 20000;
            std::atomic<bool> ok { true };

            std::thread producer ([&]
            {
                for (uint32_t i = 0; i < count;)
                {
                    const std::array<uint8_t, 4> packet { (uint8_t) i, (uint8_t) (i >> 8), (uint8_t) (i >> 16), (uint8_t) (i >> 24) };

                    if (queue.push (packet.data(), packet.size()))
                        ++i;
                    else
                        std::this_thread::yield();
                }
            });

            for (uint32_t i = 0; i < count;)
            {
                std::array<uint8_t, maxPacketBytes> out {};
                size_t len = 0;

                if (queue.pop (out.data(), len))
                {
                    const uint32_t value = (uint32_t) out[0] | ((uint32_t) out[1] << 8) | ((uint32_t) out[2] << 16) | ((uint32_t) out[3] << 24);

                    if (len != 4 || value != i)
                        ok = false;

                    ++i;
                }
                else
                {
                    std::this_thread::yield();
                }
            }

            producer.join();
            expect (ok.load(), "consumer saw packets out of order or corrupted");
            expectEquals (queue.size(), 0);
        }
    }
};

static SpscQueueTests spscQueueTests;
}
