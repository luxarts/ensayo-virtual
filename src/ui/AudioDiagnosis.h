#pragma once
#include "core/Participants.h"
#include "client/SessionClient.h"
#include <map>
#include <vector>
namespace ensayo
{
// Everything the status bar needs to say which direction of the audio is broken. Built by MainComponent from the
// session state (server-reported per-participant packet counters) and the client's own UDP counters.
struct AudioDiagnosisInput
{
    uint32_t clientId = 0;
    juce::String host; int port = 0;                       // where this client sends its audio
    double secondsInSession = 0;
    SessionClient::UdpFailure udpFailure = SessionClient::UdpFailure::none;
    uint32_t mixPacketsReceived = 0;                       // mixes this client got from the server
    double secondsSinceLastMix = 0;                        // since the mix counter last grew (0 while it flows)
    std::vector<ParticipantInfo> participants;
    std::vector<ParticipantStats> stats;                   // from the server; packets == ~0u means "not reported"
    std::map<uint32_t, double> secondsSinceJoined;         // per participant, as seen by this client
};

// Grace period before a missing stream is reported: joining, the first ping and the first stats message take ~1-2 s.
constexpr double audioDiagnosisGraceSeconds = 4.0;
// A gap shorter than this is a network hiccup (counted as loss in the status numbers), not "no audio".
constexpr double audioDiagnosisOutageSeconds = 2.0;

// Empty when the audio flows both ways (or it is too early to tell); otherwise one short Spanish sentence for the
// status bar. This client's own problems come first, then the first participant whose audio does not arrive.
juce::String diagnoseAudio (const AudioDiagnosisInput&);
}
