#include "ui/LobbyView.h"

namespace ensayo
{
int LobbyView::roleToId (Role role)
{
    switch (role)
    {
        case Role::voice:      return roleIdVoice;
        case Role::instrument: return roleIdInstrument;
        case Role::both:       break;
    }

    return roleIdBoth;
}

Role LobbyView::roleFromId (int id)
{
    if (id == roleIdVoice)       return Role::voice;
    if (id == roleIdInstrument)  return Role::instrument;
    return Role::both;
}

void LobbyView::setupLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (label);
}

LobbyView::LobbyView()
{
    setName ("lobby");

    sessionGroup.setText (ES ("Sesión"));
    audioGroup.setText ("Audio");
    vstGroup.setText ("VST (instrumento)");
    addAndMakeVisible (sessionGroup);
    addAndMakeVisible (audioGroup);
    addAndMakeVisible (vstGroup);

    setupLabel (nameLabel, "Nombre");
    setupLabel (roleLabel, "Rol");
    setupLabel (linkLabel, ES ("Link de sesión"));
    setupLabel (deviceLabel, "Dispositivo ASIO");
    setupLabel (rateLabel, "Frecuencia");
    setupLabel (bufferLabel, "Buffer");
    setupLabel (micLabel, ES ("Canal micrófono"));
    setupLabel (instrumentLabel, "Canal instrumento");
    setupLabel (outputLabel, "Salida");
    setupLabel (pluginLabel, "Plugin");
    setupLabel (latencyLabel, "Latencia: --");
    latencyLabel.setName ("audio.latency");
    setupLabel (calibrationLabel, "");
    calibrationLabel.setName ("lobby.calibration");
    setCalibration (std::nullopt);

    nameEditor.setName ("lobby.name");
    nameEditor.setTextToShowWhenEmpty ("Tu nombre", juce::Colours::grey);
    addAndMakeVisible (nameEditor);

    roleBox.setName ("lobby.role");
    roleBox.addItem ("Voz", roleIdVoice);
    roleBox.addItem ("Instrumento", roleIdInstrument);
    roleBox.addItem ("Instrumento + Voz", roleIdBoth);
    roleBox.setSelectedId (roleIdBoth, juce::dontSendNotification);
    addAndMakeVisible (roleBox);

    createButton.setName ("lobby.create");
    createButton.setButtonText (ES ("Crear sesión"));
    createButton.setTooltip ("Crea un proyecto nuevo y lo comparte");
    addAndMakeVisible (createButton);

    openButton.setName ("lobby.open");
    openButton.setButtonText (ES ("Abrir proyecto…"));
    openButton.setTooltip ("Abre un proyecto guardado y lo comparte");
    addAndMakeVisible (openButton);

    linkEditor.setName ("lobby.link");
    linkEditor.setTextToShowWhenEmpty ("ensayo://host:puerto/ID", juce::Colours::grey);
    addAndMakeVisible (linkEditor);

    joinButton.setName ("lobby.join");
    joinButton.setButtonText ("Unirse");
    addAndMakeVisible (joinButton);

    deviceBox.setName ("audio.device");
    deviceBox.setTextWhenNothingSelected (ES ("(sin dispositivo)"));
    deviceBox.setTextWhenNoChoicesAvailable ("No se encontraron drivers ASIO");
    addAndMakeVisible (deviceBox);

    rateBox.setName ("audio.rate");
    addAndMakeVisible (rateBox);

    bufferBox.setName ("audio.buffer");
    addAndMakeVisible (bufferBox);

    panelButton.setName ("audio.panel");
    panelButton.setButtonText ("Panel ASIO");
    addAndMakeVisible (panelButton);

    calibrateButton.setName ("lobby.calibrate");
    calibrateButton.setButtonText ("Calibrar latencia");
    calibrateButton.setTooltip (ES ("Conectá la salida elegida a la entrada del micrófono (cable o loopback de la placa)"));
    addAndMakeVisible (calibrateButton);

    micBox.setName ("audio.mic");
    addAndMakeVisible (micBox);

    instrumentBox.setName ("audio.instrument");
    addAndMakeVisible (instrumentBox);

    outputBox.setName ("audio.output");
    addAndMakeVisible (outputBox);

    pluginBox.setName ("vst.list");
    pluginBox.setTextWhenNothingSelected ("(ninguno)");
    addAndMakeVisible (pluginBox);

    browseButton.setName ("vst.browse");
    browseButton.setButtonText (ES ("Examinar…"));
    addAndMakeVisible (browseButton);

    editorButton.setName ("vst.editor");
    editorButton.setButtonText ("Abrir editor");
    addAndMakeVisible (editorButton);

    bypassButton.setName ("vst.bypass");
    bypassButton.setButtonText ("Bypass");
    bypassButton.setClickingTogglesState (true);
    addAndMakeVisible (bypassButton);

    removeButton.setName ("vst.remove");
    removeButton.setButtonText ("Quitar");
    addAndMakeVisible (removeButton);
}

void LobbyView::setCalibration (std::optional<int> samples)
{
    calibrationLabel.setText (samples ? ES ("Calibración: ") + juce::String (*samples) + " muestras"
                                      : ES ("Calibración: sin calibrar"),
                              juce::dontSendNotification);
}

void LobbyView::paint (juce::Graphics& g)
{
    g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
}

void LobbyView::resized()
{
    auto area = getLocalBounds().reduced (16);
    const int rowH = 28, gap = 8, labelW = 140;

    auto left = area.removeFromLeft (juce::jmax (380, area.getWidth() * 2 / 5));
    area.removeFromLeft (16);
    auto right = area;

    // Session group.
    {
        sessionGroup.setBounds (left.withHeight (7 * rowH + 9 * gap + 32));
        auto r = sessionGroup.getBounds().reduced (12).withTrimmedTop (16);

        auto row = r.removeFromTop (rowH);
        nameLabel.setBounds (row.removeFromLeft (labelW));
        nameEditor.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        roleLabel.setBounds (row.removeFromLeft (labelW));
        roleBox.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH + 4).withTrimmedLeft (labelW);
        createButton.setBounds (row.removeFromLeft ((row.getWidth() - gap) / 2));
        row.removeFromLeft (gap);
        openButton.setBounds (row);
        r.removeFromTop (gap * 2);

        row = r.removeFromTop (rowH);
        linkLabel.setBounds (row.removeFromLeft (labelW));
        linkEditor.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH + 4);
        joinButton.setBounds (row.withTrimmedLeft (labelW));
    }

    // Audio group.
    {
        audioGroup.setBounds (right.removeFromTop (8 * rowH + 9 * gap + 24));
        auto r = audioGroup.getBounds().reduced (12).withTrimmedTop (16);

        auto row = r.removeFromTop (rowH);
        deviceLabel.setBounds (row.removeFromLeft (labelW));
        deviceBox.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        rateLabel.setBounds (row.removeFromLeft (labelW));
        const int half = (row.getWidth() - gap) / 2;
        rateBox.setBounds (row.removeFromLeft (half));
        row.removeFromLeft (gap);
        {
            auto bufferRow = row;
            bufferLabel.setBounds (bufferRow.removeFromLeft (60));
            bufferBox.setBounds (bufferRow);
        }
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        micLabel.setBounds (row.removeFromLeft (labelW));
        micBox.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        instrumentLabel.setBounds (row.removeFromLeft (labelW));
        instrumentBox.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        outputLabel.setBounds (row.removeFromLeft (labelW));
        outputBox.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        panelButton.setBounds (row.removeFromLeft (labelW));
        row.removeFromLeft (gap);
        latencyLabel.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        calibrateButton.setBounds (row.removeFromLeft (labelW));
        row.removeFromLeft (gap);
        calibrationLabel.setBounds (row);
    }

    right.removeFromTop (16);

    // VST group.
    {
        vstGroup.setBounds (right.removeFromTop (2 * rowH + 3 * gap + 24));
        auto r = vstGroup.getBounds().reduced (12).withTrimmedTop (16);

        auto row = r.removeFromTop (rowH);
        pluginLabel.setBounds (row.removeFromLeft (labelW));
        pluginBox.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH).withTrimmedLeft (labelW);
        const int w = (row.getWidth() - 3 * gap) / 4;
        browseButton.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap);
        editorButton.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap);
        bypassButton.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap);
        removeButton.setBounds (row);
    }
}
}
