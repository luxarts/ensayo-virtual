#pragma once
#include "core/Project.h"
#include "core/Segments.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <set>
namespace ensayo
{
struct LiveRegion { Id user, take, track; int64_t start = 0; std::vector<Peak> peaks; };
// "Mi audio": my own monitor and send levels (dB, minFaderDb..+6) and the voice chat mute.
struct OwnAudio { float voiceMonitorDb = 0, instrumentMonitorDb = 0, voiceSendDb = 0, instrumentSendDb = 0; bool micMuted = false; };
// "Participantes": my personal level of another user's voice chat and live instrument (dB, minFaderDb..maxFaderDb).
struct ParticipantLevel { float gainDb = 0; bool muted = false; };
struct DawState
{
    std::shared_ptr<const Project> project; Id me; bool isHost = false; bool readOnly = false;
    juce::String link;
    std::map<Id, bool> online;                 // user → online
    std::vector<LiveRegion> live;              // takes being recorded right now (others or me)
    std::map<Id, float> takeAvailability;      // take → downloaded fraction; missing = 1
    int64_t playhead = 0; bool playing = false, recording = false;
    Id armedTrack; std::set<Id> solo; bool click = true, countIn = true;
    OwnAudio mine;
    std::map<Id, ParticipantLevel> participants;   // userId → level; missing = 0 dB, not muted
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
    // Transport: one Play/Pausa button and Space. onPlay when stopped (the owner records when an own track is armed),
    // onPause while playing or recording (a take ends). W = onHome, arrows = onSeek; both ignored while recording.
    std::function<void()> onPlay, onPause, onHome, onCopyLink, onLeave, onExport, onToggleEditor,
                          onImportFile, onAddVoice, onAddInstrument;
    std::function<void (int64_t)> onSeek;
    std::function<void (const Id&)> onArm;
    std::function<void (const Id&, bool)> onSolo;
    std::function<void (bool)> onClick, onCountIn;
    std::function<void (const juce::String& url)> onImportYouTube;
    std::function<void (const OwnAudio&)> onMine;                              // any "Mi audio" control changed
    std::function<void (const Id& user, const ParticipantLevel&)> onParticipant;
    // Asks a yes/no question and calls `confirmed` on yes. Defaults to an async alert window; tests replace it.
    std::function<void (const juce::String& question, std::function<void()> confirmed)> confirm;

    // Track meters, about 30 times a second: each track's peak since the last call (post volume, pan and mute), and
    // the armed input's peak, shown on the armed track when it is mine instead of its playback. The number holds the
    // highest peak for 1.5 s; a peak >= clipLevel latches the clip light until the meter is clicked. nowMs < 0 = now.
    void setMeterLevels (const std::vector<std::pair<Id, float>>& trackPeaks, float armedInputPeak, double nowMs = -1.0);
    static constexpr float clipLevel = 1.0f;
    bool isClipLatched (const Id& track) const;
    float getMeterHold (const Id& track) const;          // linear peak the number shows
    juce::String getMeterText (const Id& track) const;   // "-∞" or dBFS with one decimal

    // For tests and keyboard handling
    double getSamplesPerPixel() const noexcept;  void setSamplesPerPixel (double);
    int sampleToX (int64_t) const noexcept;      int64_t xToSample (int x) const noexcept;
    Id getSelectedRegion() const;                void selectRegion (const Id&);
    bool splitSelectedAtPlayhead();              // emits split_region when the playhead is inside the selection
    bool deleteSelected();                       // emits delete_region
    void dragRegion (const Id& region, int dx);  // emits move_region (start clamped at 0)
    void dragEdge (const Id& region, bool left, int dx);   // emits trim_region respecting take bounds

    bool keyPressed (const juce::KeyPress&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawView)
};
}
