#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <opus_custom.h>

namespace ensayo
{
class BuildConfigTests : public juce::UnitTest
{
public:
    BuildConfigTests() : juce::UnitTest ("Build configuration", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("ASIO is compiled in");
        {
            expectEquals ((int) JUCE_ASIO, 1);
        }

        beginTest ("VST3 hosting is compiled in");
        {
            expectEquals ((int) JUCE_PLUGINHOST_VST3, 1);
            juce::VST3PluginFormat vst3;
            expectEquals (vst3.getName(), juce::String ("VST3"));
        }

        beginTest ("MP3 decoding is compiled in");
        {
            expectEquals ((int) JUCE_USE_MP3AUDIOFORMAT, 1);
        }

        beginTest ("libopus custom modes are available");
        {
            int error = 0;
            auto* mode = opus_custom_mode_create (48000, 128, &error);
            expect (mode != nullptr, "opus_custom_mode_create returned null");
            expectEquals (error, 0);

            if (mode != nullptr)
                opus_custom_mode_destroy (mode);
        }
    }
};

static BuildConfigTests buildConfigTests;
}
