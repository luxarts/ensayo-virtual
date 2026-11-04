#pragma once
#include <juce_audio_formats/juce_audio_formats.h>

namespace ensayo
{
// Decodes audio files through Windows Media Foundation (IMFSourceReader): AAC in MP4/M4A (including the
// fragmented/DASH MP4 that yt-dlp writes); wav/flac/ogg/mp3 stay with the built-in JUCE readers. The reader decodes the whole file
// into memory on construction (capped at 35 minutes), so it only works on files: createReaderFor needs a
// juce::FileInputStream and returns nullptr for any other stream.
class MediaFoundationAudioFormat : public juce::AudioFormat
{
public:
    MediaFoundationAudioFormat();
    ~MediaFoundationAudioFormat() override = default;

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;
    bool canDoStereo() override;
    bool canDoMono() override;
    bool isCompressed() override;

    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream, bool deleteStreamIfOpeningFails) override;
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                                              const juce::AudioFormatWriterOptions& options) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaFoundationAudioFormat)
};
}
