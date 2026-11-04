#pragma once
#include "ui/Text.h"
#include "ui/LevelMeter.h"
#include "client/SessionClient.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>
#include <memory>
#include <vector>
namespace ensayo
{
// A fader pair (voice / instrument) with mute buttons and meters. Used for "Yo (monitoreo)" and for each remote
// participant. Names are "<prefix>.voice.fader", "<prefix>.voice.mute", ... ; participants get one "<prefix>.meter",
// the local strip gets "<prefix>.voice.meter" and "<prefix>.instrument.meter".
class FaderStrip : public juce::Component
{
public:
    FaderStrip (const juce::String& prefix, bool isLocal);
    void resized() override;
    void paint (juce::Graphics&) override;

    void setParticipant (const ParticipantInfo&);          // name, role, enables faders by role
    void setGain (const ChannelGain& voice, const ChannelGain& instrument, juce::NotificationType);
    ChannelGain getVoiceGain() const;                      // voiceDb/instrumentDb both hold the voice value
    ChannelGain getInstrumentGain() const;
    void setMeters (float voicePeak, float instrumentPeak); // local: two meters; remote: the max into one

    std::function<void()> onChanged;                       // any fader or mute changed by the user

    uint32_t participantId = 0;
    juce::Label nameLabel, roleLabel, voiceTitle, instrumentTitle;
    juce::Slider voiceFader, instrumentFader;
    juce::TextButton voiceMute, instrumentMute;
    LevelMeter voiceMeter, instrumentMeter, meter;
    const bool local;

private:
    void setupFader (juce::Slider&, const juce::String& name);
    JUCE_DECLARE_NON_COPYABLE (FaderStrip)
};

// Link, own strip, remote strips, backing track and the per-session numbers. Fully renderable from a
// SessionState (setState); MainComponent wires the callbacks to the SessionClient.
class SessionView : public juce::Component
{
public:
    SessionView();
    ~SessionView() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    void setState (const SessionState&);                   // rebuilds strips when the participant list changed
    const SessionState& getState() const noexcept          { return state; }
    void setLink (const juce::String& link, const juce::String& hint);
    void setLocalMeters (float voicePeak, float instrumentPeak);
    void setTrackGain (const ChannelGain&, juce::NotificationType);
    void setUploadProgress (const juce::String& text, float fraction);   // text empty → idle
    int getNumParticipantStrips() const noexcept           { return (int) participantStrips.size(); }
    FaderStrip& getLocalStrip() noexcept                   { return meStrip; }

    // The VST editor is also reachable from the session (the lobby is hidden while playing).
    void setPluginEditorState (bool pluginLoaded, bool editorOpen);

    std::function<void()> onCopyLink, onLeave, onLoadFile, onToggleEditor;
    std::function<void (const juce::String& url)> onDownload;
    std::function<void (const ChannelGain& voice, const ChannelGain& instrument)> onMonitorChanged;
    std::function<void (uint32_t id, const ChannelGain&)> onParticipantGain;   // voiceDb + instrumentDb + muted (any)
    std::function<void (const ChannelGain&)> onTrackGain;
    std::function<void (const juce::String& cmd, int64_t position, bool loop)> onTransport;

private:
    void rebuildStrips();
    void updateTransport();
    void layoutStrips();
    bool sameParticipants (const SessionState&) const;

    SessionState state;
    bool hasState = false;

    juce::Label linkTitle, linkLabel, hintLabel;
    juce::TextButton copyButton, leaveButton, editorButton;

    FaderStrip meStrip;
    juce::Viewport stripViewport;
    juce::Component stripContainer;
    std::vector<std::unique_ptr<FaderStrip>> participantStrips;
    juce::Label noParticipantsLabel;

    juce::GroupComponent trackGroup;
    juce::TextButton loadButton, downloadButton, playButton, stopButton, loopButton, trackMute;
    juce::TextEditor urlEditor;
    juce::Label trackName, timeLabel, trackFaderLabel;
    juce::Slider positionSlider, trackFader;
    double uploadFraction = 0.0;
    juce::ProgressBar progressBar { uploadFraction };
    bool draggingPosition = false;

    JUCE_DECLARE_NON_COPYABLE (SessionView)
};
}
