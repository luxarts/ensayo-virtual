#include "core/Protocol.h"

#include <array>

namespace ensayo
{
class ProtocolTests : public juce::UnitTest
{
public:
    ProtocolTests() : juce::UnitTest ("Protocol", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("frame size helpers");
        {
            expectEquals (frameSamples (FrameSize::f64), 64);
            expectEquals (frameSamples (FrameSize::f128), 128);
            expectEquals (codedBytesMono (FrameSize::f64), 24);
            expectEquals (codedBytesMono (FrameSize::f128), 48);
            expectEquals (codedBytesStereo (FrameSize::f64), 48);
            expectEquals (codedBytesStereo (FrameSize::f128), 96);
            expect (frameSizeForSamples (64) == FrameSize::f64);
            expect (frameSizeForSamples (128) == FrameSize::f128);
            expect (! frameSizeForSamples (256).has_value());
            expect (! frameSizeForSamples (0).has_value());
        }

        beginTest ("roles");
        {
            expectEquals (roleToString (Role::voice), juce::String ("voice"));
            expectEquals (roleToString (Role::instrument), juce::String ("instrument"));
            expectEquals (roleToString (Role::both), juce::String ("both"));
            expect (roleFromString ("voice") == Role::voice);
            expect (roleFromString ("instrument") == Role::instrument);
            expect (roleFromString ("both") == Role::both);
            expect (! roleFromString ("x").has_value());
            expect (! roleFromString ("").has_value());
            expect (roleSendsVoice (Role::voice) && ! roleSendsInstrument (Role::voice));
            expect (! roleSendsVoice (Role::instrument) && roleSendsInstrument (Role::instrument));
            expect (roleSendsVoice (Role::both) && roleSendsInstrument (Role::both));
        }

        beginTest ("audio packet round-trip with both payloads");
        {
            std::array<uint8_t, 48> voice {};
            std::array<uint8_t, 48> instrument {};

            for (size_t i = 0; i < voice.size(); ++i)
            {
                voice[i] = (uint8_t) i;
                instrument[i] = (uint8_t) (200 - i);
            }

            AudioPacketView in;
            in.flags = AudioFlags::voice | AudioFlags::instrument;
            in.frameSize = FrameSize::f128;
            in.seq = 0xBEEF;
            in.clientId = 0x01020304;
            in.sampleTime = 0xAABBCCDD;
            in.trackPosition = 0x11223344;
            in.voice = voice.data();           in.voiceLen = (int) voice.size();
            in.instrument = instrument.data(); in.instrumentLen = (int) instrument.size();

            std::array<uint8_t, maxPacketBytes> wire {};
            const auto written = encodeAudioPacket (in, wire.data(), wire.size());
            expectEquals (written, audioHeaderBytes + voice.size() + instrument.size());

            expectEquals ((int) wire[0], (int) packetMagic);
            expectEquals ((int) wire[1], (int) protocolVersion);
            expectEquals ((int) wire[2], (int) in.flags);
            expectEquals ((int) wire[3], 1);
            expectEquals ((int) wire[4], 0xEF);   // seq little-endian
            expectEquals ((int) wire[5], 0xBE);
            expectEquals ((int) wire[6], 0x04);   // clientId little-endian
            expectEquals ((int) wire[9], 0x01);

            AudioPacketView out;
            expect (decodeAudioPacket (wire.data(), written, out));
            expectEquals ((int) out.flags, (int) in.flags);
            expect (out.frameSize == FrameSize::f128);
            expectEquals ((int) out.seq, (int) in.seq);
            expectEquals ((juce::int64) out.clientId, (juce::int64) in.clientId);
            expectEquals ((juce::int64) out.sampleTime, (juce::int64) in.sampleTime);
            expectEquals ((juce::int64) out.trackPosition, (juce::int64) in.trackPosition);
            expectEquals (out.voiceLen, in.voiceLen);
            expectEquals (out.instrumentLen, in.instrumentLen);
            expect (out.voice != nullptr && std::equal (voice.begin(), voice.end(), out.voice));
            expect (out.instrument != nullptr && std::equal (instrument.begin(), instrument.end(), out.instrument));
            expect (out.voice >= wire.data() && out.voice < wire.data() + wire.size(), "pointer must alias wire");
        }

        beginTest ("mix packet with a single stereo payload");
        {
            std::array<uint8_t, 96> stereo {};
            stereo.fill (0x5A);

            AudioPacketView in;
            in.flags = AudioFlags::mix;
            in.frameSize = FrameSize::f64;
            in.seq = 7;
            in.clientId = 0;
            in.trackPosition = 48000;
            in.voice = stereo.data(); in.voiceLen = (int) stereo.size();

            std::array<uint8_t, maxPacketBytes> wire {};
            const auto written = encodeAudioPacket (in, wire.data(), wire.size());
            expectEquals (written, audioHeaderBytes + stereo.size());

            AudioPacketView out;
            expect (decodeAudioPacket (wire.data(), written, out));
            expectEquals ((int) out.flags, (int) AudioFlags::mix);
            expect (out.frameSize == FrameSize::f64);
            expectEquals (out.voiceLen, 96);
            expectEquals (out.instrumentLen, 0);
            expect (out.instrument == nullptr);
            expectEquals ((juce::int64) out.trackPosition, (juce::int64) 48000);
        }

        beginTest ("encode rejects small capacity and oversized payloads");
        {
            std::array<uint8_t, 300> big {};
            AudioPacketView in;
            in.voice = big.data(); in.voiceLen = 48;

            std::array<uint8_t, maxPacketBytes> wire {};
            expectEquals (encodeAudioPacket (in, wire.data(), audioHeaderBytes + 47), (size_t) 0);
            expectEquals (encodeAudioPacket (in, wire.data(), audioHeaderBytes + 48), audioHeaderBytes + 48);

            in.voiceLen = 256;
            expectEquals (encodeAudioPacket (in, wire.data(), wire.size()), (size_t) 0);

            in.voiceLen = 0;
            in.instrument = big.data(); in.instrumentLen = 300;
            expectEquals (encodeAudioPacket (in, wire.data(), wire.size()), (size_t) 0);

            in.instrumentLen = -1;
            expectEquals (encodeAudioPacket (in, wire.data(), wire.size()), (size_t) 0);
        }

        beginTest ("stream kind: carried by the voice/instrument flag bits");
        {
            expect (streamKindOf (AudioFlags::voice) == StreamKind::voice);
            expect (streamKindOf (AudioFlags::instrument) == StreamKind::instrument);
            expect (streamKindOf (0) == StreamKind::voice);
            expectEquals ((int) withStreamKind (AudioFlags::voice | AudioFlags::mix, StreamKind::instrument),
                          (int) (AudioFlags::instrument | AudioFlags::mix));
            expectEquals ((int) withStreamKind (AudioFlags::instrument, StreamKind::voice), (int) AudioFlags::voice);

            std::array<uint8_t, 96> stereo {};
            AudioPacketView in;
            in.flags = withStreamKind (0, StreamKind::instrument);
            in.instrument = stereo.data(); in.instrumentLen = (int) stereo.size();
            std::array<uint8_t, maxPacketBytes> wire {};
            const auto written = encodeAudioPacket (in, wire.data(), wire.size());
            AudioPacketView out;
            expect (decodeAudioPacket (wire.data(), written, out));
            expect (streamKindOf (out.flags) == StreamKind::instrument);
        }

        beginTest ("decode rejects malformed packets");
        {
            std::array<uint8_t, 24> voice {};
            AudioPacketView in;
            in.flags = AudioFlags::voice;
            in.frameSize = FrameSize::f64;
            in.voice = voice.data(); in.voiceLen = (int) voice.size();

            std::array<uint8_t, maxPacketBytes> wire {};
            const auto written = encodeAudioPacket (in, wire.data(), wire.size());
            expect (written > 0);

            AudioPacketView out;
            expect (decodeAudioPacket (wire.data(), written, out));

            auto corrupted = wire;
            corrupted[0] = 0x00;
            expect (! decodeAudioPacket (corrupted.data(), written, out), "wrong magic accepted");

            corrupted = wire;
            corrupted[1] = (uint8_t) (protocolVersion + 1);
            expect (! decodeAudioPacket (corrupted.data(), written, out), "wrong version accepted");

            expect (! decodeAudioPacket (wire.data(), audioHeaderBytes - 1, out), "truncated header accepted");
            expect (! decodeAudioPacket (wire.data(), written - 1, out), "declared length past end accepted");
            expect (! decodeAudioPacket (nullptr, written, out), "null data accepted");

            corrupted = wire;
            corrupted[3] = 7;
            expect (! decodeAudioPacket (corrupted.data(), written, out), "frameCode 7 accepted");
        }

        beginTest ("control lines round trip");
        {
            auto message = makeMessage ("join");
            auto* object = message.getDynamicObject();
            expect (object != nullptr);
            object->setProperty ("session", "ABC234");
            object->setProperty ("count", 3);

            const auto line = makeLine (message);
            expect (line.endsWithChar ('\n'));
            expectEquals (line.indexOfChar ('\n'), line.length() - 1);   // single line

            const auto parsed = parseLine (line);
            expect (parsed.has_value());

            if (parsed.has_value())
            {
                expectEquals (parsed->getProperty ("t", {}).toString(), juce::String ("join"));
                expectEquals (parsed->getProperty ("session", {}).toString(), juce::String ("ABC234"));
                expectEquals ((int) parsed->getProperty ("count", 0), 3);
            }

            expect (! parseLine ("[1, 2, 3]").has_value(), "array accepted");
            expect (! parseLine ("{\"x\": 1}").has_value(), "object without t accepted");
            expect (! parseLine ("{\"t\": 5}").has_value(), "non-string t accepted");
            expect (! parseLine ("not json").has_value(), "garbage accepted");
            expect (! parseLine ("").has_value(), "empty accepted");
        }

        beginTest ("session ids");
        {
            juce::Random random (1234);
            const juce::String allowed ("ABCDEFGHJKLMNPQRSTUVWXYZ23456789");

            for (int i = 0; i < 50; ++i)
            {
                const auto id = generateSessionId (random);
                expectEquals (id.length(), 6);
                expect (isValidSessionId (id));

                for (auto c : id)
                    expect (allowed.containsChar (c), "unexpected character in session id");
            }

            expect (! isValidSessionId ("ABC23"));
            expect (! isValidSessionId ("ABC2345"));
            expect (! isValidSessionId ("ABC0I1"));
            expect (! isValidSessionId ("abc234"));
            expect (! isValidSessionId (""));
        }

        beginTest ("transport state var round trip");
        {
            TransportState state;
            state.loaded = true; state.playing = true; state.loop = true;
            state.position = 123456789012LL; state.length = 987654321LL;
            state.name = "cancion.mp3";

            const auto back = transportFromVar (transportToVar (state));
            expect (back.loaded && back.playing && back.loop);
            expectEquals ((juce::int64) back.position, (juce::int64) state.position);
            expectEquals ((juce::int64) back.length, (juce::int64) state.length);
            expectEquals (back.name, state.name);

            const auto empty = transportFromVar (juce::var());
            expect (! empty.loaded && ! empty.playing && ! empty.loop);
            expectEquals ((juce::int64) empty.position, (juce::int64) 0);
        }
    }
};

static ProtocolTests protocolTests;
}
