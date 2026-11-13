#include "Render.h"
#include "MixMath.h"
#include "Protocol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace ensayo
{
namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr float centreGain = 0.70710678f;

float halfCosine (int64_t i, int64_t fade) noexcept   // 0 at i = 0, rising towards 1 at i = fade
{
    return (float) (0.5 - 0.5 * std::cos (pi * (double) i / (double) fade));
}
}

float regionEdgeGain (int64_t pos, int64_t length, bool fadeIn, bool fadeOut) noexcept
{
    if (length <= 0 || pos < 0 || pos >= length)
        return 0.0f;

    if (! fadeIn && ! fadeOut)
        return 1.0f;

    const auto fade = std::min<int64_t> (edgeFadeSamples, length / 2);

    if (fade <= 0)   // a one-sample region with a faded edge stays silent rather than clicking
        return 0.0f;

    float g = 1.0f;

    if (fadeIn && pos < fade)
        g *= halfCosine (pos, fade);

    const auto remaining = length - 1 - pos;

    if (fadeOut && remaining < fade)
        g *= halfCosine (remaining, fade);

    return g;
}

bool isSeamless (const Region& prev, const Region& next) noexcept
{
    return prev.take == next.take && next.start == prev.end() && next.offset == prev.offset + prev.length;
}

void panGains (float pan, float& left, float& right) noexcept
{
    pan = std::isfinite (pan) ? std::clamp (pan, -1.0f, 1.0f) : 0.0f;
    const auto angle = ((double) pan + 1.0) * (pi / 4.0);
    left = (float) std::cos (angle);
    right = (float) std::sin (angle);
}

//==============================================================================
namespace
{
struct Ramp
{
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0;

    void jump (float value) noexcept { current = target = value; step = 0.0f; remaining = 0; }

    void setTarget (float value) noexcept
    {
        if (value == target)
            return;

        target = value;

        if (value == current) { remaining = 0; step = 0.0f; return; }

        step = (target - current) / (float) rampSamples;
        remaining = rampSamples;
    }

    void advance() noexcept
    {
        if (remaining > 0 && --remaining == 0)
            current = target;
        else if (remaining > 0)
            current += step;
    }

    bool idleAt (float value) const noexcept { return remaining == 0 && current == value; }
};

struct RegionInfo { Id take; int64_t start = 0, offset = 0, length = 0; bool fadeIn = true, fadeOut = true; };

struct TrackState
{
    Id id;
    float targetGain = 0.0f, targetPan = 0.0f;
    std::vector<RegionInfo> regions;   // top → bottom
    Ramp gain, pan;                     // touched only by the audio thread once published
};

struct State
{
    std::shared_ptr<const Project> project;
    std::vector<TrackState> tracks;
};

std::shared_ptr<State> buildState (std::shared_ptr<const Project> project, const std::set<Id>& solo)
{
    auto state = std::make_shared<State>();
    state->project = std::move (project);

    if (state->project == nullptr)
        return state;

    const auto& tracks = state->project->tracks;
    const bool soloActive = std::any_of (tracks.begin(), tracks.end(),
                                         [&] (const Track& t) { return solo.count (t.id) > 0; });
    state->tracks.reserve (tracks.size());

    for (const auto& track : tracks)
    {
        TrackState ts;
        ts.id = track.id;
        const bool silent = track.muted || (soloActive && solo.count (track.id) == 0);
        ts.targetGain = silent ? 0.0f : dbToGain (track.gainDb);
        ts.targetPan = std::isfinite (track.pan) ? std::clamp (track.pan, -1.0f, 1.0f) : 0.0f;
        ts.gain.jump (ts.targetGain);
        ts.pan.jump (ts.targetPan);
        ts.regions.reserve (track.regions.size());

        for (auto it = track.regions.rbegin(); it != track.regions.rend(); ++it)
        {
            const auto& r = *it;
            RegionInfo info { r.take, r.start, r.offset, r.length, true, true };

            for (const auto& other : track.regions)
            {
                if (&other == &r)
                    continue;

                if (isSeamless (other, r))  info.fadeIn = false;
                if (isSeamless (r, other))  info.fadeOut = false;
            }

            ts.regions.push_back (std::move (info));
        }

        state->tracks.push_back (std::move (ts));
    }

    return state;
}
}

//==============================================================================
struct Renderer::Impl
{
    // Message thread.
    std::mutex messageLock;
    std::shared_ptr<const Project> project;
    std::set<Id> solo;
    std::vector<std::shared_ptr<State>> keepAlive;   // states are only ever freed here, never on the audio thread

    // Hand-over.
    std::atomic<std::shared_ptr<State>> pending;
    std::atomic<uint64_t> pendingVersion { 0 };

    // Audio thread.
    std::shared_ptr<State> current;
    uint64_t seenVersion = 0;
    int maxBlock = 0;
    std::vector<float> readA, readB, coverage, mono, stereoL, stereoR;

    void publish()   // message thread, messageLock held
    {
        auto state = buildState (project, solo);
        keepAlive.erase (std::remove_if (keepAlive.begin(), keepAlive.end(),
                                         [] (const std::shared_ptr<State>& s) { return s.use_count() == 1; }),
                         keepAlive.end());
        keepAlive.push_back (state);
        pending.store (std::move (state));
        pendingVersion.fetch_add (1, std::memory_order_release);
    }

    void pickUp() noexcept   // audio thread
    {
        const auto version = pendingVersion.load (std::memory_order_acquire);

        if (version == seenVersion)
            return;

        seenVersion = version;
        auto next = pending.load();

        if (current != nullptr && next != nullptr)
        {
            // Continue the ramps of tracks that still exist from where they are now.
            for (auto& track : next->tracks)
            {
                for (const auto& old : current->tracks)
                {
                    if (old.id != track.id)
                        continue;

                    track.gain = old.gain;
                    track.pan = old.pan;
                    track.gain.setTarget (track.targetGain);
                    track.pan.setTarget (track.targetPan);
                    break;
                }
            }
        }

        current = std::move (next);   // the previous state is still referenced by keepAlive
    }

    void renderChunk (const SampleSource& source, int64_t position, int n, float* outL, float* outR) noexcept
    {
        for (auto& track : current->tracks)
        {
            if (track.gain.idleAt (0.0f))
                continue;   // silent and not ramping: nothing to hear, nothing to advance

            std::fill (coverage.begin(), coverage.begin() + n, 1.0f);
            std::fill (mono.begin(), mono.begin() + n, 0.0f);
            std::fill (stereoL.begin(), stereoL.begin() + n, 0.0f);
            std::fill (stereoR.begin(), stereoR.begin() + n, 0.0f);
            bool hasStereo = false;

            for (const auto& region : track.regions)
            {
                const auto from = std::max ({ position, region.start, (int64_t) 0 });
                const auto to = std::min (position + n, region.start + region.length);

                if (from >= to)
                    continue;

                const int channels = source.channels (region.take);

                if (channels <= 0)
                    continue;

                const int count = (int) (to - from);
                const int first = (int) (from - position);
                const auto inRegion = from - region.start;
                source.read (region.take, 0, region.offset + inRegion, count, readA.data());

                if (channels >= 2)
                {
                    source.read (region.take, 1, region.offset + inRegion, count, readB.data());
                    hasStereo = true;
                }

                for (int k = 0; k < count; ++k)
                {
                    const int i = first + k;
                    const float c = coverage[(size_t) i];

                    if (c == 0.0f)
                        continue;

                    const float g = regionEdgeGain (inRegion + k, region.length, region.fadeIn, region.fadeOut);
                    const float w = c * g;

                    if (channels >= 2)
                    {
                        stereoL[(size_t) i] += w * readA[(size_t) k];
                        stereoR[(size_t) i] += w * readB[(size_t) k];
                    }
                    else
                    {
                        mono[(size_t) i] += w * readA[(size_t) k];
                    }

                    coverage[(size_t) i] = c * (1.0f - g);
                }
            }

            float panValue = track.pan.current, left = 0.0f, right = 0.0f;
            panGains (panValue, left, right);

            for (int i = 0; i < n; ++i)
            {
                if (track.pan.current != panValue)
                {
                    panValue = track.pan.current;
                    panGains (panValue, left, right);
                }

                const float gain = track.gain.current;
                float l = mono[(size_t) i] * left, r = mono[(size_t) i] * right;

                if (hasStereo)
                {
                    l += stereoL[(size_t) i] * std::min (1.0f, left / centreGain);
                    r += stereoR[(size_t) i] * std::min (1.0f, right / centreGain);
                }

                outL[i] += gain * l;
                outR[i] += gain * r;
                track.gain.advance();
                track.pan.advance();
            }
        }
    }
};

Renderer::Renderer() : impl (std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

void Renderer::prepare (int maxBlockSize)
{
    auto& d = *impl;
    d.maxBlock = std::max (1, maxBlockSize);

    for (auto* v : { &d.readA, &d.readB, &d.coverage, &d.mono, &d.stereoL, &d.stereoR })
        v->assign ((size_t) d.maxBlock, 0.0f);
}

void Renderer::setProject (std::shared_ptr<const Project> project)
{
    const std::lock_guard lock (impl->messageLock);
    impl->project = std::move (project);
    impl->publish();
}

void Renderer::setSolo (std::set<Id> soloTracks)
{
    const std::lock_guard lock (impl->messageLock);
    impl->solo = std::move (soloTracks);
    impl->publish();
}

void Renderer::render (const SampleSource& source, int64_t position, int n, float* outL, float* outR) noexcept
{
    if (n <= 0)
        return;

    std::fill (outL, outL + n, 0.0f);
    std::fill (outR, outR + n, 0.0f);

    auto& d = *impl;
    d.pickUp();

    if (d.current == nullptr || d.maxBlock <= 0)
        return;

    for (int done = 0; done < n;)
    {
        const int chunk = std::min (d.maxBlock, n - done);
        d.renderChunk (source, position + done, chunk, outL + done, outR + done);
        done += chunk;
    }
}

void Renderer::snapRamps() noexcept
{
    auto& d = *impl;
    d.pickUp();

    if (d.current == nullptr)
        return;

    for (auto& track : d.current->tracks)
    {
        track.gain.jump (track.targetGain);
        track.pan.jump (track.targetPan);
    }
}

//==============================================================================
void renderMetronome (double bpm, int beatsPerBar, int64_t position, int n, float* outL, float* outR) noexcept
{
    if (! (bpm > 0.0) || ! std::isfinite (bpm) || beatsPerBar < 1 || n <= 0)
        return;

    constexpr int clickSamples = sampleRate / 50;      // 20 ms
    constexpr int attackSamples = sampleRate / 1000;   // 1 ms
    constexpr double decaySamples = sampleRate * 0.003;
    const double samplesPerBeat = 60.0 * sampleRate / bpm;

    for (int i = 0; i < n; ++i)
    {
        const auto t = position + i;

        if (t < 0)
            continue;

        // Beat k starts at ceil (k * samplesPerBeat): the largest such start ≤ t belongs to k = floor (t / spb).
        const auto beat = (int64_t) std::floor ((double) t / samplesPerBeat);
        const auto start = (int64_t) std::ceil ((double) beat * samplesPerBeat);
        const auto d = t - start;

        if (d < 0 || d >= clickSamples)
            continue;

        const double freq = beat % beatsPerBar == 0 ? 1000.0 : 800.0;
        const double envelope = d < attackSamples ? (double) d / attackSamples
                                                  : std::exp (-(double) (d - attackSamples) / decaySamples);
        const auto s = (float) (0.5 * envelope * std::sin (2.0 * pi * freq * (double) d / sampleRate));
        outL[i] += s;
        outR[i] += s;
    }
}
}
