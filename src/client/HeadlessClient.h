#pragma once
#include "core/Protocol.h"
#include "client/FakeAudioDevice.h"
#include <juce_core/juce_core.h>
#include <optional>
namespace ensayo
{
// --headless: a client without UI driven by the fake audio device. Used by the integration scripts.
//
//   --host [--port N]        embedded ServerHost in-process (ticked from the audio callback); creates a session on
//                            127.0.0.1 and prints "link ensayo://127.0.0.1:port/ID"
//   --join <link>            joins; a link whose session id is "NEW" creates a session on that host:port instead
//                            (the real link is printed as above)
//   --name N --role voice|instrument|both
//   --fake-audio silence|sine|click   --block 64|128|256
//   --asio <driver name>     opens that real ASIO driver (48 kHz, --block samples) instead of the fake device and
//                            prints "device <name> rate R block B inputs N outputs M latencyIn I latencyOut O" plus
//                            one "input <i> <name>" / "output <i> <name>" line per channel. --emit-log and --probe
//                            need the fake device and are ignored. At exit prints "callbacks N" (audio callbacks run).
//   --mic N --inst N --out P input channel indexes for voice/instrument and the output pair (0 = channels 0/1)
//   --mute-monitor           no local monitoring of the own inputs (needed when the device output is looped back to
//                            its input, otherwise the monitor feeds back on itself); implied by --probe
//   --emit-log <csv>         "emittedQpcMs,sampleIndex" per click emitted by the fake inputs (TestSignal::click)
//   --probe <csv>            "detectedQpcMs,sampleIndex" per click detected in the device OUTPUT (ClickProbe).
//                            Local monitoring is muted so only clicks coming back from the server are counted;
//                            run the probe instance with --fake-audio silence.
//   --compare <csv>          the --emit-log of the other instance (same machine → same QPC clock): at exit prints
//                            "latency median <ms> p95 <ms> n <count>" ("latency n 0" if nothing paired)
//   --track <file> [--play]  uploads the file after joining; --play sends "play" once the track is loaded
//   --simulate-loss N        drops N % of received blocks before decoding
//   --duration S             seconds to stay in the session (default 10), then leaves and returns 0
//   --stats <file>           one JSON line per second: t, pingMs, jitterBlocks, underruns, late, participants,
//                            trackPlaying, trackPosition, peakVoice/peakInstrument/peakReceived/peakOutput (max of
//                            that second, linear), mixPackets (mix datagrams received), serverPacketsFromMe /
//                            serverWrongFrameFromMe (what the server reports receiving from this client; -1 = no stats yet)
//
// Stdout lines: "link ...", "joined <session> client <id>", "participants N" (on every change), "latency ...",
// "error <text>". Returns 0 on success, 1 on a runtime failure (bind/connect/device), 2 on bad arguments.
struct HeadlessOptions
{
    bool host = false; int hostPort = defaultPort;     // --host [--port N] : embedded ServerHost, prints "link ensayo://127.0.0.1:port/ID"
    juce::String link;                                  // --join <link>
    juce::String name = "Headless"; Role role = Role::both;
    int blockSize = 128; TestSignal signal = TestSignal::silence;   // --block N --fake-audio silence|sine|click
    juce::String asioDevice;                            // --asio <driver name>: real ASIO device instead of the fake one
    int micChannel = 0, instrumentChannel = 1, outputPair = 0;   // --mic N --inst N --out P
    bool muteMonitor = false;                           // --mute-monitor
    juce::File probeLog;                                // --probe <file>: CSV "detectedQpcMs,sampleIndex" per detected click
    juce::File emitLog;                                 // --emit-log <file>: CSV "emittedQpcMs,sampleIndex" per emitted click
    juce::File trackFile; bool playTrack = false;       // --track <file> [--play]
    int lossPercent = 0; double durationSeconds = 10;   // --simulate-loss N --duration S
    juce::File statsLog;                                // --stats <file>: one JSON line per second with ping, jitter, loss
    juce::File compareLog;                              // --compare <file>: emit log of the other instance → latency line at exit
};
std::optional<HeadlessOptions> parseHeadlessOptions (const juce::StringArray& args, juce::String& error);
int runHeadless (const HeadlessOptions&);   // blocks for duration; prints "joined <session> client <id>", "participants N", exits 0 on success

// Latency pairing used by --compare, exposed for tests: each detected click is paired with the latest emitted
// click at or before it (within one click period); returns the one-way latencies in ms, one per paired click.
std::vector<double> pairClickLatencies (const std::vector<ClickEvent>& emitted, const std::vector<ClickEvent>& detected,
                                        double maxLatencyMs = 250.0);
juce::String formatLatencyLine (std::vector<double> latenciesMs);   // "latency median <ms> p95 <ms> n <count>" | "latency n 0"
std::vector<ClickEvent> readClickLog (const juce::File&);            // CSV "qpcMs,sampleIndex" lines; others ignored

// Writes one line to the process's standard output. Works from the GUI-subsystem exe when its stdout is
// redirected by the parent (Start-Process -RedirectStandardOutput) and from console apps.
void headlessPrint (const juce::String& line);
}
