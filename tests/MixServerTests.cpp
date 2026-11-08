#include "server/MixServer.h"

#include <cmath>
#include <map>
#include <vector>

namespace ensayo
{
class MixServerTests : public juce::UnitTest
{
public:
    MixServerTests() : juce::UnitTest ("MixServer", "EnsayoVirtual") {}

    static constexpr FrameSize size = FrameSize::f128;
    static constexpr int frame = frameSamples (size);

    struct Collected
    {
        std::map<uint32_t, std::vector<std::vector<uint8_t>>> packets;

        MixServer::Sender sender()
        {
            return [this] (uint32_t id, const uint8_t* data, size_t len)
            {
                packets[id].emplace_back (data, data + len);
            };
        }
    };

    // Encodes `pcm` (mono frame) and pushes it as the given channel for `clientId`.
    struct Feeder
    {
        Feeder (const OpusMode& mode, uint32_t id, Role r)
            : clientId (id), role (r), voiceEncoder (mode, 1), instrumentEncoder (mode, 1) {}

        void push (MixServer& server, const float* voice, const float* instrument, double arrivalMs)
        {
            uint8_t voiceBytes[64], instrumentBytes[64];
            AudioPacketView view;
            view.frameSize = size;
            view.seq = seq++;
            view.clientId = clientId;
            view.sampleTime = (uint32_t) (seq * frame);

            if (roleSendsVoice (role))
            {
                view.flags |= AudioFlags::voice;
                view.voiceLen = voiceEncoder.encode (voice, voiceBytes, sizeof (voiceBytes));
                view.voice = voiceBytes;
            }

            if (roleSendsInstrument (role))
            {
                view.flags |= AudioFlags::instrument;
                view.instrumentLen = instrumentEncoder.encode (instrument, instrumentBytes, sizeof (instrumentBytes));
                view.instrument = instrumentBytes;
            }

            // Go through the wire format so the view matches what ServerHost would hand over.
            uint8_t wire[maxPacketBytes];
            const auto written = encodeAudioPacket (view, wire, sizeof (wire));
            AudioPacketView decoded;

            if (written > 0 && decodeAudioPacket (wire, written, decoded))
                server.pushAudio (decoded, arrivalMs);
            else
                jassertfalse;
        }

        uint32_t clientId;
        Role role;
        uint16_t seq = 0;
        OpusEncoder voiceEncoder, instrumentEncoder;
    };

    static std::vector<float> sineBlock (int blockIndex, float frequency)
    {
        std::vector<float> out ((size_t) frame);

        for (int i = 0; i < frame; ++i)
            out[(size_t) i] = 0.5f * std::sin (juce::MathConstants<float>::twoPi * frequency
                                                * (float) (blockIndex * frame + i) / (float) sampleRate);

        return out;
    }

    // Decodes every packet for `clientId` with one stereo decoder, returns interleaved pcm.
    std::vector<float> decodeAll (const OpusMode& mode, const std::vector<std::vector<uint8_t>>& packets)
    {
        OpusDecoder decoder (mode, 2);
        std::vector<float> pcm;

        for (const auto& packet : packets)
        {
            AudioPacketView view;
            expect (decodeAudioPacket (packet.data(), packet.size(), view), "mix packet does not decode");
            expectEquals ((int) view.flags, (int) AudioFlags::mix);
            expectEquals (view.voiceLen, codedBytesStereo (size));
            expectEquals (view.instrumentLen, 0);
            expectEquals ((int) view.clientId, 0);
            std::vector<float> block ((size_t) frame * 2);
            decoder.decode (view.voice, view.voiceLen, block.data());
            pcm.insert (pcm.end(), block.begin(), block.end());
        }

        return pcm;
    }

    static double rmsDb (const std::vector<float>& pcm, size_t from, size_t to)
    {
        double sum = 0.0;
        size_t n = 0;

        for (size_t i = from; i < to && i < pcm.size(); ++i, ++n)
            sum += (double) pcm[i] * pcm[i];

        return 10.0 * std::log10 (juce::jmax (sum / (double) juce::jmax (n, (size_t) 1), 1.0e-12));
    }

    static float peak (const std::vector<float>& pcm)
    {
        float p = 0.0f;

        for (auto s : pcm)
            p = juce::jmax (p, std::abs (s));

        return p;
    }

    static juce::File writeClickWav()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("EnsayoVirtual").getChildFile ("tests");
        dir.createDirectory();
        auto file = dir.getChildFile ("click-mix.wav");
        file.deleteFile();

        juce::AudioBuffer<float> buffer (2, 44100);
        buffer.clear();

        for (int i = 0; i < 44100; i += 11025)
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, 1.0f);

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        const auto options = juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16);
        auto writer = wav.createWriterFor (stream, options);

        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return file;
    }

    void runTest() override
    {
        OpusMode mode (size);
        const double blockMs = 1000.0 * frame / sampleRate;

        beginTest ("tick with zero participants returns 0");
        {
            MixServer server (size);
            expect (server.frameSize() == size);
            expect (server.isEmpty());
            Collected c;
            expectEquals (server.tick (c.sender()), 0);
            expect (c.packets.empty());
            expect (server.getStats().empty());
        }

        MixServer server (size);
        const auto a = server.addParticipant ("A", Role::both);
        const auto b = server.addParticipant ("B", Role::voice);
        const auto cId = server.addParticipant ("C", Role::instrument);

        beginTest ("participants get increasing ids from 1");
        {
            expectEquals ((int) a, 1);
            expectEquals ((int) b, 2);
            expectEquals ((int) cId, 3);
            expect (! server.isEmpty());
            const auto list = server.getParticipants();
            expectEquals ((int) list.size(), 3);
            expectEquals (list[0].name, juce::String ("A"));
            expect (list[1].role == Role::voice);
            expect (list[2].role == Role::instrument);
        }

        Feeder feedA (mode, a, Role::both), feedB (mode, b, Role::voice), feedC (mode, cId, Role::instrument);
        const std::vector<float> silence ((size_t) frame, 0.0f);
        int block = 0;
        double now = 0.0;

        auto feedAll = [&] (bool aSings)
        {
            const auto sine = sineBlock (block, 1000.0f);
            feedA.push (server, aSings ? sine.data() : silence.data(), silence.data(), now);
            feedB.push (server, silence.data(), silence.data(), now);
            feedC.push (server, silence.data(), silence.data(), now);
            ++block;
            now += blockMs;
        };

        beginTest ("one valid mix packet per participant per tick");
        {
            for (int i = 0; i < 6; ++i)
                feedAll (true);

            Collected c;

            for (int i = 0; i < 40; ++i)
            {
                feedAll (true);
                expectEquals (server.tick (c.sender()), 3);
            }

            expectEquals ((int) c.packets.size(), 3);
            expectEquals ((int) c.packets[a].size(), 40);
            expectEquals ((int) c.packets[b].size(), 40);
            expectEquals ((int) c.packets[cId].size(), 40);

            const auto bMix = decodeAll (mode, c.packets[b]);
            const auto level = rmsDb (bMix, bMix.size() / 2, bMix.size());
            logMessage ("B hears A at " + juce::String (level, 1) + " dBFS");
            expect (level > -30.0, "B's mix should carry A's sine: " + juce::String (level, 1) + " dBFS");

            const auto aMix = decodeAll (mode, c.packets[a]);
            const auto aLevel = rmsDb (aMix, aMix.size() / 2, aMix.size());
            logMessage ("A hears " + juce::String (aLevel, 1) + " dBFS");
            expect (aLevel < -50.0, "A's own mix must exclude A: " + juce::String (aLevel, 1) + " dBFS");

            // Sequence numbers increase per client.
            AudioPacketView first, second;
            expect (decodeAudioPacket (c.packets[b][0].data(), c.packets[b][0].size(), first));
            expect (decodeAudioPacket (c.packets[b][1].data(), c.packets[b][1].size(), second));
            expectEquals ((int) second.seq, (int) first.seq + 1);
        }

        beginTest ("muting A for B silences B's mix");
        {
            ChannelGain muted;
            muted.muted = true;
            server.setGain (b, a, muted);
            server.setGain (999, a, muted);   // unknown ids are ignored
            server.setGain (b, 999, muted);

            Collected c;

            for (int i = 0; i < 20; ++i)
            {
                feedAll (true);
                server.tick (c.sender());
            }

            const auto bMix = decodeAll (mode, c.packets[b]);
            const auto level = rmsDb (bMix, (size_t) 10 * frame * 2, bMix.size());
            logMessage ("B hears muted A at " + juce::String (level, 1) + " dBFS");
            expect (level < -50.0, "B's mix should be silent: " + juce::String (level, 1) + " dBFS");

            // C still hears A.
            const auto cMix = decodeAll (mode, c.packets[cId]);
            expect (rmsDb (cMix, cMix.size() / 2, cMix.size()) > -30.0);

            server.setGain (b, a, ChannelGain {});
        }

        beginTest ("backing track reaches everyone");
        {
            const auto file = writeClickWav();
            expect (server.track().load (file).wasOk());
            server.track().play();
            Collected c;

            for (int i = 0; i < 200; ++i)
            {
                feedAll (false);
                server.tick (c.sender());
            }

            expect (server.getTrackPosition() >= 200u * frame - frame);

            for (auto id : { a, b, cId })
            {
                const auto mix = decodeAll (mode, c.packets[id]);
                const auto p = peak (mix);
                logMessage ("client " + juce::String (id) + " track peak " + juce::String (p, 3));
                expect (p > 0.1f, "no click in client " + juce::String (id));
            }

            // Mute the track for A only.
            ChannelGain muted;
            muted.muted = true;
            server.setGain (a, 0, muted);
            server.track().seek (0);
            Collected c2;

            for (int i = 0; i < 200; ++i)
            {
                feedAll (false);
                server.tick (c2.sender());
            }

            const auto aMix = decodeAll (mode, c2.packets[a]);
            const auto bMix = decodeAll (mode, c2.packets[b]);
            expect (peak (std::vector<float> (aMix.begin() + 20 * frame * 2, aMix.end())) < 0.05f, "A should not hear the track");
            expect (peak (bMix) > 0.1f, "B should hear the track");
            server.track().stop();
        }

        beginTest ("stats has one entry per participant");
        {
            const auto stats = server.getStats();
            expectEquals ((int) stats.size(), 3);
            expectEquals ((int) stats[0].id, (int) a);
            expect (stats[0].jitterBlocks >= JitterBuffer::minBlocks);
        }

        beginTest ("removing a participant drops its packets");
        {
            server.removeParticipant (cId);
            server.removeParticipant (999);
            Collected c;
            feedAll (true);
            expectEquals (server.tick (c.sender()), 2);
            expect (c.packets.find (cId) == c.packets.end());
            expectEquals ((int) server.getParticipants().size(), 2);
            expectEquals ((int) server.getStats().size(), 2);

            server.removeParticipant (a);
            server.removeParticipant (b);
            expect (server.isEmpty());
            expectEquals (server.tick (c.sender()), 0);

            // Ids never repeat.
            expectEquals ((int) server.addParticipant ("D", Role::both), 4);
        }

        beginTest ("addParticipantWithId takes caller-chosen ids and keeps the counter above them");
        {
            MixServer other (size);
            expect (! other.addParticipantWithId (0, "zero", Role::both), "id 0 must be rejected");
            expect (other.addParticipantWithId (0x7FFFFFF0u, "X", Role::both));
            expect (! other.addParticipantWithId (0x7FFFFFF0u, "X again", Role::voice), "duplicate id must be rejected");
            expect (other.addParticipantWithId (77, "Y", Role::voice));
            const auto list = other.getParticipants();
            expectEquals ((int) list.size(), 2);
            expectEquals ((long long) list[0].id, 0x7FFFFFF0LL);
            expectEquals ((long long) list[1].id, 77LL);
            expectEquals ((long long) other.addParticipant ("Z", Role::both), 0x7FFFFFF1LL);
            other.removeParticipant (77);
            expectEquals ((int) other.getParticipants().size(), 2);
        }

        beginTest ("a participant whose UDP never arrives is silence, not PLC noise");
        {
            // Field report: the friend joined over TCP but his datagrams never reached the host; everybody heard a
            // constant hiss for him (Opus PLC of a decoder that never decoded anything).
            MixServer server2 (size);
            OpusMode mode2 (size);
            const auto host = server2.addParticipant ("Host", Role::both);
            const auto friendId = server2.addParticipant ("Amigo", Role::both);
            Feeder hostFeeder (mode2, host, Role::both);
            const std::vector<float> quietBlock ((size_t) frame, 0.0f);
            Collected c;

            for (int i = 0; i < 100; ++i)
            {
                hostFeeder.push (server2, quietBlock.data(), quietBlock.data(), 0.0);   // the host sends silence
                server2.tick (c.sender());                                       // the friend sends nothing
            }

            const auto hostMix = decodeAll (mode2, c.packets[host]);
            logMessage ("host hears the silent-UDP friend at peak " + juce::String (peak (hostMix), 4));
            expect (peak (hostMix) < 0.001f, "the host's mix must be silent, peak " + juce::String (peak (hostMix), 4));

            const auto stats = server2.getStats();
            expectEquals ((int) stats.size(), 2);
            expectEquals ((int) stats[1].id, (int) friendId);
            expectEquals ((int) stats[1].packets, 0);
            expectEquals ((int) stats[0].packets, 100);
        }

        beginTest ("a stream that stops is concealed briefly, then silent");
        {
            MixServer server2 (size);
            OpusMode mode2 (size);
            const auto host = server2.addParticipant ("Host", Role::voice);
            const auto friendId = server2.addParticipant ("Amigo", Role::voice);
            Feeder friendFeeder (mode2, friendId, Role::voice);
            Collected c;

            for (int i = 0; i < 60; ++i)
            {
                const auto toneBlock = sineBlock (i, 440.0f);
                friendFeeder.push (server2, toneBlock.data(), toneBlock.data(), 0.0);
                server2.tick (c.sender());
            }

            for (int i = 0; i < 60; ++i)   // the friend's stream dies (0.16 s at 128 samples)
                server2.tick (c.sender());

            const auto hostMix = decodeAll (mode2, c.packets[host]);
            const auto tail = std::vector<float> (hostMix.end() - 30 * frame * 2, hostMix.end());
            expect (peak (std::vector<float> (hostMix.begin() + 30 * frame * 2, hostMix.begin() + 60 * frame * 2)) > 0.1f,
                    "the sine must be heard while it arrives");
            expect (peak (tail) < 0.001f, "the dead stream must end in silence, peak " + juce::String (peak (tail), 4));
        }

        beginTest ("datagrams with another block size are counted, not mixed");
        {
            MixServer server2 (FrameSize::f64);
            const auto id = server2.addParticipant ("Amigo", Role::both);
            OpusMode mode128 (FrameSize::f128);
            Feeder feeder (mode128, id, Role::both);   // Feeder writes FrameSize::f128 packets
            const auto toneBlock = sineBlock (0, 440.0f);

            for (int i = 0; i < 5; ++i)
                feeder.push (server2, toneBlock.data(), toneBlock.data(), 0.0);

            const auto stats = server2.getStats();
            expectEquals ((int) stats[0].packets, 0);
            expectEquals ((int) stats[0].wrongFrame, 5);
        }
    }
};

static MixServerTests mixServerTests;
}
