#include "ui/DawView.h"
#include "ui/Text.h"
#include "MessagePump.h"
#include <cmath>

namespace ensayo
{
class DawViewTests : public juce::UnitTest
{
public:
    DawViewTests() : juce::UnitTest ("DawView", "EnsayoVirtual") {}

    struct Fixture
    {
        Id host = newId(), guest = newId();
        Id t1 = newId(), t2 = newId(), t3 = newId(), take1 = newId(), take3 = newId(), r1 = newId(), r3 = newId();
        std::shared_ptr<Project> project = std::make_shared<Project>();

        Fixture()
        {
            auto& p = *project;
            p.id = newId(); p.name = "Tema nuevo"; p.hostUser = host;
            p.users = { { host, "Lucas" }, { guest, ES ("Mart\xC3\xADn") } };
            p.takes = { { take1, guest, 1, 10, 480000, true }, { take3, host, 1, 5, 240000, true } };
            Track a { t1, ES ("Voz gu\xC3\xAD" "a"), guest, TrackKind::voice, -3.0f, -0.2f, false,
                      { { r1, take1, 96000, 48000, 192000 } } };
            Track b { t2, "Guitarra", guest, TrackKind::instrument, 0.0f, 0.0f, true, {} };
            Track c { t3, "Bajo", host, TrackKind::instrument, -6.0f, 0.35f, false,
                      { { r3, take3, 0, 0, 240000 } } };
            p.tracks = { a, b, c };
        }

        DawState state (const Id& me) const
        {
            DawState s;
            s.project = project; s.me = me; s.isHost = (me == host);
            s.link = "ensayo://100.64.0.1:22140/ABC123";
            s.online[host] = true; s.online[guest] = true;
            return s;
        }
    };

    static juce::Component* find (juce::Component& root, const juce::String& name)
    {
        if (root.getName() == name)
            return &root;

        for (auto* child : root.getChildren())
            if (auto* found = find (*child, name))
                return found;

        return nullptr;
    }

    static bool shownWithin (juce::Component& root, juce::Component* c)
    {
        for (auto* p = c; p != nullptr; p = p->getParentComponent())
        {
            if (! p->isVisible())
                return false;

            if (p == &root)
                return true;
        }

        return false;
    }

    static void click (juce::Component* c)
    {
        dynamic_cast<juce::Button*> (c)->triggerClick();
        pumpMessages (250);
    }

    static juce::MouseEvent mouse (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &c, &c, juce::Time::getCurrentTime(), downPos, juce::Time::getCurrentTime(), 1, dragged);
    }

    struct CountingPeaks : PeaksProvider
    {
        mutable int calls = 0;
        std::vector<Peak> peaksFor (const Id&) const override { ++calls; return std::vector<Peak> (500, Peak { -0.5f, 0.5f }); }
    };

    static juce::String opName (const juce::var& v) { return v["op"].toString(); }

    struct Peaks : PeaksProvider
    {
        std::vector<Peak> peaksFor (const Id&) const override
        {
            std::vector<Peak> out;

            for (int i = 0; i < 2000; ++i)
            {
                const float a = 0.15f + 0.7f * std::abs (std::sin ((float) i * 0.013f)) * (0.5f + 0.5f * std::sin ((float) i * 0.11f));
                out.push_back ({ -a, a * 0.9f });
            }

            return out;
        }
    };

    static void render (juce::Component& c, const juce::File* png = nullptr)
    {
        juce::Image image (juce::SoftwareImageType().create (juce::Image::ARGB, c.getWidth(), c.getHeight(), true));
        {
            juce::Graphics g (image);
            c.paintEntireComponent (g, true);
        }

        if (png != nullptr)
        {
            png->getParentDirectory().createDirectory();
            png->deleteFile();
            juce::FileOutputStream out (*png);
            if (! out.failedToOpen())
                juce::PNGImageFormat().writeImageToStream (image, out);
        }
    }

    void runTest() override
    {
        beginTest ("every control exists and the lock/arm split follows ownership");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));

            for (const char* name : { "daw.project", "daw.link", "daw.copy", "daw.users", "daw.editor", "daw.leave",
                                      "transport.home", "transport.play", "transport.time",
                                      "transport.bpm", "transport.meter", "transport.click", "transport.countin",
                                      "edit.split", "edit.delete", "daw.export", "timeline",
                                      "tracks.addVoice", "tracks.addInstrument", "tracks.import", "tracks.youtube",
                                      "mine", "mine.voiceMonitor", "mine.instrumentMonitor", "mine.voiceSend", "mine.instrumentSend",
                                      "mine.voiceMonitor.text", "mine.instrumentSend.text", "mine.micMute",
                                      "people", "people.1.name", "people.1.gain", "people.1.gain.text", "people.1.mute" })
            {
                auto* c = find (view, name);
                expect (c != nullptr && shownWithin (view, c) && ! c->getBounds().isEmpty(), juce::String ("missing ") + name);
            }

            for (int n = 1; n <= 3; ++n)
                for (const char* part : { "color", "name", "author", "kind", "mute", "solo", "volume", "pan", "delete", "meter" })
                {
                    const auto name = "track." + juce::String (n) + "." + part;
                    auto* c = find (view, name);
                    expect (c != nullptr && shownWithin (view, c) && ! c->getBounds().isEmpty(), "missing " + name);
                }

            expect (shownWithin (view, find (view, "track.1.arm")));
            expect (shownWithin (view, find (view, "track.2.arm")));
            expect (! shownWithin (view, find (view, "track.3.arm")));
            expect (shownWithin (view, find (view, "track.3.lock")));
            expect (! shownWithin (view, find (view, "track.1.lock")));

            expect (! find (view, "track.3.mute")->isEnabled());
            expect (! find (view, "track.3.volume")->isEnabled());
            expect (! find (view, "track.3.pan")->isEnabled());
            expect (find (view, "track.1.mute")->isEnabled());
            expect (find (view, "track.1.volume")->isEnabled());

            for (const char* gone : { "daw.listen", "transport.stop", "transport.record", "people.2.name" })
                expect (find (view, gone) == nullptr, juce::String ("still there: ") + gone);

            expectEquals (dynamic_cast<juce::Label*> (find (view, "people.1.name"))->getText(), juce::String ("Lucas"));
            expectEquals (dynamic_cast<juce::Button*> (find (view, "transport.play"))->getButtonText(), ES ("\xE2\x96\xB6 Play"));

            // As host I can edit every track but arm only my own (spec 7): the guest's tracks show the lock.
            view.setState (f.state (f.host));
            expect (find (view, "track.3.mute")->isEnabled());
            expect (find (view, "track.3.volume")->isEnabled());
            expect (find (view, "track.1.volume")->isEnabled());
            expect (find (view, "track.1.mute")->isEnabled());
            expect (! shownWithin (view, find (view, "track.1.arm")), "host must not arm the guest's track");
            expect (! shownWithin (view, find (view, "track.2.arm")), "host must not arm the guest's track");
            expect (shownWithin (view, find (view, "track.1.lock")));
            expect (shownWithin (view, find (view, "track.3.arm")), "host arms its own track");
            expect (! shownWithin (view, find (view, "track.3.lock")));
        }

        beginTest ("split emits split_region at the playhead");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            auto s = f.state (f.guest);
            s.playhead = 150000;
            view.setState (s);
            std::vector<juce::var> ops;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };

            expect (! view.splitSelectedAtPlayhead());   // nothing selected
            view.selectRegion (f.r1);
            expect (view.getSelectedRegion() == f.r1);
            expect (view.splitSelectedAtPlayhead());
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("split_region"));
            expectEquals ((int64_t) ops[0]["at"], (int64_t) 150000);
            expect (ops[0]["region"].toString() == f.r1);
            expect (ops[0]["track"].toString() == f.t1);
            expect (isValidId (ops[0]["newRegion"].toString()));

            s.playhead = 10;    // outside the region
            view.setState (s);
            expect (! view.splitSelectedAtPlayhead());

            // A region of somebody else's track cannot be selected, split or deleted.
            view.selectRegion (f.r3);
            expect (view.getSelectedRegion().isEmpty());
            expect (! view.deleteSelected());
            expectEquals ((int) ops.size(), 1);

            view.selectRegion (f.r1);
            expect (view.deleteSelected());
            expectEquals (opName (ops.back()), juce::String ("delete_region"));
        }

        beginTest ("dragging a region emits move_region clamped at zero");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));
            view.setSamplesPerPixel (1000.0);
            std::vector<juce::var> ops;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };

            view.dragRegion (f.r1, 50);
            expectEquals (opName (ops.back()), juce::String ("move_region"));
            expectEquals ((int64_t) ops.back()["start"], (int64_t) (96000 + 50000));

            view.dragRegion (f.r1, -500);
            expectEquals ((int64_t) ops.back()["start"], (int64_t) 0);

            const auto before = ops.size();
            view.dragRegion (f.r3, 20);   // not editable by this user
            expectEquals ((int) ops.size(), (int) before);
        }

        beginTest ("dragging an edge trims within the take");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));
            view.setSamplesPerPixel (1000.0);
            std::vector<juce::var> ops;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };

            // r1: start 96000, offset 48000, length 192000 (take has 480000 samples)
            view.dragEdge (f.r1, true, -10);   // wants 10000 earlier, fits
            expectEquals (opName (ops.back()), juce::String ("trim_region"));
            expectEquals ((int64_t) ops.back()["start"], (int64_t) 86000);
            expectEquals ((int64_t) ops.back()["offset"], (int64_t) 38000);
            expectEquals ((int64_t) ops.back()["length"], (int64_t) 202000);

            view.dragEdge (f.r1, true, -90);   // beyond the take start: offset clamps at 0 (start would be 6000)
            expectEquals ((int64_t) ops.back()["offset"], (int64_t) 0);
            expectEquals ((int64_t) ops.back()["start"], (int64_t) 48000);
            expectEquals ((int64_t) ops.back()["length"], (int64_t) 240000);

            view.dragEdge (f.r1, false, 10000);   // beyond the take end: clamps at take.samples - offset
            expectEquals ((int64_t) ops.back()["length"], (int64_t) (480000 - 48000));
            expectEquals ((int64_t) ops.back()["offset"], (int64_t) 48000);

            view.dragEdge (f.r1, false, -100000);   // never below a minimum length
            expect ((int64_t) ops.back()["length"] >= 1);

            // The emitted trim must be accepted by the model.
            Project p = *f.project;
            expect (applyOp (p, ops[0], f.guest).ok);
        }

        beginTest ("mute emits set_mix immediately and volume only at drag end");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));
            std::vector<juce::var> ops;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };

            auto* mute = dynamic_cast<juce::Button*> (find (view, "track.1.mute"));
            expect (mute != nullptr);
            click (mute);
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("set_mix"));
            expect (ops[0]["track"].toString() == f.t1);
            expect ((bool) ops[0]["muted"]);

            auto* volume = dynamic_cast<juce::Slider*> (find (view, "track.1.volume"));
            expect (volume != nullptr);
            expectWithinAbsoluteError (volume->getMinimum(), -60.0, 0.001);
            expectWithinAbsoluteError (volume->getMaximum(), 12.0, 0.001);
            expectWithinAbsoluteError (volume->getValue(), -3.0, 0.001);
            ops.clear();
            volume->onDragStart();
            volume->setValue (-10.0, juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 0);
            volume->onDragEnd();
            expectEquals ((int) ops.size(), 1);
            expectWithinAbsoluteError ((double) ops[0]["gainDb"], -10.0, 0.001);
        }

        beginTest ("sample and pixel mappings round-trip and arm, solo and key shortcuts fire");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));

            for (double spp : { 100.0, 777.0, 1000.0 })
            {
                view.setSamplesPerPixel (spp);
                expectWithinAbsoluteError (view.getSamplesPerPixel(), spp, 0.0001);

                for (int x = 0; x < find (view, "timeline")->getWidth(); x += 13)
                    expectEquals (view.sampleToX (view.xToSample (x)), x);
            }

            Id armed; bool soloed = false;
            view.onArm = [&] (const Id& id) { armed = id; };
            view.onSolo = [&] (const Id&, bool on) { soloed = on; };
            click (find (view, "track.2.arm"));
            expect (armed == f.t2);
            click (find (view, "track.1.solo"));
            expect (soloed);
        }

        beginTest ("Play/Pausa, Space, W and the arrows; the cursor keys do nothing during a take; R is gone");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            int plays = 0, pauses = 0, homes = 0;
            std::vector<int64_t> seeks;
            view.onPlay = [&] { ++plays; };
            view.onPause = [&] { ++pauses; };
            view.onHome = [&] { ++homes; };
            view.onSeek = [&] (int64_t at) { seeks.push_back (at); };

            auto s = f.state (f.guest);
            s.playhead = 10 * 48000;
            view.setState (s);
            auto* play = dynamic_cast<juce::Button*> (find (view, "transport.play"));

            click (play);
            expectEquals (plays, 1);
            expect (view.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
            expectEquals (plays, 2);

            s.playing = true;
            view.setState (s);
            expectEquals (play->getButtonText(), ES ("\xE2\x9D\x9A\xE2\x9D\x9A Pausa"));
            view.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
            click (play);
            expectEquals (pauses, 2);
            expectEquals (plays, 2);

            expect (view.keyPressed (juce::KeyPress ('w')));
            expect (view.keyPressed (juce::KeyPress ('W')));
            expectEquals (homes, 2);

            // Project at 120 bpm, 4/4 by default: a bar is 2 s.
            const auto bar = (int64_t) std::llround (f.project->beatsPerBar * 60.0 / f.project->bpm * 48000.0);
            view.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
            view.keyPressed (juce::KeyPress (juce::KeyPress::leftKey));
            view.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0));
            view.keyPressed (juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier, 0));
            expect (seeks == std::vector<int64_t> { 11 * 48000, 9 * 48000, 10 * 48000 + bar, 10 * 48000 - bar });

            s.playhead = 20000;   // never before 0
            view.setState (s);
            view.keyPressed (juce::KeyPress (juce::KeyPress::leftKey));
            expectEquals (seeks.back(), (int64_t) 0);

            // R no longer records.
            const auto before = plays + pauses;
            expect (! view.keyPressed (juce::KeyPress ('r')));
            expectEquals (plays + pauses, before);

            // During a take (count-in included): Space and the button end it, W and the arrows do nothing.
            s.playing = false;
            s.recording = true;
            view.setState (s);
            expectEquals (play->getButtonText(), ES ("\xE2\x9D\x9A\xE2\x9D\x9A Pausa"));
            seeks.clear();
            homes = 0;
            view.keyPressed (juce::KeyPress ('w'));
            view.keyPressed (juce::KeyPress (juce::KeyPress::leftKey));
            view.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0));
            expectEquals (homes, 0);
            expect (seeks.empty());
            view.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
            click (play);
            expectEquals (pauses, 4);
        }

        beginTest ("the shortcuts leave a focused text field alone");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));
            int homes = 0, plays = 0;
            view.onHome = [&] { ++homes; };
            view.onPlay = [&] { ++plays; };
            view.addToDesktop (0);
            view.setVisible (true);
            auto* youtube = dynamic_cast<juce::TextEditor*> (find (view, "tracks.youtube"));
            youtube->grabKeyboardFocus();
            pumpMessages (100);

            if (youtube->hasKeyboardFocus (false))
            {
                expect (! view.keyPressed (juce::KeyPress ('w')));
                expect (! view.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
                expectEquals (homes + plays, 0);
            }
            else
            {
                logMessage ("no keyboard focus without a foreground window: focus check skipped");
            }

            view.removeFromDesktop();
        }

        beginTest ("the delete button: author or host, confirmation, delete_track, disarm; not while recording on it nor during my take");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            std::vector<juce::var> ops;
            juce::String asked;
            bool answer = true;
            Id armed = f.t1;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };
            view.onArm = [&] (const Id& id) { armed = id; };
            view.confirm = [&] (const juce::String& question, std::function<void()> yes) { asked = question; if (answer) yes(); };

            auto s = f.state (f.guest);
            s.armedTrack = f.t1;
            view.setState (s);
            auto* del1 = find (view, "track.1.delete");
            auto* del3 = find (view, "track.3.delete");
            expect (del1->isEnabled());
            expect (! del3->isEnabled(), "the guest cannot delete the host's track");
            expect (dynamic_cast<juce::SettableTooltipClient*> (del3)->getTooltip().contains ("autor"));

            answer = false;
            click (del1);
            expectEquals (asked, ES ("\xC2\xBF" "Borrar la pista \xC2\xABVoz gu\xC3\xAD" "a\xC2\xBB? Sus regiones desaparecen del proyecto."));
            expect (ops.empty(), "no answer, no op");
            expect (armed == f.t1);

            answer = true;
            click (del1);
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("delete_track"));
            expectEquals (ops[0]["track"].toString(), f.t1);
            expect (armed.isEmpty(), "deleting the armed track disarms it");

            // Host: any track.
            view.setState (f.state (f.host));
            expect (find (view, "track.1.delete")->isEnabled());
            expect (find (view, "track.3.delete")->isEnabled());

            // Not while I record on that track, and during my own take no other track either (like every other edit).
            s = f.state (f.guest);
            s.armedTrack = f.t2;
            s.recording = true;
            view.setState (s);
            ops.clear();
            expect (! find (view, "track.2.delete")->isEnabled());
            expect (dynamic_cast<juce::SettableTooltipClient*> (find (view, "track.2.delete"))->getTooltip().contains ("graba"));
            expect (! find (view, "track.1.delete")->isEnabled(), "my own take locks deleting my other tracks");
            expect (dynamic_cast<juce::SettableTooltipClient*> (find (view, "track.1.delete"))->getTooltip().contains (ES ("grab\xC3\xA1s")));
            click (find (view, "track.1.delete"));
            expect (ops.empty());

            s = f.state (f.host);
            s.armedTrack = f.t3;
            s.recording = true;
            view.setState (s);
            expect (! find (view, "track.1.delete")->isEnabled(), "the host recording cannot delete a guest's track either");

            // Someone else recording on a track: only that track is locked.

            s = f.state (f.host);
            s.live.push_back ({ f.guest, newId(), f.t1, 0, {} });
            view.setState (s);
            expect (! find (view, "track.1.delete")->isEnabled());
            expect (find (view, "track.3.delete")->isEnabled());

            // Read-only: nothing.
            s = f.state (f.host);
            s.readOnly = true;
            view.setState (s);
            ops.clear();
            expect (! find (view, "track.3.delete")->isEnabled());
            expect (dynamic_cast<juce::SettableTooltipClient*> (find (view, "track.3.delete"))->getTooltip().contains ("lectura"));
            click (find (view, "track.3.delete"));
            expect (ops.empty());
        }

        beginTest ("meters: armed own input vs playback, 1.5 s hold, -inf, clip latched until clicked");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            auto s = f.state (f.guest);
            s.armedTrack = f.t1;   // my voice track
            view.setState (s);

            expectEquals (view.getMeterText (f.t1), ES ("-\xE2\x88\x9E"));
            view.setMeterLevels ({ { f.t1, 0.9f }, { f.t2, 0.5f }, { f.t3, 0.25f } }, 0.1f, 1000.0);
            expectWithinAbsoluteError (view.getMeterHold (f.t1), 0.1f, 1e-6f);   // the armed input, not the playback
            expectWithinAbsoluteError (view.getMeterHold (f.t2), 0.5f, 1e-6f);
            expectEquals (view.getMeterText (f.t3), juce::String ("-12.0"));
            expect (! view.isClipLatched (f.t2));

            // Hold: a lower peak keeps the number for 1.5 s, then it follows.
            view.setMeterLevels ({ { f.t2, 0.2f } }, 0.0f, 2400.0);
            expectWithinAbsoluteError (view.getMeterHold (f.t2), 0.5f, 1e-6f);
            view.setMeterLevels ({ { f.t2, 0.2f } }, 0.0f, 2600.0);
            expectWithinAbsoluteError (view.getMeterHold (f.t2), 0.2f, 1e-6f);

            // A clip on the playback of another track, and on my armed input.
            view.setMeterLevels ({ { f.t3, 1.0f } }, 1.3f, 2700.0);
            expect (view.isClipLatched (f.t3));
            expect (view.isClipLatched (f.t1));
            expect (! view.isClipLatched (f.t2));
            view.setMeterLevels ({}, 0.0f, 9000.0);
            expect (view.isClipLatched (f.t3), "the latch stays on");
            expectEquals (view.getMeterText (f.t3), ES ("-\xE2\x88\x9E"));

            auto* meter = find (view, "track.3.meter");
            meter->mouseDown (mouse (*meter, { 5.0f, 5.0f }, { 5.0f, 5.0f }, false));
            expect (! view.isClipLatched (f.t3), "a click clears it");
            expect (view.isClipLatched (f.t1), "only that meter");

            // When the armed track is not mine (the host arming is impossible, but the state may say so) it shows playback.
            s = f.state (f.host);
            s.armedTrack = f.t1;
            view.setState (s);
            view.setMeterLevels ({ { f.t1, 0.3f } }, 0.9f, 20000.0);
            expectWithinAbsoluteError (view.getMeterHold (f.t1), 0.3f, 1e-6f);
            render (view);
        }

        beginTest ("Mi audio and Participantes emit their levels; double click returns to 0 dB");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            auto s = f.state (f.guest);
            s.mine = { -6.0f, 3.0f, -1.5f, 0.0f, false };
            s.participants[f.host] = { -9.0f, true };
            view.setState (s);

            auto slider = [&] (const char* n) { return dynamic_cast<juce::Slider*> (find (view, n)); };
            expectWithinAbsoluteError (slider ("mine.voiceMonitor")->getValue(), -6.0, 1e-6);
            expectWithinAbsoluteError (slider ("mine.instrumentMonitor")->getMaximum(), 6.0, 1e-6);
            expectWithinAbsoluteError (slider ("mine.voiceMonitor")->getMinimum(), -60.0, 1e-6);
            expectWithinAbsoluteError (slider ("people.1.gain")->getMaximum(), 12.0, 1e-6);
            expectWithinAbsoluteError (slider ("people.1.gain")->getValue(), -9.0, 1e-6);
            expect (dynamic_cast<juce::Button*> (find (view, "people.1.mute"))->getToggleState());
            expectEquals (dynamic_cast<juce::Label*> (find (view, "mine.voiceMonitor.text"))->getText(), juce::String ("-6.0 dB"));
            expect (slider ("mine.voiceSend")->isDoubleClickReturnEnabled() && slider ("mine.voiceSend")->getDoubleClickReturnValue() == 0.0);
            expect (slider ("people.1.gain")->getDoubleClickReturnValue() == 0.0);

            std::vector<OwnAudio> mine;
            std::vector<std::pair<Id, ParticipantLevel>> people;
            view.onMine = [&] (const OwnAudio& m) { mine.push_back (m); };
            view.onParticipant = [&] (const Id& u, const ParticipantLevel& l) { people.push_back ({ u, l }); };

            slider ("mine.instrumentSend")->setValue (-12.0, juce::sendNotificationSync);
            expect (mine.size() == 1 && mine.back().instrumentSendDb == -12.0f && mine.back().voiceMonitorDb == -6.0f);
            click (find (view, "mine.micMute"));
            expect (mine.size() == 2 && mine.back().micMuted);
            slider ("mine.voiceMonitor")->setValue (-60.0, juce::sendNotificationSync);
            expectEquals (dynamic_cast<juce::Label*> (find (view, "mine.voiceMonitor.text"))->getText(), ES ("-\xE2\x88\x9E dB"));

            slider ("people.1.gain")->setValue (4.0, juce::sendNotificationSync);
            expect (people.size() == 1 && people.back().first == f.host && people.back().second.gainDb == 4.0f && people.back().second.muted);
            click (find (view, "people.1.mute"));
            expect (people.size() == 2 && ! people.back().second.muted);

            // Only online users other than me; nobody online shows the empty text.
            s.online[f.host] = false;
            view.setState (s);
            expect (find (view, "people.1.name") == nullptr);
            render (view);
        }

        beginTest ("rename, tempo and pan edits emit the right ops and respect the host");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            std::vector<juce::var> ops;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };
            view.setState (f.state (f.guest));

            auto* name = dynamic_cast<juce::Label*> (find (view, "track.1.name"));
            expect (name != nullptr && name->isEditableOnDoubleClick());
            expect (! dynamic_cast<juce::Label*> (find (view, "track.3.name"))->isEditableOnDoubleClick());
            name->setText ("Coros", juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("rename_track"));
            expectEquals (ops[0]["name"].toString(), juce::String ("Coros"));
            expect (ops[0]["track"].toString() == f.t1);

            // Non-host: tempo controls are disabled and nothing is emitted.
            auto* bpm = dynamic_cast<juce::Label*> (find (view, "transport.bpm"));
            auto* meter = dynamic_cast<juce::Label*> (find (view, "transport.meter"));
            expect (! bpm->isEnabled() && ! meter->isEnabled());
            ops.clear();
            bpm->setText ("100", juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 0);
            expectEquals (bpm->getText(), juce::String ("120"));   // reverted

            view.setState (f.state (f.host));
            expect (bpm->isEnabled() && meter->isEnabled());
            bpm->setText ("100", juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("set_tempo"));
            expectWithinAbsoluteError ((double) ops[0]["bpm"], 100.0, 0.001);
            expectEquals ((int) ops[0]["beatsPerBar"], 4);

            ops.clear();
            expectEquals (meter->getText(), juce::String ("4/4"));
            meter->setText ("3/4", juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 1);
            expectEquals ((int) ops[0]["beatsPerBar"], 3);
            meter->setText ("5", juce::sendNotificationSync);   // numerator alone is accepted
            expectEquals ((int) ops.size(), 2);
            expectEquals ((int) ops[1]["beatsPerBar"], 5);
            meter->setText ("5/8", juce::sendNotificationSync);   // other denominators are rejected
            meter->setText ("13", juce::sendNotificationSync);    // out of [1, 12]
            meter->setText ("abc", juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 2);
            expectEquals (meter->getText(), juce::String ("4/4"));   // label back to what the project says

            // Pan: only at drag end, only the pan field, and only when it changed.
            view.setState (f.state (f.guest));
            ops.clear();
            auto* pan = dynamic_cast<juce::Slider*> (find (view, "track.1.pan"));
            pan->onDragStart();
            pan->setValue (0.5, juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 0);
            pan->onDragEnd();
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("set_mix"));
            expectWithinAbsoluteError ((double) ops[0]["pan"], 0.5, 0.001);
            expect (! ops[0].hasProperty ("gainDb"));
            pan->onDragStart();
            pan->onDragEnd();   // nothing moved
            expectEquals ((int) ops.size(), 1);

            // A wheel spin / keyboard nudge (no drag) is coalesced into one op.
            ops.clear();
            auto* volume = dynamic_cast<juce::Slider*> (find (view, "track.1.volume"));
            for (double v : { -10.0, -12.0, -15.0 })
                volume->setValue (v, juce::sendNotificationSync);
            expectEquals ((int) ops.size(), 0);
            pumpMessages (700);
            expectEquals ((int) ops.size(), 1);
            expectWithinAbsoluteError ((double) ops[0]["gainDb"], -15.0, 0.001);
        }

        beginTest ("a mouse drag on the timeline emits one move on mouse up and the keys split and delete");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            auto s = f.state (f.guest);
            s.playhead = 150000;
            view.setState (s);
            view.setSamplesPerPixel (1000.0);
            std::vector<juce::var> ops;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };
            auto* timeline = find (view, "timeline");

            // r1 spans x 96..288 on lane 0; grab the middle and drag 50 px right.
            timeline->mouseDown (mouse (*timeline, { 192.0f, 30.0f }, { 192.0f, 30.0f }, false));
            expect (view.getSelectedRegion() == f.r1);
            timeline->mouseDrag (mouse (*timeline, { 217.0f, 30.0f }, { 192.0f, 30.0f }, true));
            timeline->mouseDrag (mouse (*timeline, { 242.0f, 30.0f }, { 192.0f, 30.0f }, true));
            expectEquals ((int) ops.size(), 0);
            timeline->mouseUp (mouse (*timeline, { 242.0f, 30.0f }, { 192.0f, 30.0f }, true));
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("move_region"));
            expectEquals ((int64_t) ops[0]["start"], (int64_t) 146000);

            // Edge zone trims instead.
            ops.clear();
            timeline->mouseDown (mouse (*timeline, { 98.0f, 30.0f }, { 98.0f, 30.0f }, false));
            timeline->mouseUp (mouse (*timeline, { 88.0f, 30.0f }, { 98.0f, 30.0f }, true));
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("trim_region"));

            // A click without moving emits nothing; a click on empty space deselects.
            ops.clear();
            timeline->mouseDown (mouse (*timeline, { 192.0f, 30.0f }, { 192.0f, 30.0f }, false));
            timeline->mouseUp (mouse (*timeline, { 192.0f, 30.0f }, { 192.0f, 30.0f }, false));
            expectEquals ((int) ops.size(), 0);
            timeline->mouseDown (mouse (*timeline, { 900.0f, 30.0f }, { 900.0f, 30.0f }, false));
            expect (view.getSelectedRegion().isEmpty());

            view.selectRegion (f.r1);
            expect (view.keyPressed (juce::KeyPress ('S')));
            expectEquals ((int) ops.size(), 1);
            expectEquals (opName (ops[0]), juce::String ("split_region"));
            expect (view.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));
            expectEquals ((int) ops.size(), 2);
            expectEquals (opName (ops[1]), juce::String ("delete_region"));
        }

        beginTest ("editing is locked while I am recording");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            auto s = f.state (f.host);
            s.playhead = 150000;
            view.setState (s);
            view.setSamplesPerPixel (1000.0);
            view.selectRegion (f.r1);
            expect (view.getSelectedRegion() == f.r1);
            std::vector<juce::var> ops;
            view.onOp = [&] (const juce::var& op) { ops.push_back (op); };

            s.recording = true;
            view.setState (s);
            expect (view.getSelectedRegion() == f.r1);   // selection untouched
            view.selectRegion (f.r3);
            expect (view.getSelectedRegion() == f.r1);
            expect (! view.splitSelectedAtPlayhead());
            expect (! view.deleteSelected());
            view.dragRegion (f.r1, 10);
            view.dragEdge (f.r1, true, -5);
            auto* timeline = find (view, "timeline");
            timeline->mouseDown (mouse (*timeline, { 900.0f, 30.0f }, { 900.0f, 30.0f }, false));
            expect (view.getSelectedRegion() == f.r1);

            expect (! find (view, "track.1.mute")->isEnabled());
            expect (! find (view, "track.1.volume")->isEnabled());
            expect (! find (view, "track.1.pan")->isEnabled());
            expect (! find (view, "transport.bpm")->isEnabled());
            expect (! find (view, "edit.split")->isEnabled());
            expect (shownWithin (view, find (view, "track.3.arm")));   // arming my own track stays available

            dynamic_cast<juce::Label*> (find (view, "track.1.name"))->setText ("X", juce::sendNotificationSync);
            dynamic_cast<juce::Label*> (find (view, "transport.bpm"))->setText ("90", juce::sendNotificationSync);
            auto* volume = dynamic_cast<juce::Slider*> (find (view, "track.1.volume"));
            volume->setValue (-20.0, juce::sendNotificationSync);
            pumpMessages (600);
            expectEquals ((int) ops.size(), 0);

            s.recording = false;
            view.setState (s);
            expect (view.splitSelectedAtPlayhead());
        }

        beginTest ("peaks are cached across state changes and refetched only when a take grows");
        {
            Fixture f;
            DawView view;
            CountingPeaks peaks;
            view.setPeaksProvider (&peaks);
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));
            render (view);
            const int first = peaks.calls;
            expect (first >= 1);

            auto copy = std::make_shared<Project> (*f.project);   // a new project object, same takes
            auto s = f.state (f.guest);
            s.project = copy;
            s.playhead = 4000;
            view.setState (s);
            render (view);
            expectEquals (peaks.calls, first);

            copy = std::make_shared<Project> (*copy);
            for (auto& t : copy->takes)
                t.samples += 48000;   // the take grew
            s.project = copy;
            view.setState (s);
            render (view);
            expect (peaks.calls > first);
        }

        beginTest ("the lock tooltip tells why in read-only mode");
        {
            Fixture f;
            DawView view;
            view.setSize (1100, 760);
            view.setState (f.state (f.guest));
            auto* lock = dynamic_cast<juce::Label*> (find (view, "track.3.lock"));
            expect (lock->getTooltip().contains ("autor"));
            auto s = f.state (f.guest);
            s.readOnly = true;
            view.setState (s);
            expect (lock->getTooltip().contains ("lectura"));
            expect (shownWithin (view, find (view, "track.1.lock")));

            // A take already running can always be stopped.
            int pauses = 0;
            view.onPause = [&] { ++pauses; };
            s.recording = true;
            view.setState (s);
            expect (find (view, "transport.play")->isEnabled());
            view.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
            expectEquals (pauses, 1);
        }

        beginTest ("renders empty, one-track and twelve-track projects");
        {
            Fixture f;

            for (int tracks : { 0, 1, 12 })
            {
                auto p = std::make_shared<Project> (*f.project);
                p->tracks.clear();

                for (int i = 0; i < tracks; ++i)
                    p->tracks.push_back ({ newId(), "Pista " + juce::String (i + 1), i % 2 ? f.host : f.guest, TrackKind::voice,
                                           0.0f, 0.0f, false,
                                           i == 0 ? std::vector<Region> { { newId(), f.take1, 0, 0, 100000 } } : std::vector<Region>{} });

                DawView view;
                Peaks peaks;
                view.setPeaksProvider (&peaks);
                view.setSize (1100, 760);
                auto s = f.state (f.guest);
                s.project = p;
                view.setState (s);
                render (view);
                expectEquals (view.getWidth(), 1100);
            }
        }

        beginTest ("renders the demo session to a PNG");
        {
            Fixture f;
            auto p = std::make_shared<Project> (*f.project);
            const Id importedTake = newId(), liveTake = newId();
            p->takes.push_back ({ importedTake, f.host, 2, 40, 1900000, true });
            p->takes.push_back ({ liveTake, f.guest, 1, 3, 150000, false });
            p->tracks.clear();
            p->tracks.push_back ({ f.t1, ES ("Guitarra r\xC3\xADtmica"), f.host, TrackKind::instrument, -3.0f, -0.2f, false,
                                   { { newId(), f.take3, 48000, 0, 240000 }, { f.r3, f.take3, 340000, 24000, 216000 } } });
            p->tracks.push_back ({ f.t2, ES ("Voz gu\xC3\xAD" "a"), f.host, TrackKind::voice, -6.0f, 0.0f, true,
                                   { { newId(), f.take1, 144000, 0, 300000 } } });
            p->tracks.push_back ({ f.t3, "Bajo", f.guest, TrackKind::instrument, 0.0f, 0.0f, false, {} });
            p->tracks.push_back ({ newId(), ES ("Bater\xC3\xAD" "a (YouTube)"), f.host, TrackKind::imported, -8.5f, 0.0f, false,
                                   { { newId(), importedTake, 0, 0, 1900000 } } });

            DawView view;
            Peaks peaks;
            view.setPeaksProvider (&peaks);
            view.setSize (1100, 760);
            auto s = f.state (f.host);
            s.project = p;
            s.playhead = 5 * 48000;
            s.armedTrack = f.t1;
            s.solo = { f.t2 };
            s.takeAvailability[importedTake] = 0.64f;
            LiveRegion live { f.guest, liveTake, f.t3, 5 * 48000, {} };
            live.peaks = Peaks().peaksFor ({});
            live.peaks.resize (400);
            s.live.push_back (live);
            s.participants[f.guest] = { -4.5f, false };
            view.setState (s);
            view.selectRegion (f.r3);
            view.setMeterLevels ({ { f.t2, 0.2f }, { f.t3, 1.05f } }, 0.4f);

            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("dawview").getChildFile ("daw.png");
            render (view, &file);
            expect (file.existsAsFile() && file.getSize() > 1000);
        }
    }
};

static DawViewTests dawViewTests;
}
