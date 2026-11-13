#include "core/Render.h"
#include "core/MixMath.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace ensayo
{
namespace
{
constexpr int64_t takeSamples = 4 * 48000;

// Serves a 440 Hz sine (amplitude 0.5) for every known take: sample k of channel 0 is sine (k), of channel 1
// (stereo takes) sine2 (k), the same sine 60 degrees later, so the channels differ but share the no-click bound.
class SineSource : public SampleSource
{
public:
    void add (const Id& take, int channels, int64_t length = takeSamples) { takes[take] = { channels, length }; }

    static float sine (int64_t k) noexcept
    {
        return (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) k / 48000.0));
    }

    static float sine2 (int64_t k) noexcept
    {
        return (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) k / 48000.0
                                        + juce::MathConstants<double>::pi / 3.0));
    }

    int channels (const Id& take) const noexcept override
    {
        const auto it = takes.find (take);
        return it == takes.end() ? 0 : it->second.first;
    }

    void read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept override
    {
        const auto it = takes.find (take);
        const bool second = it != takes.end() && std::min (channel, it->second.first - 1) >= 1;

        for (int i = 0; i < n; ++i)
        {
            const auto k = offset + i;
            dest[i] = (it != takes.end() && k >= 0 && k < it->second.second) ? (second ? sine2 (k) : sine (k)) : 0.0f;
        }
    }

private:
    std::map<Id, std::pair<int, int64_t>> takes;
};

struct Stereo { std::vector<float> L, R; };

float maxStep (const std::vector<float>& x, size_t from = 1, size_t to = SIZE_MAX)
{
    float m = 0.0f;
    to = std::min (to, x.size());

    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        m = std::max (m, std::abs (x[i] - x[i - 1]));

    return m;
}

float noClickBound()
{
    std::vector<float> pure (48000);

    for (size_t i = 0; i < pure.size(); ++i)
        pure[i] = SineSource::sine ((int64_t) i);

    return maxStep (pure) + 1.0e-3f;
}

Stereo renderRange (Renderer& r, const SampleSource& src, int64_t start, int total, int block)
{
    Stereo out { std::vector<float> ((size_t) total), std::vector<float> ((size_t) total) };

    for (int done = 0; done < total; done += block)
    {
        const int n = std::min (block, total - done);
        r.render (src, start + done, n, out.L.data() + done, out.R.data() + done);
    }

    return out;
}

void append (Stereo& a, const Stereo& b)
{
    a.L.insert (a.L.end(), b.L.begin(), b.L.end());
    a.R.insert (a.R.end(), b.R.begin(), b.R.end());
}

// One mono take over 4 s, one track with one region over the whole take.
struct Fixture
{
    Id host = newId(), take = newId(), track = newId(), region = newId();
    Project project = makeProject ("Song", host, "Lucas");
    SineSource source;
    bool ok = true;

    explicit Fixture (int channels = 1)
    {
        ok = apply (Ops::addTake (take, channels))
          && apply (Ops::updateTake (take, 4, takeSamples, true))
          && apply (Ops::addTrack (track, "Track", TrackKind::voice))
          && apply (Ops::addRegion (track, { region, take, 0, 0, takeSamples }));
        source.add (take, channels);
    }

    bool apply (const juce::var& op) { return applyOp (project, op, host).ok; }
    // Hard left: the left channel then carries the full 0.5-amplitude sine the no-click bound is defined for.
    bool hardLeft() { return apply (Ops::setMix (track, std::nullopt, -1.0f, std::nullopt)); }
    std::shared_ptr<const Project> snapshot() const { return std::make_shared<const Project> (project); }
};

float centre()
{
    float l = 0.0f, r = 0.0f;
    panGains (0.0f, l, r);
    return l;
}
}

class RenderTests : public juce::UnitTest
{
public:
    RenderTests() : juce::UnitTest ("Render", "EnsayoVirtual") {}

    juce::String clickSummary;   // measured steps of the current test, logged once by logClicks

    void expectNoClick (const std::vector<float>& x, const juce::String& what)
    {
        const auto step = maxStep (x), bound = noClickBound();
        clickSummary << " " << what << " " << juce::String (step, 6);
        expect (step <= bound, what + ": step " + juce::String (step, 6) + " > bound " + juce::String (bound, 6));
    }

    // Same check restricted to [edge - 200, edge + 200), so the summary shows the step at the edge itself.
    void expectNoClickAt (const std::vector<float>& x, int64_t edge, const juce::String& what)
    {
        const auto from = (size_t) std::max<int64_t> (1, edge - 200), to = (size_t) (edge + 200);
        const auto step = maxStep (x, from, to), bound = noClickBound();
        clickSummary << " " << what << "@" << juce::String (edge) << " " << juce::String (step, 6);
        expect (step <= bound, what + " @" + juce::String (edge));
    }

    void logClicks()
    {
        logMessage ("maxStep (bound " + juce::String (noClickBound(), 6) + "):" + clickSummary);
        clickSummary.clear();
    }

    Stereo renderProject (const Fixture& f, int64_t start, int total, int block = 512)
    {
        expect (f.ok, "fixture setup failed");
        Renderer r;
        r.prepare (4096);
        r.setProject (f.snapshot());
        return renderRange (r, f.source, start, total, block);
    }

    void runTest() override
    {
        constexpr int whole = (int) takeSamples + 1000;

        beginTest ("Output does not depend on the block size");
        {
            Fixture f;
            const auto reference = renderProject (f, 0, whole, 64);

            for (int block : { 128, 333, 4096 })
            {
                const auto out = renderProject (f, 0, whole, block);
                expect (out.L == reference.L && out.R == reference.R, "block " + juce::String (block));
            }

            expect (maxStep (reference.L) > 0.0f);
        }

        beginTest ("A split keeping both halves renders identical to the unsplit region");
        {
            Fixture f;
            const auto unsplit = renderProject (f, 0, whole);
            expect (f.apply (Ops::splitRegion (f.track, f.region, 59232, newId())));
            expectEquals ((int) f.project.tracks[0].regions.size(), 2);
            const auto split = renderProject (f, 0, whole);
            expect (split.L == unsplit.L && split.R == unsplit.R);
        }

        beginTest ("Deleting the middle third fades both cut points to 0 without clicks");
        {
            Fixture f;
            expect (f.hardLeft());
            const auto full = renderProject (f, 0, whole);
            const auto middle = newId(), last = newId();
            expect (f.apply (Ops::splitRegion (f.track, f.region, 64000, middle)));
            expect (f.apply (Ops::splitRegion (f.track, middle, 128000, last)));
            expect (f.apply (Ops::deleteRegion (f.track, middle)));
            const auto out = renderProject (f, 0, whole);

            expectNoClick (out.L, "delete middle third");
            expectNoClickAt (out.L, 64000, "delete middle third");
            expectNoClickAt (out.L, 128000, "delete middle third");
            logClicks();
            expectEquals (out.L[63999], 0.0f);
            expectEquals (out.L[128000], 0.0f);

            for (int i = 64000; i < 128000; ++i)
                if (out.L[(size_t) i] != 0.0f) { expect (false, "gap not silent at " + juce::String (i)); break; }

            // Untouched before the fade-out and after the fade-in.
            expectEquals (out.L[64000 - 145], full.L[64000 - 145]);
            expectEquals (out.L[128000 + 144], full.L[128000 + 144]);
            expect (std::abs (out.L[64000 - 72]) < std::abs (full.L[64000 - 72]) || full.L[64000 - 72] == 0.0f);
        }

        beginTest ("Moving a region 5000 samples later has no clicks at its new edges");
        {
            Fixture f;
            expect (f.hardLeft());
            expect (f.apply (Ops::trimRegion (f.track, f.region, 1000, 1000, 96000)));
            expect (f.apply (Ops::moveRegion (f.track, f.region, 6000)));
            const auto out = renderProject (f, 0, 110000);
            expectNoClick (out.L, "move start+end");
            expectNoClickAt (out.L, 6000, "move");
            expectNoClickAt (out.L, 102000, "move");
            logClicks();
            expectEquals (out.L[5999], 0.0f);
            expect (out.L[6200] != 0.0f);
            expectEquals (out.L[102000], 0.0f);
        }

        beginTest ("Trimming the start by 777 samples has no clicks");
        {
            Fixture f;
            expect (f.hardLeft());
            expect (f.apply (Ops::trimRegion (f.track, f.region, 777, 777, takeSamples - 777)));
            const auto out = renderProject (f, 0, whole);
            expectNoClick (out.L, "trim start");
            expectNoClickAt (out.L, 777, "trim start");
            logClicks();
            expectEquals (out.L[776], 0.0f);
        }

        beginTest ("An overlapping region on top replaces the one below");
        {
            Fixture f;
            const auto take2 = newId();
            expect (f.apply (Ops::addTake (take2, 1)));
            expect (f.apply (Ops::updateTake (take2, 4, takeSamples, true)));
            f.source.add (take2, 1);
            expect (f.apply (Ops::addRegion (f.track, { newId(), take2, 48000, 10000, 48000 })));
            expect (f.hardLeft());
            const auto clicks = renderProject (f, 0, whole);
            expectNoClick (clicks.L, "overlap");
            expectNoClickAt (clicks.L, 48000, "overlap");
            expectNoClickAt (clicks.L, 96000, "overlap");
            logClicks();

            expect (f.apply (Ops::setMix (f.track, -6.0f, 0.0f, std::nullopt)));
            const auto out = renderProject (f, 0, whole);

            const auto gain = dbToGain (-6.0f) * centre();
            bool same = true;

            for (int i = 48000 + 144; i < 96000 - 144; ++i)
            {
                const auto expected = SineSource::sine (10000 + i - 48000) * gain;
                same = same && std::abs (out.L[(size_t) i] - expected) <= 1.0e-6f
                            && std::abs (out.R[(size_t) i] - expected) <= 1.0e-6f;
            }

            expect (same, "middle of the overlap is not the top region alone");
        }

        beginTest ("set_mix gain ramps over rampSamples");
        {
            Fixture f;
            expect (f.apply (Ops::setMix (f.track, -60.0f, -1.0f, std::nullopt)));
            Renderer r;
            r.prepare (512);
            r.setProject (f.snapshot());
            auto out = renderRange (r, f.source, 0, 4800, 64);

            for (auto x : out.L)
                expectEquals (x, 0.0f);

            expect (f.apply (Ops::setMix (f.track, 0.0f, std::nullopt, std::nullopt)));
            r.setProject (f.snapshot());
            const auto after = renderRange (r, f.source, 4800, 4800, 64);
            append (out, after);
            expectNoClick (out.L, "gain -60 -> 0 dB");
            expectNoClickAt (out.L, 4800, "gain -60 -> 0 dB");
            logClicks();

            const auto full = renderProject (f, 0, 9600);
            bool rampedDown = true;

            for (int i = 4800 + 10; i < 4800 + 64; ++i)
                if (std::abs (full.L[(size_t) i]) > 0.05f)
                    rampedDown = rampedDown && std::abs (out.L[(size_t) i]) < 0.5f * std::abs (full.L[(size_t) i]);

            expect (rampedDown, "first block after the change is already at the target");
            expect (out.L[4800 + 63] != full.L[4800 + 63]);

            bool atTarget = true;

            for (int i = 4800 + 960; i < 4800 + 960 + 64; ++i)
                atTarget = atTarget && std::abs (out.L[(size_t) i] - full.L[(size_t) i]) <= 1.0e-6f;

            expect (atTarget, "block after rampSamples is not at the target");
        }

        beginTest ("Mute and solo ramp; snapRamps jumps");
        {
            Fixture f;
            expect (f.hardLeft());
            Renderer r;
            r.prepare (512);
            r.setProject (f.snapshot());
            auto out = renderRange (r, f.source, 0, 4800, 64);
            expect (f.apply (Ops::setMix (f.track, std::nullopt, std::nullopt, true)));
            r.setProject (f.snapshot());
            append (out, renderRange (r, f.source, 4800, 4800, 64));
            expectNoClick (out.L, "mute");
            expectNoClickAt (out.L, 4800, "mute");
            expect (out.L[4800 + 63] != 0.0f, "mute is not ramped");

            for (int i = 4800 + 960; i < 9600; ++i)
                if (out.L[(size_t) i] != 0.0f) { expect (false, "mute did not reach 0"); break; }

            // Solo: a second track with the same content, both at -6 dB hard left so the sum is the 0.5 sine;
            // soloing the second ramps the first out.
            Fixture g;
            const auto other = newId();
            expect (g.apply (Ops::addTrack (other, "Other", TrackKind::voice)));
            expect (g.apply (Ops::addRegion (other, { newId(), g.take, 0, 0, takeSamples })));
            expect (g.apply (Ops::setMix (g.track, -6.0206f, -1.0f, std::nullopt)));
            expect (g.apply (Ops::setMix (other, -6.0206f, -1.0f, std::nullopt)));
            Renderer s;
            s.prepare (512);
            s.setProject (g.snapshot());
            auto both = renderRange (s, g.source, 0, 4800, 64);
            s.setSolo ({ other });
            append (both, renderRange (s, g.source, 4800, 4800, 64));
            expectNoClick (both.L, "solo");
            expectNoClickAt (both.L, 4800, "solo");
            logClicks();

            const auto single = renderProject (g, 0, 9600);   // both tracks: twice one track
            bool half = true;

            for (int i = 4800 + 960; i < 9600; ++i)
                half = half && std::abs (both.L[(size_t) i] - 0.5f * single.L[(size_t) i]) <= 1.0e-6f;

            expect (half, "solo did not leave only the soloed track");

            // snapRamps: unmute and snap, the very next block is at full gain.
            expect (f.apply (Ops::setMix (f.track, std::nullopt, std::nullopt, false)));
            r.setProject (f.snapshot());
            r.snapRamps();
            const auto snapped = renderRange (r, f.source, 9600, 64, 64);
            const auto ref = renderProject (f, 9600, 64);
            expect (snapped.L == ref.L, "snapRamps did not jump to the target");
        }

        beginTest ("Stereo takes: balance law");
        {
            Fixture f (2);
            const auto gain = dbToGain (-6.0f);
            const int from = 1000, to = 48000;   // past the fade-in

            const auto check = [&] (float pan, float leftScale, float rightScale)
            {
                expect (f.apply (Ops::setMix (f.track, -6.0f, pan, std::nullopt)));
                const auto out = renderProject (f, 0, to);
                bool same = true;

                for (int i = from; i < to; ++i)
                    same = same && std::abs (out.L[(size_t) i] - SineSource::sine (i) * gain * leftScale) <= 1.0e-6f
                                && std::abs (out.R[(size_t) i] - SineSource::sine2 (i) * gain * rightScale) <= 1.0e-6f;

                expect (same, "pan " + juce::String (pan));
            };

            check (0.0f, 1.0f, 1.0f);
            check (-1.0f, 1.0f, 0.0f);
            check (1.0f, 0.0f, 1.0f);
        }

        beginTest ("Stereo takes: delete middle third and overlap have no clicks on either channel");
        {
            Fixture f (2);
            const auto middle = newId(), last = newId();
            expect (f.apply (Ops::splitRegion (f.track, f.region, 64000, middle)));
            expect (f.apply (Ops::splitRegion (f.track, middle, 128000, last)));
            expect (f.apply (Ops::deleteRegion (f.track, middle)));
            const auto cut = renderProject (f, 0, whole);

            for (const auto* ch : { &cut.L, &cut.R })
            {
                const juce::String name = ch == &cut.L ? "L" : "R";
                expectNoClick (*ch, "delete " + name);
                expectNoClickAt (*ch, 64000, "delete " + name);
                expectNoClickAt (*ch, 128000, "delete " + name);
            }

            Fixture g (2);
            const auto take2 = newId();
            expect (g.apply (Ops::addTake (take2, 2)));
            expect (g.apply (Ops::updateTake (take2, 4, takeSamples, true)));
            g.source.add (take2, 2);
            expect (g.apply (Ops::addRegion (g.track, { newId(), take2, 48000, 10000, 48000 })));
            const auto overlap = renderProject (g, 0, whole);

            for (const auto* ch : { &overlap.L, &overlap.R })
            {
                const juce::String name = ch == &overlap.L ? "L" : "R";
                expectNoClick (*ch, "overlap " + name);
                expectNoClickAt (*ch, 48000, "overlap " + name);
                expectNoClickAt (*ch, 96000, "overlap " + name);
            }

            logClicks();
        }

        beginTest ("Deleting a region under the playhead crossfades to the new layout");
        {
            Fixture f;
            expect (f.hardLeft());
            Renderer r;
            r.prepare (512);
            r.setProject (f.snapshot());
            auto out = renderRange (r, f.source, 0, 4900, 4900);
            expect (f.apply (Ops::deleteRegion (f.track, f.region)));
            r.setProject (f.snapshot());                       // between two render calls, mid-way through playback
            append (out, renderRange (r, f.source, 4900, 4700, 300));
            expectNoClick (out.L, "delete under playhead");
            expectNoClickAt (out.L, 4900, "delete under playhead");
            logClicks();
            expect (out.L[4901] != 0.0f, "the deleted region was cut, not faded");

            bool silent = true;

            for (int i = 4900 + transportFadeSamples; i < 9600; ++i)
                silent = silent && out.L[(size_t) i] == 0.0f && out.R[(size_t) i] == 0.0f;

            expect (silent, "not silent after the crossfade");
        }

        beginTest ("Replacing the snapshot with an identical project changes nothing");
        {
            Fixture f;
            Renderer a, b;

            for (auto* r : { &a, &b })
            {
                r->prepare (512);
                r->setProject (f.snapshot());
            }

            auto outA = renderRange (a, f.source, 0, 1000, 100);
            auto outB = renderRange (b, f.source, 0, 1000, 100);
            b.setProject (f.snapshot());
            append (outA, renderRange (a, f.source, 1000, 2000, 100));
            append (outB, renderRange (b, f.source, 1000, 2000, 100));
            expect (outA.L == outB.L && outA.R == outB.R);
            expect (maxStep (outA.L) > 0.0f);
        }

        beginTest ("panGains is constant power");
        {
            float l = 0.0f, r = 0.0f;
            panGains (0.0f, l, r);
            expectWithinAbsoluteError (l, 0.7071f, 1.0e-4f);
            expectWithinAbsoluteError (r, 0.7071f, 1.0e-4f);
            panGains (-1.0f, l, r);
            expectWithinAbsoluteError (l, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (r, 0.0f, 1.0e-6f);
            panGains (1.0f, l, r);
            expectWithinAbsoluteError (l, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (r, 1.0f, 1.0e-6f);
        }

        beginTest ("Metronome at 120 BPM 4/4");
        {
            constexpr int total = 5 * 24000;
            std::vector<float> L ((size_t) total, 0.0f), R ((size_t) total, 0.0f);

            for (int done = 0; done < total; done += 333)
            {
                const int n = std::min (333, total - done);
                renderMetronome (120.0, 4, done, n, L.data() + done, R.data() + done);
            }

            expect (L == R);

            for (int beat = 0; beat < 5; ++beat)
            {
                const int s = beat * 24000;

                if (s > 0)
                    expectEquals (L[(size_t) s - 1], 0.0f);

                float peak = 0.0f;
                int crossings = 0;

                for (int i = s; i < s + 960; ++i)
                {
                    peak = std::max (peak, std::abs (L[(size_t) i]));

                    if (i > s && (L[(size_t) i - 1] < 0.0f) != (L[(size_t) i] < 0.0f))
                        ++crossings;
                }

                expect (peak > 0.3f && peak <= 0.5f, "peak " + juce::String (peak));
                const int expected = beat % 4 == 0 ? 40 : 32;
                expect (std::abs (crossings - expected) <= 2,
                        "beat " + juce::String (beat) + ": " + juce::String (crossings) + " crossings");

                for (int i = s + 960; i < s + 24000; ++i)
                    if (L[(size_t) i] != 0.0f) { expect (false, "click longer than 20 ms"); break; }
            }

            // Adds to the existing contents.
            std::vector<float> a (64, 1.0f), b (64, 1.0f);
            renderMetronome (120.0, 4, 30000, 64, a.data(), b.data());
            expect (a == std::vector<float> (64, 1.0f));
        }

        beginTest ("Negative positions render silence before 0");
        {
            Fixture f;
            const auto out = renderProject (f, -1000, 2000, 2000);
            const auto ref = renderProject (f, 0, 1000, 1000);

            for (int i = 0; i < 1000; ++i)
                if (out.L[(size_t) i] != 0.0f || out.R[(size_t) i] != 0.0f) { expect (false, "not silent"); break; }

            expect (std::equal (ref.L.begin(), ref.L.end(), out.L.begin() + 1000));
            expect (maxStep (ref.L) > 0.0f);
        }

        beginTest ("An unknown take renders silence");
        {
            Fixture f;
            SineSource empty;
            Renderer r;
            r.prepare (512);
            r.setProject (f.snapshot());
            std::vector<float> L (512, 1.0f), R (512, 1.0f);
            r.render (empty, 0, 512, L.data(), R.data());
            expect (L == std::vector<float> (512, 0.0f) && R == std::vector<float> (512, 0.0f));
        }
    }
};

static RenderTests renderTests;
}
