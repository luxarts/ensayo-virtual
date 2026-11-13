#include "ui/DawView.h"
#include "ui/Text.h"
#include <algorithm>
#include <cmath>

namespace ensayo
{
namespace
{
    constexpr int topBarHeight = 44, transportHeight = 46, columnHeaderHeight = 22, rowHeight = 64;
    constexpr int headerWidth = 366, sideWidth = 236, meterWidth = 30, scrollBarWidth = 10, footerHeight = 84, edgeZone = 6, regionInset = 6;
    constexpr int64_t minRegionLength = 480;

    const juce::Colour backgroundColour (0xff1c2128), barColour (0xff171b20), transportColour (0xff20262d),
                       lineColour (0xff2c333b), mutedText (0xff7d8a98), liveColour (0xffe05a4c);

    const juce::Colour palette[8] = { juce::Colour (0xff3b82f6), juce::Colour (0xff22a06b), juce::Colour (0xffe08a2e),
                                      juce::Colour (0xff8e6bd8), juce::Colour (0xffd6559b), juce::Colour (0xff1fb5c9),
                                      juce::Colour (0xffc9a227), juce::Colour (0xff9aa84a) };

    juce::Colour authorColour (const Project& p, const Id& author)
    {
        for (size_t i = 0; i < p.users.size(); ++i)
            if (p.users[i].id == author)
                return palette[i % 8];

        return juce::Colour (0xff7d8a98);
    }

    juce::Font plainFont (float h)  { return juce::Font (juce::FontOptions (h)); }
    juce::Font boldFont (float h)   { return juce::Font (juce::FontOptions (h, juce::Font::bold)); }

    void styleButton (juce::Button& b, const juce::String& name, const juce::String& text)
    {
        b.setName (name);
        b.setButtonText (text);
        b.setWantsKeyboardFocus (false);
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2b333c));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffdde3ea));
    }

    juce::String kindText (TrackKind k)
    {
        switch (k)
        {
            case TrackKind::voice:       return "Voz";
            case TrackKind::instrument:  return "Instr.";
            case TrackKind::imported:    break;
        }

        return "Importada";
    }

    juce::String panText (float pan)
    {
        const int n = (int) std::lround (std::abs (pan) * 100.0f);
        return n == 0 ? juce::String ("C") : juce::String (pan < 0 ? "I " : "D ") + juce::String (n);
    }

    juce::String dbText (double db) { return juce::String (db, 1) + " dB"; }

    // Personal faders: their minimum is silence.
    juce::String faderText (double db) { return db <= (double) minFaderDb ? ES ("-\xE2\x88\x9E dB") : dbText (db); }

    juce::String peakText (float peak)
    {
        const float db = peak > 0.0f ? juce::Decibels::gainToDecibels (peak, -200.0f) : -200.0f;
        return db < -99.9f ? ES ("-\xE2\x88\x9E") : (db >= 0.05f ? "+" : "") + juce::String (db, 1);
    }

    constexpr double meterHoldMs = 1500.0;

    struct MeterState
    {
        float level = 0.0f;       // bar, with a fall
        float hold = 0.0f;        // number: highest peak in the last meterHoldMs
        double holdUntilMs = 0.0;
        bool clip = false;        // latched until the meter is clicked

        void push (float peak, double nowMs, float clipLevel)
        {
            peak = std::isfinite (peak) ? juce::jmax (0.0f, peak) : 0.0f;
            level = juce::jmax (peak, level * 0.8f);

            if (level < 1.0e-5f)
                level = 0.0f;

            if (peak >= hold)
            {
                hold = peak;
                holdUntilMs = nowMs + meterHoldMs;
            }
            else if (nowMs >= holdUntilMs)
            {
                hold = peak;
            }

            if (peak >= clipLevel)
                clip = true;
        }
    };

    void styleFader (juce::Slider& s, const juce::String& n, double lo, double hi)
    {
        s.setName (n);
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setRange (lo, hi, 0.1);
        s.setDoubleClickReturnValue (true, 0.0);
        s.setWantsKeyboardFocus (false);
        s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff11151a));
        s.setColour (juce::Slider::trackColourId, juce::Colour (0xff3b82f6));
        s.setColour (juce::Slider::thumbColourId, juce::Colour (0xffdde3ea));
    }

    void styleSmallLabel (juce::Label& l, const juce::String& n, juce::Colour colour)
    {
        l.setName (n);
        l.setFont (juce::Font (juce::FontOptions (11.5f)));
        l.setColour (juce::Label::textColourId, colour);
        l.setInterceptsMouseClicks (false, false);
        l.setBorderSize (juce::BorderSize<int> (0, 2, 0, 2));
        l.setMinimumHorizontalScale (0.8f);
    }
}

//==============================================================================
struct DawView::Impl
{
    //==========================================================================
    struct Swatch : juce::Component
    {
        juce::Colour colour;
        void paint (juce::Graphics& g) override { g.fillAll (colour); }
    };

    struct UsersStrip : juce::Component
    {
        struct Chip { juce::String text; juce::Colour colour; };
        std::vector<Chip> chips;

        void paint (juce::Graphics& g) override
        {
            int x = 0;
            g.setFont (plainFont (12.0f));
            g.setColour (juce::Colour (0xffdde3ea));
            const auto caption = juce::String ("En la sesi") + ES ("\xC3\xB3") + "n:";
            const int captionWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), caption) + 6;
            g.drawText (caption, 0, 0, captionWidth, getHeight(), juce::Justification::centredLeft, false);
            x = captionWidth + 4;

            for (auto& chip : chips)
            {
                const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), chip.text) + 12;

                if (x + w > getWidth())
                {
                    g.setColour (mutedText);
                    g.drawText ("...", x, 0, 24, getHeight(), juce::Justification::centredLeft, false);
                    break;
                }

                const auto r = juce::Rectangle<int> (x, (getHeight() - 20) / 2, w, 20);
                g.setColour (chip.colour);
                g.fillRoundedRectangle (r.toFloat(), 10.0f);
                g.setColour (juce::Colours::white);
                g.drawText (chip.text, r, juce::Justification::centred, false);
                x += w + 6;
            }
        }
    };

    //==========================================================================
    struct TrackMeter : juce::Component, public juce::SettableTooltipClient
    {
        Impl& impl;
        Id trackId;

        TrackMeter (Impl& i, const juce::String& n) : impl (i)
        {
            setName (n);
            setTooltip (ES ("Pico en dBFS. Clic: apaga el testigo de clip"));
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            if (auto it = impl.meters.find (trackId); it != impl.meters.end() && it->second.clip)
            {
                it->second.clip = false;
                repaint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            const auto it = impl.meters.find (trackId);
            const auto m = it != impl.meters.end() ? it->second : MeterState();
            auto r = getLocalBounds();
            g.setColour (juce::Colour (0xff101418));
            g.fillRoundedRectangle (r.toFloat(), 2.0f);

            auto text = r.removeFromBottom (14);
            g.setColour (m.clip ? juce::Colour (0xffff6b5e) : juce::Colour (0xffc3ccd6));
            g.setFont (plainFont (10.0f));
            g.drawText (peakText (m.hold), text, juce::Justification::centred, false);

            auto bar = r.reduced (9, 3);
            const auto led = bar.removeFromTop (5);
            g.setColour (m.clip ? juce::Colour (0xffff3b30) : juce::Colour (0xff3a2626));
            g.fillRect (led);
            bar.removeFromTop (2);

            g.setColour (juce::Colour (0xff1d2329));
            g.fillRect (bar);
            const float db = juce::Decibels::gainToDecibels (m.level, -60.0f);
            const float fraction = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            const int h = juce::roundToInt ((float) bar.getHeight() * fraction);
            g.setColour (db > -6.0f ? juce::Colours::orange : juce::Colour (0xff4caf50));
            g.fillRect (bar.removeFromBottom (h));
        }
    };

    struct TrackHeader : juce::Component, private juce::Timer
    {
        Impl& impl;
        Id trackId;
        bool draggingVolume = false, draggingPan = false, pendingVolume = false, pendingPan = false;
        double sentVolume = 0.0, sentPan = 0.0;   // what the project last said (or what we last emitted)
        Swatch swatch;
        juce::Label name, author, kind, lock, volumeText, panLabel;
        juce::TextButton mute, solo, arm, remove;
        juce::Slider volume, pan;
        TrackMeter meter;

        TrackHeader (Impl& i, int number) : impl (i), meter (i, "track." + juce::String (number) + ".meter")
        {
            const auto prefix = "track." + juce::String (number);
            setName (prefix);
            addAndMakeVisible (meter);

            styleButton (remove, prefix + ".delete", ES ("\xC3\x97"));
            remove.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe08a80));
            remove.onClick = [this] { impl.askDeleteTrack (trackId); };
            addAndMakeVisible (remove);

            swatch.setName (prefix + ".color");
            addAndMakeVisible (swatch);

            name.setName (prefix + ".name");
            name.setFont (boldFont (14.0f));
            name.setMinimumHorizontalScale (1.0f);
            name.setBorderSize (juce::BorderSize<int> (0, 2, 0, 2));
            name.setColour (juce::Label::textColourId, juce::Colour (0xffe8edf2));
            name.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff11151a));
            name.setColour (juce::Label::textWhenEditingColourId, juce::Colours::white);
            name.onTextChange = [this] { renameFromLabel(); };
            addAndMakeVisible (name);

            for (auto* l : { &author, &kind })
            {
                l->setFont (plainFont (11.0f));
                l->setColour (juce::Label::textColourId, mutedText);
                l->setInterceptsMouseClicks (false, false);
                l->setBorderSize (juce::BorderSize<int> (0, 2, 0, 2));
                addAndMakeVisible (*l);
            }

            author.setName (prefix + ".author");
            kind.setName (prefix + ".kind");

            lock.setName (prefix + ".lock");
            lock.setText ("Bloq.", juce::dontSendNotification);
            lock.setFont (plainFont (11.0f));
            lock.setColour (juce::Label::textColourId, juce::Colour (0xff6c7886));
            lock.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (lock);

            styleButton (mute, prefix + ".mute", "M");
            mute.setClickingTogglesState (true);
            mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffb03a2e));
            mute.onClick = [this] { if (impl.v.onOp) impl.v.onOp (Ops::setMix (trackId, std::nullopt, std::nullopt, mute.getToggleState())); };
            addAndMakeVisible (mute);

            styleButton (solo, prefix + ".solo", "S");
            solo.setClickingTogglesState (true);
            solo.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc9a227));
            solo.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            solo.onClick = [this] { if (impl.v.onSolo) impl.v.onSolo (trackId, solo.getToggleState()); };
            addAndMakeVisible (solo);

            styleButton (arm, prefix + ".arm", ES ("\xE2\x97\x8F") + "R");
            arm.setClickingTogglesState (true);
            arm.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffb03a2e));
            arm.setColour (juce::TextButton::textColourOffId, liveColour);
            arm.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            arm.onClick = [this] { if (impl.v.onArm) impl.v.onArm (arm.getToggleState() ? trackId : Id()); };
            addAndMakeVisible (arm);

            setupSlider (volume, prefix + ".volume", -60.0, 12.0, 0.1);
            setupSlider (pan, prefix + ".pan", -1.0, 1.0, 0.01);

            for (auto* l : { &volumeText, &panLabel })
            {
                l->setFont (plainFont (11.0f));
                l->setColour (juce::Label::textColourId, juce::Colour (0xffc3ccd6));
                l->setInterceptsMouseClicks (false, false);
                l->setBorderSize (juce::BorderSize<int> (0, 2, 0, 0));
                addAndMakeVisible (*l);
            }

            volumeText.setName (prefix + ".volume.text");
            panLabel.setName (prefix + ".pan.text");

            volume.onDragStart = [this] { draggingVolume = true; };
            volume.onValueChange = [this]
            {
                volumeText.setText (dbText (volume.getValue()), juce::dontSendNotification);

                if (! draggingVolume)
                    schedule (pendingVolume);   // wheel or keyboard nudge: coalesced into one op
            };
            volume.onDragEnd = [this] { draggingVolume = false; pendingVolume = false; emitMix (true); };

            pan.onDragStart = [this] { draggingPan = true; };
            pan.onValueChange = [this]
            {
                panLabel.setText (panText ((float) pan.getValue()), juce::dontSendNotification);

                if (! draggingPan)
                    schedule (pendingPan);
            };
            pan.onDragEnd = [this] { draggingPan = false; pendingPan = false; emitMix (false); };
        }

        void setupSlider (juce::Slider& s, const juce::String& n, double lo, double hi, double step)
        {
            s.setName (n);
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setRange (lo, hi, step);
            s.setDoubleClickReturnValue (true, 0.0);
            s.setWantsKeyboardFocus (false);
            s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff11151a));
            addAndMakeVisible (s);
        }

        void schedule (bool& pending)
        {
            pending = true;
            startTimer (300);
        }

        void timerCallback() override
        {
            stopTimer();

            if (pendingVolume)  { pendingVolume = false; emitMix (true); }
            if (pendingPan)     { pendingPan = false; emitMix (false); }
        }

        // Emits only when the value differs from what the project holds (or what was last emitted).
        void emitMix (bool isVolume)
        {
            if (! impl.v.onOp || ! impl.canEdit (currentTrack()))
                return;

            if (isVolume)
            {
                if (volume.getValue() == sentVolume)  return;
                sentVolume = volume.getValue();
                impl.v.onOp (Ops::setMix (trackId, (float) sentVolume, std::nullopt, std::nullopt));
            }
            else
            {
                if (pan.getValue() == sentPan)  return;
                sentPan = pan.getValue();
                impl.v.onOp (Ops::setMix (trackId, std::nullopt, (float) sentPan, std::nullopt));
            }
        }

        const Track& currentTrack() const
        {
            static const Track none;
            const auto* p = impl.currentProject();
            const auto* t = p != nullptr ? p->findTrack (trackId) : nullptr;
            return t != nullptr ? *t : none;
        }

        void renameFromLabel()
        {
            const auto* track = impl.currentProject() != nullptr ? impl.currentProject()->findTrack (trackId) : nullptr;
            const auto text = clampName (name.getText());

            if (track == nullptr || text.isEmpty() || text == track->name || ! impl.canEdit (*track))
            {
                if (track != nullptr)
                    name.setText (track->name, juce::dontSendNotification);

                return;
            }

            if (impl.v.onOp)
                impl.v.onOp (Ops::renameTrack (trackId, text));
        }

        void update (const Project& p, const Track& t)
        {
            trackId = t.id;
            const bool editable = impl.canEdit (t);              // false while I am recording
            // Spec 7: only the author records on a track (the host may edit any track but not arm it); read-only
            // locks everything.
            const bool own = ! impl.state.readOnly && t.author == impl.state.me;
            const auto colour = authorColour (p, t.author);

            swatch.colour = colour;
            swatch.repaint();

            if (! name.isBeingEdited())
                name.setText (t.name, juce::dontSendNotification);

            name.setEditable (false, editable, true);
            author.setText (ES ("\xC2\xB7") + " " + p.userName (t.author), juce::dontSendNotification);
            kind.setText (ES ("\xC2\xB7") + " " + kindText (t.kind), juce::dontSendNotification);

            mute.setToggleState (t.muted, juce::dontSendNotification);
            solo.setToggleState (impl.state.solo.count (t.id) > 0, juce::dontSendNotification);
            arm.setToggleState (impl.state.armedTrack == t.id, juce::dontSendNotification);
            arm.setVisible (own && t.kind != TrackKind::imported);
            lock.setVisible (! own);
            lock.setTooltip (impl.state.readOnly ? ES ("S\xC3\xB3lo lectura: el host est\xC3\xA1 desconectado")
                                                 : ES ("S\xC3\xB3lo su autor puede grabar en esta pista"));

            if (! draggingVolume && ! pendingVolume)  { volume.setValue (t.gainDb, juce::dontSendNotification); sentVolume = volume.getValue(); }
            if (! draggingPan && ! pendingPan)        { pan.setValue (t.pan, juce::dontSendNotification); sentPan = pan.getValue(); }

            volumeText.setText (dbText (volume.getValue()), juce::dontSendNotification);
            panLabel.setText (panText ((float) pan.getValue()), juce::dontSendNotification);

            for (auto* c : std::initializer_list<juce::Component*> { &mute, &volume, &pan })
                c->setEnabled (editable);

            volume.setColour (juce::Slider::trackColourId, colour);
            volume.setColour (juce::Slider::thumbColourId, colour.brighter (0.3f));
            pan.setColour (juce::Slider::trackColourId, colour.withAlpha (0.5f));
            pan.setColour (juce::Slider::thumbColourId, colour.brighter (0.3f));
            arm.setEnabled (! impl.state.readOnly);

            remove.setEnabled (impl.canDeleteTrack (t));
            remove.setTooltip (impl.deleteTooltip (t));

            if (meter.trackId != t.id)
            {
                meter.trackId = t.id;
                meter.repaint();
            }
        }

        void resized() override
        {
            swatch.setBounds (0, 0, 5, getHeight());
            name.setBounds (10, 6, 140, 22);
            author.setBounds (152, 6, 76, 22);
            kind.setBounds (228, 6, 68, 22);
            remove.setBounds (300, 6, 26, 22);
            meter.setBounds (getWidth() - meterWidth - 5, 3, meterWidth, getHeight() - 7);

            const int y = 36, h = 22;
            mute.setBounds (10, y, 24, h);
            solo.setBounds (37, y, 24, h);
            arm.setBounds (64, y, 32, h);
            lock.setBounds (64, y, 38, h);
            volume.setBounds (104, y, 76, h);
            volumeText.setBounds (182, y, 56, h);
            pan.setBounds (240, y, 44, h);
            panLabel.setBounds (286, y, 40, h);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (transportColour);
            g.setColour (lineColour);
            g.fillRect (0, getHeight() - 1, getWidth(), 1);
            g.fillRect (getWidth() - 1, 0, 1, getHeight());
        }
    };

    //==========================================================================
    enum class DragMode { none, move, trimLeft, trimRight };

    struct Timeline : juce::Component
    {
        Impl& impl;
        Id dragRegionId;
        DragMode mode = DragMode::none;
        int dragDx = 0;

        explicit Timeline (Impl& i) : impl (i) { setName ("timeline"); }

        struct Hit { const Track* track = nullptr; const Region* region = nullptr; DragMode mode = DragMode::none; };

        Hit hitTest (juce::Point<int> pos) const
        {
            Hit hit;
            const auto* p = impl.currentProject();

            if (p == nullptr || pos.y < 0)
                return hit;

            const size_t lane = (size_t) (pos.y / rowHeight);

            if (lane >= p->tracks.size())
                return hit;

            const auto& track = p->tracks[lane];

            for (auto it = track.regions.rbegin(); it != track.regions.rend(); ++it)   // top-most first
            {
                const auto r = impl.regionRect ((int) lane, *it);

                if (! r.contains (pos))
                    continue;

                const int zone = juce::jmin (edgeZone, r.getWidth() / 3);
                hit.track = &track;
                hit.region = &*it;
                hit.mode = pos.x - r.getX() < zone ? DragMode::trimLeft
                         : r.getRight() - pos.x <= zone ? DragMode::trimRight : DragMode::move;
                return hit;
            }

            return hit;
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const auto hit = hitTest (e.getPosition());
            const bool edge = hit.region != nullptr && impl.canEdit (*hit.track) && hit.mode != DragMode::move;
            setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            impl.v.grabKeyboardFocus();
            const auto hit = hitTest (e.getPosition());
            mode = DragMode::none;
            dragDx = 0;

            if (hit.region != nullptr && impl.canEdit (*hit.track))
            {
                impl.select (hit.region->id);
                dragRegionId = hit.region->id;
                mode = hit.mode;
            }
            else
            {
                impl.select ({});
            }

            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (mode == DragMode::none)
                return;

            dragDx = e.getDistanceFromDragStartX();
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const auto finished = mode;
            const auto id = dragRegionId;
            const int dx = e.mouseWasDraggedSinceMouseDown() ? e.getDistanceFromDragStartX() : 0;
            mode = DragMode::none;
            dragDx = 0;

            if (finished != DragMode::none && dx != 0)
            {
                if (finished == DragMode::move)           impl.v.dragRegion (id, dx);
                else                                      impl.v.dragEdge (id, finished == DragMode::trimLeft, dx);
            }

            repaint();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (e.mods.isCtrlDown() || e.mods.isCommandDown())
            {
                const double factor = std::pow (1.35, -(double) wheel.deltaY * 4.0);
                const auto anchor = impl.xToSample (e.x);
                impl.spp = juce::jlimit (1.0, 5.0e6, impl.spp * factor);
                impl.userZoomed = true;
                impl.originSample = juce::jmax ((int64_t) 0, anchor - (int64_t) std::llround ((double) e.x * impl.spp));
                impl.repaintTimeline();
            }
            else if (e.mods.isShiftDown())
            {
                impl.viewport.setViewPosition (impl.viewport.getViewPositionX(),
                                               impl.viewport.getViewPositionY() - (int) std::lround (wheel.deltaY * 300.0f));
            }
            else
            {
                const float d = wheel.deltaX != 0.0f ? -wheel.deltaX : wheel.deltaY;
                impl.scrollBy ((int) std::lround ((double) d * -220.0));
            }
        }

        void paint (juce::Graphics& g) override
        {
            const auto clip = g.getClipBounds();
            const auto* p = impl.currentProject();
            g.fillAll (backgroundColour);

            if (p == nullptr)
                return;

            const int lanes = (int) p->tracks.size();

            for (int i = 0; i < lanes; ++i)
            {
                const auto lane = juce::Rectangle<int> (0, i * rowHeight, getWidth(), rowHeight);

                if (! lane.intersects (clip))
                    continue;

                if (i % 2 == 1)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.02f));
                    g.fillRect (lane);
                }
            }

            impl.forEachBarLine (clip.getX(), clip.getRight(), [&] (int x, int64_t, bool major)
            {
                g.setColour (juce::Colours::white.withAlpha (major ? 0.07f : 0.03f));
                g.fillRect (x, clip.getY(), 1, clip.getHeight());
            });

            g.setColour (lineColour);

            for (int i = 1; i <= lanes; ++i)
                g.fillRect (0, i * rowHeight - 1, getWidth(), 1);

            if (lanes == 0)
            {
                g.setColour (mutedText);
                g.setFont (plainFont (14.0f));
                g.drawText (ES ("Sin pistas: agrega una pista de voz o de instrumento"), getLocalBounds().withHeight (rowHeight * 2),
                            juce::Justification::centred, true);
            }

            for (int i = 0; i < lanes; ++i)
            {
                const auto& track = p->tracks[(size_t) i];
                const auto lane = juce::Rectangle<int> (0, i * rowHeight, getWidth(), rowHeight);

                if (! lane.intersects (clip))
                    continue;

                const float alpha = track.muted ? 0.45f : 1.0f;
                const auto colour = authorColour (*p, track.author);

                for (auto& region : track.regions)
                    drawRegion (g, i, track, region, colour, alpha, clip);

                for (auto& live : impl.state.live)
                    if (live.track == track.id)
                        drawLive (g, i, live, p->userName (live.user), clip);
            }

            drawPlayhead (g, impl.state.playhead, 0, getHeight());
        }

        void drawRegion (juce::Graphics& g, int lane, const Track& track, const Region& stored, juce::Colour author, float alpha, juce::Rectangle<int> clip)
        {
            Region region = stored;

            if (mode != DragMode::none && stored.id == dragRegionId && dragDx != 0)
                if (auto edited = impl.editedRegion (stored, mode, dragDx))
                    region = *edited;

            const auto r = impl.regionRect (lane, region);

            if (! r.intersects (clip))
                return;

            float availability = 1.0f;
            const auto found = impl.state.takeAvailability.find (region.take);

            if (found != impl.state.takeAvailability.end())
            {
                availability = found->second;
            }
            else if (const auto* take = impl.currentProject()->findTake (region.take))
            {
                const auto needed = region.offset + region.length;

                if (take->samples < needed && needed > 0)
                    availability = (float) take->samples / (float) needed;
            }

            const bool downloading = availability < 1.0f;
            const auto fill = downloading ? juce::Colour (0xff4a5058) : author.darker (0.45f);
            const auto border = downloading ? juce::Colour (0xff8a949f) : author.brighter (0.15f);
            const auto rf = r.toFloat();

            g.setColour (fill.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (rf, 4.0f);

            if (! downloading)
            {
                const auto& peaks = impl.peaksOf (region.take);

                if (! peaks.empty())
                {
                    juce::Graphics::ScopedSaveState saved (g);
                    g.reduceClipRegion (r.withTrimmedTop (14).withTrimmedBottom (2).reduced (1, 0));
                    impl.drawWave (g, r, peaks, region.offset, author.brighter (0.7f).withAlpha (0.75f * alpha));
                }
            }

            const bool isSelected = region.id == impl.selected;
            g.setColour ((isSelected ? juce::Colours::white : border).withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (rf.reduced (isSelected ? 1.0f : 0.5f), 4.0f, isSelected ? 2.0f : 1.0f);

            g.setColour (juce::Colours::white.withAlpha (0.9f * alpha));
            g.setFont (plainFont (11.0f));
            auto label = track.name;

            if (downloading)
                label << "  -  descargando " << juce::String ((int) std::floor (availability * 100.0f)) << " %";

            g.drawText (label, r.reduced (5, 1).withHeight (14), juce::Justification::centredLeft, true);
        }

        void drawLive (juce::Graphics& g, int lane, const LiveRegion& live, const juce::String& who, juce::Rectangle<int> clip)
        {
            Region asRegion;
            asRegion.start = live.start;
            asRegion.length = juce::jmax ((int64_t) 1, (int64_t) live.peaks.size() * peakBlock);
            const auto r = impl.regionRect (lane, asRegion);

            if (! r.intersects (clip))
                return;

            g.setColour (juce::Colour (0xff7a2a24));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);

            {
                juce::Graphics::ScopedSaveState saved (g);
                g.reduceClipRegion (r.withTrimmedTop (14).withTrimmedBottom (2).reduced (1, 0));
                impl.drawWave (g, r, live.peaks, 0, juce::Colour (0xffffb4aa).withAlpha (0.8f));
            }

            g.setColour (liveColour);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 4.0f, 1.0f);
            g.setColour (juce::Colours::white);
            g.setFont (plainFont (11.0f));
            const auto text = ES ("\xE2\x97\x8F") + " grabando (" + who + ")";
            const int textWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text);

            if (r.getWidth() >= textWidth + 10)
            {
                g.drawText (text, r.reduced (5, 1).withHeight (14), juce::Justification::centredLeft, true);
            }
            else   // too narrow yet: the label rides to the right of the growing region
            {
                g.setColour (liveColour);
                g.drawText (text, r.getRight() + 4, r.getY() + 1, textWidth + 4, 14, juce::Justification::centredLeft, false);
            }
        }

        void drawPlayhead (juce::Graphics& g, int64_t sample, int y, int h)
        {
            const int x = impl.sampleToX (sample);

            if (x >= -2 && x <= getWidth())
            {
                g.setColour (juce::Colour (0xfff5f5f5).withAlpha (0.9f));
                g.fillRect (x, y, 2, h);
            }
        }
    };

    //==========================================================================
    struct Ruler : juce::Component
    {
        Impl& impl;
        explicit Ruler (Impl& i) : impl (i) { setName ("timeline.ruler"); }

        void seekTo (int x)
        {
            if (impl.v.onSeek)
                impl.v.onSeek (juce::jmax ((int64_t) 0, impl.xToSample (x)));
        }

        void mouseDown (const juce::MouseEvent& e) override { impl.v.grabKeyboardFocus(); seekTo (e.x); }
        void mouseDrag (const juce::MouseEvent& e) override { seekTo (e.x); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (barColour);
            g.setColour (lineColour);
            g.fillRect (0, getHeight() - 1, getWidth(), 1);
            g.setFont (plainFont (11.0f));

            impl.forEachBarLine (0, getWidth(), [&] (int x, int64_t bar, bool major)
            {
                g.setColour (juce::Colour (0xff39414a));
                g.fillRect (x, major ? 0 : getHeight() / 2, 1, major ? getHeight() : getHeight() / 2);

                if (major)
                {
                    g.setColour (mutedText);
                    g.drawText (juce::String (bar + 1), x + 3, 0, 48, getHeight(), juce::Justification::centredLeft, false);
                }
            });

            const int x = impl.sampleToX (impl.state.playhead);

            if (x >= -2 && x <= getWidth())
            {
                g.setColour (juce::Colour (0xfff5f5f5).withAlpha (0.9f));
                g.fillRect (x, 0, 2, getHeight());
            }
        }
    };

    //==========================================================================
    struct Body : juce::Component
    {
        void paint (juce::Graphics& g) override { g.fillAll (backgroundColour); }
    };

    //==========================================================================
    // A caption, its value and a horizontal dB slider, stacked (40 px).
    struct Fader
    {
        juce::Label caption, value;
        juce::Slider slider;

        void setup (juce::Component& parent, const juce::String& name, const juce::String& text, double maxDb)
        {
            styleSmallLabel (caption, name + ".caption", juce::Colour (0xffc3ccd6));
            caption.setText (text, juce::dontSendNotification);
            styleSmallLabel (value, name + ".text", juce::Colour (0xffdde3ea));
            value.setJustificationType (juce::Justification::centredRight);
            styleFader (slider, name, (double) minFaderDb, maxDb);

            for (auto* c : std::initializer_list<juce::Component*> { &caption, &value, &slider })
                parent.addAndMakeVisible (*c);
        }

        void showValue() { value.setText (faderText (slider.getValue()), juce::dontSendNotification); }

        void set (float db)
        {
            if (! slider.isMouseButtonDown())
                slider.setValue (db, juce::dontSendNotification);

            showValue();
        }

        void layout (juce::Rectangle<int> r)
        {
            auto top = r.removeFromTop (17);
            value.setBounds (top.removeFromRight (64));
            caption.setBounds (top);
            slider.setBounds (r.removeFromTop (22));
        }
    };

    static void paintPanel (juce::Graphics& g, juce::Component& c, const juce::String& title)
    {
        g.fillAll (transportColour);
        g.setColour (lineColour);
        g.fillRect (0, 0, 1, c.getHeight());
        g.fillRect (0, c.getHeight() - 1, c.getWidth(), 1);
        g.setColour (juce::Colour (0xffe8edf2));
        g.setFont (boldFont (13.0f));
        g.drawText (title, 12, 4, c.getWidth() - 20, 22, juce::Justification::centredLeft, false);
    }

    struct MinePanel : juce::Component
    {
        Impl& impl;
        Fader voiceMonitor, instrumentMonitor, voiceSend, instrumentSend;
        juce::ToggleButton micMute;

        explicit MinePanel (Impl& i) : impl (i)
        {
            setName ("mine");
            const auto maxDb = (double) maxOwnFaderDb;
            voiceMonitor.setup (*this, "mine.voiceMonitor", ES ("Voz \xE2\x80\x93 monitor"), maxDb);
            instrumentMonitor.setup (*this, "mine.instrumentMonitor", ES ("Instrumento \xE2\x80\x93 monitor"), maxDb);
            voiceSend.setup (*this, "mine.voiceSend", ES ("Voz \xE2\x80\x93 env\xC3\xADo"), maxDb);
            instrumentSend.setup (*this, "mine.instrumentSend", ES ("Instrumento \xE2\x80\x93 env\xC3\xADo"), maxDb);
            voiceMonitor.slider.setTooltip (ES ("Cu\xC3\xA1nto te escuch\xC3\xA1s (s\xC3\xB3lo con una pista de voz propia armada)"));
            instrumentMonitor.slider.setTooltip (ES ("Cu\xC3\xA1nto te escuch\xC3\xA1s (s\xC3\xB3lo con una pista de instrumento propia armada)"));
            voiceSend.slider.setTooltip (ES ("Ganancia del micr\xC3\xB3" "fono antes de grabar y del chat de voz: queda en la toma"));
            instrumentSend.slider.setTooltip (ES ("Ganancia del instrumento antes de grabar: queda en la toma"));

            for (auto* f : { &voiceMonitor, &instrumentMonitor, &voiceSend, &instrumentSend })
                f->slider.onValueChange = [this, f] { f->showValue(); emit(); };

            micMute.setName ("mine.micMute");
            micMute.setButtonText (ES ("Silenciar mi micr\xC3\xB3" "fono"));
            micMute.setWantsKeyboardFocus (false);
            micMute.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffdde3ea));
            micMute.setTooltip (ES ("Deja de enviar tu voz al chat; no cambia la grabaci\xC3\xB3n ni el monitor"));
            micMute.onClick = [this] { emit(); };
            addAndMakeVisible (micMute);
        }

        OwnAudio current() const
        {
            return { (float) voiceMonitor.slider.getValue(), (float) instrumentMonitor.slider.getValue(),
                     (float) voiceSend.slider.getValue(), (float) instrumentSend.slider.getValue(), micMute.getToggleState() };
        }

        void emit()
        {
            impl.state.mine = current();

            if (impl.v.onMine)
                impl.v.onMine (impl.state.mine);
        }

        void update (const OwnAudio& m)
        {
            voiceMonitor.set (m.voiceMonitorDb);
            instrumentMonitor.set (m.instrumentMonitorDb);
            voiceSend.set (m.voiceSendDb);
            instrumentSend.set (m.instrumentSendDb);
            micMute.setToggleState (m.micMuted, juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12, 0).withTrimmedTop (28);

            for (auto* f : { &voiceMonitor, &instrumentMonitor, &voiceSend, &instrumentSend })
            {
                f->layout (r.removeFromTop (40));
                r.removeFromTop (2);
            }

            micMute.setBounds (r.removeFromTop (26));
        }

        void paint (juce::Graphics& g) override { paintPanel (g, *this, "Mi audio"); }
    };

    struct PersonRow : juce::Component
    {
        Impl& impl;
        Id user;
        Fader fader;
        juce::TextButton mute;

        PersonRow (Impl& i, int number) : impl (i)
        {
            const auto prefix = "people." + juce::String (number);
            setName (prefix);
            fader.setup (*this, prefix + ".gain", {}, (double) maxFaderDb);
            fader.caption.setName (prefix + ".name");
            fader.caption.setFont (boldFont (12.0f));
            fader.caption.setColour (juce::Label::textColourId, juce::Colour (0xffe8edf2));
            fader.slider.onValueChange = [this] { fader.showValue(); emit(); };

            styleButton (mute, prefix + ".mute", "M");
            mute.setClickingTogglesState (true);
            mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffb03a2e));
            mute.setTooltip (ES ("Silenciar a este integrante (s\xC3\xB3lo para vos)"));
            mute.onClick = [this] { emit(); };
            addAndMakeVisible (mute);
        }

        void emit()
        {
            const ParticipantLevel level { (float) fader.slider.getValue(), mute.getToggleState() };
            impl.state.participants[user] = level;

            if (impl.v.onParticipant)
                impl.v.onParticipant (user, level);
        }

        void update (const Project& p, const Id& id)
        {
            user = id;
            const auto it = impl.state.participants.find (id);
            const auto level = it != impl.state.participants.end() ? it->second : ParticipantLevel();
            fader.caption.setText (p.userName (id), juce::dontSendNotification);
            fader.set (level.gainDb);
            mute.setToggleState (level.muted, juce::dontSendNotification);
            fader.slider.setColour (juce::Slider::trackColourId, authorColour (p, id));
        }

        void resized() override
        {
            auto r = getLocalBounds();
            mute.setBounds (r.removeFromRight (26).withTrimmedTop (17).withHeight (22));
            r.removeFromRight (4);
            fader.layout (r);
        }
    };

    struct PeoplePanel : juce::Component
    {
        Impl& impl;
        std::vector<Id> ids;
        std::vector<std::unique_ptr<PersonRow>> rows;
        juce::Viewport viewport;
        juce::Component list;

        explicit PeoplePanel (Impl& i) : impl (i)
        {
            setName ("people");
            viewport.setViewedComponent (&list, false);
            viewport.setScrollBarsShown (true, false);
            viewport.setScrollBarThickness (8);
            addAndMakeVisible (viewport);
        }

        // One row per online user other than me, in the project's user order.
        void update()
        {
            std::vector<Id> next;
            const auto* p = impl.currentProject();

            if (p != nullptr)
                for (const auto& u : p->users)
                    if (const auto o = impl.state.online.find (u.id); u.id != impl.state.me && o != impl.state.online.end() && o->second)
                        next.push_back (u.id);

            if (next != ids)
            {
                ids = next;
                rows.clear();

                for (size_t i = 0; i < ids.size(); ++i)
                {
                    rows.push_back (std::make_unique<PersonRow> (impl, (int) i + 1));
                    list.addAndMakeVisible (*rows.back());
                }

                resized();
                repaint();
            }

            if (p != nullptr)
                for (size_t i = 0; i < rows.size(); ++i)
                    rows[i]->update (*p, ids[i]);
        }

        void resized() override
        {
            viewport.setBounds (getLocalBounds().withTrimmedTop (28).withTrimmedLeft (12).withTrimmedRight (4).withTrimmedBottom (4));
            const int rowH = 46, w = juce::jmax (1, viewport.getWidth() - 10);
            list.setSize (w, juce::jmax (1, (int) rows.size() * rowH));

            for (size_t i = 0; i < rows.size(); ++i)
                rows[i]->setBounds (0, (int) i * rowH, w, rowH - 4);
        }

        void paint (juce::Graphics& g) override
        {
            paintPanel (g, *this, "Participantes");

            if (rows.empty())
            {
                g.setColour (mutedText);
                g.setFont (plainFont (12.0f));
                g.drawText (ES ("Nadie m\xC3\xA1s en la sesi\xC3\xB3n"), 12, 30, getWidth() - 20, 20, juce::Justification::centredLeft, false);
            }
        }
    };

    //==========================================================================
    DawView& v;
    DawState state;
    const PeaksProvider* peaksProvider = nullptr;

    double spp = 1000.0;
    bool userZoomed = false;
    int64_t originSample = 0;
    Id selected;
    struct CachedPeaks { std::vector<Peak> peaks; int64_t samples = -1; float availability = -1.0f; };
    std::map<Id, CachedPeaks> peaksCache;
    std::vector<Id> headerIds;
    std::vector<std::unique_ptr<TrackHeader>> headers;
    juce::Label projectName, link, bpmLabel, meterLabel, timeLabel;
    UsersStrip users;
    juce::ToggleButton click, countIn;
    juce::TextButton copy, editor, leave, home, play, split, del, exportMix;
    juce::TextButton addVoice, addInstrument, importFile;
    juce::TextEditor youtube;
    juce::Rectangle<int> bpmCaption, meterCaption;
    juce::Viewport viewport;
    Body body;
    Timeline timeline;
    Ruler ruler;
    std::map<Id, MeterState> meters;   // by track id: survives header rebuilds; the clip latches are local
    MinePanel mine;
    PeoplePanel people;

    explicit Impl (DawView& owner) : v (owner), timeline (*this), ruler (*this), mine (*this), people (*this)
    {
        auto label = [this] (juce::Label& l, const juce::String& n, float size, bool bold)
        {
            l.setName (n);
            l.setFont (bold ? boldFont (size) : plainFont (size));
            l.setColour (juce::Label::textColourId, juce::Colour (0xffe8edf2));
            v.addAndMakeVisible (l);
        };

        label (projectName, "daw.project", 16.0f, true);
        label (link, "daw.link", 12.0f, false);
        link.setColour (juce::Label::textColourId, juce::Colour (0xff9fb0c2));
        link.setColour (juce::Label::backgroundColourId, juce::Colour (0xff262d35));
        link.setBorderSize (juce::BorderSize<int> (2, 8, 2, 8));
        link.setMinimumHorizontalScale (0.8f);

        styleButton (copy, "daw.copy", "Copiar link");
        styleButton (editor, "daw.editor", ES ("Editor VST"));
        styleButton (leave, "daw.leave", "Salir");
        leave.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7a2e2e));
        leave.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        users.setName ("daw.users");
        users.setInterceptsMouseClicks (false, false);

        for (auto* t : { &click, &countIn })
        {
            t->setWantsKeyboardFocus (false);
            t->setColour (juce::ToggleButton::textColourId, juce::Colour (0xffdde3ea));
        }

        click.setName ("transport.click");
        click.setButtonText ("Click");
        countIn.setName ("transport.countin");
        countIn.setButtonText ("Cuenta");

        styleButton (home, "transport.home", "|<");
        styleButton (play, "transport.play", ES ("\xE2\x96\xB6") + " Play");
        play.setClickingTogglesState (false);
        play.setTooltip (ES ("Barra espaciadora. Con una pista propia armada, Play graba. W: al inicio; "
                             "\xE2\x86\x90/\xE2\x86\x92: 1 s; May\xC3\xBA" "s+\xE2\x86\x90/\xE2\x86\x92: un comp\xC3\xA1s"));
        styleButton (split, "edit.split", ES ("\xE2\x9C\x82") + " Cortar (S)");
        styleButton (del, "edit.delete", "Borrar (Supr)");
        styleButton (exportMix, "daw.export", ES ("Exportar mezcla\xE2\x80\xA6"));
        styleButton (addVoice, "tracks.addVoice", "+ Pista de voz");
        styleButton (addInstrument, "tracks.addInstrument", "+ Pista de instrumento");
        styleButton (importFile, "tracks.import", ES ("Importar archivo\xE2\x80\xA6"));

        timeLabel.setName ("transport.time");
        timeLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::plain)));
        timeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        timeLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff11151a));
        timeLabel.setJustificationType (juce::Justification::centred);

        for (auto* l : { &bpmLabel, &meterLabel })
        {
            l->setFont (boldFont (13.0f));
            l->setColour (juce::Label::textColourId, juce::Colours::white);
            l->setColour (juce::Label::backgroundColourId, juce::Colour (0xff11151a));
            l->setColour (juce::Label::textWhenEditingColourId, juce::Colours::white);
            l->setJustificationType (juce::Justification::centred);
        }

        bpmLabel.setName ("transport.bpm");
        meterLabel.setName ("transport.meter");
        bpmLabel.onTextChange = [this] { commitTempo(); };
        meterLabel.onTextChange = [this] { commitTempo(); };

        youtube.setName ("tracks.youtube");
        youtube.setTextToShowWhenEmpty ("URL de YouTube", juce::Colours::grey);
        youtube.setFont (plainFont (12.0f));
        youtube.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff11151a));
        youtube.setColour (juce::TextEditor::outlineColourId, lineColour);
        youtube.onReturnKey = [this]
        {
            const auto url = youtube.getText().trim();

            if (url.isNotEmpty() && v.onImportYouTube)
            {
                v.onImportYouTube (url);
                youtube.clear();
            }
        };

        for (auto* c : std::initializer_list<juce::Component*> { &users, &copy, &editor, &leave, &home, &play, &mine, &people,
                                                                 &timeLabel, &bpmLabel, &meterLabel, &click, &countIn, &split, &del,
                                                                 &exportMix, &ruler, &viewport })
            v.addAndMakeVisible (*c);

        for (auto* c : std::initializer_list<juce::Component*> { &timeline, &addVoice, &addInstrument, &importFile, &youtube })
            body.addAndMakeVisible (*c);

        viewport.setViewedComponent (&body, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (scrollBarWidth);

        copy.onClick = [this] { if (v.onCopyLink) v.onCopyLink(); };
        editor.onClick = [this] { if (v.onToggleEditor) v.onToggleEditor(); };
        leave.onClick = [this] { if (v.onLeave) v.onLeave(); };
        home.onClick = [this] { if (v.onHome) v.onHome(); };
        play.onClick = [this] { togglePlay(); };
        split.onClick = [this] { v.splitSelectedAtPlayhead(); };
        del.onClick = [this] { v.deleteSelected(); };
        exportMix.onClick = [this] { if (v.onExport) v.onExport(); };
        addVoice.onClick = [this] { if (v.onAddVoice) v.onAddVoice(); };
        addInstrument.onClick = [this] { if (v.onAddInstrument) v.onAddInstrument(); };
        importFile.onClick = [this] { if (v.onImportFile) v.onImportFile(); };
        click.onClick = [this] { if (v.onClick) v.onClick (click.getToggleState()); };
        countIn.onClick = [this] { if (v.onCountIn) v.onCountIn (countIn.getToggleState()); };

        applyState();
    }

    //==========================================================================
    const Project* currentProject() const noexcept { return state.project.get(); }

    // Ownership (author or host) and read-only only: what selection clean-up follows.
    bool canEditBase (const Track& t) const
    {
        return ! state.readOnly && currentProject() != nullptr && canEditTrack (*currentProject(), t, state.me);
    }

    // While I am recording, editing is locked as well.
    bool canEdit (const Track& t) const { return ! state.recording && canEditBase (t); }

    bool isRecordingOn (const Track& t) const
    {
        if (state.recording && state.armedTrack == t.id)
            return true;

        return std::any_of (state.live.begin(), state.live.end(), [&] (const LiveRegion& l) { return l.track == t.id; });
    }

    // The author or the host, never in read-only mode nor while someone records on the track.
    bool canDeleteTrack (const Track& t) const { return canEditBase (t) && ! isRecordingOn (t); }

    juce::String deleteTooltip (const Track& t) const
    {
        if (state.readOnly)
            return ES ("S\xC3\xB3lo lectura: el host est\xC3\xA1 desconectado");

        if (currentProject() == nullptr || ! canEditTrack (*currentProject(), t, state.me))
            return ES ("S\xC3\xB3lo el autor de la pista o el host pueden borrarla");

        if (isRecordingOn (t))
            return ES ("No se puede borrar una pista mientras se graba en ella");

        return "Borrar la pista";
    }

    void askDeleteTrack (const Id& id)
    {
        const auto* t = currentProject() != nullptr ? currentProject()->findTrack (id) : nullptr;

        if (t == nullptr || ! canDeleteTrack (*t) || ! v.confirm)
            return;

        const auto question = ES ("\xC2\xBF" "Borrar la pista \xC2\xAB") + t->name + ES ("\xC2\xBB? Sus regiones desaparecen del proyecto.");
        v.confirm (question, [safe = juce::Component::SafePointer<DawView> (&v), id]
        {
            if (safe != nullptr)
                safe->impl->deleteTrack (id);
        });
    }

    void deleteTrack (const Id& id)
    {
        const auto* t = currentProject() != nullptr ? currentProject()->findTrack (id) : nullptr;

        if (t == nullptr || ! canDeleteTrack (*t))   // the state may have changed while the question was open
            return;

        if (state.armedTrack == id && v.onArm)
            v.onArm ({});

        if (v.onOp)
            v.onOp (Ops::deleteTrack (id));
    }

    void togglePlay()
    {
        if (state.playing || state.recording)  { if (v.onPause) v.onPause(); }
        else                                   { if (v.onPlay) v.onPlay(); }
    }

    // Seeks by delta samples from the playhead, never before 0; not while recording.
    void nudge (int64_t delta)
    {
        if (! state.recording && v.onSeek)
            v.onSeek (juce::jmax ((int64_t) 0, state.playhead + delta));
    }

    // My armed track records an input: its meter shows that input instead of the playback.
    bool showsArmedInput (const Track& t) const
    {
        return t.id == state.armedTrack && t.author == state.me && t.kind != TrackKind::imported;
    }

    int timelineWidth() const { return juce::jmax (1, timeline.getWidth()); }

    int sampleToX (int64_t s) const noexcept { return (int) std::lround ((double) (s - originSample) / spp); }
    int64_t xToSample (int x) const noexcept { return originSample + (int64_t) std::llround ((double) x * spp); }

    void repaintTimeline() { timeline.repaint(); ruler.repaint(); }

    void scrollBy (int pixels)
    {
        originSample = juce::jmax ((int64_t) 0, originSample + (int64_t) std::llround ((double) pixels * spp));
        repaintTimeline();
    }

    void fitZoom()
    {
        const auto* p = currentProject();
        const int64_t end = p != nullptr ? p->endSample() : 0;
        const int64_t span = juce::jmax ((int64_t) 2 * sampleRate, end + (int64_t) 10 * sampleRate);
        spp = (double) span / (double) timelineWidth();
        originSample = 0;
    }

    int64_t barSamples() const
    {
        const auto* p = currentProject();
        const double bpm = p != nullptr ? juce::jmax (1.0, p->bpm) : 120.0;
        const int beats = p != nullptr ? juce::jmax (1, p->beatsPerBar) : 4;
        return juce::jmax ((int64_t) 1, (int64_t) std::llround ((double) beats * 60.0 / bpm * sampleRate));
    }

    // Calls fn (x, barIndex, major) for each grid line in [x0, x1]; "major" lines carry the bar number.
    template <typename Fn>
    void forEachBarLine (int x0, int x1, Fn&& fn) const
    {
        const double bar = (double) barSamples();
        int64_t step = 1;

        while ((double) step * bar / spp < 56.0 && step < (int64_t) 1 << 40)
            step *= 2;

        const auto first = juce::jmax ((int64_t) 0, xToSample (x0) / barSamples() - 1);

        for (int64_t b = first - first % step; ; b += step)
        {
            const int x = sampleToX (b * barSamples());

            if (x > x1)
                break;

            if (x >= x0 - 1)
                fn (x, b, true);

            if (step == 1 && bar / spp >= 160.0)
            {
                const int beats = juce::jmax (1, currentProject() != nullptr ? currentProject()->beatsPerBar : 4);

                for (int i = 1; i < beats; ++i)
                {
                    const int bx = sampleToX (b * barSamples() + (int64_t) std::llround (bar * i / beats));

                    if (bx >= x0 - 1 && bx <= x1)
                        fn (bx, b, false);
                }
            }
        }
    }

    juce::Rectangle<int> regionRect (int lane, const Region& r) const
    {
        const int x1 = sampleToX (r.start), x2 = juce::jmax (x1 + 2, sampleToX (r.end()));
        return { x1, lane * rowHeight + regionInset, x2 - x1, rowHeight - 2 * regionInset };
    }

    const std::vector<Peak>& peaksOf (const Id& take)
    {
        static const std::vector<Peak> none;

        if (peaksProvider == nullptr)
            return none;

        // Peaks only grow, so a take is re-fetched only when its known length or download progress changed.
        const auto* known = currentProject() != nullptr ? currentProject()->findTake (take) : nullptr;
        const int64_t samples = known != nullptr ? known->samples : 0;
        const auto availability = state.takeAvailability.find (take);
        const float fraction = availability != state.takeAvailability.end() ? availability->second : 1.0f;
        auto it = peaksCache.find (take);

        if (it != peaksCache.end() && it->second.samples == samples && it->second.availability == fraction)
            return it->second.peaks;

        auto peaks = peaksProvider->peaksFor (take);

        if (peaks.empty())
            return it != peaksCache.end() ? it->second.peaks : none;   // not cached: the peaks may still arrive

        auto& entry = peaksCache[take];
        entry.peaks = std::move (peaks);
        entry.samples = samples;
        entry.availability = fraction;
        return entry.peaks;
    }

    // One pixel column per min/max over the peaks that cover it; `firstSample` is the take sample under r's left edge.
    void drawWave (juce::Graphics& g, juce::Rectangle<int> r, const std::vector<Peak>& peaks, int64_t firstSample, juce::Colour colour) const
    {
        if (peaks.empty())
            return;

        const auto clip = g.getClipBounds().getIntersection (r);
        const float cy = (float) r.getCentreY() + 5.0f, half = ((float) r.getHeight() - 20.0f) * 0.5f;
        g.setColour (colour);

        for (int x = clip.getX(); x < clip.getRight(); ++x)
        {
            const double s0 = (double) firstSample + (double) (x - r.getX()) * spp;
            const auto i0 = (int64_t) std::floor (juce::jmax (0.0, s0) / peakBlock);
            auto i1 = (int64_t) std::floor (juce::jmax (0.0, s0 + spp - 1.0) / peakBlock);

            if (i0 >= (int64_t) peaks.size())
                break;

            i1 = juce::jlimit (i0, (int64_t) peaks.size() - 1, i1);
            float lo = peaks[(size_t) i0].min, hi = peaks[(size_t) i0].max;

            for (auto i = i0 + 1; i <= i1; ++i)
            {
                lo = juce::jmin (lo, peaks[(size_t) i].min);
                hi = juce::jmax (hi, peaks[(size_t) i].max);
            }

            hi = juce::jlimit (-1.0f, 1.0f, hi);
            lo = juce::jlimit (-1.0f, 1.0f, lo);
            const float top = cy - hi * half, bottom = cy - lo * half;
            g.fillRect (juce::Rectangle<float> ((float) x, top, 1.0f, juce::jmax (1.0f, bottom - top)));
        }
    }

    //==========================================================================
    const Track* trackOfRegion (const Id& region, const Region** found = nullptr) const
    {
        if (const auto* p = currentProject())
            for (auto& t : p->tracks)
                for (auto& r : t.regions)
                    if (r.id == region)
                    {
                        if (found != nullptr)
                            *found = &r;

                        return &t;
                    }

        return nullptr;
    }

    // The region as it would be after dragging by dx pixels; nullopt for a mode that changes nothing.
    std::optional<Region> editedRegion (const Region& r, DragMode m, int dx) const
    {
        const auto delta = (int64_t) std::llround ((double) dx * spp);
        Region out = r;
        const auto* take = currentProject() != nullptr ? currentProject()->findTake (r.take) : nullptr;
        const auto minLength = juce::jmin (minRegionLength, r.length);

        switch (m)
        {
            case DragMode::move:
                out.start = juce::jmax ((int64_t) 0, r.start + delta);
                return out;

            case DragMode::trimLeft:
            {
                const auto lowest = juce::jmax ((int64_t) 0, r.start - r.offset);   // not before the take's first sample or time 0
                const auto highest = r.end() - minLength;
                out.start = juce::jlimit (lowest, juce::jmax (lowest, highest), r.start + delta);
                out.offset = r.offset + (out.start - r.start);
                out.length = r.length - (out.start - r.start);
                return out;
            }

            case DragMode::trimRight:
            {
                const auto longest = take != nullptr ? take->samples - r.offset : r.length + delta;
                out.length = juce::jlimit (minLength, juce::jmax (minLength, longest), r.length + delta);
                return out;
            }

            case DragMode::none:
                break;
        }

        return std::nullopt;
    }

    void emitEdited (const Id& id, DragMode m, int dx)
    {
        const Region* region = nullptr;
        const auto* track = trackOfRegion (id, &region);

        if (track == nullptr || ! canEdit (*track) || ! v.onOp)
            return;

        const auto edited = editedRegion (*region, m, dx);

        if (! edited)
            return;

        if (m == DragMode::move)
            v.onOp (Ops::moveRegion (track->id, id, edited->start));
        else
            v.onOp (Ops::trimRegion (track->id, id, edited->start, edited->offset, edited->length));
    }

    void select (const Id& region)
    {
        if (state.recording)
            return;   // editing (selection included) is locked while I record

        if (region.isNotEmpty())
        {
            const auto* track = trackOfRegion (region);

            if (track == nullptr || ! canEdit (*track))
            {
                selected = {};
                updateEditButtons();
                timeline.repaint();
                return;
            }
        }

        selected = region;
        updateEditButtons();
        timeline.repaint();
    }

    void updateEditButtons()
    {
        const auto* track = selected.isNotEmpty() ? trackOfRegion (selected) : nullptr;
        const bool can = track != nullptr && canEdit (*track);
        split.setEnabled (can);
        del.setEnabled (can);

        // No imports while I am recording (an import would compete with the take for the upload queue).
        importFile.setEnabled (! state.recording && ! state.readOnly);
        youtube.setEnabled (! state.recording && ! state.readOnly);
    }

    void commitTempo()
    {
        const auto* p = currentProject();

        if (p == nullptr || ! state.isHost || state.readOnly || state.recording)
        {
            refreshTempoLabels();
            return;
        }

        const double bpm = bpmLabel.getText().getDoubleValue();

        // The model has no denominator: accept "N" or "N/4" with N in [1, 12], reject anything else.
        const auto meter = meterLabel.getText().trim();
        const auto numerator = meter.upToFirstOccurrenceOf ("/", false, false).trim();
        const auto denominator = meter.contains ("/") ? meter.fromFirstOccurrenceOf ("/", false, false).trim() : juce::String ("4");
        const int beats = numerator.getIntValue();
        const bool meterOk = numerator.isNotEmpty() && numerator.containsOnly ("0123456789") && denominator == "4" && beats >= 1 && beats <= 12;

        if (bpm >= 30.0 && bpm <= 300.0 && meterOk)
        {
            if ((bpm != p->bpm || beats != p->beatsPerBar) && v.onOp)
                v.onOp (Ops::setTempo (bpm, beats));
        }

        refreshTempoLabels();
    }

    void refreshTempoLabels()
    {
        const auto* p = currentProject();

        if (p == nullptr)
            return;

        if (! bpmLabel.isBeingEdited())    bpmLabel.setText (juce::String (p->bpm, std::fabs (p->bpm - std::round (p->bpm)) < 0.005 ? 0 : 1), juce::dontSendNotification);
        if (! meterLabel.isBeingEdited())  meterLabel.setText (juce::String (p->beatsPerBar) + "/4", juce::dontSendNotification);
    }

    juce::String timeText() const
    {
        const auto* p = currentProject();
        const int64_t s = juce::jmax ((int64_t) 0, state.playhead);
        const auto totalMs = s * 1000 / sampleRate;
        auto text = juce::String::formatted ("%d:%02d.%03d", (int) (totalMs / 60000), (int) (totalMs / 1000 % 60), (int) (totalMs % 1000));

        if (p != nullptr)
        {
            const double beats = (double) s / sampleRate * p->bpm / 60.0;
            const int bpb = juce::jmax (1, p->beatsPerBar);
            const auto wholeBeats = (int64_t) std::floor (beats);
            text << " " << ES ("\xC2\xB7") << " " << (wholeBeats / bpb + 1) << "." << (wholeBeats % bpb + 1) << "."
                 << (int) ((beats - (double) wholeBeats) * 4.0) + 1;
        }

        return text;
    }

    //==========================================================================
    void applyState()
    {
        const auto* p = currentProject();
        const bool hostEditable = state.isHost && ! state.readOnly && ! state.recording;

        projectName.setText (p != nullptr ? p->name : juce::String(), juce::dontSendNotification);
        link.setText (state.link, juce::dontSendNotification);
        timeLabel.setText (timeText(), juce::dontSendNotification);
        bpmLabel.setEditable (hostEditable, false, true);
        meterLabel.setEditable (hostEditable, false, true);
        bpmLabel.setEnabled (hostEditable);
        meterLabel.setEnabled (hostEditable);
        refreshTempoLabels();

        click.setToggleState (state.click, juce::dontSendNotification);
        countIn.setToggleState (state.countIn, juce::dontSendNotification);
        const bool running = state.playing || state.recording;
        play.setToggleState (running, juce::dontSendNotification);
        play.setButtonText (running ? ES ("\xE2\x9D\x9A\xE2\x9D\x9A Pausa") : ES ("\xE2\x96\xB6 Play"));
        play.setColour (juce::TextButton::buttonOnColourId, state.recording ? juce::Colour (0xffb03a2e) : juce::Colour (0xff2f7a4f));
        for (auto* b : { &addVoice, &addInstrument, &importFile })
            b->setEnabled (! state.readOnly);
        youtube.setEnabled (! state.readOnly);

        users.chips.clear();

        if (p != nullptr)
            for (auto& u : p->users)
            {
                const auto o = state.online.find (u.id);

                if (u.id != state.me && (o == state.online.end() || ! o->second))
                    continue;

                auto text = u.name;

                if (u.id == p->hostUser)
                    text << " (host)";

                for (auto& l : state.live)
                    if (l.user == u.id)
                    {
                        text << " " << ES ("\xE2\x97\x8F") << " grabando";
                        break;
                    }

                users.chips.push_back ({ text, authorColour (*p, u.id) });
            }

        users.repaint();
        mine.update (state.mine);
        people.update();

        // Headers follow the track list.
        std::vector<Id> ids;

        if (p != nullptr)
            for (auto& t : p->tracks)
                ids.push_back (t.id);

        const bool rebuilt = ids != headerIds;

        if (rebuilt)
        {
            headers.clear();
            headerIds = ids;

            for (size_t i = 0; i < ids.size(); ++i)
            {
                headers.push_back (std::make_unique<TrackHeader> (*this, (int) i + 1));
                body.addAndMakeVisible (*headers.back());
            }
        }

        if (p != nullptr)
            for (size_t i = 0; i < headers.size(); ++i)
                headers[i]->update (*p, p->tracks[i]);

        if (selected.isNotEmpty())
        {
            const auto* track = trackOfRegion (selected);

            if (track == nullptr || ! canEditBase (*track))
                selected = {};
        }

        updateEditButtons();

        if (rebuilt)
            layoutBody();

        if (! userZoomed)
            fitZoom();
        else if (state.playing)
        {
            const int x = sampleToX (state.playhead);

            if (x < 0 || x > timelineWidth())
                originSample = juce::jmax ((int64_t) 0, state.playhead);
        }

        repaintTimeline();
    }

    void layoutBody()
    {
        const int count = (int) headers.size();
        const int width = juce::jmax (headerWidth + 1, viewport.getWidth() - scrollBarWidth);
        const int height = juce::jmax (viewport.getHeight(), count * rowHeight + footerHeight);

        body.setSize (width, height);
        timeline.setBounds (headerWidth, 0, width - headerWidth, height);

        for (int i = 0; i < count; ++i)
            headers[(size_t) i]->setBounds (0, i * rowHeight, headerWidth, rowHeight);

        const int y = count * rowHeight + 10;
        addVoice.setBounds (8, y, 110, 24);
        addInstrument.setBounds (124, y, 160, 24);
        importFile.setBounds (8, y + 32, 130, 24);
        youtube.setBounds (144, y + 32, 176, 24);
    }
};

//==============================================================================
DawView::DawView() : impl (std::make_unique<Impl> (*this))
{
    confirm = [this] (const juce::String& question, std::function<void()> confirmed)
    {
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon, "Borrar pista",
                                                                     question, "Borrar", "Cancelar", this);
        juce::AlertWindow::showAsync (options, [confirmed = std::move (confirmed)] (int result)
        {
            if (result == 1 && confirmed)
                confirmed();
        });
    };

    setWantsKeyboardFocus (true);
    setVisible (true);
    setSize (1100, 760);
}

DawView::~DawView() = default;

void DawView::setState (const DawState& s)
{
    impl->state = s;

    impl->applyState();
}

const DawState& DawView::getState() const noexcept { return impl->state; }

void DawView::setPeaksProvider (const PeaksProvider* p)
{
    impl->peaksProvider = p;
    impl->peaksCache.clear();
    impl->repaintTimeline();
}

void DawView::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop (topBarHeight).reduced (10, 7);
    auto bar = area.removeFromTop (transportHeight).reduced (10, 8);

    // Top bar: project, link, copy | who is online | editor, leave
    impl->projectName.setBounds (top.removeFromLeft (130));
    top.removeFromLeft (6);
    impl->link.setBounds (top.removeFromLeft (215));
    top.removeFromLeft (6);
    impl->copy.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (12);
    impl->leave.setBounds (top.removeFromRight (64));
    top.removeFromRight (6);
    impl->editor.setBounds (top.removeFromRight (96));
    top.removeFromRight (6);
    impl->users.setBounds (top);

    // Transport
    const int gap = 5;
    auto take = [&] (int w, juce::Component& c) { c.setBounds (bar.removeFromLeft (w)); bar.removeFromLeft (gap); };
    take (34, impl->home);
    take (92, impl->play);
    take (150, impl->timeLabel);
    impl->bpmCaption = bar.removeFromLeft (30);
    take (44, impl->bpmLabel);
    impl->meterCaption = bar.removeFromLeft (52);
    take (44, impl->meterLabel);
    take (56, impl->click);
    take (70, impl->countIn);
    impl->exportMix.setBounds (bar.removeFromRight (122));
    bar.removeFromRight (gap);
    impl->del.setBounds (bar.removeFromRight (104));
    bar.removeFromRight (gap);
    impl->split.setBounds (bar.removeFromRight (92));

    // Right column: "Mi audio" over "Participantes"
    auto side = area.removeFromRight (sideWidth);
    impl->mine.setBounds (side.removeFromTop (238));
    impl->people.setBounds (side);

    // Track area
    auto header = area.removeFromTop (columnHeaderHeight);
    header.removeFromLeft (headerWidth);
    impl->ruler.setBounds (header.withTrimmedRight (scrollBarWidth));
    impl->viewport.setBounds (area);
    impl->layoutBody();

    if (! impl->userZoomed)
        impl->fitZoom();

    impl->repaintTimeline();
}

void DawView::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
    g.setColour (barColour);
    g.fillRect (0, 0, getWidth(), topBarHeight);
    g.setColour (transportColour);
    g.fillRect (0, topBarHeight, getWidth(), transportHeight);
    g.setColour (barColour);
    g.fillRect (0, topBarHeight + transportHeight, headerWidth, columnHeaderHeight);
    g.setColour (lineColour);
    g.fillRect (0, topBarHeight - 1, getWidth(), 1);
    g.fillRect (0, topBarHeight + transportHeight - 1, getWidth(), 1);
    g.fillRect (0, topBarHeight + transportHeight + columnHeaderHeight - 1, headerWidth, 1);
    g.fillRect (headerWidth - 1, topBarHeight + transportHeight, 1, columnHeaderHeight);

    g.setColour (mutedText);
    g.setFont (plainFont (12.0f));
    g.drawText ("Pistas", 10, topBarHeight + transportHeight, 100, columnHeaderHeight, juce::Justification::centredLeft, false);

    g.setColour (juce::Colour (0xff9fb0c2));
    g.drawText ("BPM", impl->bpmCaption, juce::Justification::centredLeft, false);
    g.drawText (ES ("Comp\xC3\xA1s"), impl->meterCaption, juce::Justification::centredLeft, false);
}

//==============================================================================
double DawView::getSamplesPerPixel() const noexcept { return impl->spp; }

void DawView::setSamplesPerPixel (double s)
{
    impl->spp = juce::jlimit (1.0, 5.0e6, s);
    impl->userZoomed = true;
    impl->repaintTimeline();
}

int DawView::sampleToX (int64_t s) const noexcept { return impl->sampleToX (s); }
int64_t DawView::xToSample (int x) const noexcept { return impl->xToSample (x); }

Id DawView::getSelectedRegion() const { return impl->selected; }
void DawView::selectRegion (const Id& region) { impl->select (region); }

bool DawView::splitSelectedAtPlayhead()
{
    const Region* region = nullptr;
    const auto* track = impl->selected.isNotEmpty() ? impl->trackOfRegion (impl->selected, &region) : nullptr;

    if (track == nullptr || ! impl->canEdit (*track) || ! onOp)
        return false;

    const auto at = impl->state.playhead;

    if (at <= region->start || at >= region->end())
        return false;

    onOp (Ops::splitRegion (track->id, region->id, at, newId()));
    return true;
}

bool DawView::deleteSelected()
{
    const auto* track = impl->selected.isNotEmpty() ? impl->trackOfRegion (impl->selected) : nullptr;

    if (track == nullptr || ! impl->canEdit (*track) || ! onOp)
        return false;

    onOp (Ops::deleteRegion (track->id, impl->selected));
    impl->selected = {};
    impl->updateEditButtons();
    impl->timeline.repaint();
    return true;
}

void DawView::setMeterLevels (const std::vector<std::pair<Id, float>>& trackPeaks, float armedInputPeak, double nowMs)
{
    if (nowMs < 0.0)
        nowMs = juce::Time::getMillisecondCounterHiRes();

    const auto* p = impl->currentProject();

    if (p == nullptr)
        return;

    std::map<Id, float> peaks (trackPeaks.begin(), trackPeaks.end());
    std::map<Id, MeterState> next;

    for (const auto& t : p->tracks)
    {
        auto state = impl->meters.count (t.id) > 0 ? impl->meters[t.id] : MeterState();
        const auto it = peaks.find (t.id);
        state.push (impl->showsArmedInput (t) ? armedInputPeak : (it != peaks.end() ? it->second : 0.0f), nowMs, clipLevel);
        next[t.id] = state;
    }

    impl->meters = std::move (next);   // deleted tracks drop out

    for (auto& h : impl->headers)
        h->meter.repaint();
}

bool DawView::isClipLatched (const Id& track) const
{
    const auto it = impl->meters.find (track);
    return it != impl->meters.end() && it->second.clip;
}

float DawView::getMeterHold (const Id& track) const
{
    const auto it = impl->meters.find (track);
    return it != impl->meters.end() ? it->second.hold : 0.0f;
}

juce::String DawView::getMeterText (const Id& track) const { return peakText (getMeterHold (track)); }

void DawView::dragRegion (const Id& region, int dx) { impl->emitEdited (region, Impl::DragMode::move, dx); }

void DawView::dragEdge (const Id& region, bool left, int dx)
{
    impl->emitEdited (region, left ? Impl::DragMode::trimLeft : Impl::DragMode::trimRight, dx);
}

bool DawView::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    if (mods.isCtrlDown() || mods.isAltDown() || mods.isCommandDown())
        return false;

    const auto code = key.getKeyCode();
    const auto letter = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code);

    // A text field (YouTube URL, a name or the tempo being edited) keeps its keys.
    if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;

    if (code == juce::KeyPress::spaceKey)
    {
        impl->togglePlay();
        return true;
    }

    // Cursor moves: never during a take (the record point is fixed).
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const int64_t step = mods.isShiftDown() ? impl->barSamples() : (int64_t) sampleRate;
        impl->nudge (code == juce::KeyPress::leftKey ? -step : step);
        return true;
    }

    if (mods.isShiftDown())
        return false;

    if (letter == 'w')                      { if (! impl->state.recording && onHome) onHome(); return true; }
    if (code == juce::KeyPress::deleteKey)  { deleteSelected(); return true; }
    if (letter == 's')                      { splitSelectedAtPlayhead(); return true; }

    return false;
}
}
