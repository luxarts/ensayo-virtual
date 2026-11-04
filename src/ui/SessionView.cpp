#include "ui/SessionView.h"

namespace ensayo
{
//==============================================================================
void FaderStrip::setupFader (juce::Slider& fader, const juce::String& name)
{
    fader.setName (name);
    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setRange ((double) minFaderDb, (double) maxFaderDb, 0.1);
    fader.setValue (0.0, juce::dontSendNotification);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 18);
    fader.setTextValueSuffix (" dB");
    fader.setNumDecimalPlacesToDisplay (1);
    fader.onValueChange = [this] { if (onChanged) onChanged(); };
    addAndMakeVisible (fader);
}

FaderStrip::FaderStrip (const juce::String& prefix, bool isLocal) : local (isLocal)
{
    setName (prefix);

    nameLabel.setName (prefix + ".name");
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (nameLabel);

    roleLabel.setName (prefix + ".role");
    roleLabel.setJustificationType (juce::Justification::centred);
    roleLabel.setFont (juce::FontOptions (12.0f));
    roleLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    roleLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (roleLabel);

    voiceTitle.setText ("Voz", juce::dontSendNotification);
    voiceTitle.setJustificationType (juce::Justification::centred);
    voiceTitle.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (voiceTitle);

    instrumentTitle.setText ("Instrumento", juce::dontSendNotification);
    instrumentTitle.setJustificationType (juce::Justification::centred);
    instrumentTitle.setFont (juce::FontOptions (12.0f));
    instrumentTitle.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (instrumentTitle);

    setupFader (voiceFader, prefix + ".voice.fader");
    setupFader (instrumentFader, prefix + ".instrument.fader");

    for (auto* mute : { &voiceMute, &instrumentMute })
    {
        mute->setButtonText ("Mute");
        mute->setClickingTogglesState (true);
        mute->setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkred);
        mute->onClick = [this] { if (onChanged) onChanged(); };
        addAndMakeVisible (*mute);
    }

    voiceMute.setName (prefix + ".voice.mute");
    instrumentMute.setName (prefix + ".instrument.mute");

    if (local)
    {
        voiceMeter.setName (prefix + ".voice.meter");
        instrumentMeter.setName (prefix + ".instrument.meter");
        addAndMakeVisible (voiceMeter);
        addAndMakeVisible (instrumentMeter);
    }
    else
    {
        meter.setName (prefix + ".meter");
        addAndMakeVisible (meter);
    }
}

void FaderStrip::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (local ? juce::Colour (0xff2b3a4a) : juce::Colour (0xff2a2f36));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
}

void FaderStrip::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    nameLabel.setBounds (r.removeFromTop (22));
    roleLabel.setBounds (r.removeFromTop (16));
    r.removeFromTop (4);

    const int meterW = 12, gap = 4;

    if (! local)
        meter.setBounds (r.removeFromRight (meterW));
    else
        r.removeFromRight (0);

    const int colW = (r.getWidth() - gap) / 2;
    auto voiceCol = r.removeFromLeft (colW);
    r.removeFromLeft (gap);
    auto instrCol = r;

    auto layoutColumn = [&] (juce::Rectangle<int> col, juce::Label& title, juce::Slider& fader, juce::TextButton& mute, LevelMeter* m)
    {
        title.setBounds (col.removeFromTop (16));
        mute.setBounds (col.removeFromBottom (22).reduced (2, 0));
        col.removeFromBottom (gap);

        if (m != nullptr)
        {
            m->setBounds (col.removeFromRight (meterW).withTrimmedBottom (20));
            col.removeFromRight (gap);
        }

        fader.setBounds (col);
    };

    layoutColumn (voiceCol, voiceTitle, voiceFader, voiceMute, local ? &voiceMeter : nullptr);
    layoutColumn (instrCol, instrumentTitle, instrumentFader, instrumentMute, local ? &instrumentMeter : nullptr);
}

void FaderStrip::setParticipant (const ParticipantInfo& info)
{
    participantId = info.id;
    nameLabel.setText (info.name, juce::dontSendNotification);
    roleLabel.setText (roleToSpanish (info.role), juce::dontSendNotification);

    const bool voice = roleSendsVoice (info.role), instrument = roleSendsInstrument (info.role);
    voiceFader.setEnabled (voice);
    voiceMute.setEnabled (voice);
    instrumentFader.setEnabled (instrument);
    instrumentMute.setEnabled (instrument);
}

void FaderStrip::setGain (const ChannelGain& voice, const ChannelGain& instrument, juce::NotificationType n)
{
    voiceFader.setValue ((double) voice.voiceDb, n);
    voiceMute.setToggleState (voice.muted, n);
    instrumentFader.setValue ((double) instrument.instrumentDb, n);
    instrumentMute.setToggleState (instrument.muted, n);
}

ChannelGain FaderStrip::getVoiceGain() const
{
    const auto db = (float) voiceFader.getValue();
    return { db, db, voiceMute.getToggleState() };
}

ChannelGain FaderStrip::getInstrumentGain() const
{
    const auto db = (float) instrumentFader.getValue();
    return { db, db, instrumentMute.getToggleState() };
}

void FaderStrip::setMeters (float voicePeak, float instrumentPeak)
{
    if (local)
    {
        voiceMeter.setLevel (voicePeak);
        instrumentMeter.setLevel (instrumentPeak);
    }
    else
    {
        meter.setLevel (juce::jmax (voicePeak, instrumentPeak));
    }
}

//==============================================================================
SessionView::SessionView() : meStrip ("me", true)
{
    setName ("session");

    linkTitle.setText ("Link:", juce::dontSendNotification);
    addAndMakeVisible (linkTitle);

    linkLabel.setName ("session.link");
    linkLabel.setJustificationType (juce::Justification::centredLeft);
    linkLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff1c2128));
    linkLabel.setColour (juce::Label::outlineColourId, juce::Colours::white.withAlpha (0.2f));
    linkLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (linkLabel);

    hintLabel.setFont (juce::FontOptions (12.0f));
    hintLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    hintLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (hintLabel);

    copyButton.setName ("session.copy");
    copyButton.setButtonText ("Copiar link");
    copyButton.onClick = [this] { if (onCopyLink) onCopyLink(); };
    addAndMakeVisible (copyButton);

    leaveButton.setName ("session.leave");
    leaveButton.setButtonText ("Salir");
    leaveButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7a2e2e));
    leaveButton.onClick = [this] { if (onLeave) onLeave(); };
    addAndMakeVisible (leaveButton);

    editorButton.setName ("session.editor");
    editorButton.setButtonText ("Abrir editor VST");
    editorButton.setEnabled (false);
    editorButton.onClick = [this] { if (onToggleEditor) onToggleEditor(); };
    addAndMakeVisible (editorButton);

    meStrip.nameLabel.setText ("Yo (monitoreo)", juce::dontSendNotification);
    meStrip.roleLabel.setText ("", juce::dontSendNotification);
    meStrip.onChanged = [this]
    {
        if (onMonitorChanged)
            onMonitorChanged (meStrip.getVoiceGain(), meStrip.getInstrumentGain());
    };
    addAndMakeVisible (meStrip);

    stripViewport.setViewedComponent (&stripContainer, false);
    stripViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripViewport);

    noParticipantsLabel.setText (ES ("Todavía no hay otros participantes. Compartí el link."), juce::dontSendNotification);
    noParticipantsLabel.setJustificationType (juce::Justification::centred);
    noParticipantsLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    stripContainer.addAndMakeVisible (noParticipantsLabel);

    trackGroup.setText ("Backing track");
    addAndMakeVisible (trackGroup);

    loadButton.setName ("track.load");
    loadButton.setButtonText (ES ("Cargar archivo…"));
    loadButton.onClick = [this] { if (onLoadFile) onLoadFile(); };
    addAndMakeVisible (loadButton);

    urlEditor.setName ("track.url");
    urlEditor.setTextToShowWhenEmpty ("URL de YouTube", juce::Colours::grey);
    urlEditor.onReturnKey = [this] { if (onDownload) onDownload (urlEditor.getText().trim()); };
    addAndMakeVisible (urlEditor);

    downloadButton.setName ("track.download");
    downloadButton.setButtonText ("Descargar");
    downloadButton.onClick = [this] { if (onDownload) onDownload (urlEditor.getText().trim()); };
    addAndMakeVisible (downloadButton);

    trackName.setName ("track.name");
    trackName.setText ("(sin track)", juce::dontSendNotification);
    trackName.setJustificationType (juce::Justification::centredLeft);
    trackName.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (trackName);

    playButton.setName ("track.play");
    playButton.setButtonText ("Play");
    playButton.onClick = [this]
    {
        if (onTransport)
            onTransport (state.transport.playing ? "pause" : "play", state.transport.position, state.transport.loop);
    };
    addAndMakeVisible (playButton);

    stopButton.setName ("track.stop");
    stopButton.setButtonText ("Stop");
    stopButton.onClick = [this] { if (onTransport) onTransport ("stop", 0, state.transport.loop); };
    addAndMakeVisible (stopButton);

    loopButton.setName ("track.loop");
    loopButton.setButtonText ("Loop");
    loopButton.setClickingTogglesState (true);
    loopButton.onClick = [this] { if (onTransport) onTransport ("loop", state.transport.position, loopButton.getToggleState()); };
    addAndMakeVisible (loopButton);

    positionSlider.setName ("track.position");
    positionSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    positionSlider.setRange (0.0, 1.0, 1.0);
    positionSlider.onDragStart = [this] { draggingPosition = true; };
    positionSlider.onDragEnd = [this]
    {
        draggingPosition = false;

        if (onTransport)
            onTransport ("seek", (int64_t) positionSlider.getValue(), state.transport.loop);
    };
    addAndMakeVisible (positionSlider);

    timeLabel.setName ("track.time");
    timeLabel.setText ("0:00 / 0:00", juce::dontSendNotification);
    timeLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (timeLabel);

    trackFaderLabel.setText ("Volumen", juce::dontSendNotification);
    addAndMakeVisible (trackFaderLabel);

    trackFader.setName ("track.fader");
    trackFader.setSliderStyle (juce::Slider::LinearHorizontal);
    trackFader.setRange ((double) minFaderDb, (double) maxFaderDb, 0.1);
    trackFader.setValue (0.0, juce::dontSendNotification);
    trackFader.setDoubleClickReturnValue (true, 0.0);
    trackFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    trackFader.setTextValueSuffix (" dB");
    trackFader.setNumDecimalPlacesToDisplay (1);
    trackFader.onValueChange = [this]
    {
        if (onTrackGain)
            onTrackGain ({ (float) trackFader.getValue(), (float) trackFader.getValue(), trackMute.getToggleState() });
    };
    addAndMakeVisible (trackFader);

    trackMute.setName ("track.mute");
    trackMute.setButtonText ("Mute");
    trackMute.setClickingTogglesState (true);
    trackMute.setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkred);
    trackMute.onClick = [this]
    {
        if (onTrackGain)
            onTrackGain ({ (float) trackFader.getValue(), (float) trackFader.getValue(), trackMute.getToggleState() });
    };
    addAndMakeVisible (trackMute);

    progressBar.setName ("track.progress");
    progressBar.setPercentageDisplay (false);
    progressBar.setTextToDisplay ("");
    addAndMakeVisible (progressBar);

    rebuildStrips();
    updateTransport();
}

SessionView::~SessionView()
{
    stripViewport.setViewedComponent (nullptr, false);
}

void SessionView::paint (juce::Graphics& g)
{
    g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
}

void SessionView::setLink (const juce::String& link, const juce::String& hint)
{
    linkLabel.setText (link, juce::dontSendNotification);
    hintLabel.setText (hint, juce::dontSendNotification);
}

void SessionView::setLocalMeters (float voicePeak, float instrumentPeak)
{
    meStrip.setMeters (voicePeak, instrumentPeak);
}

void SessionView::setTrackGain (const ChannelGain& gain, juce::NotificationType n)
{
    trackFader.setValue ((double) gain.voiceDb, n);
    trackMute.setToggleState (gain.muted, n);
}

void SessionView::setPluginEditorState (bool pluginLoaded, bool editorOpen)
{
    editorButton.setEnabled (pluginLoaded);
    editorButton.setButtonText (editorOpen ? "Cerrar editor VST" : "Abrir editor VST");
}

void SessionView::setUploadProgress (const juce::String& text, float fraction)
{
    uploadFraction = (double) juce::jlimit (0.0f, 1.0f, fraction);
    progressBar.setTextToDisplay (text);
    progressBar.repaint();
}

bool SessionView::sameParticipants (const SessionState& s) const
{
    if (! hasState || s.clientId != state.clientId || s.participants.size() != state.participants.size())
        return false;

    for (size_t i = 0; i < s.participants.size(); ++i)
    {
        const auto& a = s.participants[i];
        const auto& b = state.participants[i];

        if (a.id != b.id || a.name != b.name || a.role != b.role)
            return false;
    }

    return true;
}

void SessionView::setState (const SessionState& s)
{
    const bool rebuild = ! sameParticipants (s);
    state = s;
    hasState = true;

    if (rebuild)
        rebuildStrips();

    for (auto& strip : participantStrips)
    {
        float voicePeak = 0.0f, instrumentPeak = 0.0f;

        for (const auto& st : state.stats)
        {
            if (st.id == strip->participantId)
            {
                voicePeak = st.voicePeak;
                instrumentPeak = st.instrumentPeak;
                break;
            }
        }

        strip->setMeters (voicePeak, instrumentPeak);
    }

    for (const auto& info : state.participants)
        if (info.id == state.clientId)
            meStrip.roleLabel.setText (roleToSpanish (info.role), juce::dontSendNotification);

    updateTransport();
    setUploadProgress (state.trackUploadStatus, state.trackUploadProgress);
}

void SessionView::rebuildStrips()
{
    // Keep the existing gains for participants that are still here.
    std::vector<std::pair<uint32_t, std::pair<ChannelGain, ChannelGain>>> previous;

    for (auto& strip : participantStrips)
        previous.push_back ({ strip->participantId, { strip->getVoiceGain(), strip->getInstrumentGain() } });

    participantStrips.clear();
    int n = 0;

    for (const auto& info : state.participants)
    {
        if (info.id == state.clientId)
            continue;

        auto strip = std::make_unique<FaderStrip> ("participant." + juce::String (++n), false);
        strip->setParticipant (info);

        for (const auto& p : previous)
            if (p.first == info.id)
                strip->setGain (p.second.first, p.second.second, juce::dontSendNotification);

        auto* raw = strip.get();
        strip->onChanged = [this, raw]
        {
            if (onParticipantGain == nullptr)
                return;

            const auto voice = raw->getVoiceGain();
            const auto instrument = raw->getInstrumentGain();
            ChannelGain combined { voice.muted ? minFaderDb : voice.voiceDb,
                                   instrument.muted ? minFaderDb : instrument.instrumentDb,
                                   voice.muted && instrument.muted };
            onParticipantGain (raw->participantId, combined);
        };

        stripContainer.addAndMakeVisible (*strip);
        participantStrips.push_back (std::move (strip));
    }

    noParticipantsLabel.setVisible (participantStrips.empty());
    layoutStrips();
}

void SessionView::updateTransport()
{
    const auto& t = state.transport;
    trackName.setText (t.loaded ? t.name : juce::String ("(sin track)"), juce::dontSendNotification);
    playButton.setButtonText (t.playing ? "Pausa" : "Play");
    loopButton.setToggleState (t.loop, juce::dontSendNotification);

    const auto length = juce::jmax ((int64_t) 1, t.length);
    positionSlider.setRange (0.0, (double) length, 1.0);

    if (! draggingPosition)
        positionSlider.setValue ((double) juce::jlimit ((int64_t) 0, length, t.position), juce::dontSendNotification);

    timeLabel.setText (formatTime (t.position) + " / " + formatTime (t.length), juce::dontSendNotification);

    for (auto* b : { &playButton, &stopButton, &loopButton })
        b->setEnabled (t.loaded);

    positionSlider.setEnabled (t.loaded);
}

void SessionView::layoutStrips()
{
    const int stripW = 190, gap = 8;
    const int viewH = stripViewport.getHeight();
    const int count = (int) participantStrips.size();
    const int contentW = juce::jmax (stripViewport.getWidth(), count * (stripW + gap));
    const bool needsScrollbar = count * (stripW + gap) > stripViewport.getWidth();
    const int stripH = juce::jmax (1, viewH - (needsScrollbar ? stripViewport.getScrollBarThickness() : 0));

    stripContainer.setSize (contentW, stripH);
    int x = 0;

    for (auto& strip : participantStrips)
    {
        strip->setBounds (x, 0, stripW, stripH);
        x += stripW + gap;
    }

    noParticipantsLabel.setBounds (stripContainer.getLocalBounds());
}

void SessionView::resized()
{
    auto area = getLocalBounds().reduced (16, 12);
    const int rowH = 28, gap = 8;

    // Header: link + copy + leave.
    {
        auto row = area.removeFromTop (rowH);
        leaveButton.setBounds (row.removeFromRight (90));
        row.removeFromRight (gap);
        editorButton.setBounds (row.removeFromRight (140));
        row.removeFromRight (gap);
        copyButton.setBounds (row.removeFromRight (110));
        row.removeFromRight (gap);
        linkTitle.setBounds (row.removeFromLeft (44));
        linkLabel.setBounds (row);
        hintLabel.setBounds (area.removeFromTop (18).withTrimmedLeft (44));
    }

    area.removeFromTop (gap);

    // Backing track at the bottom.
    const int trackH = 4 * rowH + 5 * gap + 20;
    auto trackArea = area.removeFromBottom (trackH);
    area.removeFromBottom (gap);

    // Strips fill the middle.
    {
        auto strips = area;
        meStrip.setBounds (strips.removeFromLeft (230));
        strips.removeFromLeft (gap);
        stripViewport.setBounds (strips);
        layoutStrips();
    }

    // Backing track.
    {
        trackGroup.setBounds (trackArea);
        auto r = trackArea.reduced (12).withTrimmedTop (14);

        auto row = r.removeFromTop (rowH);
        loadButton.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (gap);
        downloadButton.setBounds (row.removeFromRight (110));
        row.removeFromRight (gap);
        urlEditor.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        playButton.setBounds (row.removeFromLeft (80));  row.removeFromLeft (gap);
        stopButton.setBounds (row.removeFromLeft (80));  row.removeFromLeft (gap);
        loopButton.setBounds (row.removeFromLeft (80));  row.removeFromLeft (gap);
        timeLabel.setBounds (row.removeFromRight (120));
        trackName.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        positionSlider.setBounds (row);
        r.removeFromTop (gap);

        row = r.removeFromTop (rowH);
        trackFaderLabel.setBounds (row.removeFromLeft (70));
        trackMute.setBounds (row.removeFromRight (80));
        row.removeFromRight (gap);
        progressBar.setBounds (row.removeFromRight (260));
        row.removeFromRight (gap);
        trackFader.setBounds (row);
    }
}
}
