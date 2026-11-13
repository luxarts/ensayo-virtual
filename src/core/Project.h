#pragma once
#include "core/Protocol.h"
#include <juce_core/juce_core.h>
#include <optional>
#include <vector>
namespace ensayo
{
using Id = juce::String;               // 32 lowercase hex chars (juce::Uuid().toString())
Id newId();
bool isValidId (const Id&) noexcept;

enum class TrackKind { voice, instrument, imported };
juce::String trackKindToString (TrackKind);                 // "voice" | "instrument" | "imported"
std::optional<TrackKind> trackKindFromString (const juce::String&);

struct Region { Id id, take; int64_t start = 0, offset = 0, length = 0;
                int64_t end() const noexcept { return start + length; } };
struct Track  { Id id; juce::String name; Id author; TrackKind kind = TrackKind::voice;
                float gainDb = 0.0f, pan = 0.0f; bool muted = false; std::vector<Region> regions; };   // regions bottom → top
struct Take   { Id id, author; int channels = 1; int segments = 0; int64_t samples = 0; bool complete = false; };
struct User   { Id id; juce::String name; };

struct Project
{
    int format = 1; Id id; juce::String name; Id hostUser;
    double bpm = 120.0; int beatsPerBar = 4; int64_t version = 0;
    std::vector<User> users; std::vector<Take> takes; std::vector<Track> tracks;

    const Track* findTrack (const Id&) const noexcept;  Track* findTrack (const Id&) noexcept;
    const Take*  findTake  (const Id&) const noexcept;  Take*  findTake  (const Id&) noexcept;
    juce::String userName (const Id&) const;            // "" when unknown
    int64_t endSample() const noexcept;                 // max region end over all tracks, 0 when empty
};

Project makeProject (const juce::String& name, const Id& hostUser, const juce::String& hostName);   // new id, version 0

juce::var toVar (const Project&);
std::optional<Project> projectFromVar (const juce::var&, juce::String& error);
juce::String toJson (const Project&);                                   // pretty JSON (the project.json format, spec §4)
std::optional<Project> projectFromJson (const juce::String&, juce::String& error);
juce::String contentHash (const Project&);                              // SHA-1 hex of the compact canonical JSON

bool canEditTrack (const Project&, const Track&, const Id& by) noexcept;   // author or hostUser

// A user-supplied track or project name made acceptable to applyOp's length rule: trimmed, at most 64 characters
// (juce::String characters, so a multibyte character is never cut in half), no trailing space after the cut.
// Can be empty: the caller supplies a fallback.
juce::String clampName (const juce::String&);

struct OpResult { bool ok = false; juce::String error; };
// Validates `op` (an object with "op") for user `by`, applies it and increments version on success.
// Unknown op, missing/invalid field, unknown id, broken invariant or missing permission → ok=false, project untouched.
OpResult applyOp (Project&, const juce::var& op, const Id& by);

namespace Ops
{
    juce::var addTrack (const Id& track, const juce::String& name, TrackKind);          // author = sender
    juce::var renameTrack (const Id& track, const juce::String& name);
    juce::var deleteTrack (const Id& track);
    juce::var setMix (const Id& track, std::optional<float> gainDb, std::optional<float> pan, std::optional<bool> muted);
    juce::var addRegion (const Id& track, const Region&);
    juce::var splitRegion (const Id& track, const Id& region, int64_t at, const Id& newRegion);
    juce::var deleteRegion (const Id& track, const Id& region);
    juce::var moveRegion (const Id& track, const Id& region, int64_t start);
    juce::var trimRegion (const Id& track, const Id& region, int64_t start, int64_t offset, int64_t length);
    juce::var setTempo (double bpm, int beatsPerBar);
    juce::var renameProject (const juce::String& name);
    juce::var addUser (const Id& user, const juce::String& name);                         // host-internal
    juce::var addTake (const Id& take, int channels);                                     // author = sender
    juce::var updateTake (const Id& take, int segments, int64_t samples, bool complete);  // host-internal
}
}
