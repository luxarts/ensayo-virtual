#pragma once
#include "ui/Text.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <optional>
namespace ensayo
{
// Name, role, create/open/join, audio device panel (with latency calibration) and VST panel. Pure presentation: MainComponent wires the
// callbacks of the public controls and fills the combo boxes.
class LobbyView : public juce::Component
{
public:
    LobbyView();
    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int roleIdVoice = 1, roleIdInstrument = 2, roleIdBoth = 3;
    static int roleToId (Role);
    static Role roleFromId (int);

    juce::GroupComponent sessionGroup, audioGroup, vstGroup;
    juce::Label nameLabel, roleLabel, linkLabel, deviceLabel, rateLabel, bufferLabel, micLabel, instrumentLabel,
                outputLabel, latencyLabel, pluginLabel, calibrationLabel;
    juce::TextEditor nameEditor, linkEditor;
    juce::ComboBox roleBox, deviceBox, rateBox, bufferBox, micBox, instrumentBox, outputBox, pluginBox;
    juce::TextButton createButton, openButton, joinButton, panelButton, calibrateButton, browseButton, editorButton, bypassButton,
                     removeButton;

    void setCalibration (std::optional<int> samples);   // "Calibración: N muestras" / "sin calibrar"

private:
    void setupLabel (juce::Label&, const juce::String& text);
    JUCE_DECLARE_NON_COPYABLE (LobbyView)
};
}
