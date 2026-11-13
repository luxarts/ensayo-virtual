#pragma once
#include "core/Project.h"
#include "client/FakeAudioDevice.h"
#include <juce_core/juce_core.h>
#include <optional>
#include <vector>
namespace ensayo
{
// --headless: the collaborative recorder without UI, driven by the fake audio device (or a real ASIO driver). Used by
// the integration scripts.
//
//   --host <projectDir> [--port N]   hosts that project in-process (created when it has no project.json) and connects
//                                    to it over 127.0.0.1 as the host user
//   --join <link>                    joins ensayo://host:port/ID
//   --name N                         user name (default "Headless")
//   --user <id>                      user id, 32 hex chars (default: a new one per run; --resume and re-hosting an
//                                    existing project need the same id as before)
//   --cache <dir>                    take cache + pending uploads (default %LOCALAPPDATA%\EnsayoVirtual\cache\headless\<user>)
//   --fake-audio silence|sine|click|loopback   --block N (default 256)   --latency-in N --latency-out N
//                                    fake device; loopback = input is the output delayed by the two latencies
//   --asio <driver> [--mic N --inst N --out P]   real ASIO driver at 48 kHz instead of the fake device; prints
//                                    "device <name> rate R block B inputs N outputs M latencyIn I latencyOut O", "blocks ...",
//                                    "rates ...", "input <i> <name>", "output <i> <name>" and at exit "callbacks N"
//   --mute-monitor                   no local monitoring of the own inputs (implied by --fake-audio loopback)
//   --calibrate                      runs "Calibrar latencia" on the open device; prints "calibration <samples>" or
//                                    "error ..."; the value is used for this run only (nothing is saved). Without
//                                    --host/--join the run ends there.
//   --add-track voice|instrument <name>   adds a track of mine (repeatable), waits until applied
//   --import <file>                  imports the file as a new `imported` track (region at 0), waits until queued
//   --record <trackName> --at <seconds> --for <seconds>   records on that track (mine) from timeline position --at for
//                                    --for seconds of device time; count-in and click are off
//   --ops <jsonl>                    one op object per line, sent in order (each waits until applied or rejected)
//   --wait-takes N                   waits until N takes are complete and fully cached here (timeout: --duration,
//                                    default 120 s)
//   --resume                         connects, lets pending uploads flush and finishes every take left unfinished in the
//                                    cache (a crash mid-take), then exits
//   --export <wav>                   at the end, renders the mix (24-bit WAV) from the local cache
//   --dump-project <file>            at the end, writes the replica's project.json
//   --duration S                     seconds to stay connected after the welcome (default 10); ignored with --resume
//
// Order: calibrate, connect, (resume), add tracks, import, record, ops, wait for my own takes to be complete on the
// host, wait-takes or the duration, export, dump, hash.
//
// Stdout lines: "link <ensayo://...>", "welcome <session> host|guest", "applied <version>" (every version after the
// welcome's), "rejected <reason>", "take_complete <take> <samples>" (a take that became complete during the run, and every
// take finished by --resume), "cached <take> <segments>" (a complete take fully in the local cache, once each, also the
// ones already complete at the welcome), "calibration <samples>", "exported <wav>", "project_hash <sha1>" (at exit),
// "error <text>". Returns 0 on success, 1 on a runtime failure, 2 on bad arguments.
struct HeadlessOptions
{
    struct NewTrack { TrackKind kind = TrackKind::voice; juce::String name; };

    juce::File hostProject; int hostPort = defaultPort;            // --host <projectDir> [--port N]
    juce::String link;                                              // --join <link>
    juce::String name = "Headless";                                 // --name
    juce::String user;                                              // --user <id>
    juce::File cacheDir;                                            // --cache <dir>
    TestSignal signal = TestSignal::silence; int blockSize = 256;   // --fake-audio, --block
    int latencyIn = 0, latencyOut = 0;                              // --latency-in, --latency-out
    juce::String asioDevice;                                        // --asio <driver>
    int micChannel = 0, instrumentChannel = 1, outputPair = 0;     // --mic, --inst, --out
    bool muteMonitor = false;                                       // --mute-monitor
    bool calibrate = false, resume = false;                         // --calibrate, --resume
    std::vector<NewTrack> addTracks;                                // --add-track kind name (repeatable)
    juce::String recordTrack; double recordAt = 0.0, recordFor = 0.0;   // --record <track> --at S --for S
    juce::File importFile, opsFile, exportFile, dumpProject;        // --import, --ops, --export, --dump-project
    int waitTakes = 0;                                              // --wait-takes N (0 = no wait)
    double durationSeconds = 10.0; bool durationGiven = false;      // --duration S

    bool hasSession() const noexcept { return hostProject != juce::File() || link.isNotEmpty(); }
};
std::optional<HeadlessOptions> parseHeadlessOptions (const juce::StringArray& args, juce::String& error);
int runHeadless (const HeadlessOptions&);   // message thread with JUCE initialised; blocks until done

// Writes one line to the process's standard output. Works from the GUI-subsystem exe when its stdout is
// redirected by the parent (Start-Process -RedirectStandardOutput) and from console apps.
void headlessPrint (const juce::String& line);
}
