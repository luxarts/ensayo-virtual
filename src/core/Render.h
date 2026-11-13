#pragma once
#include "core/Project.h"
#include "client/TakeCache.h"
#include <memory>
#include <set>
#include <utility>
#include <vector>
namespace ensayo
{
constexpr int edgeFadeSamples = 144, rampSamples = 960, transportFadeSamples = 240;

// Gain of a region at sample `pos` inside it (0 ≤ pos < length): half-cosine fade over the first/last
// edgeFadeSamples (shortened to length/2 for short regions) unless that edge is seamless.
float regionEdgeGain (int64_t pos, int64_t length, bool fadeIn, bool fadeOut) noexcept;
// True when `next` continues `prev` without a gap: same take, next.start == prev.end(), next.offset == prev.offset + prev.length.
bool isSeamless (const Region& prev, const Region& next) noexcept;
void panGains (float pan, float& left, float& right) noexcept;            // constant power, centre = 0.7071 each

class Renderer
{
public:
    Renderer();
    ~Renderer();                                                          // defined where Impl is complete
    void prepare (int maxBlockSize);                                     // allocates scratch; message thread
    void setProject (std::shared_ptr<const Project>);                     // message thread; picked up atomically
    void setSolo (std::set<Id> soloTracks);                               // message thread
    // Audio thread. Overwrites outL/outR with the mix of [position, position + n). Negative positions render
    // silence for the part before 0. Gain/pan/mute/solo changes ramp over rampSamples.
    void render (const SampleSource&, int64_t position, int n, float* outL, float* outR) noexcept;
    // Audio thread: jump all ramps to their targets (after a seek or at play start, where the transport fade hides it).
    void snapRamps() noexcept;
    // Message thread. Per track of the latest project: the largest |sample| it put into the mix (post volume, pan and
    // mute, pre master) since the last call, then reset.
    std::vector<std::pair<Id, float>> takeTrackPeaks();
private:
    struct Impl; std::unique_ptr<Impl> impl;
};

// Metronome: adds clicks for [position, position + n) to outL/outR. Bar downbeat 1 kHz, other beats 800 Hz,
// 20 ms with a 1 ms attack and exponential decay, amplitude 0.5.
void renderMetronome (double bpm, int beatsPerBar, int64_t position, int n, float* outL, float* outR) noexcept;
}
