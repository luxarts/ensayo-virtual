#pragma once
#include "Protocol.h"
namespace ensayo
{
struct ParticipantInfo { uint32_t id = 0; juce::String name; Role role = Role::both; };
struct ParticipantStats { uint32_t id = 0; int jitterBlocks = 0; uint32_t underruns = 0, late = 0; float voicePeak = 0, instrumentPeak = 0; };
}
