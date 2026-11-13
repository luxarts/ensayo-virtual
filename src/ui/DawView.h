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

    bool keyPressed (const juce::KeyPress&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawView)
};
}
