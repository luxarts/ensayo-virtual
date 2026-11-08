#include "ui/AudioDiagnosis.h"

namespace ensayo
{
class AudioDiagnosisTests : public juce::UnitTest
{
public:
    AudioDiagnosisTests() : juce::UnitTest ("AudioDiagnosis", "EnsayoVirtual") {}

    static constexpr uint32_t me = 10, other = 20;

    // Two participants, both sending and receiving, well past the grace period.
    static AudioDiagnosisInput healthy()
    {
        AudioDiagnosisInput in;
        in.clientId = me;
        in.host = "100.116.198.119";
        in.port = 22140;
        in.secondsInSession = 30;
        in.mixPacketsReceived = 5000;
        in.participants = { { me, "Yo", Role::both }, { other, "Amigo", Role::both } };
        ParticipantStats a; a.id = me; a.packets = 5000;
        ParticipantStats b; b.id = other; b.packets = 5000;
        in.stats = { a, b };
        in.secondsSinceJoined = { { me, 30.0 }, { other, 30.0 } };
        return in;
    }

    void runTest() override
    {
        beginTest ("audio flowing both ways says nothing");
        expect (diagnoseAudio (healthy()).isEmpty());

        beginTest ("nothing is reported during the grace period");
        {
            auto in = healthy();
            in.secondsInSession = 1.0;
            in.mixPacketsReceived = 0;
            in.stats[0].packets = 0;
            expect (diagnoseAudio (in).isEmpty());
        }

        beginTest ("the UDP socket could not be opened (reported at once)");
        {
            auto in = healthy();
            in.secondsInSession = 0.5;
            in.udpFailure = SessionClient::UdpFailure::bind;
            expect (diagnoseAudio (in).contains ("puerto UDP"));
            in.udpFailure = SessionClient::UdpFailure::resolve;
            expect (diagnoseAudio (in).contains ("100.116.198.119"));
        }

        beginTest ("the server gets nothing from me: my UDP does not reach the host");
        {
            auto in = healthy();
            in.stats[0].packets = 0;
            in.mixPacketsReceived = 0;   // the server never learnt where to send the mix either
            const auto text = diagnoseAudio (in);
            logMessage (text);
            expect (text.contains ("El host no recibe tu audio"));
            expect (text.contains ("100.116.198.119:22140"));
        }

        beginTest ("the server drops my audio for its block size");
        {
            auto in = healthy();
            in.stats[0].packets = 0;
            in.stats[0].wrongFrame = 300;
            expect (diagnoseAudio (in).contains ("buffer no coincide"));
        }

        beginTest ("the server gets me but its mix does not reach me: this PC blocks inbound UDP");
        {
            auto in = healthy();
            in.mixPacketsReceived = 0;
            const auto text = diagnoseAudio (in);
            logMessage (text);
            expect (text.contains ("el suyo no llega a esta PC"));
        }

        beginTest ("an older server without counters falls back to the old message");
        {
            auto in = healthy();
            in.stats[0].packets = ~0u;
            in.stats[1].packets = ~0u;
            in.mixPacketsReceived = 0;
            expectEquals (diagnoseAudio (in), juce::String ("Sin audio del servidor"));
            in.mixPacketsReceived = 100;
            expect (diagnoseAudio (in).isEmpty(), "unknown counters must not be read as zero");
        }

        beginTest ("the host sees which participant's audio does not arrive");
        {
            auto in = healthy();
            in.stats[1].packets = 0;
            const auto text = diagnoseAudio (in);
            logMessage (text);
            expect (text.contains ("No llega el audio de Amigo"));

            in.stats[1].wrongFrame = 50;
            expect (diagnoseAudio (in).contains ("Amigo usa otro tamaño de buffer"));
        }

        beginTest ("a participant who just joined gets the grace period too");
        {
            auto in = healthy();
            in.stats[1].packets = 0;
            in.secondsSinceJoined[other] = 1.0;
            expect (diagnoseAudio (in).isEmpty());
        }

        beginTest ("a short gap is a hiccup, a 2 s outage is no audio");
        {
            // Field report: "Sin audio del servidor" flashed on every dropout because it followed the jitter buffer
            // re-priming. Momentary loss belongs to the loss % in the status bar.
            auto in = healthy();
            in.secondsSinceLastMix = 0.3;
            expect (diagnoseAudio (in).isEmpty());
            in.secondsSinceLastMix = 2.5;
            expectEquals (diagnoseAudio (in), juce::String ("Sin audio del servidor"));
        }

        beginTest ("own problems are reported before other participants'");
        {
            auto in = healthy();
            in.stats[0].packets = 0;
            in.stats[1].packets = 0;
            expect (diagnoseAudio (in).contains ("El host no recibe tu audio"));
        }
    }
};

static AudioDiagnosisTests audioDiagnosisTests;
}
