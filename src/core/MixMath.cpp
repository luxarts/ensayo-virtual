#include "MixMath.h"
#include "Protocol.h"

#include <cmath>

namespace ensayo
{
float dbToGain (float db) noexcept
{
    if (! (db > minFaderDb))   // also catches NaN
        return 0.0f;

    if (db > maxFaderDb)
        db = maxFaderDb;

    return std::pow (10.0f, db * 0.05f);
}

float ChannelGain::voiceGain() const noexcept
{
    return muted ? 0.0f : dbToGain (voiceDb);
}

float ChannelGain::instrumentGain() const noexcept
{
    return muted ? 0.0f : dbToGain (instrumentDb);
}

void addMonoCentred (float* L, float* R, const float* mono, float gain, int n) noexcept
{
    if (L == nullptr || R == nullptr || mono == nullptr || gain == 0.0f)
        return;

    for (int i = 0; i < n; ++i)
    {
        const float s = mono[i] * gain;
        L[i] += s;
        R[i] += s;
    }
}

void addStereo (float* L, float* R, const float* inL, const float* inR, float gain, int n) noexcept
{
    if (L == nullptr || R == nullptr || inL == nullptr || inR == nullptr || gain == 0.0f)
        return;

    for (int i = 0; i < n; ++i)
    {
        L[i] += inL[i] * gain;
        R[i] += inR[i] * gain;
    }
}

float peakOf (const float* data, int n) noexcept
{
    if (data == nullptr || n <= 0)
        return 0.0f;

    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
        peak = juce::jmax (peak, std::abs (data[i]));

    return peak;
}

void interleave (const float* L, const float* R, float* out, int n) noexcept
{
    if (L == nullptr || R == nullptr || out == nullptr)
        return;

    for (int i = 0; i < n; ++i)
    {
        out[2 * i] = L[i];
        out[2 * i + 1] = R[i];
    }
}

void deinterleave (const float* in, float* L, float* R, int n) noexcept
{
    if (in == nullptr || L == nullptr || R == nullptr)
        return;

    for (int i = 0; i < n; ++i)
    {
        L[i] = in[2 * i];
        R[i] = in[2 * i + 1];
    }
}
}
