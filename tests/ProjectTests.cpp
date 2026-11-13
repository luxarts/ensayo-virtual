#include "core/Project.h"
#include <limits>
#include <set>

namespace ensayo
{
class ProjectTests : public juce::UnitTest
{
public:
    ProjectTests() : juce::UnitTest ("Project", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("newId is 32 lowercase hex and unique");
        {
            std::set<juce::String> seen;

            for (int i = 0; i < 1000; ++i)
            {
                const auto id = newId();
                expect (isValidId (id));
                expectEquals (id.length(), 32);
                seen.insert (id);
            }

            expectEquals ((int) seen.size(), 1000);
            expect (! isValidId ("xyz"));
            expect (! isValidId (newId().toUpperCase()));
            expect (! isValidId ({}));
        }

        beginTest ("JSON round trip keeps the content hash");
        {
            const auto host = newId(), other = newId();
            auto p = makeProject ("Song", host, "Lucas");
            expect (applyOp (p, Ops::addUser (other, "Ana"), host).ok);
            const auto t1 = newId(), t2 = newId();

            for (auto t : { t1, t2 })
            {
                expect (applyOp (p, Ops::addTake (t, 1), host).ok);
                expect (applyOp (p, Ops::updateTake (t, 3, 480000, true), host).ok);
            }

            for (auto author : { host, other })
            {
                const auto tr = newId();
                expect (applyOp (p, Ops::addTrack (tr, "Track", TrackKind::instrument), author).ok);
                expect (applyOp (p, Ops::setMix (tr, -3.5f, 0.25f, true), author).ok);
                expect (applyOp (p, Ops::addRegion (tr, { newId(), t1, 0, 0, 1000 }), author).ok);
                expect (applyOp (p, Ops::addRegion (tr, { newId(), t2, 2000, 10, 5000 }), author).ok);
            }

            expect (applyOp (p, Ops::setTempo (92.5, 3), host).ok);

            juce::String err;
            const auto back = projectFromJson (toJson (p), err);
            expect (back.has_value(), err);

            if (back)
            {
                expectEquals (contentHash (*back), contentHash (p));
                expectEquals (back->version, p.version);
                expectEquals ((int) back->tracks.size(), 2);
                expectEquals ((int) back->users.size(), 2);
                expectEquals ((int) back->takes.size(), 2);
                expectEquals (back->tracks[1].regions[1].offset, (int64_t) 10);
                expectEquals (back->endSample(), (int64_t) 7000);
                expectEquals (back->userName (other), juce::String ("Ana"));
            }

            auto v = toVar (p);
            v.getDynamicObject()->removeProperty ("tracks");
            juce::String err2;
            expect (! projectFromVar (v, err2).has_value());
            expect (err2.isNotEmpty());

            juce::String err3;
            expect (! projectFromJson ("{ nope", err3).has_value());
            expect (err3.isNotEmpty());
        }

        beginTest ("add_track sets the author to the sender and validates");
        {
            const auto host = newId(), b = newId(), tr = newId();
            auto p = makeProject ("T", host, "Lucas");
            expect (applyOp (p, Ops::addTrack (tr, "  Voz  ", TrackKind::voice), b).ok);
            expect (p.findTrack (tr)->author == b);
            expectEquals (p.findTrack (tr)->name, juce::String ("Voz"));
            expect (! applyOp (p, Ops::addTrack (tr, "Again", TrackKind::voice), b).ok);
            expect (! applyOp (p, Ops::addTrack (newId(), "   ", TrackKind::voice), b).ok);
            expect (! applyOp (p, Ops::addTrack (newId(), juce::String::repeatedString ("x", 65), TrackKind::voice), b).ok);
            expect (! applyOp (p, Ops::addTrack ("bad", "Name", TrackKind::voice), b).ok);
            expectEquals ((int) p.tracks.size(), 1);
        }

        beginTest ("track permissions: author and host only");
        {
            const auto host = newId(), a = newId(), b = newId(), tr = newId(), take = newId(), reg = newId();
            auto p = makeProject ("T", host, "Lucas");
            applyOp (p, Ops::addTake (take, 1), a);
            applyOp (p, Ops::updateTake (take, 1, 48000, true), host);
            applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), a);
            applyOp (p, Ops::addRegion (tr, { reg, take, 0, 0, 1000 }), a);

            const auto v = p.version;
            expect (! applyOp (p, Ops::renameTrack (tr, "Hacked"), b).ok);
            expect (! applyOp (p, Ops::setMix (tr, -6.0f, {}, {}), b).ok);
            expect (! applyOp (p, Ops::deleteRegion (tr, reg), b).ok);
            expect (! applyOp (p, Ops::deleteTrack (tr), b).ok);
            expectEquals (p.version, v);
            expectEquals (p.findTrack (tr)->name, juce::String ("A"));

            expect (applyOp (p, Ops::renameTrack (tr, "ByHost"), host).ok);
            expect (applyOp (p, Ops::renameTrack (tr, "ByAuthor"), a).ok);
            expect (applyOp (p, Ops::setMix (tr, -6.0f, -0.5f, false), a).ok);
            expectEquals (p.findTrack (tr)->gainDb, -6.0f);
            expectEquals (p.findTrack (tr)->pan, -0.5f);
            expect (applyOp (p, Ops::deleteRegion (tr, reg), host).ok);
            expect (applyOp (p, Ops::deleteTrack (tr), a).ok);
            expect (p.tracks.empty());
        }

        beginTest ("set_mix rejects out-of-range values");
        {
            const auto host = newId(), tr = newId();
            auto p = makeProject ("T", host, "Lucas");
            applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), host);
            expect (! applyOp (p, Ops::setMix (tr, 12.5f, {}, {}), host).ok);
            expect (! applyOp (p, Ops::setMix (tr, -61.0f, {}, {}), host).ok);
            expect (! applyOp (p, Ops::setMix (tr, {}, 1.5f, {}), host).ok);
            expect (applyOp (p, Ops::setMix (tr, 12.0f, 1.0f, true), host).ok);
        }

        beginTest ("add_region invariants");
        {
            const auto host = newId(), tr = newId(), take = newId();
            auto p = makeProject ("T", host, "Lucas");
            applyOp (p, Ops::addTake (take, 2), host);
            applyOp (p, Ops::updateTake (take, 1, 1000, true), host);
            applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), host);

            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, 0, 500, 501 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { newId(), newId(), 0, 0, 100 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, -1, 0, 100 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, 0, -1, 100 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, 0, 0, 0 }), host).ok);
            expect (p.findTrack (tr)->regions.empty());

            const auto r1 = newId(), r2 = newId();
            expect (applyOp (p, Ops::addRegion (tr, { r1, take, 0, 0, 1000 }), host).ok);
            expect (applyOp (p, Ops::addRegion (tr, { r2, take, 100, 500, 500 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { r2, take, 0, 0, 10 }), host).ok);   // id in use
            const auto& rs = p.findTrack (tr)->regions;
            expectEquals ((int) rs.size(), 2);
            expect (rs[1].id == r2);
        }

        beginTest ("split_region");
        {
            const auto host = newId(), track = newId(), take = newId(), region = newId();
            auto p = makeProject ("T", host, "Lucas");
            applyOp (p, Ops::addTake (take, 1), host);
            applyOp (p, Ops::updateTake (take, 1, 48000, true), host);
            applyOp (p, Ops::addTrack (track, "Gtr", TrackKind::voice), host);
            applyOp (p, Ops::addRegion (track, { region, take, 500, 100, 2000 }), host);
            const auto other = newId();
            applyOp (p, Ops::addRegion (track, { other, take, 5000, 0, 100 }), host);

            expect (! applyOp (p, Ops::splitRegion (track, region, 500, newId()), host).ok);
            expect (! applyOp (p, Ops::splitRegion (track, region, 2500, newId()), host).ok);
            expect (! applyOp (p, Ops::splitRegion (track, region, 1000, other), host).ok);   // id in use

            const auto right = newId();
            expect (applyOp (p, Ops::splitRegion (track, region, 1000, right), host).ok);
            const auto& rs = p.findTrack (track)->regions;
            expectEquals ((int) rs.size(), 3);
            expect (rs[0].id == region && rs[0].start == 500 && rs[0].offset == 100 && rs[0].length == 500);
            expect (rs[1].id == right && rs[1].start == 1000 && rs[1].offset == 600 && rs[1].length == 1500);
            expect (rs[1].take == take);
            expect (rs[2].id == other);
        }

        beginTest ("move_region and trim_region move to the top");
        {
            const auto host = newId(), tr = newId(), take = newId();
            const auto r1 = newId(), r2 = newId(), r3 = newId();
            auto p = makeProject ("T", host, "Lucas");
            applyOp (p, Ops::addTake (take, 1), host);
            applyOp (p, Ops::updateTake (take, 1, 10000, true), host);
            applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), host);
            applyOp (p, Ops::addRegion (tr, { r1, take, 1000, 500, 2000 }), host);
            applyOp (p, Ops::addRegion (tr, { r2, take, 0, 0, 100 }), host);
            applyOp (p, Ops::addRegion (tr, { r3, take, 0, 0, 100 }), host);

            expect (! applyOp (p, Ops::moveRegion (tr, r1, -1), host).ok);
            expect (applyOp (p, Ops::moveRegion (tr, r1, 3000), host).ok);
            expect (p.findTrack (tr)->regions.back().id == r1);
            expectEquals (p.findTrack (tr)->regions.back().start, (int64_t) 3000);

            // r2 has start - offset = 0; trimming the tail keeps it anchored and raises it.
            expect (applyOp (p, Ops::trimRegion (tr, r2, 0, 0, 50), host).ok);
            expect (p.findTrack (tr)->regions.back().id == r2);

            // r1 now has start - offset = 2500.
            expect (! applyOp (p, Ops::trimRegion (tr, r1, 3100, 500, 1000), host).ok);   // anchor changed
            expect (! applyOp (p, Ops::trimRegion (tr, r1, 3500, 1000, 9500), host).ok);  // beyond the take
            expect (! applyOp (p, Ops::trimRegion (tr, r1, 3000, 500, 0), host).ok);
            expect (applyOp (p, Ops::trimRegion (tr, r1, 3100, 600, 1000), host).ok);
            const auto& last = p.findTrack (tr)->regions.back();
            expect (last.id == r1 && last.start == 3100 && last.offset == 600 && last.length == 1000);
        }

        beginTest ("huge int64 values cannot overflow region invariants");
        {
            const auto host = newId(), tr = newId(), take = newId(), reg = newId();
            const auto big = std::numeric_limits<int64_t>::max();
            auto p = makeProject ("T", host, "Lucas");
            applyOp (p, Ops::addTake (take, 1), host);
            applyOp (p, Ops::updateTake (take, 1, 10000, true), host);
            applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), host);

            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, 0, big, 1 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, big, 0, 1 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, big - 5, 0, 100 }), host).ok);
            expect (! applyOp (p, Ops::addRegion (tr, { newId(), take, 0, 0, big }), host).ok);
            expect (applyOp (p, Ops::addRegion (tr, { reg, take, 0, 0, 100 }), host).ok);

            expect (! applyOp (p, Ops::moveRegion (tr, reg, big), host).ok);
            expect (! applyOp (p, Ops::trimRegion (tr, reg, 0, big, 1), host).ok);
            expect (! applyOp (p, Ops::trimRegion (tr, reg, big, big, 1), host).ok);
            expect (! applyOp (p, Ops::trimRegion (tr, reg, 0, 0, big), host).ok);
            expect (! applyOp (p, Ops::splitRegion (tr, reg, big, newId()), host).ok);
            expect (! applyOp (p, Ops::updateTake (take, 1, big, true), host).ok);
            expectEquals (p.findTrack (tr)->regions[0].length, (int64_t) 100);
        }

        beginTest ("beatsPerBar is limited to 1..12");
        {
            const auto host = newId();
            auto p = makeProject ("T", host, "Lucas");
            expect (applyOp (p, Ops::setTempo (120.0, 1), host).ok);
            expect (applyOp (p, Ops::setTempo (120.0, 12), host).ok);
            expect (! applyOp (p, Ops::setTempo (120.0, 0), host).ok);
            expect (! applyOp (p, Ops::setTempo (120.0, 13), host).ok);
        }

        beginTest ("loader rejects projects that break the invariants");
        {
            const auto host = newId(), tr = newId(), take = newId();
            auto p = makeProject ("T", host, "Lucas");
            applyOp (p, Ops::addTake (take, 1), host);
            applyOp (p, Ops::updateTake (take, 1, 1000, true), host);
            applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), host);
            applyOp (p, Ops::addRegion (tr, { newId(), take, 0, 0, 500 }), host);

            juce::String err;
            expect (projectFromVar (toVar (p), err).has_value(), err);

            // Edits the serialized project, then expects the loader to refuse it and name the field.
            auto rejects = [&] (const juce::String& what, const juce::String& field, auto&& mutate)
            {
                auto v = toVar (p);
                mutate (v);
                juce::String e;
                expect (! projectFromVar (v, e).has_value(), what);
                expect (e.containsIgnoreCase (field), what + ": error was " + e);
            };

            auto track0 = [] (juce::var& v) { return v["tracks"][0].getDynamicObject(); };
            auto region0 = [] (juce::var& v) { return v["tracks"][0]["regions"][0].getDynamicObject(); };

            rejects ("bpm 0", "bpm", [] (juce::var& v) { v.getDynamicObject()->setProperty ("bpm", 0.0); });
            rejects ("beatsPerBar 13", "beatsPerBar", [] (juce::var& v) { v.getDynamicObject()->setProperty ("beatsPerBar", 13); });
            rejects ("beatsPerBar huge", "beatsPerBar", [] (juce::var& v) { v.getDynamicObject()->setProperty ("beatsPerBar", (juce::int64) 1 << 33); });
            rejects ("empty name", "name", [] (juce::var& v) { v.getDynamicObject()->setProperty ("name", ""); });
            rejects ("gain 1000", "gain", [&] (juce::var& v) { track0 (v)->setProperty ("gainDb", 1000.0); });
            rejects ("channels 3", "channels", [] (juce::var& v) { v["takes"][0].getDynamicObject()->setProperty ("channels", 3); });
            rejects ("missing take", "take", [&] (juce::var& v) { region0 (v)->setProperty ("take", newId()); });
            rejects ("region beyond take", "regions", [&] (juce::var& v) { region0 (v)->setProperty ("length", 5000); });
            rejects ("duplicate track ids", "tracks.id", [] (juce::var& v)
            {
                auto* tracks = v["tracks"].getArray();
                auto copy = (*tracks)[0].clone();
                copy.getDynamicObject()->setProperty ("regions", juce::Array<juce::var>());
                tracks->add (copy);
            });
        }

        beginTest ("set_tempo and rename_project are host only");
        {
            const auto host = newId(), b = newId();
            auto p = makeProject ("T", host, "Lucas");
            expect (! applyOp (p, Ops::setTempo (100.0, 4), b).ok);
            expect (! applyOp (p, Ops::renameProject ("X"), b).ok);
            expect (! applyOp (p, Ops::addUser (b, "B"), b).ok);
            expect (! applyOp (p, Ops::setTempo (29.0, 4), host).ok);
            expect (! applyOp (p, Ops::setTempo (301.0, 4), host).ok);
            expect (! applyOp (p, Ops::setTempo (120.0, 0), host).ok);
            expect (applyOp (p, Ops::setTempo (92.0, 4), host).ok);
            expectEquals (p.bpm, 92.0);
            expect (applyOp (p, Ops::renameProject ("Nueva"), host).ok);
            expectEquals (p.name, juce::String ("Nueva"));
            expect (applyOp (p, Ops::addUser (b, "B"), host).ok);
            expect (applyOp (p, Ops::addUser (b, "Bea"), host).ok);
            expectEquals ((int) p.users.size(), 2);
            expectEquals (p.userName (b), juce::String ("Bea"));
        }

        beginTest ("add_take and update_take rules");
        {
            const auto host = newId(), a = newId(), b = newId(), take = newId();
            auto p = makeProject ("T", host, "Lucas");
            expect (! applyOp (p, Ops::addTake (newId(), 3), a).ok);
            expect (applyOp (p, Ops::addTake (take, 2), a).ok);
            expect (! applyOp (p, Ops::addTake (take, 1), a).ok);
            expect (p.findTake (take)->author == a && p.findTake (take)->segments == 0 && ! p.findTake (take)->complete);

            expect (! applyOp (p, Ops::updateTake (take, 1, 100, false), b).ok);
            expect (applyOp (p, Ops::updateTake (take, 2, 200, false), a).ok);
            expect (! applyOp (p, Ops::updateTake (take, 1, 200, false), a).ok);   // segments shrink
            expect (! applyOp (p, Ops::updateTake (take, 2, 100, false), a).ok);   // samples shrink
            expect (applyOp (p, Ops::updateTake (take, 3, 300, true), host).ok);
            expect (! applyOp (p, Ops::updateTake (take, 3, 300, false), host).ok);   // un-complete
            expect (p.findTake (take)->complete);
            expect (! applyOp (p, Ops::updateTake (newId(), 1, 1, false), host).ok);
        }

        beginTest ("version increments once per accepted op only");
        {
            const auto host = newId(), tr = newId();
            auto p = makeProject ("T", host, "Lucas");
            expectEquals (p.version, (int64_t) 0);
            expect (applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), host).ok);
            expectEquals (p.version, (int64_t) 1);
            expect (! applyOp (p, Ops::addTrack (tr, "A", TrackKind::voice), host).ok);
            expect (! applyOp (p, juce::var(), host).ok);
            expect (! applyOp (p, Ops::renameTrack (newId(), "Z"), host).ok);
            auto* unknown = new juce::DynamicObject();
            unknown->setProperty ("op", "explode");
            expect (! applyOp (p, juce::var (unknown), host).ok);
            expectEquals (p.version, (int64_t) 1);
            expect (applyOp (p, Ops::renameTrack (tr, "B"), host).ok);
            expectEquals (p.version, (int64_t) 2);
        }
    }
};

static ProjectTests projectTests;
}
