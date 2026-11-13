#include "ui/Screenshot.h"
#include <juce_graphics/juce_graphics.h>

namespace ensayo
{
namespace
{
    // Mirrors tests/integration/expected-controls.json; used only when the repository file cannot be located.
    const char* const embeddedExpected = R"json({
  "lobby": [
    "lobby.name", "lobby.role", "lobby.create", "lobby.open", "lobby.link", "lobby.join",
    "audio.device", "audio.rate", "audio.buffer", "audio.panel", "audio.mic", "audio.instrument",
    "audio.output", "audio.latency", "lobby.calibrate", "lobby.calibration",
    "vst.list", "vst.browse", "vst.editor", "vst.bypass", "vst.remove",
    "status.text", "status.retry"
  ],
  "session": [
    "daw.project", "daw.link", "daw.copy", "daw.users", "daw.listen", "daw.editor", "daw.leave",
    "transport.home", "transport.play", "transport.stop", "transport.record", "transport.time",
    "transport.bpm", "transport.meter", "transport.click", "transport.countin",
    "edit.split", "edit.delete", "daw.export", "timeline", "timeline.ruler",
    "tracks.addVoice", "tracks.addInstrument", "tracks.import", "tracks.youtube",
    "track.1.color", "track.1.name", "track.1.author", "track.1.kind", "track.1.mute", "track.1.solo", "track.1.arm",
    "track.1.volume", "track.1.volume.text", "track.1.pan", "track.1.pan.text",
    "track.2.color", "track.2.name", "track.2.author", "track.2.kind", "track.2.mute", "track.2.solo", "track.2.arm",
    "track.2.volume", "track.2.volume.text", "track.2.pan", "track.2.pan.text",
    "track.3.color", "track.3.name", "track.3.author", "track.3.kind", "track.3.mute", "track.3.solo", "track.3.lock",
    "track.3.volume", "track.3.volume.text", "track.3.pan", "track.3.pan.text",
    "track.4.color", "track.4.name", "track.4.author", "track.4.kind", "track.4.mute", "track.4.solo",
    "track.4.volume", "track.4.volume.text", "track.4.pan", "track.4.pan.text",
    "status.text", "status.sync", "status.ping", "status.transfers", "status.device", "status.cpu", "status.retry"
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
        expectEquals (typeOf ("lobby.open", "lobby"), juce::String ("TextButton"));
        expectEquals (typeOf ("lobby.calibrate", "lobby"), juce::String ("TextButton"));
        expectEquals (typeOf ("lobby.calibration", "lobby"), juce::String ("Label"));
        expectEquals (typeOf ("transport.record", "session"), juce::String ("TextButton"));
        expectEquals (typeOf ("track.1.volume", "session"), juce::String ("Slider"));
        expectEquals (typeOf ("track.1.name", "session"), juce::String ("Label"));
        expectEquals (typeOf ("tracks.youtube", "session"), juce::String ("TextEditor"));
        expectEquals (typeOf ("status.sync", "session"), juce::String ("Label"));

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
