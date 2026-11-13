#pragma once
#include "Protocol.h"
namespace ensayo
{
struct ParticipantInfo { uint32_t id = 0; juce::String name; Role role = Role::both; };
// packets: audio datagrams the server accepted from this participant; wrongFrame: datagrams it dropped because their
// block size is not the session's. Both are totals since the participant joined; packets == 0 a few seconds after
// joining means the participant's UDP never reaches the server.
struct ParticipantStats { uint32_t id = 0; int jitterBlocks = 0; uint32_t underruns = 0, late = 0; float voicePeak = 0, instrumentPeak = 0;
                          uint32_t packets = 0, wrongFrame = 0; };
}
