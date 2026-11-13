#include "core/Project.h"
#include "core/Sha1.h"
#include <climits>
#include <cmath>

namespace ensayo
{
namespace
{
constexpr int maxNameLength = 64;
constexpr double minBpm = 30.0, maxBpm = 300.0;
constexpr int maxBeatsPerBar = 32;

juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();

    for (const auto& [k, v] : props)
        o->setProperty (k, v);

    return juce::var (o);
}

juce::var i64 (int64_t v)   { return juce::var ((juce::int64) v); }

// Typed, failure-latching access to the fields of a JSON object. Once a field is missing or has the
// wrong type, `ok` stays false and `error` names the first offender.
class Reader
{
public:
    explicit Reader (const juce::var& v) : object (v.getDynamicObject())
    {
        if (object == nullptr)
            fail ("not an object");
    }

    bool ok = true;
    juce::String error;

    Id id (const char* key)
    {
        const auto v = get (key);
        const auto s = v.isString() ? v.toString() : juce::String();

        if (! isValidId (s))
            fail (juce::String ("invalid id: ") + key);

        return s;
    }

    juce::String str (const char* key)
    {
        const auto v = get (key);

        if (! v.isString())
            fail (juce::String ("not a string: ") + key);

        return v.toString();
    }

    int64_t integer (const char* key)
    {
        const auto v = get (key);

        if (v.isInt())    return (int64_t) (int) v;
        if (v.isInt64())  return (int64_t) (juce::int64) v;

        fail (juce::String ("not an integer: ") + key);
        return 0;
    }

    double number (const char* key)
    {
        const auto v = get (key);

        if ((v.isInt() || v.isInt64() || v.isDouble()) && std::isfinite ((double) v))
            return (double) v;

        fail (juce::String ("not a number: ") + key);
        return 0.0;
    }

    bool boolean (const char* key)
    {
        const auto v = get (key);

        if (! v.isBool())
            fail (juce::String ("not a boolean: ") + key);

        return (bool) v;
    }

    bool has (const char* key) const   { return object != nullptr && object->hasProperty (key); }

    juce::var get (const char* key)
    {
        if (object == nullptr || ! object->hasProperty (key))
        {
            fail (juce::String ("missing field: ") + key);
            return {};
        }

        return object->getProperty (key);
    }

    juce::Array<juce::var>* array (const char* key)
    {
        const auto v = get (key);
        auto* a = v.getArray();

        if (a == nullptr)
            fail (juce::String ("not an array: ") + key);

        return a;
    }

private:
    void fail (const juce::String& why)
    {
        if (ok)
            error = why;

        ok = false;
    }

    juce::DynamicObject* object = nullptr;
};

juce::var regionToVar (const Region& r)
{
    return makeObject ({ { "id", r.id }, { "take", r.take }, { "start", i64 (r.start) },
                         { "offset", i64 (r.offset) }, { "length", i64 (r.length) } });
}

juce::var trackToVar (const Track& t)
{
    juce::Array<juce::var> regions;

    for (const auto& r : t.regions)
        regions.add (regionToVar (r));

    return makeObject ({ { "id", t.id }, { "name", t.name }, { "author", t.author },
                         { "kind", trackKindToString (t.kind) }, { "gainDb", (double) t.gainDb },
                         { "pan", (double) t.pan }, { "muted", t.muted }, { "regions", regions } });
}

bool validName (const juce::String& trimmed)   { return trimmed.isNotEmpty() && trimmed.length() <= maxNameLength; }

bool validMix (float gainDb, float pan) noexcept
{
    return std::isfinite (gainDb) && gainDb >= minFaderDb && gainDb <= maxFaderDb
        && std::isfinite (pan) && pan >= -1.0f && pan <= 1.0f;
}

bool idUsedByRegion (const Project& p, const Id& id) noexcept
{
    for (const auto& t : p.tracks)
        for (const auto& r : t.regions)
            if (r.id == id)
                return true;

    return false;
}

int indexOfRegion (const Track& t, const Id& id) noexcept
{
    for (size_t i = 0; i < t.regions.size(); ++i)
        if (t.regions[i].id == id)
            return (int) i;

    return -1;
}

OpResult fail (const juce::String& why)   { return { false, why }; }
OpResult success()                        { return { true, {} }; }

// Applies one operation to `p` (a scratch copy; the caller discards it on failure).
OpResult apply (Project& p, const juce::var& opVar, const Id& by)
{
    if (opVar.getDynamicObject() == nullptr)
        return fail ("operation is not an object");

    Reader r (opVar);
    const auto op = r.str ("op");

    if (! r.ok)
        return fail (r.error);

    auto bad = [&r] { return fail (r.error); };
    const auto isHost = (by == p.hostUser);

    if (op == "add_track")
    {
        const auto trackId = r.id ("track");
        const auto name = r.str ("name").trim();
        const auto kind = trackKindFromString (r.str ("kind"));

        if (! r.ok)              return bad();
        if (! kind)              return fail ("unknown track kind");
        if (! validName (name))  return fail ("invalid track name");
        if (p.findTrack (trackId) != nullptr)  return fail ("track id in use");

        Track t;
        t.id = trackId;
        t.name = name;
        t.author = by;
        t.kind = *kind;
        p.tracks.push_back (std::move (t));
        return success();
    }

    if (op == "set_tempo" || op == "rename_project" || op == "add_user")
    {
        if (! isHost)
            return fail ("only the host can do this");

        if (op == "set_tempo")
        {
            const auto bpm = r.number ("bpm");
            const auto beats = r.integer ("beatsPerBar");

            if (! r.ok)  return bad();
            if (bpm < minBpm || bpm > maxBpm)  return fail ("bpm out of range");
            if (beats < 1 || beats > maxBeatsPerBar)  return fail ("beatsPerBar out of range");

            p.bpm = bpm;
            p.beatsPerBar = (int) beats;
            return success();
        }

        if (op == "rename_project")
        {
            const auto name = r.str ("name").trim();

            if (! r.ok)  return bad();
            if (! validName (name))  return fail ("invalid project name");

            p.name = name;
            return success();
        }

        const auto userId = r.id ("user");
        const auto name = r.str ("name").trim();

        if (! r.ok)  return bad();
        if (! validName (name))  return fail ("invalid user name");

        for (auto& u : p.users)
            if (u.id == userId)
            {
                u.name = name;
                return success();
            }

        p.users.push_back ({ userId, name });
        return success();
    }

    if (op == "add_take")
    {
        const auto takeId = r.id ("take");
        const auto channels = r.integer ("channels");

        if (! r.ok)  return bad();
        if (channels != 1 && channels != 2)  return fail ("channels must be 1 or 2");
        if (p.findTake (takeId) != nullptr)  return fail ("take id in use");

        Take t;
        t.id = takeId;
        t.author = by;
        t.channels = (int) channels;
        p.takes.push_back (t);
        return success();
    }

    if (op == "update_take")
    {
        const auto takeId = r.id ("take");
        const auto segments = r.integer ("segments");
        const auto samples = r.integer ("samples");
        const auto complete = r.boolean ("complete");

        if (! r.ok)  return bad();

        auto* take = p.findTake (takeId);

        if (take == nullptr)                       return fail ("unknown take");
        if (by != take->author && ! isHost)        return fail ("not allowed to update this take");
        if (segments < take->segments || segments > INT_MAX)  return fail ("segments can only grow");
        if (samples < take->samples)               return fail ("samples can only grow");
        if (take->complete && ! complete)          return fail ("a complete take cannot be reopened");

        take->segments = (int) segments;
        take->samples = samples;
        take->complete = complete;
        return success();
    }

    // Everything below addresses an existing track.
    const auto trackId = r.id ("track");

    if (! r.ok)  return bad();

    auto* track = p.findTrack (trackId);

    if (track == nullptr)
        return fail ("unknown track");

    if (! canEditTrack (p, *track, by))
        return fail ("not allowed to edit this track");

    if (op == "rename_track")
    {
        const auto name = r.str ("name").trim();

        if (! r.ok)  return bad();
        if (! validName (name))  return fail ("invalid track name");

        track->name = name;
        return success();
    }

    if (op == "delete_track")
    {
        p.tracks.erase (p.tracks.begin() + (track - p.tracks.data()));
        return success();
    }

    if (op == "set_mix")
    {
        float gain = track->gainDb, pan = track->pan;
        bool muted = track->muted;

        if (r.has ("gainDb"))  gain = (float) r.number ("gainDb");
        if (r.has ("pan"))     pan = (float) r.number ("pan");
        if (r.has ("muted"))   muted = r.boolean ("muted");

        if (! r.ok)  return bad();
        if (! validMix (gain, pan))  return fail ("mix value out of range");

        track->gainDb = gain;
        track->pan = pan;
        track->muted = muted;
        return success();
    }

    if (op == "add_region")
    {
        Region reg;
        reg.id = r.id ("region");
        reg.take = r.id ("take");
        reg.start = r.integer ("start");
        reg.offset = r.integer ("offset");
        reg.length = r.integer ("length");

        if (! r.ok)  return bad();

        const auto* take = p.findTake (reg.take);

        if (take == nullptr)  return fail ("unknown take");
        if (idUsedByRegion (p, reg.id))  return fail ("region id in use");
        if (reg.start < 0 || reg.offset < 0 || reg.length < 1)  return fail ("invalid region bounds");
        if (reg.offset + reg.length > take->samples)  return fail ("region reaches beyond the take");

        track->regions.push_back (reg);
        return success();
    }

    const auto regionId = r.id ("region");

    if (! r.ok)  return bad();

    const auto index = indexOfRegion (*track, regionId);

    if (index < 0)  return fail ("unknown region");

    auto& regions = track->regions;

    if (op == "delete_region")
    {
        regions.erase (regions.begin() + index);
        return success();
    }

    if (op == "split_region")
    {
        const auto at = r.integer ("at");
        const auto newRegion = r.id ("newRegion");

        if (! r.ok)  return bad();

        const auto left = regions[(size_t) index];

        if (at <= left.start || at >= left.end())  return fail ("split point outside the region");
        if (idUsedByRegion (p, newRegion))  return fail ("region id in use");

        Region right { newRegion, left.take, at, left.offset + (at - left.start), left.end() - at };
        regions[(size_t) index].length = at - left.start;
        regions.insert (regions.begin() + index + 1, right);
        return success();
    }

    if (op == "move_region")
    {
        const auto start = r.integer ("start");

        if (! r.ok)  return bad();
        if (start < 0)  return fail ("invalid start");

        auto moved = regions[(size_t) index];
        moved.start = start;
        regions.erase (regions.begin() + index);
        regions.push_back (moved);
        return success();
    }

    if (op == "trim_region")
    {
        const auto start = r.integer ("start");
        const auto offset = r.integer ("offset");
        const auto length = r.integer ("length");

        if (! r.ok)  return bad();

        auto trimmed = regions[(size_t) index];
        const auto* take = p.findTake (trimmed.take);

        if (take == nullptr)  return fail ("unknown take");
        if (start < 0 || offset < 0 || length < 1)  return fail ("invalid region bounds");
        if (offset + length > take->samples)  return fail ("region reaches beyond the take");
        if (start - offset != trimmed.start - trimmed.offset)  return fail ("trim must keep the audio anchored");

        trimmed.start = start;
        trimmed.offset = offset;
        trimmed.length = length;
        regions.erase (regions.begin() + index);
        regions.push_back (trimmed);
        return success();
    }

    return fail ("unknown operation: " + op);
}
}

Id newId()   { return juce::Uuid().toString(); }

bool isValidId (const Id& id) noexcept
{
    if (id.length() != 32)
        return false;

    for (auto c : id)
        if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;

    return true;
}

juce::String trackKindToString (TrackKind k)
{
    switch (k)
    {
        case TrackKind::voice:       return "voice";
        case TrackKind::instrument:  return "instrument";
        case TrackKind::imported:    return "imported";
    }

    return "voice";
}

std::optional<TrackKind> trackKindFromString (const juce::String& s)
{
    if (s == "voice")       return TrackKind::voice;
    if (s == "instrument")  return TrackKind::instrument;
    if (s == "imported")    return TrackKind::imported;

    return std::nullopt;
}

const Track* Project::findTrack (const Id& i) const noexcept
{
    for (const auto& t : tracks)
        if (t.id == i)
            return &t;

    return nullptr;
}

Track* Project::findTrack (const Id& i) noexcept
{
    return const_cast<Track*> (std::as_const (*this).findTrack (i));
}

const Take* Project::findTake (const Id& i) const noexcept
{
    for (const auto& t : takes)
        if (t.id == i)
            return &t;

    return nullptr;
}

Take* Project::findTake (const Id& i) noexcept
{
    return const_cast<Take*> (std::as_const (*this).findTake (i));
}

juce::String Project::userName (const Id& i) const
{
    for (const auto& u : users)
        if (u.id == i)
            return u.name;

    return {};
}

int64_t Project::endSample() const noexcept
{
    int64_t end = 0;

    for (const auto& t : tracks)
        for (const auto& r : t.regions)
            end = std::max (end, r.end());

    return end;
}

Project makeProject (const juce::String& name, const Id& hostUser, const juce::String& hostName)
{
    Project p;
    p.id = newId();
    p.name = name;
    p.hostUser = hostUser;
    p.users.push_back ({ hostUser, hostName });
    return p;
}

juce::var toVar (const Project& p)
{
    juce::Array<juce::var> users, takes, tracks;

    for (const auto& u : p.users)
        users.add (makeObject ({ { "id", u.id }, { "name", u.name } }));

    for (const auto& t : p.takes)
        takes.add (makeObject ({ { "id", t.id }, { "author", t.author }, { "channels", t.channels },
                                 { "segments", t.segments }, { "samples", i64 (t.samples) }, { "complete", t.complete } }));

    for (const auto& t : p.tracks)
        tracks.add (trackToVar (t));

    return makeObject ({ { "format", p.format }, { "id", p.id }, { "name", p.name }, { "hostUser", p.hostUser },
                         { "sampleRate", sampleRate }, { "bpm", p.bpm }, { "beatsPerBar", p.beatsPerBar },
                         { "version", i64 (p.version) }, { "users", users }, { "takes", takes }, { "tracks", tracks } });
}

std::optional<Project> projectFromVar (const juce::var& v, juce::String& error)
{
    Reader r (v);
    Project p;
    p.format = (int) r.integer ("format");
    p.id = r.id ("id");
    p.name = r.str ("name");
    p.hostUser = r.id ("hostUser");
    const auto rate = r.integer ("sampleRate");
    p.bpm = r.number ("bpm");
    p.beatsPerBar = (int) r.integer ("beatsPerBar");
    p.version = r.integer ("version");

    if (r.ok && p.format != 1)  { error = "unsupported format"; return std::nullopt; }
    if (r.ok && rate != sampleRate)  { error = "unsupported sample rate"; return std::nullopt; }

    if (auto* users = r.array ("users"))
        for (const auto& uv : *users)
        {
            Reader u (uv);
            p.users.push_back ({ u.id ("id"), u.str ("name") });

            if (! u.ok)  { error = "user: " + u.error; return std::nullopt; }
        }

    if (auto* takes = r.array ("takes"))
        for (const auto& tv : *takes)
        {
            Reader t (tv);
            Take take;
            take.id = t.id ("id");
            take.author = t.id ("author");
            take.channels = (int) t.integer ("channels");
            take.segments = (int) t.integer ("segments");
            take.samples = t.integer ("samples");
            take.complete = t.boolean ("complete");

            if (! t.ok)  { error = "take: " + t.error; return std::nullopt; }

            p.takes.push_back (take);
        }

    if (auto* tracks = r.array ("tracks"))
        for (const auto& tv : *tracks)
        {
            Reader t (tv);
            Track track;
            track.id = t.id ("id");
            track.name = t.str ("name");
            track.author = t.id ("author");
            const auto kind = trackKindFromString (t.str ("kind"));
            track.gainDb = (float) t.number ("gainDb");
            track.pan = (float) t.number ("pan");
            track.muted = t.boolean ("muted");

            if (t.ok && ! kind)  { error = "track: unknown kind"; return std::nullopt; }

            if (auto* regions = t.array ("regions"))
                for (const auto& rv : *regions)
                {
                    Reader g (rv);
                    Region reg;
                    reg.id = g.id ("id");
                    reg.take = g.id ("take");
                    reg.start = g.integer ("start");
                    reg.offset = g.integer ("offset");
                    reg.length = g.integer ("length");

                    if (! g.ok)  { error = "region: " + g.error; return std::nullopt; }

                    track.regions.push_back (reg);
                }

            if (! t.ok)  { error = "track: " + t.error; return std::nullopt; }

            track.kind = *kind;
            p.tracks.push_back (std::move (track));
        }

    if (! r.ok)
    {
        error = r.error;
        return std::nullopt;
    }

    return p;
}

juce::String toJson (const Project& p)   { return juce::JSON::toString (toVar (p), false); }

std::optional<Project> projectFromJson (const juce::String& json, juce::String& error)
{
    juce::var parsed;
    const auto result = juce::JSON::parse (json, parsed);

    if (result.failed())
    {
        error = "invalid JSON: " + result.getErrorMessage();
        return std::nullopt;
    }

    return projectFromVar (parsed, error);
}

juce::String contentHash (const Project& p)
{
    const auto text = juce::JSON::toString (toVar (p), true).toStdString();
    return Sha1::hexOf (text.data(), text.size());
}

bool canEditTrack (const Project& p, const Track& t, const Id& by) noexcept
{
    return by == t.author || by == p.hostUser;
}

OpResult applyOp (Project& p, const juce::var& op, const Id& by)
{
    Project scratch = p;
    const auto result = apply (scratch, op, by);

    if (result.ok)
    {
        ++scratch.version;
        p = std::move (scratch);
    }

    return result;
}

namespace Ops
{
    juce::var addTrack (const Id& track, const juce::String& name, TrackKind kind)
    {
        return makeObject ({ { "op", "add_track" }, { "track", track }, { "name", name }, { "kind", trackKindToString (kind) } });
    }

    juce::var renameTrack (const Id& track, const juce::String& name)
    {
        return makeObject ({ { "op", "rename_track" }, { "track", track }, { "name", name } });
    }

    juce::var deleteTrack (const Id& track)   { return makeObject ({ { "op", "delete_track" }, { "track", track } }); }

    juce::var setMix (const Id& track, std::optional<float> gainDb, std::optional<float> pan, std::optional<bool> muted)
    {
        auto op = makeObject ({ { "op", "set_mix" }, { "track", track } });
        auto* o = op.getDynamicObject();

        if (gainDb)  o->setProperty ("gainDb", (double) *gainDb);
        if (pan)     o->setProperty ("pan", (double) *pan);
        if (muted)   o->setProperty ("muted", *muted);

        return op;
    }

    juce::var addRegion (const Id& track, const Region& r)
    {
        return makeObject ({ { "op", "add_region" }, { "track", track }, { "region", r.id }, { "take", r.take },
                             { "start", i64 (r.start) }, { "offset", i64 (r.offset) }, { "length", i64 (r.length) } });
    }

    juce::var splitRegion (const Id& track, const Id& region, int64_t at, const Id& newRegion)
    {
        return makeObject ({ { "op", "split_region" }, { "track", track }, { "region", region },
                             { "at", i64 (at) }, { "newRegion", newRegion } });
    }

    juce::var deleteRegion (const Id& track, const Id& region)
    {
        return makeObject ({ { "op", "delete_region" }, { "track", track }, { "region", region } });
    }

    juce::var moveRegion (const Id& track, const Id& region, int64_t start)
    {
        return makeObject ({ { "op", "move_region" }, { "track", track }, { "region", region }, { "start", i64 (start) } });
    }

    juce::var trimRegion (const Id& track, const Id& region, int64_t start, int64_t offset, int64_t length)
    {
        return makeObject ({ { "op", "trim_region" }, { "track", track }, { "region", region },
                             { "start", i64 (start) }, { "offset", i64 (offset) }, { "length", i64 (length) } });
    }

    juce::var setTempo (double bpm, int beatsPerBar)
    {
        return makeObject ({ { "op", "set_tempo" }, { "bpm", bpm }, { "beatsPerBar", beatsPerBar } });
    }

    juce::var renameProject (const juce::String& name)   { return makeObject ({ { "op", "rename_project" }, { "name", name } }); }

    juce::var addUser (const Id& user, const juce::String& name)
    {
        return makeObject ({ { "op", "add_user" }, { "user", user }, { "name", name } });
    }

    juce::var addTake (const Id& take, int channels)
    {
        return makeObject ({ { "op", "add_take" }, { "take", take }, { "channels", channels } });
    }

    juce::var updateTake (const Id& take, int segments, int64_t samples, bool complete)
    {
        return makeObject ({ { "op", "update_take" }, { "take", take }, { "segments", segments },
                             { "samples", i64 (samples) }, { "complete", complete } });
    }
}
}
