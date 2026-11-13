# Grabación multipista colaborativa — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use nashe:subagent-driven-development (recommended) or nashe:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn Ensayo Virtual from live jamming into a collaborative multitrack recorder: the host's PC keeps the project, everyone plays and records locally against the timeline, takes stream to the others in lossless 1 s FLAC segments while they are recorded, and edits are operations the host validates, orders and broadcasts.

**Architecture:** Pure core units (`core/Project`, `core/Segments`, `core/Render`) carry the model, the segment codec and the click-free renderer. The host side (`server/ProjectHost`) stores the project and relays control, segments and the optional live preview. The client side (`client/TakeCache`, `client/ProjectClient`, `client/Recorder`, `client/StudioEngine`, `client/Offline`) keeps a replica, caches takes in RAM, records, plays, imports and exports. `ui/DawView` is the new session screen; `MainComponent` wires everything. Old live-mix code is removed in the last tasks.

**Tech Stack:** C++20, JUCE 8.0.17 (juce_audio_*, juce_gui_*), libopus 1.6.1 (custom modes, live preview only), FLAC via JUCE, Windows ASIO, MSVC 2022, CMake ≥ 3.22, JUCE UnitTest (category "EnsayoVirtual"), PowerShell 5.1 integration scripts.

**Spec:** `docs/specs/2026-11-13-grabacion-multipista-design.md` (read it first; this plan argues from it).

## Global Constraints

- Sample rate 48000 everywhere (`ensayo::sampleRate` in `core/Protocol.h`); the device must run at 48 kHz, any ASIO buffer size (64…2048).
- Timeline and take positions are `int64_t` samples at 48 kHz.
- Takes are immutable; regions are non-destructive (`start`, `offset`, `length`).
- Segment = 48000 samples per channel (the last one of a take may be shorter), FLAC 24-bit, 48 kHz, 1 or 2 channels.
- Audio callback: no allocation, no locks that can block, no file or socket calls. Hand-offs use lock-free queues or atomics.
- Edge fade 144 samples (3 ms) half-cosine; gain/pan ramps 960 samples (20 ms) linear; transport fade 240 samples (5 ms); pan law constant power (−3 dB centre); gain range [−60, +12] dB; pan [−1, +1]; bpm [30, 300]; beatsPerBar [1, 12].
- Ids are `juce::Uuid().toDashedString()`-free lowercase 32-hex strings: `juce::Uuid().toString()`.
- Control channel: one JSON object per line with `"t"`; `protocol` = 2. Data channel: same framing (JSON line + raw bytes).
- UI text in Spanish via `ES()` (`ui/Text.h`) for any non-ASCII literal; code, comments and commit messages in English.
- Commits: message names the change only — no `Co-Authored-By`, no "Generated with", no tool names.
- Build/test from PowerShell (Git Bash turns `/m` into a path):
  `cmake --build build --config Debug --target EnsayoTests -- /m` then `.\build\EnsayoTests_artefacts\Debug\EnsayoTests.exe <SuiteName>`.
- Each task adds its sources to `CMakeLists.txt` under its own `# Tn` comment inside the existing lists (`ENSAYO_CORE_SOURCES`, `ENSAYO_SERVER_SOURCES`, `ENSAYO_UI_SOURCES`, `ENSAYO_TEST_SOURCES`) so parallel branches merge with trivial conflicts.
- Tests that bind ports use the range 22200–22260; each test file uses its own sub-range noted in its task.

## File map

| File | Task | Responsibility |
|---|---|---|
| `src/core/Project.h/.cpp` | T1 | Model, JSON, ops, permissions |
| `src/core/Segments.h/.cpp` | T2 | FLAC 24 segment codec, peaks |
| `src/client/TakeCache.h/.cpp` | T2 | RAM chunked takes (lock-free reads), disk segments, peaks |
| `src/core/Render.h/.cpp` | T3 | Click-free timeline renderer, metronome |
| `src/server/ProjectHost.h/.cpp` | T4 | Host: project on disk, control/data TCP, UDP preview relay |
| `src/client/ProjectClient.h/.cpp` | T5 | Client networking, replica, upload queue, downloads |
| `src/client/Offline.h/.cpp` | T6 | Import (decode + resample) and export (render to WAV 24 TPDF) |
| `src/client/Recorder.h/.cpp`, `src/client/LatencyCalibrator.h/.cpp` | T7 | Capture → writer thread → segments/peaks/preview; calibration |
| `src/client/StudioEngine.h/.cpp`, `src/client/FakeAudioDevice.*` (loopback mode) | T8 | Device callback: monitor, playback, record, metronome, preview, calibration |
| `src/ui/DawView.h/.cpp` (+ `TrackHeader`, `TimelineView`, `TransportBar` inside) | T9 | Session screen |
| `src/ui/MainComponent.*`, `src/ui/LobbyView.*`, `src/client/Settings.*`, `src/Main.cpp`, `src/server/ServerMain.cpp`, `src/client/HeadlessClient.*` | T10 | Wiring, lobby additions, settings, headless v2, server exe; delete old live code |
| `tests/integration/*`, `tests/integration/expected-controls.json`, `src/ui/Screenshot.cpp`, `README.md`, `docs/plans/2026-11-13-verification.md` | T11 | End-to-end scripts, screenshots, docs |

Execution phases (tasks in a phase are independent and run in parallel, each in its own worktree cut from the same base commit; the coordinator merges them before the next phase):

- **Phase A:** T1, T2, T3
- **Phase B:** T4, T5, T6, T7
- **Phase C:** T8, T9
- **Phase D:** T10, then T11

---

### Task 1: Project model, JSON and operations

**Files:**
- Create: `src/core/Project.h`, `src/core/Project.cpp`
- Test: `tests/ProjectTests.cpp`
- Modify: `CMakeLists.txt` (`# T1` entries in `ENSAYO_CORE_SOURCES` and `ENSAYO_TEST_SOURCES`)

**Interfaces:**
- Consumes: `ensayo::sampleRate`, `minFaderDb`, `maxFaderDb` (`core/Protocol.h`); `ensayo::Sha1::hexOf`/`of` (`core/Sha1.h`).
- Produces (verbatim header):

```cpp
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
```

Op rules (exact, from spec §4):
- `add_track`: new id must be valid and unused; name trimmed, 1–64 chars; author = `by`. Anyone.
- `rename_track`, `delete_track`, `set_mix`: `canEditTrack`. `set_mix` clamps nothing — out-of-range values are rejected.
- `add_region`: `canEditTrack`; take exists; region id valid and unused in the project; `start ≥ 0`, `offset ≥ 0`, `length ≥ 1`, `offset + length ≤ take.samples`. Appended on top.
- `split_region`: `canEditTrack`; `region.start < at < region.end()`; `newRegion` valid and unused. Left keeps id with `length = at − start`; right = `{newRegion, take, at, offset + (at − start), end − at}` inserted right above the left one.
- `delete_region`: `canEditTrack`; region exists in that track.
- `move_region`: `canEditTrack`; `start ≥ 0`; region moves to the top of the stack.
- `trim_region`: `canEditTrack`; new `start ≥ 0`, `offset ≥ 0`, `length ≥ 1`, `offset + length ≤ take.samples`, and `start − offset` must equal the old `start − offset` (trim keeps the audio anchored); moves to top.
- `set_tempo`, `rename_project`: `by == hostUser`.
- `add_user`: `by == hostUser`; inserts or renames.
- `add_take`: valid unused id, channels 1 or 2, author = `by`, `segments = 0`, `samples = 0`, `complete = false`.
- `update_take`: `by == take.author` or `by == hostUser`; `segments`/`samples` may only grow; `complete` may only go false → true.

- [ ] **Step 1: Write the failing tests** in `tests/ProjectTests.cpp` (suite `juce::UnitTest ("Project", "EnsayoVirtual")`). Required cases, each its own `beginTest`:
  1. `newId` is 32 lowercase hex and unique over 1000 calls; `isValidId ("xyz")` false.
  2. JSON round trip: build a project with 2 users, 2 takes, 2 tracks × 2 regions, `toJson` → `projectFromJson` → `contentHash` equal; a missing `tracks` field → nullopt with non-empty error.
  3. `add_track` by any user sets author = sender; duplicate id rejected; empty name rejected.
  4. Permissions: user B cannot `rename_track`/`set_mix`/`delete_region` on A's track (`ok == false`, version unchanged); host can; author can.
  5. `add_region` invariants: `offset + length > take.samples` rejected; unknown take rejected; valid one appended last.
  6. `split_region` at 1000 on region {start 500, offset 100, length 2000}: left {500, 100, 500}, right {1000, 600, 1500}, right directly above left; `at == start` and `at == end` rejected.
  7. `move_region` and `trim_region` move the region to the top; trim that changes `start − offset` rejected; trim beyond take rejected.
  8. `set_tempo` by non-host rejected; bpm 29 and 301 rejected; 92/4 accepted.
  9. `update_take` cannot shrink segments, cannot un-complete, only author or host.
  10. Version increments by exactly 1 per accepted op and not at all on rejection.

```cpp
// Example (case 6):
auto p = makeProject ("T", host, "Lucas");
applyOp (p, Ops::addTake (take, 1), host);
applyOp (p, Ops::updateTake (take, 1, 48000, true), host);
applyOp (p, Ops::addTrack (track, "Gtr", TrackKind::voice), host);
applyOp (p, Ops::addRegion (track, { region, take, 500, 100, 2000 }), host);
const auto right = newId();
expect (applyOp (p, Ops::splitRegion (track, region, 1000, right), host).ok);
const auto& rs = p.findTrack (track)->regions;
expectEquals ((int) rs.size(), 2);
expect (rs[0].id == region && rs[0].start == 500 && rs[0].offset == 100 && rs[0].length == 500);
expect (rs[1].id == right && rs[1].start == 1000 && rs[1].offset == 600 && rs[1].length == 1500);
```

- [ ] **Step 2:** Build and run `EnsayoTests.exe Project` — expect compile failure (no `core/Project.h`).
- [ ] **Step 3:** Implement `Project.h/.cpp` exactly as the header above; JSON keys as in spec §4 (`format`, `id`, `name`, `hostUser`, `sampleRate`, `bpm`, `beatsPerBar`, `version`, `users`, `takes`, `tracks`; region keys `id`, `take`, `start`, `offset`, `length`); int64 values stored as `juce::int64` vars. `contentHash` hashes `juce::JSON::toString (toVar (p), true)` with `Sha1::hexOf`.
- [ ] **Step 4:** Run `EnsayoTests.exe Project` — all pass; run the whole suite — still green.
- [ ] **Step 5:** Commit `Add the multitrack project model with validated operations`.

---

### Task 2: Segment codec, peaks and take cache

**Files:**
- Create: `src/core/Segments.h/.cpp`, `src/client/TakeCache.h/.cpp`
- Test: `tests/SegmentsTests.cpp`, `tests/TakeCacheTests.cpp`
- Modify: `CMakeLists.txt` (`# T2`)

**Interfaces:**
- Consumes: `Id` (T1 header, include `core/Project.h`; T1 runs in parallel — use the header text from Task 1 verbatim; if the file is not on your base yet, create only `using Id = juce::String;` in a local `#include` guard is NOT allowed — instead include `core/Project.h` and copy Task 1's header into your branch unchanged; the coordinator keeps T1's version on merge).
- Produces:

```cpp
// src/core/Segments.h
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
namespace ensayo
{
constexpr int segmentSamples = 48000;
constexpr int peakBlock = 256;
struct Peak { float min = 0.0f, max = 0.0f; };

// FLAC, 24 bit, 48 kHz, audio.getNumChannels() channels (1 or 2), the first numSamples samples.
// Samples are clamped to [-1, 1). Returns an empty block on failure.
juce::MemoryBlock encodeSegment (const juce::AudioBuffer<float>& audio, int numSamples);
// Resizes `out` to channels × samples. False on anything that is not a 48 kHz FLAC of 1–2 channels.
bool decodeSegment (const void* data, size_t size, juce::AudioBuffer<float>& out);
// Appends one Peak per peakBlock samples (the last may cover fewer), min/max across all channels.
void appendPeaks (const juce::AudioBuffer<float>& audio, int numSamples, std::vector<Peak>& out);
// What a sample becomes after a round trip through 24-bit FLAC (for exact comparisons in tests).
float quantize24 (float sample) noexcept;
}
```

```cpp
// src/client/TakeCache.h
#pragma once
#include "core/Project.h"
#include "core/Segments.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
namespace ensayo
{
// Read side used by the renderer (audio thread).
class SampleSource
{
public:
    virtual ~SampleSource() = default;
    virtual int channels (const Id& take) const noexcept = 0;                       // 0 = unknown take
    // Copies samples [offset, offset + n) of `channel` (clamped to the take's last channel) into dest.
    // Samples not received yet, or outside the take, are written as 0. Never blocks, never allocates.
    virtual void read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept = 0;
};

class TakeCache : public SampleSource
{
public:
    static constexpr int maxChunks = 4 * 3600;   // 4 h of 1 s chunks per take (pointer table preallocated)

    explicit TakeCache (juce::File directory);  // <cache>/<project>/takes ; created if missing
    ~TakeCache() override;

    // Loads every take found on disk (segments n.flac in order, stops at the first gap). Message thread.
    void loadFromDisk();
    // Any non-audio thread, serialised internally. `index` must equal segmentCount(take) or the call is ignored
    // and returns false. Decodes, appends to RAM, writes <dir>/<take>/<index>.flac, extends peaks.
    bool appendSegment (const Id& take, int index, const void* flac, size_t size);
    int segmentCount (const Id& take) const;
    int64_t sampleCount (const Id& take) const;
    std::vector<Peak> peaks (const Id& take) const;
    juce::MemoryBlock segmentBytes (const Id& take, int index) const;   // from disk; empty if missing
    std::vector<Id> takes() const;

    int channels (const Id& take) const noexcept override;
    void read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept override;

    std::function<void (const Id& take)> onTakeChanged;   // after each append, on the appending thread

private:
    struct TakeData;                                       // chunk pointer table (atomic), channel count, sample count
    std::shared_ptr<TakeData> find (const Id&) const noexcept;
    juce::File dir;
    mutable std::mutex writeLock;                          // appenders only; readers never take it
    std::atomic<std::shared_ptr<const std::map<Id, std::shared_ptr<TakeData>>>> index;   // copy-on-write
};
}
```

Implementation notes (required behaviour):
- `TakeData` holds, per channel, `std::unique_ptr<std::atomic<float*>[]>` of `maxChunks` entries; each chunk is a `float[segmentSamples]` allocated by the appender; `std::atomic<int64_t> samples` is published with `memory_order_release` **after** the chunk data and pointers are written. `read` loads `samples` with acquire and never reads beyond it.
- Takes never shrink; chunks are freed only in the destructor.
- The `index` map is copy-on-write: appenders build a new map and `store` it; `read` does one `load`.
- Segments shorter than `segmentSamples` are allowed only as the last one: after a short segment, further appends for that take return false.

- [ ] **Step 1: Failing tests.** `tests/SegmentsTests.cpp` (`"Segments"`):
  1. Mono 48000-sample sine (0.8 amplitude): `decodeSegment (encodeSegment (x))` equals `quantize24 (x[i])` exactly for every sample.
  2. Stereo, two different sines, 30000 samples (short segment) round trip exact; channel count 2.
  3. Three consecutive segments of a continuous sine decoded and concatenated equal the quantized continuous sine (no seam: the sample at each join equals the quantized original).
  4. `decodeSegment` on garbage bytes → false; on a 44.1 kHz FLAC (write one with `juce::FlacAudioFormat`) → false.
  5. `appendPeaks` on 1000 samples → 4 peaks; values equal min/max of each 256 block.

  `tests/TakeCacheTests.cpp` (`"TakeCache"`, temp dir `%TEMP%\EnsayoVirtual\tests\takecache`):
  1. Append 3 segments of a mono ramp; `sampleCount` = 144000; `read` of [47990, 48010) crosses the chunk boundary and equals the quantized ramp; read beyond the end writes zeros.
  2. Out-of-order append (index 2 when count is 1) → false, nothing changes.
  3. After a short segment, another append → false.
  4. `loadFromDisk` in a new `TakeCache` on the same dir restores segment count, samples and peaks.
  5. Concurrency: a thread appends 60 segments while another thread calls `read` 100000 times on random positions ≤ the sample count it saw before the call; no crash, every read value equals the expected ramp value or 0 beyond the observed count.
  6. Stereo take: `read` of channel 5 clamps to channel 1.
- [ ] **Step 2:** Build/run `EnsayoTests.exe Segments` and `TakeCache` — compile failure.
- [ ] **Step 3:** Implement. FLAC writing: `juce::FlacAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (48000).withNumChannels (ch).withBitsPerSample (24))` on a `juce::MemoryOutputStream`; reading with `createReaderFor (new juce::MemoryInputStream (data, size, false), true)`.
- [ ] **Step 4:** Both suites pass; full suite green.
- [ ] **Step 5:** Commit `Add lossless take segments and a lock-free take cache`.

---

### Task 3: Click-free renderer and metronome

**Files:**
- Create: `src/core/Render.h/.cpp`
- Test: `tests/RenderTests.cpp`
- Modify: `CMakeLists.txt` (`# T3`)

**Interfaces:**
- Consumes: `Project`, `Track`, `Region`, `Take`, `Id` (T1); `SampleSource` (T2 — declare nothing yourself: include `client/TakeCache.h`; until T2 merges, copy T2's header verbatim into your branch).
- Produces:

```cpp
#pragma once
#include "core/Project.h"
#include "client/TakeCache.h"
#include <memory>
#include <set>
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
    void prepare (int maxBlockSize);                                     // allocates scratch; message thread
    void setProject (std::shared_ptr<const Project>);                     // message thread; picked up atomically
    void setSolo (std::set<Id> soloTracks);                               // message thread
    // Audio thread. Overwrites outL/outR with the mix of [position, position + n). Negative positions render
    // silence for the part before 0. Gain/pan/mute/solo changes ramp over rampSamples.
    void render (const SampleSource&, int64_t position, int n, float* outL, float* outR) noexcept;
    // Audio thread: jump all ramps to their targets (after a seek or at play start, where the transport fade hides it).
    void snapRamps() noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl;
};

// Metronome: adds clicks for [position, position + n) to outL/outR. Bar downbeat 1 kHz, other beats 800 Hz,
// 20 ms with a 1 ms attack and exponential decay, amplitude 0.5.
void renderMetronome (double bpm, int beatsPerBar, int64_t position, int n, float* outL, float* outR) noexcept;
}
```

Rendering algorithm (required): for each track (skip when muted/not soloed — via ramped gain, not by skipping, so muting ramps), walk regions **top to bottom** with a per-sample coverage `c` starting at 1: for each sample inside a region, `g = regionEdgeGain(...)` (fadeIn = previous-in-time region on this track is not seamless with it; fadeOut likewise), `out += c * g * sample`, then `c *= (1 − g)`. Stop a sample's walk when `c == 0`. Mono takes go to both sides before panning; stereo takes use `panGains` as a balance (left × l/0.7071, right × r/0.7071, each capped at 1). Track gain = `dbToGain (gainDb)` (`core/MixMath.h`) ramped linearly over `rampSamples` toward the target.

- [ ] **Step 1: Failing tests** in `tests/RenderTests.cpp` (`"Render"`), using a test `SampleSource` that serves a 440 Hz sine (amplitude 0.5) per take. Helper `maxStep (buffer)` = max |x[i] − x[i−1]|; the bound for "no click" is `maxStep(pure sine) + 1e-3` (pure sine 440 Hz at 48 kHz, amp 0.5: ≈ 0.0288).
  1. One region over a whole take, rendered in blocks of 64, 128, 333 and 4096: identical output for every block size (renderer is block-size independent).
  2. Split at 1.234 s keeping both halves: output equals the unsplit render sample for sample (seamless join).
  3. Delete the middle third: `maxStep` ≤ bound across the whole render; the samples at the two cut points ramp to 0 within 144 samples.
  4. Move a region 5000 samples later: no step above the bound at its new start and end.
  5. Trim the start by 777 samples: no step above the bound.
  6. Overlap: a second region on top over [1 s, 2 s): `maxStep` ≤ bound; in the middle of the overlap only the top region is audible (output equals top region's samples × track gain).
  7. `set_mix` gain −60 → 0 dB while playing: step ≤ bound (ramped); a block right after the change is not at the target yet, the block after 960 samples is.
  8. Mute and solo ramp the same way; `snapRamps` jumps immediately.
  9. `panGains (0)` = (0.7071, 0.7071) ± 1e-4; `panGains (-1)` = (1, 0).
  10. Metronome at 120 BPM 4/4: clicks start at samples 0, 24000, 48000…; the first of each bar is 1 kHz (zero-crossing count in 20 ms ≈ 40), others 800 Hz (≈ 32).
  11. Negative position −1000 with n = 2000: first 1000 output samples are 0.
  12. Unknown take (source returns channels 0): renders silence, no crash.
- [ ] **Step 2:** Run `EnsayoTests.exe Render` — compile failure.
- [ ] **Step 3:** Implement. Scratch buffers sized in `prepare`; the project snapshot is held as `std::shared_ptr<const Project>` swapped with `std::atomic_store`/`atomic_load` (C++20 `std::atomic<std::shared_ptr>` is acceptable); per-track ramp state lives in `Impl` keyed by track id in a fixed `std::vector` rebuilt only on the message thread when the project changes (the audio thread picks the rebuilt vector together with the snapshot).
- [ ] **Step 4:** Suite passes; full suite green.
- [ ] **Step 5:** Commit `Add a click-free timeline renderer and metronome`.

---

### Task 4: Project host (server side)

**Files:**
- Create: `src/server/ProjectHost.h/.cpp`
- Test: `tests/ProjectHostTests.cpp` (ports 22200–22219)
- Modify: `CMakeLists.txt` (`# T4`, server sources)

**Interfaces:**
- Consumes: T1 (`Project`, `applyOp`, `Ops`, `toJson`, `projectFromJson`), T2 (`decodeSegment` only to validate the channel count and sample count of an upload), existing `makeLine`/`parseLine`/`makeMessage`, `generateSessionId`, `Sha1`, `decodeAudioPacket`.
- Produces:

```cpp
#pragma once
#include "core/Project.h"
#include <functional>
#include <memory>
namespace ensayo
{
constexpr int protocolVersionV2 = 2;

struct HostConfig
{
    int port = defaultPort;            // TCP (control + data) and UDP (preview relay)
    int maxClients = 8;
};

juce::File defaultProjectsDirectory();                                   // %APPDATA%\EnsayoVirtual\projects
juce::Array<juce::File> listProjects (const juce::File& projectsDirectory);   // dirs with project.json, newest first
juce::Result saveProjectAtomically (const Project&, const juce::File& projectJson);   // temp file + replace

class ProjectHost
{
public:
    explicit ProjectHost (HostConfig);
    ~ProjectHost();
    // Opens projectDir/project.json, or creates a new project there named newProjectName when missing.
    // Registers the host user, picks a session id, binds TCP + UDP and starts the threads.
    juce::Result start (const juce::File& projectDir, const Id& hostUser, const juce::String& hostName,
                        const juce::String& newProjectName);
    void stop();
    juce::String getSessionId() const;
    int getPort() const noexcept;
    std::shared_ptr<const Project> snapshot() const;
    std::function<void (const juce::String&)> onLog;   // any thread
};
}
```

Protocol (both T4 and T5 implement exactly this):

Control connection (client opens first):
| Direction | Message |
|---|---|
| C→H | `{"t":"hello","protocol":2,"session":"7KXQ2M","user":"<id>","name":"Lucas"}` |
| H→C | `{"t":"welcome","clientId":12345,"token":"<16 hex>","udpPort":22140,"isHost":true,"project":{…project.json…}}` or `{"t":"error","message":"…"}` + close (`versión incompatible`, `sesión no encontrada`, `sesión llena`) |
| C→H | `{"t":"op","seq":7,"op":{…}}` |
| H→all | `{"t":"applied","version":129,"by":"<id>","op":{…}}` (the op as applied, including for the sender) |
| H→C | `{"t":"rejected","seq":7,"reason":"…"}` |
| H→all | `{"t":"presence","users":[{"id":"…","name":"…","online":true,"recording":{"take":"…","track":"…","start":192000}}]}` on any join/leave/rec change |
| C→H→others | `{"t":"rec_start","take":"…","track":"…","start":192000}`, `{"t":"rec_peaks","take":"…","from":0,"peaks":[[-0.1,0.2],…]}`, `{"t":"rec_stop","take":"…"}` (host adds `"user"` when relaying) |
| C→H | `{"t":"take_complete","take":"…"}` → host applies `update_take(complete=true)` with the take's current counts |
| C→H | `{"t":"listen","enabled":true}` |
| C→H / H→C | `{"t":"ping","ms":…}` / `{"t":"pong","ms":…}` |
| C→H | `{"t":"resync"}` → H→C `{"t":"project","project":{…}}` |

Data connection (second TCP to the same port, opened after `welcome`):
| Direction | Message |
|---|---|
| C→H | `{"t":"data_hello","clientId":12345,"token":"…","have":{"<take>":3,…}}` |
| H→C | for every take, segments `have[take] … take.segments-1` as `{"t":"segment","take":"…","index":3,"samples":48000,"channels":1,"bytes":N,"sha1":"…"}` + N bytes; afterwards every new segment as soon as it is stored (never echoed to its uploader) |
| C→H | `{"t":"segment",…}` + bytes (upload) |
| H→C | `{"t":"segment_ok","take":"…","index":3}` or `{"t":"segment_bad","take":"…","index":3,"expected":2,"reason":"…"}` |

Host rules: an upload is accepted only from the take's author, with `index == take.segments`, SHA-1 matching, decodable with the take's channel count; stored at `<projectDir>/takes/<take>/<index>.flac`; then the host applies `update_take(segments+1, samples+n, false)` as the author (broadcast as `applied`) and pushes the segment to every other data connection. `project.json` is saved with `saveProjectAtomically` after every applied op. UDP: datagrams decoded with `decodeAudioPacket`; the sender is identified by `clientId` (endpoint learned like today); each datagram is forwarded unchanged to every other client whose `listen` is on and whose UDP endpoint is known (learned from any datagram they sent: clients send a 1-byte keepalive `0x00` every second while listening). Host user = the user id given to `start`; that user is `isHost` in `welcome`.

- [ ] **Step 1: Failing tests** (`"ProjectHost"`), each against a host on 127.0.0.1 with a temp project dir, using raw `juce::StreamingSocket` clients:
  1. `start` creates `project.json` with the host user; `listProjects` finds it; a second `ProjectHost` reopening the dir sees the same project id.
  2. `hello` with protocol 1 → `error` "versión incompatible"; wrong session → "sesión no encontrada".
  3. Two clients: A sends `op add_track`; both A and B receive `applied` with version 1; B sends `rename_track` on A's track → `rejected` to B only.
  4. Upload: A `add_take`, data_hello, uploads 3 segments (FLAC from `encodeSegment`); gets 3 `segment_ok`; B (data connection opened before) receives the 3 segments live in order and 3 `applied update_take`; the files exist on disk.
  5. Out-of-order (index 5) → `segment_bad` with `expected`; bad SHA-1 → `segment_bad`; upload by a non-author → `segment_bad`.
  6. Late joiner C with `have {}` receives the 3 stored segments on its data connection.
  7. `take_complete` → `applied update_take complete:true`.
  8. Crash safety: after 20 ops, kill (`stop`) and reopen: version 20 and same `contentHash`.
  9. UDP relay: B listening sends keepalive; A sends a preview datagram; B receives it byte-identical; C not listening receives nothing.
  10. `presence` lists both users online; after B disconnects, B is `online:false`.
- [ ] **Step 2:** Run `EnsayoTests.exe ProjectHost` — compile failure.
- [ ] **Step 3:** Implement: one accept thread, one reader thread per connection, one writer queue per connection (reuse the `writeAll` with a 2 s stall timeout pattern from `ServerHost.cpp`), lines capped at 4 MB for `welcome`/`project` and 8 KB otherwise; segment bytes capped at 2 MB. Ops applied under one mutex, broadcast in apply order.
- [ ] **Step 4:** Suite passes; full suite green.
- [ ] **Step 5:** Commit `Add the project host: ordered ops, segment store and preview relay`.

---

### Task 5: Project client

**Files:**
- Create: `src/client/ProjectClient.h/.cpp`
- Test: `tests/ProjectClientTests.cpp` (ports 22220–22239; uses `ProjectHost` from T4 — until it merges, test against a minimal fake host in the test file implementing the protocol table above)
- Modify: `CMakeLists.txt` (`# T5`)

**Interfaces:**
- Consumes: T1, T2 (`TakeCache`, `encodeSegment`), protocol table of Task 4, `SessionLink` (`client/SessionLink.h`), `JitterBuffer`.
- Produces:

```cpp
#pragma once
#include "core/Project.h"
#include "core/JitterBuffer.h"
#include "client/TakeCache.h"
#include "client/SessionLink.h"
#include <functional>
#include <memory>
namespace ensayo
{
struct ClientConfig { juce::File cacheRoot; Id user; juce::String userName; };   // cacheRoot = %LOCALAPPDATA%\EnsayoVirtual\cache
juce::File defaultCacheRoot();

struct LiveRecording { Id user, take, track; int64_t start = 0; std::vector<Peak> peaks; };
struct Presence { Id user; juce::String name; bool online = false; };
struct Transfers { int pendingUploads = 0; int downloadingTakes = 0; };

class ProjectClient
{
public:
    explicit ProjectClient (ClientConfig);
    ~ProjectClient();
    // Message thread. Async: onConnected after `welcome` and the data connection are up; onError otherwise.
    void connect (const SessionLink&);
    void disconnect();
    bool isConnected() const noexcept;
    bool isHost() const noexcept;
    const Id& user() const noexcept;
    std::shared_ptr<const Project> project() const;          // latest replica (message thread)
    TakeCache* cache() noexcept;                              // created at `welcome`, per project id; nullptr before
    void sendOp (const juce::var& op);                         // queued; result arrives as onProjectChanged or onRejected
    // Recording plumbing (message thread unless noted)
    void beginTake (const Id& take, int channels, const Id& track, int64_t timelineStart);   // add_take op + rec_start
    void sendPeaks (const Id& take, int64_t fromPeak, const std::vector<Peak>&);           // any thread
    // Any thread: appends to the local cache right away, persists to <cacheRoot>/pending/<take>/<index>.flac,
    // uploads in order; survives disconnects (resent on the next connect, starting at the host's take.segments).
    void queueSegment (const Id& take, int index, const juce::MemoryBlock& flac);
    // When every segment of `take` is acknowledged: take_complete, then add_region `region` on `track`, then rec_stop.
    void finishTake (const Id& take, const Id& track, const Region& region);
    void setListening (bool);
    void sendPreview (const uint8_t* packet, size_t len) noexcept;   // any thread (UDP)
    JitterBuffer* previewBuffer() noexcept;                           // mix-free: datagrams from whoever records
    std::vector<Presence> presence() const;
    std::vector<LiveRecording> liveRecordings() const;
    Transfers transfers() const;
    int pingMs() const noexcept;

    std::function<void()> onConnected, onProjectChanged, onPresenceChanged, onTransfersChanged;
    std::function<void (const juce::String&)> onRejected, onError, onDisconnected;   // message thread
};
}
```

Behaviour: replica = the `welcome` project, then each `applied` op re-applied locally with `applyOp (replica, op, by)` (must succeed; if it does not, or `version` skips, send `resync` and replace the replica with the received `project`). Downloads: every segment received on the data connection goes to `cache()->appendSegment`; `Transfers::downloadingTakes` = takes whose cached segment count < project take segment count. Preview: incoming UDP datagrams pushed into a `JitterBuffer (codedBytesStereo (FrameSize::f128), 128.0 / 48.0)` (mono packets are stored the same way; StudioEngine decodes by flags). Keepalive byte every second while listening.

- [ ] **Step 1: Failing tests** (`"ProjectClient"`), host = `ProjectHost` (or the fake host before T4 merges):
  1. Connect A (host user) and B; `onConnected` fires; `project()` equals the host snapshot (`contentHash`).
  2. A `sendOp (add_track)` → both replicas get version 1, same hash; B renaming A's track → B's `onRejected`, replicas unchanged.
  3. B records: `beginTake`, 3 × `queueSegment`, `finishTake` with region {start 0, offset 0, length = total}; A's cache ends with 3 segments and A's replica has the region and `complete:true`.
  4. Live: A sees `liveRecordings()` with B's take while peaks arrive, empty after `rec_stop`.
  5. Disconnect B after 2 of 4 segments are queued (close its sockets from the test), reconnect: the host ends with 4 segments, no duplicates, `pending` dir empty.
  6. Cache reuse: B disconnects, a new `ProjectClient` for B connects with the same cacheRoot: `have` reports the cached segments and nothing is downloaded again (count bytes received on the data socket via a test hook `bytesDownloaded()` — add it as a public method for tests).
  7. Version gap: inject a skipped `applied` (fake host) → client sends `resync` and ends equal to the host.
- [ ] **Step 2:** Run `EnsayoTests.exe ProjectClient` — compile failure.
- [ ] **Step 3:** Implement with the thread structure of `SessionClient` (control reader, writer thread, data reader, data writer, UDP thread); all callbacks posted to the message thread with a generation counter (as `SessionClient::post`).
- [ ] **Step 4:** Suite passes; full suite green.
- [ ] **Step 5:** Commit `Add the project client: replica, progressive uploads and cached downloads`.

---

### Task 6: Import and export

**Files:**
- Create: `src/client/Offline.h/.cpp`
- Test: `tests/OfflineTests.cpp`
- Modify: `CMakeLists.txt` (`# T6`)

**Interfaces:**
- Consumes: T2 (`encodeSegment`, `segmentSamples`, `SampleSource`), T3 (`Renderer`), `MediaFoundationAudioFormat` (`server/MediaFoundationAudioFormat.h`).
- Produces:

```cpp
#pragma once
#include "core/Render.h"
#include <functional>
namespace ensayo
{
// Decodes wav/flac/ogg/mp3 (JUCE) and m4a/mp4/aac (Media Foundation), keeps 1 or 2 channels (more → first two),
// resamples to 48 kHz with juce::WindowedSincInterpolator when needed. Fails on > 4 h.
juce::Result loadAndResample (const juce::File&, juce::AudioBuffer<float>& out);
// Cuts audio into segmentSamples-long FLAC segments (last one shorter).
std::vector<juce::MemoryBlock> makeSegments (const juce::AudioBuffer<float>& audio);
// Renders [0, project.endSample()) with a fresh Renderer (no solo, no metronome) in 4096-sample blocks and writes
// a 24-bit stereo WAV at 48 kHz with TPDF dither (±1 LSB triangular, juce::Random seeded 1). progress(0..1) may
// return false to cancel (file deleted).
juce::Result exportMix (std::shared_ptr<const Project>, const SampleSource&, const juce::File& wav,
                        std::function<bool (float)> progress = {});
}
```

- [ ] **Step 1: Failing tests** (`"Offline"`):
  1. A 44.1 kHz stereo WAV of a 1 kHz sine (2 s) → `loadAndResample` gives 96000 samples × 2 channels; the resampled sine's frequency (zero crossings) is 1000 ± 1 Hz and its RMS within 0.1 dB of the source.
  2. 48 kHz mono WAV → no resampling, samples identical.
  3. `makeSegments` of 130000 samples → 3 segments of 48000, 48000, 34000 (decoded lengths).
  4. `exportMix` of a project with one region over a test source (sine) equals `Renderer::render` of the same range within 2 LSB at 24 bits; file is 24-bit, 48 kHz, 2 channels; length = `endSample()`.
  5. Cancel at 50 % deletes the file and returns a failed result.
- [ ] **Step 2–4:** run (fail), implement, run (pass), full suite green.
- [ ] **Step 5:** Commit `Add audio import with resampling and mix export to 24-bit WAV`.

---

### Task 7: Recorder and latency calibrator

**Files:**
- Create: `src/client/Recorder.h/.cpp`, `src/client/LatencyCalibrator.h/.cpp`
- Test: `tests/RecorderTests.cpp`, `tests/LatencyCalibratorTests.cpp`
- Modify: `CMakeLists.txt` (`# T7`)

**Interfaces:**
- Consumes: T2 (`encodeSegment`, `appendPeaks`, `segmentSamples`, `Peak`), T1 (`Id`).
- Produces:

```cpp
// src/client/Recorder.h
#pragma once
#include "core/Project.h"
#include "core/Segments.h"
#include <atomic>
#include <functional>
#include <memory>
namespace ensayo
{
// Samples to drop from the start of a take so its first kept sample is what was played over the record point:
// count-in + output latency + input latency + calibration (never negative).
int64_t recordingOffset (int64_t countInSamples, int inputLatency, int outputLatency, int calibration) noexcept;

class Recorder
{
public:
    Recorder();
    ~Recorder();
    // Message thread. channels 1 or 2; maxBlockSize sizes nothing on the audio thread: the FIFO holds 8 s.
    // localWav receives the full take as 32-bit float (crash-safe copy, flushed every segment).
    juce::Result start (const Id& take, int channels, const juce::File& localWav);
    void push (const float* const* channels, int n) noexcept;      // audio thread; drops and counts on overflow
    void stop();                                                    // message thread; flushes, joins, fires onFinished
    bool isRecording() const noexcept;
    uint32_t overflows() const noexcept;
    int64_t samplesCaptured() const noexcept;

    // Writer thread callbacks:
    std::function<void (int index, const juce::MemoryBlock& flac, int samples)> onSegment;
    std::function<void (int64_t fromPeak, const std::vector<Peak>&)> onPeaks;          // about every 100 ms
    std::function<void (const float* const* channels, int numChannels, int n)> onLiveAudio;   // every chunk drained
    std::function<void (int segments, int64_t samples)> onFinished;
    std::function<void (const juce::String&)> onError;                                  // write failure; recording stops
};
}
```

```cpp
// src/client/LatencyCalibrator.h
#pragma once
#include <atomic>
#include <optional>
#include <vector>
namespace ensayo
{
class LatencyCalibrator
{
public:
    static constexpr int clicks = 8, periodSamples = 24000;   // a click every 0.5 s
    void start() noexcept;                                     // message thread
    // Audio thread: writes the click train to out (both channels), scans `in` for each click (threshold 0.1 after
    // 20 ms of quiet), records the round trip in samples. Done after `clicks` clicks or 6 s.
    void process (const float* in, float* outL, float* outR, int n) noexcept;
    bool isRunning() const noexcept;
    std::optional<int> roundTrip() const;                     // median; nullopt if fewer than 5 clicks detected
    static int calibrationFor (int roundTrip, int inputLatency, int outputLatency) noexcept
        { return roundTrip - inputLatency - outputLatency; }
};
}
```

- [ ] **Step 1: Failing tests.** `"Recorder"`: (1) push 3.5 s of a mono ramp in blocks of 256 from a test thread; after `stop`, `onSegment` fired 4 times with 48000, 48000, 48000, 24000 samples whose decoded concatenation equals the quantized ramp; `onFinished (4, 168000)`; `localWav` has 168000 float samples equal to the ramp exactly. (2) stereo works. (3) overflow: push 10 s instantly without letting the writer run (a test hook `Recorder::setWriterPausedForTest(bool)`) → `overflows() > 0`, no crash, captured count excludes dropped samples. (4) `onPeaks` total covers ceil(168000/256) peaks. (5) `recordingOffset (96000, 200, 264, 12) == 96476`; negative calibration that would go below 0 clamps to 0.
  `"LatencyCalibrator"`: simulate a loopback that delays output by 517 samples and adds noise at −60 dB, processing blocks of 128: `roundTrip() == 517 ± 1`; with no loopback (silence in) → `nullopt` after 6 s.
- [ ] **Step 2–4:** run (fail), implement (writer thread drains a `juce::AbstractFifo`-backed float ring, cuts segments, writes the WAV with `juce::WavAudioFormat` 32-bit float), run (pass), full suite green.
- [ ] **Step 5:** Commit `Add the take recorder and the loopback latency calibrator`.

---

### Task 8: Studio engine (device callback)

**Files:**
- Create: `src/client/StudioEngine.h/.cpp`
- Modify: `src/client/FakeAudioDevice.h/.cpp` (loopback mode), `CMakeLists.txt` (`# T8`)
- Test: `tests/StudioEngineTests.cpp`

**Interfaces:**
- Consumes: T3 `Renderer`, `renderMetronome`, `transportFadeSamples`; T2 `SampleSource`; T7 `Recorder`, `recordingOffset`, `LatencyCalibrator`; existing `PluginSlot`, `FakeAudioDevice`, `AsioDeviceManager` (move it from `AudioEngine.h` into `StudioEngine.h` unchanged in T10; in T8 declare `class StudioDeviceManager` with the same body to avoid touching `AudioEngine.h`), `OpusMode/OpusEncoder/OpusDecoder`, `encodeAudioPacket`, `JitterBuffer`.
- Produces:

```cpp
// FakeAudioDevice additions (FakeAudioDevice.h)
enum class TestSignal { silence, sine440, click, loopback };   // loopback: input = output delayed by the latencies below
struct FakeLatency { int input = 0, output = 0; };              // reported AND applied in loopback mode
// FakeAudioDeviceType (int blockSize, TestSignal, FakeLatency = {}) and FakeAudioDevice (int blockSize, TestSignal, FakeLatency = {})
```

```cpp
// src/client/StudioEngine.h
#pragma once
#include "core/Render.h"
#include "client/Recorder.h"
#include "client/LatencyCalibrator.h"
#include "client/PluginSlot.h"
#include "client/FakeAudioDevice.h"
#include "core/JitterBuffer.h"
#include <juce_audio_devices/juce_audio_devices.h>
namespace ensayo
{
class StudioDeviceManager : public juce::AudioDeviceManager
{
public:
    void createAudioDeviceTypes (juce::OwnedArray<juce::AudioIODeviceType>&) override;   // ASIO only
};

struct EngineMeters { float voice = 0, instrument = 0, output = 0; };
struct RecordingResult { Id take; int channels = 1; int segments = 0; int64_t samples = 0; Region region; };

class StudioEngine : private juce::AudioIODeviceCallback
{
public:
    StudioEngine();
    ~StudioEngine() override;
    juce::AudioDeviceManager& getDeviceManager() noexcept;
    juce::StringArray getAsioDeviceNames();
    juce::String openDevice (const juce::String& type, const juce::String& name, double rate, int bufferSize);   // "" ok
    void useFakeDevice (int blockSize, TestSignal, FakeLatency = {});
    FakeAudioDevice* getFakeDevice() const noexcept;
    PluginSlot& getPluginSlot() noexcept;
    juce::String takeDeviceError();

    void setChannels (int mic, int instrument, int outputPair) noexcept;
    void setMonitor (float voiceGain, float instrumentGain) noexcept;      // own inputs, 0 ms
    void setCalibration (int samples) noexcept;
    void setMetronome (bool click, bool countIn) noexcept;

    void setSampleSource (const SampleSource*) noexcept;                     // the project's TakeCache or nullptr
    void setProject (std::shared_ptr<const Project>);
    void setSolo (std::set<Id>);

    // Transport (message thread). Position in timeline samples.
    void play();
    void stop();
    void setPosition (int64_t);
    int64_t getPosition() const noexcept;
    bool isPlaying() const noexcept;

    // Records the inputs that match `track.kind` (voice: mic channel, mono; instrument: plugin output, stereo) from the
    // current position. Starts playback beatsPerBar beats earlier when count-in is on. Segments, peaks and live audio
    // come out through the Recorder callbacks set by the caller on getRecorder() before calling this.
    juce::Result startRecording (const Track&, const Id& take, const juce::File& localWav);
    // Stops the take and playback; the result's region is aligned with recordingOffset and starts at the record point.
    std::optional<RecordingResult> stopRecording();
    bool isRecording() const noexcept;
    Recorder& getRecorder() noexcept;

    void setPreviewBuffer (JitterBuffer*) noexcept;    // live listening; nullptr = off
    // Encodes the recorder's live audio into 128-sample Opus frames and passes datagrams here (any thread).
    std::function<void (const uint8_t* packet, size_t len)> onPreviewPacket;

    void startCalibration();
    bool isCalibrating() const noexcept;
    std::optional<int> calibrationRoundTrip() const;

    EngineMeters takeMeters() noexcept;
    uint32_t getCallbackCount() const noexcept;
};
}
```

Callback order (required): read inputs → plugin on instrument → own monitor → if calibrating: calibrator only → if playing: `renderer.render` at position (+ metronome when click on, or during the count-in when count-in on) with the transport fade on start/stop → if recording: push the selected inputs to the recorder (from the very first sample of playback, including the count-in) → preview decode/add → meters → position += n.

- [ ] **Step 1: Failing tests** (`"StudioEngine"`), all on the fake device:
  1. Loopback alignment: fake device block 256, `FakeLatency {200, 264}`, calibration 0. Project with an imported track whose take is a click every 24000 samples (load into a `TakeCache` in a temp dir via `appendSegment`). Record a voice track with count-in at 120 BPM from position 48000 for 3 s (mic channel 0 = loopback of output L). The result's region `start == 48000`, `offset == recordingOffset (96000, 200, 264, 0)`; the recorded take's samples at `offset + (24000·k − 48000)` contain the clicks: every detected click in the region lies within ±1 sample of a reference click position on the timeline.
  2. Same with block sizes 64 and 1024 → same alignment (±1).
  3. Calibration with `FakeLatency {200, 264}` → `calibrationRoundTrip() == 464 ± 1`.
  4. Play/stop produce no step above `maxStep(sine)+1e-3` at the output (sine take, probe on the fake output).
  5. Metronome sounds only in the output, never in the recorded take (record with click on and a non-loopback silent input: recorded take is all zeros).
  6. `stopRecording` while the writer is still draining returns only after `onFinished`; `RecordingResult.samples` equals the sum of segment samples.
- [ ] **Step 2–4:** run (fail), implement, run (pass), full suite green.
- [ ] **Step 5:** Commit `Add the studio engine with aligned recording, metronome and calibration`.

---

### Task 9: DAW view

**Files:**
- Create: `src/ui/DawView.h/.cpp`
- Test: `tests/DawViewTests.cpp`
- Modify: `CMakeLists.txt` (`# T9`, UI sources)

**Interfaces:**
- Consumes: T1 (`Project`, `Ops`, `newId`, `canEditTrack`), T2 (`Peak`), `ES`, `formatTime` (`ui/Text.h`), `LevelMeter`.
- Produces:

```cpp
#pragma once
#include "core/Project.h"
#include "core/Segments.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <set>
namespace ensayo
{
struct LiveRegion { Id user, take, track; int64_t start = 0; std::vector<Peak> peaks; };
struct DawState
{
    std::shared_ptr<const Project> project; Id me; bool isHost = false; bool readOnly = false;
    juce::String link;
    std::map<Id, bool> online;                 // user → online
    std::vector<LiveRegion> live;              // takes being recorded right now (others or me)
    std::map<Id, float> takeAvailability;      // take → downloaded fraction; missing = 1
    int64_t playhead = 0; bool playing = false, recording = false;
    Id armedTrack; std::set<Id> solo; bool click = true, countIn = true, listen = false;
};
class PeaksProvider { public: virtual ~PeaksProvider() = default; virtual std::vector<Peak> peaksFor (const Id& take) const = 0; };

class DawView : public juce::Component
{
public:
    DawView();
    ~DawView() override;
    void setState (const DawState&);
    const DawState& getState() const noexcept;
    void setPeaksProvider (const PeaksProvider*);   // not owned
    void resized() override; void paint (juce::Graphics&) override;

    std::function<void (const juce::var& op)> onOp;                      // edits as Ops::… with fresh ids
    std::function<void()> onPlay, onStop, onRecord, onHome, onCopyLink, onLeave, onExport, onToggleEditor,
                          onImportFile, onAddVoice, onAddInstrument;
    std::function<void (int64_t)> onSeek;
    std::function<void (const Id&)> onArm;
    std::function<void (const Id&, bool)> onSolo;
    std::function<void (bool)> onClick, onCountIn, onListen;
    std::function<void (const juce::String& url)> onImportYouTube;

    // For tests and keyboard handling
    double getSamplesPerPixel() const noexcept;  void setSamplesPerPixel (double);
    int sampleToX (int64_t) const noexcept;      int64_t xToSample (int x) const noexcept;
    Id getSelectedRegion() const;                void selectRegion (const Id&);
    bool splitSelectedAtPlayhead();              // emits split_region when the playhead is inside the selection
    bool deleteSelected();                       // emits delete_region
    void dragRegion (const Id& region, int dx);  // emits move_region (start clamped at 0)
    void dragEdge (const Id& region, bool left, int dx);   // emits trim_region respecting take bounds
};
}
```

Control names (component `setName`, used by screenshots and tests): `daw.project`, `daw.link`, `daw.copy`, `daw.users`, `daw.listen`, `daw.editor`, `daw.leave`, `transport.home`, `transport.play`, `transport.stop`, `transport.record`, `transport.time`, `transport.bpm`, `transport.meter`, `transport.click`, `transport.countin`, `edit.split`, `edit.delete`, `daw.export`, per track `track.N.color|name|author|kind|mute|solo|arm|lock|volume|pan` (N from 1 in track order; `arm` visible only on editable tracks, `lock` only on the others), `timeline`, `tracks.addVoice`, `tracks.addInstrument`, `tracks.import`, `tracks.youtube`.

Behaviour: mute/volume/pan controls are enabled only when `canEditTrack`; their changes emit `set_mix` (volume/pan on drag end, mute immediately); BPM/compás editable only by the host (emit `set_tempo`); renaming a track (double click on the name) emits `rename_track`; regions of takes with availability < 1 draw grey with the percentage; live regions draw red growing from their peaks; the playhead is a 2 px line; keyboard S = split, Delete = delete, Space = play/stop, R = record.

- [ ] **Step 1: Failing tests** (`"DawView"`): (1) with a 3-track project (2 mine, 1 other's), every control name exists, visible, non-empty bounds, and `track.3.arm` is hidden while `track.3.lock` is visible; mute/volume of track 3 disabled for a non-host, enabled for the host. (2) `selectRegion` + playhead inside + `splitSelectedAtPlayhead` emits a `split_region` op with `at == playhead`. (3) `dragRegion (r, 50)` at 1000 samples/px emits `move_region` with `start + 50000`; a drag to negative clamps to 0. (4) `dragEdge (r, true, -10)` beyond the take start emits a trim clamped at offset 0. (5) Clicking mute emits `set_mix {muted:true}` for that track. (6) `sampleToX (xToSample (x)) == x` over the visible range. (7) Render into an image (1100×760) without crashing for empty, 1-track and 12-track projects.
- [ ] **Step 2–4:** run (fail), implement, run (pass), full suite green.
- [ ] **Step 5:** Commit `Add the DAW session view`.

---

### Task 10: Wiring, lobby, settings, headless v2, removal of live mixing

**Files:**
- Modify: `src/ui/MainComponent.h/.cpp`, `src/ui/LobbyView.h/.cpp`, `src/client/Settings.h/.cpp`, `src/Main.cpp`, `src/server/ServerMain.cpp`, `src/client/HeadlessClient.h/.cpp`, `src/ui/Screenshot.cpp`, `CMakeLists.txt`, `tests/UiTests.cpp`, `tests/HeadlessClientTests.cpp`, `tests/SettingsTests.cpp`, `tests/ScreenshotTests.cpp`, `tests/integration/expected-controls.json`
- Delete: `src/server/MixServer.*`, `src/server/BackingTrack.*`, `src/server/ServerHost.*`, `src/client/SessionClient.*`, `src/client/AudioEngine.*`, `src/ui/SessionView.*`, `src/ui/AudioDiagnosis.*`, `src/core/MixMath.*` only if unused (Render uses `dbToGain`: keep), their tests (`MixServerTests`, `BackingTrackTests`, `ServerHostTests`, `SessionClientTests`, `AudioEngineTests`, `AudioDiagnosisTests`), and `tests/integration/{two-instances,embedded-host,backing-track,packet-loss,udp-blocked}.ps1`.

**Interfaces:**
- Consumes: everything above.
- Produces:
  - `AppSettings` gains `juce::String userId` (generated with `newId()` on first load and saved), `std::map<juce::String,int> calibration` keyed `"<device>@<buffer>"`, `juce::String lastProject` (dir path); `bufferSize` default 256.
  - Lobby gains `lobby.open` "Abrir proyecto…", `lobby.calibrate` "Calibrar latencia", `lobby.calibration` label ("Calibración: N muestras" / "sin calibrar"); `lobby.create` creates a new project under `defaultProjectsDirectory()` named "Proyecto <fecha>" (or opens the one chosen with Abrir) and hosts it with `ProjectHost` on `settings.hostPort`, then connects its own `ProjectClient` to `127.0.0.1`; `lobby.join` connects a `ProjectClient` to the link.
  - Session = `DawView`; MainComponent maps `ProjectClient` + `StudioEngine` state to `DawState` every 50 ms timer tick; `onOp` → `ProjectClient::sendOp`; record → `ProjectClient::beginTake` + `StudioEngine::startRecording` with recorder callbacks wired to `queueSegment`, `sendPeaks`, `sendPreview`; stop → `stopRecording` → `finishTake`; import file/YouTube → `loadAndResample` → new take (`beginTake`, `queueSegment` for each of `makeSegments`, `finishTake` with region at 0) on a new `imported` track; export → `FileChooser` + `exportMix`.
  - Status bar in session: `status.text`, `status.sync` ("Sincronizado (v N)" / "Sólo lectura: host desconectado"), `status.ping`, `status.transfers` ("Subidas: n · Descargas: m"), `status.device`, `status.cpu`, `status.retry`.
  - `EnsayoServer.exe --project <dir> [--port N] [--name <project name>] [--user <id>] [--user-name <name>]` hosts a project headless (prints `listening port=N session=ID`, exits on stdin EOF as today).
  - Headless client v2 (`--headless`): `--host <projectDir> [--port N]` or `--join <link>`; `--name N`; `--user <id>`; `--fake-audio silence|sine|click|loopback --block N --latency-in N --latency-out N`; `--add-track voice|instrument <name>`; `--record <trackName> --at <seconds> --for <seconds>` (count-in off); `--import <file>`; `--ops <jsonl>` (each line an op object, sent in order); `--wait-takes N` (until N complete takes are cached); `--export <wav>`; `--dump-project <file>` (project.json at exit); `--duration S`. Stdout lines: `link …`, `welcome <session> host|guest`, `applied <version>`, `rejected <reason>`, `take_complete <take> <samples>`, `cached <take> <segments>`, `project_hash <sha1>` (at exit), `error …`.
- [ ] **Step 1:** Update the tests listed above for the new screens and options (Ui: lobby has the 3 new controls; session view is `DawView` with all its names; Settings: userId persisted and stable across loads, calibration map round trip; Headless: option parsing for every new flag and rejection of the removed ones such as `--simulate-loss`).
- [ ] **Step 2:** Run — failures.
- [ ] **Step 3:** Implement the wiring; delete the old files and their CMake entries; keep `JitterBuffer`, `OpusCodec`, `Protocol` (packet format for the preview).
- [ ] **Step 4:** Full suite green; Release build of `EnsayoVirtual`, `EnsayoServer`, `EnsayoTests` with no warnings.
- [ ] **Step 5:** Commit `Switch the app to collaborative multitrack recording and remove live mixing`.

---

### Task 11: End-to-end scripts, screenshots and docs

**Files:**
- Create: `tests/integration/record-sync.ps1` (22240), `tests/integration/edit-sync.ps1` (22241), `tests/integration/reconnect.ps1` (22242), `tests/integration/reopen.ps1` (22243), `tests/integration/import-export.ps1` (22244), `tests/integration/asio-calibration.ps1` (22245), `docs/plans/2026-11-13-verification.md`
- Modify: `tests/integration/run-all.ps1`, `tests/integration/README.md`, `tests/integration/screenshots.ps1`, `tests/integration/common.ps1` (server start with `--project`), `README.md`

Scripts (each prints PASS/FAIL/SKIP like today):
- `record-sync`: `EnsayoServer --project <tmp>`; A (host user) adds an imported click track (`--import click.wav`); B joins with `--fake-audio loopback --latency-in 200 --latency-out 264 --block 256`, adds a voice track and records 4 s at 1.0 s; A `--wait-takes 2`. Assert: A printed `cached <take> k` lines growing during B's recording (at least 3 distinct k before B's `take_complete`), both `project_hash` equal, and the clicks in B's region align with the click track within ±1 sample (A runs `--export` and the script reads the WAV: every click of the recorded track coincides with a reference click ±1 sample — export with the imported track muted vs unmuted and compare onset positions).
- `edit-sync`: A, B, C; B's ops file (add track, add region on its take, split, move, trim, set_mix) and a forbidden op on A's track; all three `project_hash` equal; B printed one `rejected`.
- `reconnect`: B records 6 s; the script kills B's process after 3 s and restarts it with the same `--user` and cache; the host ends with the complete take (all segments, no duplicates) and `complete:true` once B sends `take_complete` after reconnecting (B is started the second time with `--resume`, which only flushes pending uploads and finishes the take).
- `reopen`: host process stops and restarts on the same project dir; a client reconnects and gets the same `project_hash`.
- `import-export`: import a 44.1 kHz file and a YouTube URL (SKIP the YouTube part when tools are missing or the download fails), export the mix; WAV is 24-bit 48 kHz stereo with the expected length.
- `asio-calibration`: SKIP unless `MiniFuse ASIO Driver` is installed; headless with `--asio "MiniFuse ASIO Driver" --out 1 --mic 2` (LOOPBACK outputs) runs calibration, then records a click track through the loopback; recorded clicks align within ±48 samples (1 ms).
- `screenshots`: `--screenshot` renders the lobby and the DAW view (demo project with 4 tracks, one recording, one downloading) and checks every name in `expected-controls.json`; captures the real `--demo` window.
- [ ] **Step 1:** Write the scripts and the new expected controls.
- [ ] **Step 2:** Run `run-all.ps1` — iterate until all PASS (or SKIP for missing hardware/tools).
- [ ] **Step 3:** Rewrite README sections (what the app is now, recording/editing, calibration, troubleshooting) and write the verification report with numbers.
- [ ] **Step 4:** Commit `Add end-to-end tests for multitrack recording and update the docs`.

---

## Self-review

- Spec coverage: §1/§2 decisions → T1–T10; model §4 → T1; protocol §5 → T4/T5; render/fades/ramps/metronome §6 → T3; cache §6 → T2; recording/alignment/calibration §6 → T7/T8; import/export §6 → T6; live preview §5/§6 → T4/T5/T8; UI §7 → T9/T10; errors §8 → T4 (atomic save, segment validation), T5 (resync, pending queue), T7 (write errors), T10 (read-only status); tests §9 → each task + T11.
- No placeholders; every type used by a later task is declared in an earlier task's header (`SampleSource` T2, `Renderer` T3, `Recorder`/`recordingOffset` T7, `Peak` T2, `LiveRegion` T9 vs `LiveRecording` T5 — MainComponent converts between them in T10).
