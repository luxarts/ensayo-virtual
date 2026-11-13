# Chat de voz, transporte y vúmetros: plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: nashe:subagent-driven-development. Tasks run sequentially in the worktree `.worktrees/ensayo-virtual-core`, branch `feat/grabacion-multipista` (PR #3).

**Goal:** implement `docs/specs/2026-11-13-chat-de-voz-y-transporte-design.md`.

**Architecture:**
- **Network:** UDP audio packets gain a stream kind (voice/instrument). The host relays them to every other peer that has a UDP address. Presence carries `clientId`. The client demultiplexes by `(clientId, kind)` into per-stream jitter buffers.
- **Engine:**
  - captures the mic continuously into a lock-free FIFO that a chat-encoder thread drains;
  - mixes N received streams with per-user gains and fades;
  - computes per-track peaks in the Renderer and the armed input peak;
  - monitors only the armed track's input;
  - applies send gains before the recorder and the chat.
- **UI:** transport and shortcuts, per-track delete and meters, "Mi audio" panel, "Participantes" mixer.

**Tech stack:** C++20, JUCE 8, libopus custom modes (`OpusMode{FrameSize::f128}`), Win32 sockets.

## Global Constraints
- Code and comments in English; every user-visible string in Spanish (rioplatense), as in the existing UI.
- Audio thread: no allocation, no locks (try-lock or atomics only), no I/O. Every start, stop, mute, gain change or concealment end of a received stream is faded. The priority is no clicks.
- Recording stays bit-exact and sample-aligned. Every existing alignment test keeps `worst == 0`.
- Commits: plain messages. No `Co-Authored-By`, no "Generated with", no emoji, never mention Claude or NASHE. Do not push.
- Build: `cmake --build build --config Release -- /m`, run from PowerShell (Git Bash mangles `/m`).
- Unit tests: `build\EnsayoTests_artefacts\Release\EnsayoTests.exe`.
- Integration: `powershell -NoProfile -ExecutionPolicy Bypass -File tests\integration\run-all.ps1`. Never run it at the same time as the unit tests.
- Never kill processes you did not start. That includes any "Ensayo Virtual.exe" the user has open: stop only PIDs you launched.

---

### Task 1: network streams per sender
**Files:**
- `src/core/AudioPacket*` (or wherever `decodeAudioPacket`/`AudioPacketView` live)
- `src/server/ProjectHost.cpp`
- `src/client/ProjectClient.h/.cpp`
- tests: ProjectHostTests, ProjectClientTests, AudioPacket tests

**Changes:**
- Add a stream kind (`voice = 0`, `instrument = 1`) to the audio packet; keep the 5-byte keepalive.
- The host relays every audio packet to all other peers that have a known UDP address. Drop the `listening` gate and its control message, or keep it accepted as a no-op for compatibility.
- Presence entries include `clientId` for online users.

**Client API:**
- Replace `previewBuffer()` with a per-stream registry: `struct RemoteStream { uint32_t clientId; StreamKind kind; JitterBuffer buffer; Id user; }`.
- Expose it to the audio thread through a fixed-capacity, lock-free table, for example `std::array<RemoteStreamSlot, 16>`. Each slot has a `std::atomic<uint32_t> key` (`clientId << 1 | kind`) and its own `JitterBuffer`. Slots are claimed by the UDP thread on first packet and released after 5 s of silence or when presence says the user left.
- The audio thread reads `remoteStreams()` (a span of slots) and `userForClient(clientId)` (message-thread map, mirrored in each slot as `std::atomic<uint64_t>` hash; the engine needs only a gain lookup per slot, see Task 2).
- `sendAudio(StreamKind, packet, len)` replaces `sendPreview`.

**Tests:**
- two senders interleaved produce two independent buffers with correct sequence order;
- the host relays without "listen";
- presence carries `clientId`.

### Task 2: engine (chat, per-sender mix, meters, monitor, send gains)
**Files:**
- `src/client/StudioEngine.h/.cpp`
- `src/core/Render.h/.cpp`
- `src/client/Recorder.*`, only if the send gain must be applied before `push`
- tests: StudioEngineTests, RenderTests

**Changes:**
- **Send gains.** `setSendGains(float voice, float instrument)` (atomics, smoothed over 960 samples) is applied to the inputs before they reach the recorder, the chat and the armed meter.
- **Monitor.** It sounds only the armed kind (`setArmed(std::optional<TrackKind>)`), with a 240-sample fade on change. No monitor when nothing is armed.
- **Chat send.** While a session sink is set (`std::function<void(StreamKind, const uint8_t*, size_t)> onChatPacket`, set on the message thread before `setChatEnabled(true)`), the callback pushes the mic (post send gain, mono) into a lock-free FIFO.
  - A chat-encoder thread owned by the engine encodes 128-sample Opus frames and calls the sink.
  - While recording an instrument track, the instrument (stereo) goes to a second FIFO and encoder with kind `instrument`.
  - `setMicMuted(bool)` stops sending voice: a fade-out frame, then nothing.
  - Remove the recorder-based `PreviewEncoder` path.
- **Chat receive.** `setRemoteStreams(RemoteStreamTable*)`, with the same Dekker hand-off as the current preview buffer. Per slot it keeps an Opus decoder (mono for voice, stereo for instrument), the concealment state (max 18 frames, then a faded silence), a fade-in on the first frame, and a fade-out when a slot goes silent.
  - Gain per slot comes from `setParticipantGain(clientId, gain, muted)`: an atomic table indexed like the slots and ramped per block.
  - Preallocate all decoders and scratch for the 16 slots in `audioDeviceAboutToStart`.
- **Meters.**
  - The Renderer exposes per-track peaks (post volume, pan and mute, pre master) in a fixed table keyed by track index of the current snapshot, plus a track id lookup.
  - The engine exposes `takeTrackMeters() -> std::vector<std::pair<Id,float>>` (message thread; peaks since the last call, with reset) and `takeArmedInputPeak()`.
  - Clip detection = `peak >= 1.0f`.

**Tests:**
- monitor only for the armed kind;
- send gain lands in the recorded take exactly (gain × input) once the ramp has settled;
- two remote streams mixed with independent gains;
- no sample step above 0.043 when a stream starts or stops, or when concealment ends;
- per-track peaks match the rendered gain;
- mic mute stops voice packets;
- the instrument stream is sent only while recording an instrument track.

### Task 3: controller, settings and UI
**Files:**
- `src/client/SessionController.*`
- `src/client/Settings.*`
- `src/ui/DawView.*`
- `src/ui/MainComponent.*`
- `src/ui/Screenshot.cpp`
- `tests/integration/expected-controls.json`
- tests: DawViewTests, SettingsTests, SessionController/Headless tests

**Changes:**
- **Transport.** A single Play/Pausa button (`daw.play`; label "▶ Play" / "❚❚ Pausa"); remove `daw.stop`, `daw.record` and the listen toggle.
  - Space toggles. Play with an own armed track starts a take: the existing `startTake` path, with count-in.
  - Pausa during a take stops and finishes it; otherwise it pauses (the position is kept).
  - W goes home; ←/→ move ±1 s; Shift+←/→ move ±1 bar. These shortcuts are ignored while recording, and also when a text field has focus. Remove R.
- **Delete track.** "×" per track header (`daw.track.<i>.delete`). It shows a confirmation dialog and emits `Ops::deleteTrack`.
  - It is enabled for the author or the host, and disabled while recording on that track or in read-only mode.
  - Disarm when the armed track is deleted.
- **Meters.** A per-track meter (`daw.track.<i>.meter`) with a dB number and a clip latch. Clicking the meter clears the latch. Refresh at 30 Hz from `takeTrackMeters`/`takeArmedInputPeak`, with a peak hold of 1.5 s.
- **Mi audio panel.** Faders `mine.voiceMonitor`, `mine.instrumentMonitor`, `mine.voiceSend`, `mine.instrumentSend` (-60..+6 dB, double click = 0 dB) and the toggle `mine.micMute`, all persisted in Settings.
- **Participantes panel.** One row per online user other than me (`people.<i>.gain`, `people.<i>.mute`, with the name), -60..+12 dB, persisted by userId in Settings.
- **Wiring.** SessionController wires the engine's chat sink to `ProjectClient::sendAudio`, the remote stream table, and the participant gains (userId → clientId via presence).
- **Screenshots.** Update the demo state and `expected-controls.json`.

**Tests:**
- DawView keys and buttons emit the right callbacks, including the ignore-while-recording rule;
- delete permissions;
- meter clip latch set/clear;
- settings round-trip of the new fields.

### Task 4: integration and docs
- **Headless:**
  - `--chat-seconds S` (or default behaviour) keeps the mic chat flowing with `--fake-audio sine`;
  - prints `chat_rx <clientId> <kind> <frames>` per remote stream at exit.
- **New `tests/integration/voice-chat.ps1` (port 22246):** host A plus client B, both with a sine fake input. Each must report receiving ≥ 80 % of the other's voice frames over 5 s without recording. Add it to run-all and the README table.
- **Existing scripts:** update any that used the listen flag or the record or stop semantics.
- **Docs:**
  - README (Spanish): new controls, shortcuts, chat de voz, auriculares;
  - spec §5 of the multitrack design (protocol);
  - `docs/plans/2026-11-13-verification.md` addendum with the results.
- **Run:** the full unit suite and run-all, then capture screenshots and check them visually.
