# Ensayo Virtual Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use nashe:subagent-driven-development (recommended) or nashe:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Windows app (client + embeddable/headless server) for low-latency online band rehearsal: ASIO, two inputs, VST3 on the instrument, Opus over UDP with a personalised server mix, per-participant voice/instrument faders, local self-monitoring, and a server-injected synchronised backing track (MP3 file or YouTube audio via yt-dlp).

**Architecture:** One ASIO callback per client encodes voice + instrument (post-VST) as two mono Opus custom-mode frames in one UDP datagram and decodes one stereo mix coming back through a sequence-indexed jitter buffer, while monitoring itself locally. `MixServer` (pure, tick-driven) holds a session: per-participant jitter buffers, per-client gain matrix, the backing track, and produces one stereo Opus packet per client per tick. `ServerHost` wraps it with TCP control (JSON lines) + UDP audio and is driven either by the host client's ASIO callback (embedded) or a high-resolution timer thread (`EnsayoServer.exe`).

**Tech Stack:** C++20, JUCE 8.0.17-security (ASIO + VST3 bundled), libopus 1.6.1 (custom modes), CMake ≥ 3.22, MSVC 2022 (`Visual Studio 17 2022`, x64), JUCE `UnitTest`, PowerShell for integration scripts.

**Spec:** `docs/specs/2026-11-04-ensayo-virtual-design.md`

## Global Constraints

- Windows 11 x64 only. Generator `Visual Studio 17 2022`, `-A x64`. CMake ≥ 3.22.
- JUCE tag `8.0.17-security`; libopus tag `v1.6.1` with `OPUS_CUSTOM_MODES=ON`. Both are pre-cloned at `C:\Users\Lucas\Desktop\ensayo-virtual\.deps\JUCE` and `...\.deps\opus`; CMake uses them through `FETCHCONTENT_SOURCE_DIR_JUCE` / `FETCHCONTENT_SOURCE_DIR_OPUS` when those directories exist, otherwise fetches from GitHub.
- Compile definitions on every target: `JUCE_ASIO=1 JUCE_WASAPI=0 JUCE_DIRECTSOUND=0 JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1 CUSTOM_MODES=1`.
- All code in `namespace ensayo`. JUCE style: Allman braces, space before call parentheses, `juce::` prefix, no `using namespace`.
- Sample rate is always 48000. Frame sizes 64 or 128 samples. CBR coded bytes per frame: mono 64 → 24, 128 → 48; stereo 64 → 48, 128 → 96.
- UI strings in Spanish. Non-ASCII literals must go through `juce::CharPointer_UTF8` (helper `ES("...")` in `src/ui/Text.h`); never a bare `const char*` with accents.
- Faders are dB in [−60, +12], default 0, double-click resets to 0. Mute is a separate toggle.
- Settings file: `%APPDATA%\EnsayoVirtual\settings.json`. Temp uploads: `%TEMP%\EnsayoVirtual\`.
- Default ports: TCP 22140 and UDP 22140. Session IDs: 6 chars from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`.
- Nothing in the audio callback allocates, locks (except try-lock), logs or does syscalls.
- Commit messages name the change only: no `Co-Authored-By`, no "Generated with", no tool names.
- Every component gets `setName` with the identifiers listed in Task 10 so `--screenshot` can verify them.

### Commands (PowerShell, from the worktree root `C:\Users\Lucas\Desktop\ensayo-virtual\.worktrees\ensayo-virtual-core`)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFETCHCONTENT_SOURCE_DIR_JUCE="C:\Users\Lucas\Desktop\ensayo-virtual\.deps\JUCE" -DFETCHCONTENT_SOURCE_DIR_OPUS="C:\Users\Lucas\Desktop\ensayo-virtual\.deps\opus"
cmake --build build --config Debug --target EnsayoTests -- /m
.\build\EnsayoTests_artefacts\Debug\EnsayoTests.exe          # exit 0 = all pass
cmake --build build --config Release --target EnsayoVirtual EnsayoServer -- /m
.\build\EnsayoVirtual_artefacts\Release\EnsayoVirtual.exe
.\build\EnsayoServer_artefacts\Release\EnsayoServer.exe --port 22140
```

## File Structure

```
CMakeLists.txt                      JUCE + opus, three targets, source lists ENSAYO_CORE_SOURCES / ENSAYO_TEST_SOURCES
src/core/Protocol.h/.cpp            constants, Role, AudioPacket encode/decode, control JSON helpers        (Task 2)
src/core/MixMath.h/.cpp             dB↔gain, ChannelGain, mixing helpers                                     (Task 2)
src/core/OpusCodec.h/.cpp           OpusMode, OpusEncoder, OpusDecoder on custom modes                        (Task 3)
src/core/JitterBuffer.h/.cpp        sequence-indexed ring with auto size                                      (Task 3)
src/core/SpscQueue.h                fixed-size single-producer/single-consumer byte-packet queue              (Task 3)
src/server/BackingTrack.h/.cpp      file → 48 k stereo memory, transport, render                              (Task 4)
src/server/MixServer.h/.cpp         one session: participants, gains, jitter buffers, tick                    (Task 4)
src/server/ServerHost.h/.cpp        TCP control + UDP audio + sessions + clocks                               (Task 5)
src/server/HighResClock.h/.cpp      waitable-timer thread calling a callback every frame                      (Task 5)
src/server/ServerMain.cpp           EnsayoServer.exe                                                          (Task 5)
src/client/PluginSlot.h/.cpp        VST3 slot (ported from discord-share-guitar)                              (Task 6)
src/client/FakeAudioDevice.h/.cpp   juce::AudioIODevice driven by a thread; TestSignal, ClickProbe            (Task 6)
src/client/AudioEngine.h/.cpp       ASIO/fake device callback, monitor, encode/send, receive/decode           (Task 6)
src/client/Settings.h/.cpp          AppSettings JSON                                                          (Task 6)
src/client/SessionLink.h/.cpp       ensayo:// parse/format, URL scheme registration                           (Task 7)
src/client/SessionClient.h/.cpp     TCP control client + UDP audio socket + state events                      (Task 7)
src/client/YouTubeImporter.h/.cpp   yt-dlp runner                                                             (Task 7)
src/client/HeadlessClient.h/.cpp    --headless mode (fake audio, probe, latency log)                          (Task 8)
src/ui/Text.h                       ES() UTF-8 helper                                                         (Task 9)
src/ui/LevelMeter.h                 meter component                                                           (Task 9)
src/ui/LobbyView.h/.cpp             name, role, create/join, audio + VST panels                               (Task 9)
src/ui/SessionView.h/.cpp           link, me strip, participant strips, backing track, status                 (Task 9)
src/ui/MainComponent.h/.cpp         switches views, owns engine + client + optional embedded server           (Task 9)
src/ui/Screenshot.h/.cpp            --screenshot: render views + controls.json                                 (Task 10)
src/Main.cpp                        JUCEApplication: CLI modes, single instance, ensayo:// forwarding         (Task 9)
tests/TestMain.cpp                  runner, category "EnsayoVirtual"                                          (Task 1)
tests/*Tests.cpp                    one per unit                                                              (each task)
tests/fixtures/click.wav            1 s, 44.1 kHz stereo, click every 250 ms (generated by Task 4 test setup)
tests/integration/*.ps1             two-instance, embedded, backing track, loss, screenshots                  (Task 11)
tests/integration/expected-controls.json                                                                      (Task 10)
README.md                           build, run, setup, yt-dlp notes                                           (Task 12)
```

## Execution phases (parallelism)

- **Phase A (sequential):** Task 1 → Task 2 → Task 3. These define the contracts every other task consumes.
- **Phase B (parallel, disjoint files):** Task 4+5 (server), Task 6 (client audio), Task 7 (client session). Each agent appends only its own files to the CMake source lists (edit only the lines marked `# T4`, `# T6`, `# T7` to avoid merge conflicts — one agent per marker).
- **Phase C (parallel):** Task 8 (headless client), Task 9 (UI + Main), Task 10 (screenshot mode) — Task 10 depends on Task 9's component names, so Task 10 starts after Task 9's headers exist; Task 11 (integration scripts) in parallel with 9/10.
- **Phase D (sequential):** Task 12 run everything, fix, README, final review.

---

### Task 1: Scaffold — CMake, JUCE, libopus, three targets, test runner

**Files:**
- Create: `CMakeLists.txt`, `src/Main.cpp` (placeholder window), `src/server/ServerMain.cpp` (placeholder), `tests/TestMain.cpp`, `tests/BuildConfigTests.cpp`

**Interfaces:**
- Produces: CMake lists `ENSAYO_CORE_SOURCES`, `ENSAYO_TEST_SOURCES`, targets `EnsayoVirtual` (GUI), `EnsayoServer` (console), `EnsayoTests` (console); link target `Opus::opus`.

- [ ] **Step 1: `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.22)
project(EnsayoVirtual VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(ENSAYO_DEPS_DIR "${CMAKE_SOURCE_DIR}/../../.deps" CACHE PATH "Pre-cloned dependencies")
if(NOT FETCHCONTENT_SOURCE_DIR_JUCE AND EXISTS "${ENSAYO_DEPS_DIR}/JUCE/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_JUCE "${ENSAYO_DEPS_DIR}/JUCE")
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_OPUS AND EXISTS "${ENSAYO_DEPS_DIR}/opus/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_OPUS "${ENSAYO_DEPS_DIR}/opus")
endif()

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.17-security
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

set(OPUS_CUSTOM_MODES ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.6.1)
FetchContent_MakeAvailable(opus)

# Sources shared by the app, the server and the test runner. Each marker is owned by one task.
set(ENSAYO_CORE_SOURCES
    # T2
    # T3
    # T4
    # T6
    # T7
    # T8
    # T10
)
set(ENSAYO_SERVER_SOURCES
    # T5
)
set(ENSAYO_UI_SOURCES
    # T9
)
set(ENSAYO_TEST_SOURCES
    tests/TestMain.cpp
    tests/BuildConfigTests.cpp
    # T2-tests
    # T3-tests
    # T4-tests
    # T5-tests
    # T6-tests
    # T7-tests
    # T8-tests
    # T10-tests
)

set(ENSAYO_MODULES
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_devices
    juce::juce_audio_formats juce::juce_audio_basics juce::juce_gui_extra
    juce::juce_events juce::juce_core)

set(ENSAYO_DEFINITIONS
    JUCE_ASIO=1 JUCE_WASAPI=0 JUCE_DIRECTSOUND=0 JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1 CUSTOM_MODES=1 JUCE_DISPLAY_SPLASH_SCREEN=0)

set(ENSAYO_SYSTEM_LIBS ws2_32 avrt winmm)

juce_add_gui_app(EnsayoVirtual PRODUCT_NAME "Ensayo Virtual" COMPANY_NAME "EnsayoVirtual")
target_sources(EnsayoVirtual PRIVATE src/Main.cpp ${ENSAYO_UI_SOURCES} ${ENSAYO_SERVER_SOURCES} ${ENSAYO_CORE_SOURCES})
target_include_directories(EnsayoVirtual PRIVATE src)
target_compile_definitions(EnsayoVirtual PRIVATE ${ENSAYO_DEFINITIONS})
target_link_libraries(EnsayoVirtual PRIVATE ${ENSAYO_MODULES} Opus::opus ${ENSAYO_SYSTEM_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

juce_add_console_app(EnsayoServer PRODUCT_NAME "EnsayoServer")
target_sources(EnsayoServer PRIVATE src/server/ServerMain.cpp ${ENSAYO_SERVER_SOURCES} ${ENSAYO_CORE_SOURCES})
target_include_directories(EnsayoServer PRIVATE src)
target_compile_definitions(EnsayoServer PRIVATE ${ENSAYO_DEFINITIONS})
target_link_libraries(EnsayoServer PRIVATE ${ENSAYO_MODULES} Opus::opus ${ENSAYO_SYSTEM_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

juce_add_console_app(EnsayoTests PRODUCT_NAME "EnsayoTests")
target_sources(EnsayoTests PRIVATE ${ENSAYO_TEST_SOURCES} ${ENSAYO_UI_SOURCES} ${ENSAYO_SERVER_SOURCES} ${ENSAYO_CORE_SOURCES})
target_include_directories(EnsayoTests PRIVATE src)
target_compile_definitions(EnsayoTests PRIVATE ${ENSAYO_DEFINITIONS} ENSAYO_TESTS=1)
target_link_libraries(EnsayoTests PRIVATE ${ENSAYO_MODULES} Opus::opus ${ENSAYO_SYSTEM_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
```

Note: `ENSAYO_UI_SOURCES` is in the test target so `--screenshot` logic (Task 10) and UI construction can be unit-tested; UI files must therefore not depend on `JUCEApplication` being alive beyond a `ScopedJuceInitialiser_GUI`.

- [ ] **Step 2: `tests/TestMain.cpp`** — same as the reference project but category `"EnsayoVirtual"`; prints one line per result and `TOTAL`, exit 1 when zero results or any failure. Accept an optional argv[1] test-name filter (`runner.runTests ({ test })`) for faster loops.

- [ ] **Step 3: `tests/BuildConfigTests.cpp`** — asserts `JUCE_ASIO == 1`, `JUCE_PLUGINHOST_VST3 == 1`, `JUCE_USE_MP3AUDIOFORMAT == 1`, and that `opus_custom_mode_create (48000, 128, &err)` returns non-null (include `<opus_custom.h>`).

- [ ] **Step 4: placeholders** — `src/Main.cpp`: minimal `JUCEApplication` opening an empty 400×300 window titled "Ensayo Virtual"; `src/server/ServerMain.cpp`: `int main() { return 0; }`.

- [ ] **Step 5: configure + build + run tests** (commands above). Expected: `TOTAL: 1 results, 0 failures`.

- [ ] **Step 6: Commit** — `git add CMakeLists.txt src tests && git commit -m "Scaffold JUCE app, server and test runner with ASIO, VST3 and libopus"`.

---

### Task 2: Protocol and MixMath (pure)

**Files:**
- Create: `src/core/Protocol.h/.cpp`, `src/core/MixMath.h/.cpp`, `tests/ProtocolTests.cpp`, `tests/MixMathTests.cpp`
- Modify: `CMakeLists.txt` markers `# T2`, `# T2-tests`

**Interfaces — Produces (verbatim header `src/core/Protocol.h`):**

```cpp
#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace ensayo
{
constexpr int sampleRate = 48000;
constexpr uint8_t packetMagic = 0xEA;
constexpr uint8_t protocolVersion = 1;
constexpr size_t maxPacketBytes = 512;
constexpr int defaultPort = 22140;
constexpr float minFaderDb = -60.0f, maxFaderDb = 12.0f;

enum class FrameSize : uint8_t { f64 = 0, f128 = 1 };
constexpr int frameSamples (FrameSize f) noexcept           { return f == FrameSize::f64 ? 64 : 128; }
constexpr int codedBytesMono (FrameSize f) noexcept         { return f == FrameSize::f64 ? 24 : 48; }
constexpr int codedBytesStereo (FrameSize f) noexcept       { return f == FrameSize::f64 ? 48 : 96; }
std::optional<FrameSize> frameSizeForSamples (int samples) noexcept;   // 64 / 128 only

enum class Role { voice, instrument, both };
juce::String roleToString (Role);                 // "voice" | "instrument" | "both"
std::optional<Role> roleFromString (const juce::String&);
bool roleSendsVoice (Role) noexcept;
bool roleSendsInstrument (Role) noexcept;

namespace AudioFlags
{
    constexpr uint8_t voice = 1, instrument = 2, mix = 4;
}

// Wire layout (little-endian): magic u8, version u8, flags u8, frameCode u8, seq u16,
// clientId u32, sampleTime u32, trackPosition u32, lenVoice u16, lenInstrument u16, payloads.
constexpr size_t audioHeaderBytes = 20;

struct AudioPacketView
{
    uint8_t flags = 0;
    FrameSize frameSize = FrameSize::f128;
    uint16_t seq = 0;
    uint32_t clientId = 0;
    uint32_t sampleTime = 0;
    uint32_t trackPosition = 0;
    const uint8_t* voice = nullptr;      int voiceLen = 0;        // for mix packets: the stereo payload
    const uint8_t* instrument = nullptr; int instrumentLen = 0;
};

// Real-time safe. Returns bytes written, 0 if `out` is too small or lengths exceed 255.
size_t encodeAudioPacket (const AudioPacketView&, uint8_t* out, size_t capacity) noexcept;
// Real-time safe. Pointers in the result point into `data`. False on any malformed field.
bool decodeAudioPacket (const uint8_t* data, size_t len, AudioPacketView& out) noexcept;

// Control channel: one JSON object per line, field "t" = type.
juce::String makeLine (const juce::var& object);               // JSON + '\n', single line
std::optional<juce::var> parseLine (const juce::String& line); // nullopt unless an object with string "t"
juce::var makeMessage (const juce::String& type);              // {"t": type}
juce::String generateSessionId (juce::Random&);                // 6 chars of "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
bool isValidSessionId (const juce::String&);

struct TransportState
{
    bool loaded = false, playing = false, loop = false;
    int64_t position = 0, length = 0;   // samples @ 48 k
    juce::String name;
};
juce::var transportToVar (const TransportState&);
TransportState transportFromVar (const juce::var&);
}
```

**`src/core/MixMath.h`:**

```cpp
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
namespace ensayo
{
float dbToGain (float db) noexcept;                 // clamps to [minFaderDb, maxFaderDb]; -60 → 0 exactly
struct ChannelGain { float voiceDb = 0.0f, instrumentDb = 0.0f; bool muted = false;
                     float voiceGain() const noexcept; float instrumentGain() const noexcept; };  // 0 when muted
// out[L/R] += gain * mono (centred). Real-time safe.
void addMonoCentred (float* L, float* R, const float* mono, float gain, int n) noexcept;
void addStereo (float* L, float* R, const float* inL, const float* inR, float gain, int n) noexcept;
float peakOf (const float* data, int n) noexcept;
void interleave (const float* L, const float* R, float* out, int n) noexcept;
void deinterleave (const float* in, float* L, float* R, int n) noexcept;
}
```

- [ ] **Step 1: tests** — `ProtocolTests`: round-trip with both payloads; mix packet (`flags = mix`, instrumentLen 0); rejects wrong magic, wrong version, truncated header, declared length past end, frameCode 7; `encodeAudioPacket` returns 0 when capacity too small; `makeLine`/`parseLine` round trip and reject non-object / missing `"t"`; `generateSessionId` length 6 and only allowed chars; `roleFromString ("x")` empty. `MixMathTests`: `dbToGain (0) == 1`, `dbToGain (-60) == 0`, `dbToGain (20)` clamps to +12 dB, muted gain 0, `addMonoCentred` accumulates, `peakOf`.
- [ ] **Step 2: run, see them fail to compile; implement; run → pass.**
- [ ] **Step 3: Commit** — `"Add wire protocol, control message helpers and mix math"`.

---

### Task 3: OpusCodec, JitterBuffer, SpscQueue

**Files:**
- Create: `src/core/OpusCodec.h/.cpp`, `src/core/JitterBuffer.h/.cpp`, `src/core/SpscQueue.h`, `tests/OpusCodecTests.cpp`, `tests/JitterBufferTests.cpp`, `tests/SpscQueueTests.cpp`
- Modify: `CMakeLists.txt` markers `# T3`, `# T3-tests`

**Interfaces — Produces:**

```cpp
// src/core/OpusCodec.h
#pragma once
#include "Protocol.h"
#include <memory>
struct OpusCustomMode; struct OpusCustomEncoder; struct OpusCustomDecoder;
namespace ensayo
{
class OpusMode            // one per FrameSize, shared by all encoders/decoders of that size; outlives them
{
public:
    explicit OpusMode (FrameSize);
    ~OpusMode();
    OpusCustomMode* get() const noexcept { return mode; }
    FrameSize frameSize() const noexcept { return size; }
private:
    OpusCustomMode* mode = nullptr; FrameSize size;
};

class OpusEncoder         // CBR; encode() always produces exactly codedBytes()
{
public:
    OpusEncoder (const OpusMode&, int channels);   // channels 1 or 2
    ~OpusEncoder();
    int codedBytes() const noexcept;               // codedBytesMono/Stereo (frameSize)
    // pcm interleaved, frameSamples (frameSize) frames. Returns codedBytes() or 0 on error. Real-time safe.
    int encode (const float* pcm, uint8_t* out, int capacity) noexcept;
private:
    OpusCustomEncoder* encoder = nullptr; int channels, bytes, samples;
};

class OpusDecoder
{
public:
    OpusDecoder (const OpusMode&, int channels);
    ~OpusDecoder();
    // data == nullptr → packet-loss concealment. Writes frameSamples*channels interleaved floats. Real-time safe.
    void decode (const uint8_t* data, int len, float* pcm) noexcept;
private:
    OpusCustomDecoder* decoder = nullptr; int channels, samples;
};
}
```

Implementation notes: `opus_custom_encoder_ctl (enc, OPUS_SET_VBR (0))`, `OPUS_SET_COMPLEXITY (2)`, `OPUS_SET_PACKET_LOSS_PERC (20)`, `OPUS_SET_BITRATE (bytes * 8 * 48000 / samples)`; `opus_custom_encode_float (enc, pcm, samples, out, bytes)`; decode with `opus_custom_decode_float (dec, data, len, pcm, samples)` and `nullptr, 0` for PLC. `OPUS_APPLICATION_RESTRICTED_LOWDELAY` is implicit in custom modes (CELT only).

```cpp
// src/core/JitterBuffer.h
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
namespace ensayo
{
// Sequence-indexed ring of fixed-size blocks. One producer (network thread) calls push, one consumer
// (audio/tick thread) calls pop. Auto mode re-targets the size once per second from arrival jitter.
class JitterBuffer
{
public:
    static constexpr int minBlocks = 2, maxBlocks = 20, initialBlocks = 4;
    JitterBuffer (int blockBytes, double blockDurationMs);
    void setAutoSize (bool enabled) noexcept;      // default true
    void setSizeBlocks (int blocks) noexcept;      // manual; clamps; resets window
    int getSizeBlocks() const noexcept;
    // Producer. Late (behind the read position) → counted as late and dropped. More than maxBlocks ahead → window resync.
    void push (uint16_t seq, const uint8_t* data, int len, double arrivalMs) noexcept;
    // Consumer. Copies one block into out and returns true; false → caller does PLC. Advances the read sequence.
    bool pop (uint8_t* out, int& len) noexcept;
    void reset() noexcept;                         // forgets everything; next push defines the window
    struct Stats { int sizeBlocks = 0; uint32_t underruns = 0, late = 0, resyncs = 0; double jitterMs = 0.0; bool primed = false; };
    Stats getStats() const noexcept;
private:
    // storage: maxBlocks slots of blockBytes + len + valid + seq; readSeq; primed flag that waits for sizeBlocks
    // packets before the first pop; jitter estimator: EWMA of |arrival interval - blockDurationMs| and its
    // max over the last second; target = clamp(ceil((mean + 3*stddev + max) / blockDurationMs) + 1).
};
}
```

Behaviour the tests pin down (all deterministic, time passed explicitly):
1. In-order push/pop returns the same bytes; first pop waits until `initialBlocks` packets are buffered (primed) and until then returns false without counting underruns.
2. Missing seq → pop returns false exactly once for that seq, then continues.
3. Out-of-order arrival within the window is reordered.
4. A packet with seq behind readSeq is counted `late` and ignored.
5. Jump ahead > `maxBlocks` → `resyncs++`, window restarts at that seq.
6. Auto mode: feeding arrivals with ±3 ms jitter for 2 s raises `sizeBlocks` above 4; feeding exact-interval arrivals for 10 s brings it back down (never below `minBlocks`); the size is only re-evaluated once per 1000 ms of arrival time; up moves apply immediately, down moves need three consecutive lower evaluations (hysteresis).
7. `pop` never blocks and never allocates (verified by design; `storage` sized in constructor).

```cpp
// src/core/SpscQueue.h — fixed-capacity queue of byte packets (max maxPacketBytes each)
#pragma once
#include "Protocol.h"
#include <atomic>
#include <array>
#include <vector>
namespace ensayo
{
class PacketQueue
{
public:
    explicit PacketQueue (int capacity = 64);
    bool push (const uint8_t* data, size_t len) noexcept;   // false when full or len > maxPacketBytes
    bool pop (uint8_t* out, size_t& len) noexcept;          // false when empty
    int size() const noexcept;
private:
    struct Slot { std::array<uint8_t, maxPacketBytes> bytes; uint16_t len = 0; };
    std::vector<Slot> slots; std::atomic<int> head { 0 }, tail { 0 };
};
}
```

- [ ] **Step 1: tests** — `OpusCodecTests`: encode a 440 Hz sine (mono, 128) → decode → after 10 frames, SNR of frame 10 ≥ 15 dB; `encode` returns exactly `codedBytes()`; stereo at 64 also; PLC call after a valid frame outputs finite values; `encode` with capacity too small returns 0. `JitterBufferTests`: the seven behaviours above. `SpscQueueTests`: push/pop order, full/empty, oversize rejected.
- [ ] **Step 2: implement, run → pass.**
- [ ] **Step 3: Commit** — `"Add Opus custom-mode codec wrappers, jitter buffer and packet queue"`.

---

### Task 4: BackingTrack and MixServer (server core, no sockets)

**Files:**
- Create: `src/server/BackingTrack.h/.cpp`, `src/server/MixServer.h/.cpp`, `tests/BackingTrackTests.cpp`, `tests/MixServerTests.cpp`, `tests/fixtures/` (written by tests at runtime into `%TEMP%`, not committed)
- Modify: `CMakeLists.txt` markers `# T4`, `# T4-tests`

**Interfaces — Consumes:** Task 2, Task 3. **Produces:**

```cpp
// src/server/BackingTrack.h
#pragma once
#include "core/Protocol.h"
#include <juce_audio_formats/juce_audio_formats.h>
namespace ensayo
{
class BackingTrack
{
public:
    static constexpr int64_t maxLengthSamples = 30LL * 60 * sampleRate;
    BackingTrack();
    // Worker/message thread. Decodes to 48 kHz stereo float (LagrangeInterpolator when needed), then swaps in atomically.
    juce::Result load (const juce::File&);
    void unload();
    void play() noexcept; void pause() noexcept; void stop() noexcept;   // stop = pause + seek 0
    void seek (int64_t samples) noexcept; void setLoop (bool) noexcept;
    TransportState getState() const noexcept;
    // Audio thread. Adds nothing when not playing. Advances position. At the end: loop or pause at length.
    void render (float* L, float* R, int numSamples) noexcept;
private:
    juce::AudioFormatManager formats; // registerBasicFormats
    // std::shared_ptr<const juce::AudioBuffer<float>> held under a SpinLock the audio thread try-locks
};
}
```

```cpp
// src/server/MixServer.h
#pragma once
#include "core/Protocol.h"
#include "core/OpusCodec.h"
#include "core/JitterBuffer.h"
#include "core/MixMath.h"
#include "BackingTrack.h"
#include <functional>
#include <map>
#include <vector>
namespace ensayo
{
struct ParticipantInfo { uint32_t id = 0; juce::String name; Role role = Role::both; };
struct ParticipantStats { uint32_t id = 0; int jitterBlocks = 0; uint32_t underruns = 0, late = 0; float voicePeak = 0, instrumentPeak = 0; };

// One session. All public methods except tick()/pushAudio() are "control" calls (any thread, serialised by
// the caller through a lock); tick() runs on the clock thread; pushAudio() on the network thread.
class MixServer
{
public:
    using Sender = std::function<void (uint32_t clientId, const uint8_t* data, size_t len)>;
    explicit MixServer (FrameSize);
    FrameSize frameSize() const noexcept;

    uint32_t addParticipant (const juce::String& name, Role);   // ids start at 1 and never repeat
    void removeParticipant (uint32_t id);
    std::vector<ParticipantInfo> getParticipants() const;
    bool isEmpty() const;

    // target 0 = backing track. Unknown ids ignored.
    void setGain (uint32_t client, uint32_t target, const ChannelGain&);
    BackingTrack& track() noexcept;

    void pushAudio (const AudioPacketView&, double arrivalMs) noexcept;   // network thread
    // Clock thread. Pops one block per participant, decodes (PLC on gaps), renders the track, mixes one stereo
    // block per client excluding that client, encodes and calls send. Returns the number of packets sent.
    int tick (const Sender& send) noexcept;
    std::vector<ParticipantStats> getStats() const;
    uint32_t getTrackPosition() const noexcept;
private:
    // per participant: two JitterBuffers (voice, instrument), two OpusDecoders, decoded mono buffers,
    // peak, fade-in gain (0→1 over 200 ms), gains map<targetId, ChannelGain>, OpusEncoder stereo, outSeq.
    // Participant map changes happen under a SpinLock that tick() try-locks (skips the tick on contention).
};
}
```

- [ ] **Step 1: tests** — `BackingTrackTests`: write a 44.1 kHz stereo WAV of 1 s with a click (+1.0 sample) every 250 ms to `%TEMP%\EnsayoVirtual\tests\click.wav` using `juce::WavAudioFormat`; `load` → `getState().length == 48000` (±1), `loaded`; `render` while paused writes silence (buffers pre-filled with 0 stay 0); `play` then render 12000 samples → first click near sample 0 and the second near 12000; `seek (24000)` + render → click near 0; `loop` wraps; at end without loop → `playing == false` and position == length; unsupported file → failed Result. `MixServerTests`: in-process: `addParticipant` A (both), B (voice), C (instrument). Build packets with real `OpusEncoder` of a 1 kHz sine for A's voice and silence for the others; push for 6 ticks; `tick` with a collecting sender → one packet per participant per tick, each a valid mix packet with `codedBytesStereo` payload; decode B's mix → contains energy (A's sine); set B's gain for A to muted → after 10 ticks B's mix energy < −50 dBFS; A's mix excludes A (energy ≈ 0 when only A sends); `track` loaded + play → everyone's mix carries the clicks; `removeParticipant` → packets drop; `getStats` has one entry per participant; `tick` with zero participants returns 0.
- [ ] **Step 2: implement, run → pass.**
- [ ] **Step 3: Commit** — `"Add backing track player and per-client mixing server core"`.

---

### Task 5: ServerHost (TCP control + UDP audio), HighResClock, EnsayoServer.exe

**Files:**
- Create: `src/server/ServerHost.h/.cpp`, `src/server/HighResClock.h/.cpp`, `tests/ServerHostTests.cpp`
- Modify: `src/server/ServerMain.cpp`, `CMakeLists.txt` markers `# T5`, `# T5-tests`

**Interfaces — Consumes:** Task 2–4. **Produces:**

```cpp
// src/server/HighResClock.h
#pragma once
#include <functional>
#include <thread>
#include <atomic>
namespace ensayo
{
// Thread (MMCSS "Pro Audio", CreateWaitableTimerExW high resolution, absolute deadlines) calling fn every periodUs.
class HighResClock
{
public:
    HighResClock (double periodMicroseconds, std::function<void()> fn);
    ~HighResClock();
    void start(); void stop();
    struct Stats { double maxLateUs = 0, meanLateUs = 0; uint64_t ticks = 0; };
    Stats getStats() const noexcept;
};
}
```

```cpp
// src/server/ServerHost.h
#pragma once
#include "MixServer.h"
#include <juce_core/juce_core.h>
namespace ensayo
{
struct ServerConfig
{
    int tcpPort = defaultPort, udpPort = defaultPort;
    FrameSize frameSize = FrameSize::f128;
    juce::File uploadDirectory;        // default %TEMP%\EnsayoVirtual\server
    int maxParticipantsPerSession = 8;
};

class ServerHost
{
public:
    explicit ServerHost (ServerConfig);
    ~ServerHost();
    juce::Result start();              // binds TCP + UDP, starts accept + UDP threads. No clock.
    void stop();
    void startInternalClock();         // headless: HighResClock at frame period calling tick()
    void tick() noexcept;              // embedded: call from the host's audio callback once per frame
    int getSessionCount() const;
    std::vector<juce::String> getSessionIds() const;
    int getUdpPort() const noexcept;
    std::function<void (const juce::String&)> onLog;   // message-thread-agnostic; called from any thread
};
}
```

Behaviour:
- TCP: accept thread (`juce::StreamingSocket`), one thread per connection reading `\n`-terminated lines (max 64 KB, else disconnect). Messages per spec §3.2. `create` → new session id, `addParticipant`, reply `joined`. `join` with unknown/invalid id → `error`. `participants` broadcast on every change. `gain` → `MixServer::setGain`. `transport` → `BackingTrack` calls + broadcast `transport_state`. `track_upload_begin {name,size,sha1}` → read exactly `size` raw bytes after the line into `uploadDirectory/<sessionId>-<sanitized name>`, verify SHA-1 (`juce::SHA1`), `track().load`, broadcast `track_ready` + `transport_state`; errors → `error` and delete file. `ping {t0}` → `pong {t0}`. Every 1 s per session: `stats` and `transport_state` to all. Connection close → `removeParticipant`; empty session → deleted.
- UDP: one raw Winsock socket (`SIO_UDP_CONNRESET` false), blocking `recvfrom` loop on an MMCSS thread. First valid packet from a `clientId` records its `sockaddr`; later packets from a different address for the same id are ignored unless the previous address has been silent 2 s. `decodeAudioPacket` → `pushAudio` on the session owning that clientId. Sender: `sendto` to the recorded address.
- Clock: `tick()` loops sessions under a try-locked `SpinLock` (skip on contention) and calls `MixServer::tick`. `startInternalClock()` → `HighResClock (frameSamples/48000*1e6, tick)`.
- `ServerMain.cpp`: args `--port N` (both), `--udp-port N`, `--frame 64|128`, `--log`; prints `listening tcp=<port> udp=<port>` and runs until Ctrl-C or stdin EOF.

- [ ] **Step 1: tests** — `ServerHostTests` (ports 22160+, loopback): start host; connect with `juce::StreamingSocket`, send `create`, receive `joined` with a 6-char session and clientId 1; second socket `join` → both get `participants` with 2 entries; `join` bad id → `error`; `ping` → `pong` with same `t0`; send 20 UDP audio packets from a `juce::DatagramSocket` for client 1 while calling `host.tick()` manually 20 times → the datagram socket of client 1 receives ≥ 10 mix packets that decode with `decodeAudioPacket`; upload a 44.1 k WAV (`track_upload_begin` + bytes) → `track_ready`, then `transport play` → `transport_state.playing == true`; close socket 1 → remaining gets `participants` with 1 entry; `HighResClock` at 2666.67 µs for 500 ms → ticks within ±10 % of 187 and `maxLateUs < 3000`.
- [ ] **Step 2: implement, run → pass.** Build `EnsayoServer` and run `EnsayoServer.exe --port 22161` for 2 s to see the `listening` line.
- [ ] **Step 3: Commit** — `"Add TCP/UDP server host, high-resolution clock and EnsayoServer executable"`.

---

### Task 6: Client audio — PluginSlot, FakeAudioDevice, AudioEngine, Settings

**Files:**
- Create: `src/client/PluginSlot.h/.cpp` (port of `C:\Users\Lucas\Desktop\discord-share-guitar\src\PluginSlot.*` into `namespace ensayo`, unchanged API), `src/client/FakeAudioDevice.h/.cpp`, `src/client/AudioEngine.h/.cpp`, `src/client/Settings.h/.cpp`, `tests/PluginSlotTests.cpp`, `tests/FakeAudioDeviceTests.cpp`, `tests/AudioEngineTests.cpp`, `tests/SettingsTests.cpp`
- Modify: `CMakeLists.txt` markers `# T6`, `# T6-tests`

**Interfaces — Consumes:** Task 2, 3. **Produces:**

```cpp
// src/client/FakeAudioDevice.h
#pragma once
#include "core/Protocol.h"
#include <juce_audio_devices/juce_audio_devices.h>
namespace ensayo
{
enum class TestSignal { silence, sine440, click };   // click: +1.0 impulse every 250 ms on both inputs
struct ClickEvent { int64_t sampleIndex; double qpcMs; };   // qpcMs = juce::Time::getMillisecondCounterHiRes()

// Two inputs, two outputs, thread-driven at exact block cadence (HighResClock-style waitable timer).
class FakeAudioDevice : public juce::AudioIODevice
{
public:
    FakeAudioDevice (int blockSize, TestSignal);
    // Called on the device thread after every callback with the output block; used by the probe.
    std::function<void (const float* L, const float* R, int n, int64_t firstSampleIndex, double qpcMs)> onOutputBlock;
    std::function<void (ClickEvent)> onClickEmitted;    // when TestSignal::click fires
    // juce::AudioIODevice overrides: open/close/start/stop/isOpen/isPlaying, names "Fake In/Out L/R",
    // getAvailableSampleRates {48000}, getAvailableBufferSizes {64,128,256}, latencies 0.
};

class FakeAudioDeviceType : public juce::AudioIODeviceType   // typeName "Fake"; one device "Fake Audio"
{
public:
    FakeAudioDeviceType (int blockSize, TestSignal);
    // overrides create the FakeAudioDevice
};

// Detects clicks (sample > 0.5 after 20 ms of quiet) in a stream and reports arrival time.
class ClickProbe
{
public:
    std::function<void (ClickEvent)> onClickDetected;
    void process (const float* L, const float* R, int n, int64_t firstSampleIndex, double qpcMs) noexcept;
};
}
```

```cpp
// src/client/Settings.h
#pragma once
#include "core/Protocol.h"
namespace ensayo
{
struct AppSettings
{
    juce::String name; Role role = Role::both;
    juce::String deviceType = "ASIO";   // "ASIO" | "Fake"
    juce::String deviceName; double sampleRate = 48000.0; int bufferSize = 128;
    int micChannel = 0, instrumentChannel = 1, outputPair = 0;
    juce::String pluginFile, pluginState; bool pluginBypassed = false;
    float monitorVoiceDb = 0, monitorInstrumentDb = 0; bool monitorVoiceMuted = false, monitorInstrumentMuted = false;
    float receiveDb = 0; float trackDb = 0; bool trackMuted = false;
    int hostPort = defaultPort; juce::String lastLink; juce::String ytDlpPath;
};
juce::String toJson (const AppSettings&);  AppSettings fromJson (const juce::String&);
juce::File defaultSettingsFile();           // %APPDATA%\EnsayoVirtual\settings.json
AppSettings loadSettings (const juce::File&); bool saveSettings (const AppSettings&, const juce::File&);
}
```

```cpp
// src/client/AudioEngine.h
#pragma once
#include "core/Protocol.h"
#include "core/OpusCodec.h"
#include "core/JitterBuffer.h"
#include "core/SpscQueue.h"
#include "core/MixMath.h"
#include "PluginSlot.h"
#include "FakeAudioDevice.h"
#include <juce_audio_devices/juce_audio_devices.h>
namespace ensayo
{
// What the engine needs from the network side. SessionClient (Task 7) implements it.
class AudioTransport
{
public:
    virtual ~AudioTransport() = default;
    virtual void sendAudio (const uint8_t* packet, size_t len) noexcept = 0;   // audio thread → queue
    virtual JitterBuffer& receiveBuffer() noexcept = 0;                        // filled by the network thread
    virtual uint32_t getClientId() const noexcept = 0;
    virtual uint32_t getTrackPosition() const noexcept = 0;
};
// Called once per frame from the audio thread when this client hosts the embedded server (ServerHost::tick).
using FrameHook = std::function<void()>;

struct EnginePeaks { float voice = 0, instrument = 0, received = 0, output = 0; };

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;
    juce::AudioDeviceManager& getDeviceManager() noexcept;
    juce::StringArray getAsioDeviceNames();
    void useFakeDevice (int blockSize, TestSignal);        // registers FakeAudioDeviceType and opens it
    FakeAudioDevice* getFakeDevice() const noexcept;
    juce::String openDevice (const juce::String& deviceType, const juce::String& name, double sampleRate, int bufferSize); // "" on success
    juce::AudioIODevice* getDevice() const noexcept;
    PluginSlot& getPluginSlot() noexcept;

    void setChannels (int mic, int instrument, int outputPair) noexcept;
    void setRole (Role) noexcept;
    void setMonitor (const ChannelGain&) noexcept;         // own voice/instrument
    void setReceiveGain (float db) noexcept;
    void setTransport (AudioTransport*) noexcept;         // nullptr = offline (monitor only)
    void setFrameHook (FrameHook) ;                        // set while the device is stopped
    void setSimulatedLossPercent (int) noexcept;          // drops that % of received blocks before decode (tests)

    EnginePeaks takePeaks() noexcept;
    uint32_t getCallbackCount() const noexcept;
    FrameSize getFrameSize() const noexcept;               // from the device block size (256 → f128 ×2)
    juce::String takeDeviceError();
private:
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;
    // per frame: voice/instr → plugin → monitor; encode voice+instr(L+R)/2 into AudioPacketView; encodeAudioPacket into
    // a stack buffer; transport->sendAudio; receiveBuffer().pop → OpusDecoder stereo (PLC) → addStereo(receiveGain);
    // frameHook() when set; sampleTime counter; seq counter.
};
}
```

Device manager: subclass that offers ASIO plus, when enabled, the `FakeAudioDeviceType` (same idea as the reference `AsioDeviceManager`). A 256 block → two 128 frames per callback (two packets, two pops).

- [ ] **Step 1: tests** — `PluginSlotTests`: port the reference tests (folder listing, dry pass-through, failed load). `FakeAudioDeviceTests`: open with block 128 + `click`, start for 600 ms → `onClickEmitted` fired ≥ 2 times, callback count within ±15 % of 225, `onOutputBlock` called; `ClickProbe` detects a synthetic click at the right sample index and ignores a second one 5 ms later. `AudioEngineTests`: `useFakeDevice (128, sine440)`, no transport, `setMonitor ({0, 0, false})` → output block peak ≈ 1.0 for voice; `setMonitor ({-60,-60,false})` → peak < 0.01; with a stub `AudioTransport` collecting packets: role `voice` → packets have `flags == voice` and `instrumentLen == 0`; role `both` → both payloads sized `codedBytesMono (f128)`; stub pushes a stereo-encoded sine into `receiveBuffer` → output contains it; `setSimulatedLossPercent (100)` → output silent but no crash. `SettingsTests`: round-trip, invalid JSON → defaults.
- [ ] **Step 2: implement, run → pass.**
- [ ] **Step 3: Commit** — `"Add client audio engine with ASIO and fake devices, VST3 slot and settings"`.

---

### Task 7: SessionClient, SessionLink, YouTubeImporter

**Files:**
- Create: `src/client/SessionLink.h/.cpp`, `src/client/SessionClient.h/.cpp`, `src/client/YouTubeImporter.h/.cpp`, `tests/SessionLinkTests.cpp`, `tests/SessionClientTests.cpp`, `tests/YouTubeImporterTests.cpp`
- Modify: `CMakeLists.txt` markers `# T7`, `# T7-tests`

**Interfaces — Consumes:** Task 2, 3; the `AudioTransport` interface from Task 6 (copy its exact declaration from this plan if Task 6's header is not yet present; it must match). Tests use `ServerHost` from Task 5 when available — until then, a tiny in-test TCP responder using `juce::StreamingSocket` that replies `joined`.

```cpp
// src/client/SessionLink.h
#pragma once
#include <juce_core/juce_core.h>
#include <optional>
namespace ensayo
{
struct SessionLink { juce::String host; int port = 0; juce::String sessionId;
                     juce::String toString() const; };               // "ensayo://host:port/ID"
std::optional<SessionLink> parseSessionLink (const juce::String&);   // accepts with/without scheme, trims, uppercases ID
bool registerUrlScheme (const juce::File& exe);                      // HKCU\Software\Classes\ensayo (...)
std::optional<SessionLink> linkFromCommandLine (const juce::StringArray& args);
}
```

```cpp
// src/client/SessionClient.h
#pragma once
#include "core/Protocol.h"
#include "core/JitterBuffer.h"
#include "core/SpscQueue.h"
#include "client/AudioEngine.h"      // AudioTransport
#include "server/MixServer.h"        // ParticipantInfo, ParticipantStats
namespace ensayo
{
struct SessionState
{
    bool connected = false; juce::String sessionId; uint32_t clientId = 0; juce::String host; int port = 0;
    std::vector<ParticipantInfo> participants; std::vector<ParticipantStats> stats;
    TransportState transport; int pingMs = -1; FrameSize frameSize = FrameSize::f128;
    juce::String trackUploadStatus; float trackUploadProgress = 0;   // 0..1
};

class SessionClient : public AudioTransport
{
public:
    SessionClient(); ~SessionClient() override;
    // Message thread. Callbacks are delivered on the message thread (juce::MessageManager::callAsync).
    void create (const juce::String& host, int port, const juce::String& name, Role);
    void join (const SessionLink&, const juce::String& name, Role);
    void leave();
    void sendGain (uint32_t target, const ChannelGain&);
    void sendTransport (const juce::String& cmd, int64_t position = 0, bool loop = false);
    void uploadTrack (const juce::File&);          // background thread; progress via onStateChanged
    const SessionState& getState() const noexcept;
    std::function<void (const SessionState&)> onStateChanged;
    std::function<void (const juce::String&)> onError;        // also fires on disconnect
    std::function<void()> onJoined;
    // AudioTransport
    void sendAudio (const uint8_t*, size_t) noexcept override;
    JitterBuffer& receiveBuffer() noexcept override;
    uint32_t getClientId() const noexcept override;
    uint32_t getTrackPosition() const noexcept override;
    // UDP thread: sends queued packets, receives mix packets → receiveBuffer().push (seq, payload, arrival), stores trackPosition.
    // Control thread: StreamingSocket read loop; ping every 1 s; reconnect is not attempted.
};
}
```

```cpp
// src/client/YouTubeImporter.h
#pragma once
#include <juce_core/juce_core.h>
namespace ensayo
{
class YouTubeImporter
{
public:
    static juce::File findYtDlp (const juce::String& configuredPath);   // configured → next to exe → PATH; File() if none
    static bool looksLikeYouTubeUrl (const juce::String&);
    static juce::StringArray buildCommand (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir);
      // {ytdlp, "-x", "--audio-format", "mp3", "--audio-quality", "0", "--no-playlist", "-o", outDir/"%(title)s.%(ext)s", url}
    // Background thread; callbacks on the message thread. Progress parsed from "[download]  42.0%".
    void start (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir);
    void cancel();
    std::function<void (float)> onProgress; std::function<void (const juce::File&)> onFinished; std::function<void (const juce::String&)> onError;
};
}
```

- [ ] **Step 1: tests** — `SessionLinkTests`: parse `ensayo://1.2.3.4:22140/ab12cd` → host, port, `AB12CD`; parse `host.name:22140/ABCDEF` (no scheme); reject missing port, bad id, port 0/70000; `toString` round trip; `linkFromCommandLine`. `SessionClientTests` (needs Task 5 built; ports 22170+): start `ServerHost`; client A `create` → `onJoined`, state has sessionId; client B `join` with A's link → both `participants.size() == 2`; `sendGain` doesn't crash; A `uploadTrack (wav)` → `transport.loaded` within 5 s; `sendTransport ("play")` → `transport.playing`; feed `sendAudio` from a loop with `host.tick()` → B's `receiveBuffer().getStats().primed`; `leave` → `connected == false`. `YouTubeImporterTests`: `looksLikeYouTubeUrl` for youtube.com/watch, youtu.be, music.youtube.com and false for others; `buildCommand` exact; `findYtDlp` with a non-existent configured path and no PATH entry → `File()`; `start` with a fake `yt-dlp.exe` (a `.cmd` written to temp that prints `[download]  50.0%` then creates `out.mp3`) → `onProgress ≥ 0.5` then `onFinished` with that file.
- [ ] **Step 2: implement, run → pass.**
- [ ] **Step 3: Commit** — `"Add session client, session link parsing and yt-dlp importer"`.

---

### Task 8: Headless client mode

**Files:**
- Create: `src/client/HeadlessClient.h/.cpp`, `tests/HeadlessClientTests.cpp`
- Modify: `CMakeLists.txt` markers `# T8`, `# T8-tests`; `src/Main.cpp` dispatch (Task 9 owns Main.cpp — Task 8 only provides `int runHeadless (const juce::StringArray& args)` and Task 9 calls it).

**Interfaces — Consumes:** Tasks 5–7. **Produces:**

```cpp
// src/client/HeadlessClient.h
#pragma once
#include <juce_core/juce_core.h>
namespace ensayo
{
struct HeadlessOptions
{
    bool host = false; int hostPort = defaultPort;     // --host [--port N] : embedded ServerHost, prints "link ensayo://127.0.0.1:port/ID"
    juce::String link;                                  // --join <link>
    juce::String name = "Headless"; Role role = Role::both;
    int blockSize = 128; TestSignal signal = TestSignal::silence;   // --block N --fake-audio silence|sine|click
    juce::File probeLog;                                // --probe <file>: CSV "detectedQpcMs,sampleIndex" per detected click
    juce::File emitLog;                                 // --emit-log <file>: CSV "emittedQpcMs,sampleIndex" per emitted click
    juce::File trackFile; bool playTrack = false;       // --track <file> [--play]
    int lossPercent = 0; double durationSeconds = 10;   // --simulate-loss N --duration S
    juce::File statsLog;                                // --stats <file>: one JSON line per second with ping, jitter, loss
};
std::optional<HeadlessOptions> parseHeadlessOptions (const juce::StringArray& args, juce::String& error);
int runHeadless (const HeadlessOptions&);   // blocks for duration; prints "joined <session> client <id>", "participants N", exits 0 on success
}
```

Behaviour: runs a `juce::MessageManager` loop (`ScopedJuceInitialiser_GUI` is fine in a console-less GUI exe); `AudioEngine::useFakeDevice`; when `host`, create `ServerHost`, `start()`, set `AudioEngine::setFrameHook ([&] { host.tick(); })`, then `SessionClient::create ("127.0.0.1", port, …)`; print the link. With `--join`, `SessionClient::join`. The probe uses `FakeAudioDevice::onOutputBlock → ClickProbe` and appends lines to `probeLog`. Prints `latency median <ms> p95 <ms> n <count>` at exit when both logs are given on the same machine (the headless client computes it when `--emit-log` of another instance is passed via `--compare <file>`).

- [ ] **Step 1: tests** — `parseHeadlessOptions` for a full argument set and for errors (`--role x`, `--block 100`). `runHeadless` smoke: in-process two threads is overkill; instead test the option→engine wiring by running `runHeadless` with `--host --duration 1 --fake-audio silence` on a free port and asserting exit 0 (it must bind, create and leave cleanly).
- [ ] **Step 2: implement, run → pass.**
- [ ] **Step 3: Commit** — `"Add headless client mode with fake audio and click latency probe"`.

---

### Task 9: UI — Lobby, Session, MainComponent, Main

**Files:**
- Create: `src/ui/Text.h`, `src/ui/LevelMeter.h`, `src/ui/LobbyView.h/.cpp`, `src/ui/SessionView.h/.cpp`, `src/ui/MainComponent.h/.cpp`, `tests/UiTests.cpp`
- Modify: `src/Main.cpp`, `CMakeLists.txt` marker `# T9` (UI sources) and add `tests/UiTests.cpp` under `# T10-tests` is Task 10's; Task 9 adds its test under a new line right after `# T8-tests`.

**Interfaces — Consumes:** Tasks 6, 7, 8 (and 5 for the embedded server). **Produces:** component names (exact, used by Task 10):

Lobby: `lobby.name`, `lobby.role`, `lobby.create`, `lobby.link`, `lobby.join`, `audio.device`, `audio.rate`, `audio.buffer`, `audio.panel`, `audio.mic`, `audio.instrument`, `audio.output`, `audio.latency`, `vst.list`, `vst.browse`, `vst.editor`, `vst.bypass`, `vst.remove`, `status.text`, `status.retry`.
Session: `session.link`, `session.copy`, `session.leave`, `me.voice.fader`, `me.voice.mute`, `me.voice.meter`, `me.instrument.fader`, `me.instrument.mute`, `me.instrument.meter`, `participant.<n>.name`, `participant.<n>.role`, `participant.<n>.voice.fader`, `participant.<n>.voice.mute`, `participant.<n>.instrument.fader`, `participant.<n>.instrument.mute`, `participant.<n>.meter` (n = 1..), `track.load`, `track.url`, `track.download`, `track.name`, `track.play`, `track.stop`, `track.loop`, `track.position`, `track.time`, `track.fader`, `track.mute`, `track.progress`, `status.ping`, `status.jitter`, `status.latency`, `status.loss`, `status.cpu`, `status.text`, `status.retry`.

```cpp
// src/ui/MainComponent.h (public surface)
class MainComponent : public juce::Component, private juce::Timer
{
public:
    MainComponent();                                   // loads settings, builds engine, shows lobby
    ~MainComponent() override;
    void showDemoSession();                            // fake state: 3 participants, track loaded & playing (for --demo / --screenshot)
    void joinFromLink (const SessionLink&);            // used by Main for ensayo:// arguments
    LobbyView& getLobby(); SessionView& getSession();  // for screenshots/tests
    bool isInSession() const;
};
```

Behaviour: lobby **Crear sesión** → `ServerHost` on `settings.hostPort`, engine `setFrameHook (host.tick)`, `SessionClient::create ("127.0.0.1", …)`; on `onJoined` switch to session view showing the link with the machine's first non-loopback IPv4 (`juce::IPAddress::getAllAddresses`) — plus a hint "si estás fuera de tu red, abrí el puerto UDP/TCP". **Unirse** parses the link field. Faders → `sendGain` (debounced 50 ms) / `setMonitor`. Track: **Cargar archivo…** (`FileChooser` *.wav;*.mp3;*.flac;*.ogg) → `uploadTrack`; **Descargar** → `YouTubeImporter` → `uploadTrack`; transport buttons → `sendTransport`; position slider drag-end → `seek`. Timer 30 Hz: meters, status, position. Status bar texts: `Ping: 12 ms`, `Jitter: 4/4`, `Latencia est.: 34 ms` (ping + frame + 0.7·(client+server)·frame + ASIO in/out + 1.5), `Pérdida: 0.0 %`. All Spanish labels: "Nombre", "Rol", "Voz", "Instrumento", "Instrumento + Voz", "Crear sesión", "Link de sesión", "Unirse", "Dispositivo ASIO", "Frecuencia", "Buffer", "Panel ASIO", "Canal micrófono", "Canal instrumento", "Salida", "VST (instrumento)", "Examinar…", "Abrir editor", "Bypass", "Quitar", "Copiar link", "Salir", "Yo (monitoreo)", "Backing track", "Cargar archivo…", "URL de YouTube", "Descargar", "Play", "Pausa", "Stop", "Loop", "Mute", "Reintentar".

`Main.cpp`: `moreThanOneInstanceAllowed() = false`; `anotherInstanceStarted (cmd)` → parse link → `joinFromLink`; `initialise`: if args contain `--headless` → `runHeadless` then `quit()`; `--screenshot <dir>` → Task 10's `runScreenshots` then quit; `--demo` → window + `showDemoSession()`; `ensayo://` arg → join; first run → `registerUrlScheme`.

- [ ] **Step 1: tests** — `UiTests`: construct `MainComponent` under `ScopedJuceInitialiser_GUI`, `setSize (1100, 760)`; every lobby name above is found via `findChildWithName` recursively (write a helper `findByName (Component&, name)`), visible, non-empty bounds; `showDemoSession()` → every session name with `participant.1..3` found; lobby controls hidden.
- [ ] **Step 2: implement, run → pass.** Build `EnsayoVirtual` Release and launch `--demo` for 3 s (no crash).
- [ ] **Step 3: Commit** — `"Add lobby and session UI, main window and command-line modes"`.

---

### Task 10: Screenshot mode

**Files:**
- Create: `src/ui/Screenshot.h/.cpp`, `tests/ScreenshotTests.cpp`, `tests/integration/expected-controls.json`
- Modify: `CMakeLists.txt` markers `# T10`, `# T10-tests`

```cpp
// src/ui/Screenshot.h
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace ensayo
{
// Renders lobby.png and session.png (1100×760, SoftwareImageType) and controls.json:
// [{"name":"lobby.name","type":"TextEditor","x":..,"y":..,"w":..,"h":..,"visible":true,"view":"lobby"}, ...]
juce::Result runScreenshots (const juce::File& outDir);
}
```

`expected-controls.json`: the full name list from Task 9 with `participant.1..3`.

- [ ] **Step 1: test** — `runScreenshots (temp)` → both PNGs exist and are 1100×760 (`juce::PNGImageFormat`), `controls.json` parses and contains every name of `expected-controls.json` with `visible == true` and `w,h > 0`.
- [ ] **Step 2: implement, run → pass.** Run `EnsayoVirtual.exe --screenshot build\shots` and look at the PNGs.
- [ ] **Step 3: Commit** — `"Add screenshot mode that renders both views and dumps control metadata"`.

---

### Task 11: Integration scripts

**Files:**
- Create: `tests/integration/common.ps1`, `two-instances.ps1`, `embedded-host.ps1`, `backing-track.ps1`, `packet-loss.ps1`, `screenshots.ps1`, `run-all.ps1`, `README.md`

Each script: `$ErrorActionPreference = 'Stop'`; takes `-BuildDir` (default `build`) and `-Config` (default `Release`); starts processes with `Start-Process -PassThru -RedirectStandardOutput`; always kills them in `finally`; prints `PASS <name>` / `FAIL <name>: reason` and exits non-zero on failure.

1. `two-instances.ps1`: `EnsayoServer.exe --port 22150`; wait for `listening`; A: `EnsayoVirtual.exe --headless --join ensayo://127.0.0.1:22150/NEW --name A --role both --fake-audio click --emit-log A.csv --duration 12` — `NEW` means create (Task 8 treats id `NEW` as `create`; add that to Task 8's parser: `--join` with id `NEW` creates and prints the link); read the link from A's stdout; B: `--join <link> --name B --fake-audio silence --probe B.csv --compare A.csv --duration 10`; assert B printed `participants 2`, `n >= 10`, `median < 40` (ms). Print the latency line.
2. `embedded-host.ps1`: A with `--host --port 22151 --fake-audio click --emit-log`; B joins the printed link; same assertions.
3. `backing-track.ps1`: server + A `--track tests\fixtures\click.wav --play --duration 8` (the fixture WAV is generated by the script with a small PowerShell `[System.IO.BinaryWriter]` writer if missing) + B with probe; assert B detects ≥ 8 clicks and its `--stats` log shows `"trackPlaying":true`.
4. `packet-loss.ps1`: as 1 with B `--simulate-loss 10`; assert exit 0 and stats show `underruns > 0`? (loss is simulated after the buffer, so assert the process survives and `n >= 5`).
5. `screenshots.ps1`: `EnsayoVirtual.exe --screenshot out\shots`; compare `controls.json` with `expected-controls.json` (every name present, visible, w/h > 0); then launch `--demo`, wait 2 s, capture the real window with `System.Drawing` (`[System.Windows.Forms.Screen]`, `Graphics.CopyFromScreen` over the window rect from `GetWindowRect` via `Add-Type` P/Invoke) to `out\shots\demo-window.png`, kill it.
6. `run-all.ps1` runs all and summarises.

- [ ] **Step 1: write scripts; run each against the Release build; fix until all PASS.**
- [ ] **Step 2: Commit** — `"Add integration scripts: two instances, embedded host, backing track, loss, screenshots"`.

---

### Task 12: End-to-end verification, README, review

- [ ] Run unit tests (Debug) and `run-all.ps1` (Release); record outputs in `docs/plans/2026-11-04-verification.md` (latency numbers, screenshots paths).
- [ ] Open `lobby.png`, `session.png`, `demo-window.png` and check visually: every button, slider, checkbox and label from Task 9's list is present, legible, not overlapping, Spanish text correct.
- [ ] `README.md`: build, run, how to create/join, ports to open, ASIO setup, VST, backing tracks and the yt-dlp note (user-installed, ToS), link format, troubleshooting table.
- [ ] Code review pass (nashe:requesting-code-review) on the whole branch; fix findings.
- [ ] Commit — `"Add README and verification report"`.
