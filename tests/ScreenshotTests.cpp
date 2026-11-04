#include "ui/Screenshot.h"
#include <juce_graphics/juce_graphics.h>

namespace ensayo
{
namespace
{
    // Mirrors tests/integration/expected-controls.json; used only when the repository file cannot be located.
    const char* const embeddedExpected = R"json({
      "lobby": [
        "lobby.name", "lobby.role", "lobby.create", "lobby.link", "lobby.join",
        "audio.device", "audio.rate", "audio.buffer", "audio.panel", "audio.mic", "audio.instrument",
        "audio.output", "audio.latency",
        "vst.list", "vst.browse", "vst.editor", "vst.bypass", "vst.remove",
        "status.text", "status.retry"
      ],
      "session": [
        "session.link", "session.copy", "session.leave",
        "me.voice.fader", "me.voice.mute", "me.voice.meter",
        "me.instrument.fader", "me.instrument.mute", "me.instrument.meter",
        "participant.1.name", "participant.1.role", "participant.1.voice.fader", "participant.1.voice.mute",
        "participant.1.instrument.fader", "participant.1.instrument.mute", "participant.1.meter",
        "participant.2.name", "participant.2.role", "participant.2.voice.fader", "participant.2.voice.mute",
        "participant.2.instrument.fader", "participant.2.instrument.mute", "participant.2.meter",
        "participant.3.name", "participant.3.role", "participant.3.voice.fader", "participant.3.voice.mute",
        "participant.3.instrument.fader", "participant.3.instrument.mute", "participant.3.meter",
        "track.load", "track.url", "track.download", "track.name", "track.play", "track.stop", "track.loop",
        "track.position", "track.time", "track.fader", "track.mute", "track.progress",
        "status.ping", "status.jitter", "status.latency", "status.loss", "status.cpu", "status.text", "status.retry"
      ]
    })json";

    juce::File findExpectedControlsFile()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int depth = 0; depth < 12 && dir.isDirectory(); ++depth)
        {
            const auto candidate = dir.getChildFile ("tests").getChildFile ("integration").getChildFile ("expected-controls.json");

            if (candidate.existsAsFile())
                return candidate;

            const auto parent = dir.getParentDirectory();

            if (parent == dir)
                break;

            dir = parent;
        }

        return {};
    }
}

class ScreenshotTests : public juce::UnitTest
{
public:
    ScreenshotTests() : juce::UnitTest ("Screenshot", "EnsayoVirtual") {}

    void runTest() override
    {
        juce::TemporaryFile temp;
        const auto outDir = temp.getFile().getSiblingFile ("ensayo-shots-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));

        beginTest ("runScreenshots writes both PNGs at 1100x760 and a controls.json");
        const auto result = runScreenshots (outDir);
        expect (result.wasOk(), result.getErrorMessage());

        for (auto* fileName : { "lobby.png", "session.png" })
        {
            const auto file = outDir.getChildFile (fileName);
            expect (file.existsAsFile(), juce::String (fileName) + " missing");

            const auto image = juce::ImageFileFormat::loadFrom (file);
            expect (image.isValid(), juce::String (fileName) + " does not decode");
            expectEquals (image.getWidth(), 1100, fileName);
            expectEquals (image.getHeight(), 760, fileName);
        }

        const auto controlsFile = outDir.getChildFile ("controls.json");
        expect (controlsFile.existsAsFile(), "controls.json missing");
        const auto controls = juce::JSON::parse (controlsFile);
        expect (controls.isArray(), "controls.json is not an array");

        beginTest ("every expected control is present, visible and non-empty in its view");
        const auto expectedFile = findExpectedControlsFile();
        const auto expected = expectedFile.existsAsFile() ? juce::JSON::parse (expectedFile) : juce::JSON::parse (embeddedExpected);
        expect (expected.isObject(), "expected-controls.json does not parse");
        logMessage (expectedFile.existsAsFile() ? "using " + expectedFile.getFullPathName() : "using the embedded control list");

        for (auto* view : { "lobby", "session" })
        {
            const auto* names = expected[view].getArray();
            expect (names != nullptr && names->size() > 0, juce::String (view) + ": empty expectation list");

            if (names == nullptr)
                continue;

            for (const auto& controlName : *names)
                checkControl (controls, controlName.toString(), view);
        }

        beginTest ("entries carry the full metadata set");
        if (auto* array = controls.getArray())
        {
            expect (array->size() > 0);

            for (const auto& entry : *array)
            {
                auto* object = entry.getDynamicObject();
                expect (object != nullptr);

                if (object == nullptr)
                    continue;

                for (auto* key : { "name", "type", "x", "y", "w", "h", "visible", "view" })
                    expect (object->hasProperty (key), "missing key " + juce::String (key) + " in " + entry["name"].toString());

                expect (entry["name"].toString().isNotEmpty(), "unnamed entry");
                expect (entry["view"].toString() == "lobby" || entry["view"].toString() == "session");
            }
        }

        const auto typeOf = [&] (const juce::String& controlName, const juce::String& view) -> juce::String
        {
            if (auto* array = controls.getArray())
                for (const auto& entry : *array)
                    if (entry["name"].toString() == controlName && entry["view"].toString() == view)
                        return entry["type"].toString();

            return {};
        };

        expectEquals (typeOf ("lobby.name", "lobby"), juce::String ("TextEditor"));
        expectEquals (typeOf ("lobby.role", "lobby"), juce::String ("ComboBox"));
        expectEquals (typeOf ("lobby.create", "lobby"), juce::String ("TextButton"));
        expectEquals (typeOf ("me.voice.fader", "session"), juce::String ("Slider"));
        expectEquals (typeOf ("me.voice.meter", "session"), juce::String ("LevelMeter"));
        expectEquals (typeOf ("track.progress", "session"), juce::String ("ProgressBar"));
        expectEquals (typeOf ("track.name", "session"), juce::String ("Label"));

        outDir.deleteRecursively();
    }

private:
    void checkControl (const juce::var& controls, const juce::String& controlName, const juce::String& view)
    {
        auto* array = controls.getArray();

        if (array == nullptr)
            return;

        for (const auto& entry : *array)
        {
            if (entry["name"].toString() != controlName || entry["view"].toString() != view)
                continue;

            expect ((bool) entry["visible"], view + ": " + controlName + " not visible");
            expect ((int) entry["w"] > 0 && (int) entry["h"] > 0, view + ": " + controlName + " has empty bounds");
            return;
        }

        expect (false, view + ": missing " + controlName);
    }
};

static ScreenshotTests screenshotTests;
}
