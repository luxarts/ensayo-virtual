#include "MediaFoundationAudioFormat.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ensayo
{
namespace
{
const char* const mfFormatName = "Media Foundation";
// Only the containers JUCE cannot read itself: wav/flac/ogg/mp3 keep their built-in (cross-platform) readers.
const char* const mfExtensions[] = { ".m4a", ".mp4", ".aac", ".m4b", nullptr };

// BackingTrack rejects anything over 30 minutes; decoding stops a little past that so a huge file cannot
// exhaust memory (35 min of stereo 48 kHz float is ~800 MB).
constexpr double maxDecodedSeconds = 35.0 * 60.0;

// Minimal owning COM pointer (juce::ComSmartPtr lives in a private JUCE header).
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr (const ComPtr&) = delete;
    ComPtr& operator= (const ComPtr&) = delete;

    T** resetAndGetPointerAddress() { reset(); return &object; }
    T* operator->() const noexcept { return object; }
    T& operator*() const noexcept { return *object; }
    T* get() const noexcept { return object; }
    bool operator== (std::nullptr_t) const noexcept { return object == nullptr; }

    void reset() noexcept
    {
        if (object != nullptr)
        {
            object->Release();
            object = nullptr;
        }
    }

private:
    T* object = nullptr;
};

//==============================================================================
class MediaFoundationReader final : public juce::AudioFormatReader
{
public:
    MediaFoundationReader (juce::InputStream* stream, const juce::File& file)
        : juce::AudioFormatReader (stream, TRANS (mfFormatName))
    {
        usesFloatingPointData = true;
        bitsPerSample = 32;

        const HRESULT coResult = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        comInitialised = SUCCEEDED (coResult);   // RPC_E_CHANGED_MODE: already initialised with another model, fine.

        if (FAILED (coResult) && coResult != RPC_E_CHANGED_MODE)
            return;

        mfStarted = SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_NOSOCKET));

        if (! mfStarted)
            return;

        if (! decode (file))
        {
            channels.clear();
            sampleRate = 0.0;
            numChannels = 0;
            lengthInSamples = 0;
        }
    }

    ~MediaFoundationReader() override
    {
        if (mfStarted)
            MFShutdown();

        if (comInitialised)
            CoUninitialize();
    }

    bool isValid() const noexcept { return sampleRate > 0.0 && numChannels > 0 && lengthInSamples > 0; }

    bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override
    {
        if (! isValid() || numSamples <= 0)
            return isValid();

        for (int ch = 0; ch < numDestChannels; ++ch)
        {
            auto* dest = destChannels[ch];

            if (dest == nullptr)
                continue;

            auto* out = reinterpret_cast<float*> (dest + startOffsetInDestBuffer);
            std::fill (out, out + numSamples, 0.0f);

            if (ch >= (int) numChannels)
                continue;

            const juce::int64 first = juce::jmax<juce::int64> (0, startSampleInFile);
            const juce::int64 last = juce::jmin<juce::int64> (lengthInSamples, startSampleInFile + numSamples);

            if (last <= first)
                continue;

            const auto count = (size_t) (last - first);
            std::memcpy (out + (first - startSampleInFile), channels[(size_t) ch].data() + first, count * sizeof (float));
        }

        return true;
    }

private:
    bool comInitialised = false;
    bool mfStarted = false;
    std::vector<std::vector<float>> channels;   // deinterleaved, lengthInSamples each

    bool decode (const juce::File& file)
    {
        ComPtr<IMFSourceReader> reader;
        const auto path = file.getFullPathName().toWideCharPointer();

        if (FAILED (MFCreateSourceReaderFromURL (path, nullptr, reader.resetAndGetPointerAddress())))
            return false;

        if (FAILED (reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE))
            || FAILED (reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)))
            return false;

        bool floatOutput = true;

        if (! setOutputType (*reader, MFAudioFormat_Float))
        {
            floatOutput = false;

            if (! setOutputType (*reader, MFAudioFormat_PCM))
                return false;
        }

        ComPtr<IMFMediaType> type;

        if (FAILED (reader->GetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, type.resetAndGetPointerAddress())))
            return false;

        UINT32 rate = 0, chans = 0, bits = 0;

        if (FAILED (type->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate))
            || FAILED (type->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &chans))
            || FAILED (type->GetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)))
            return false;

        if (rate == 0 || chans == 0 || chans > 64)
            return false;

        if (floatOutput ? (bits != 32) : (bits != 16))
            return false;

        const auto maxSamples = (juce::int64) (maxDecodedSeconds * (double) rate);
        juce::int64 expectedSamples = 0;

        {
            PROPVARIANT var;
            PropVariantInit (&var);

            if (SUCCEEDED (reader->GetPresentationAttribute ((DWORD) MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))
                && var.vt == VT_UI8)
                expectedSamples = (juce::int64) std::llround ((double) var.uhVal.QuadPart * (double) rate / 1.0e7);

            PropVariantClear (&var);
        }

        channels.assign (chans, {});

        for (auto& c : channels)
            c.reserve ((size_t) juce::jlimit<juce::int64> (0, maxSamples, expectedSamples));

        const size_t bytesPerFrame = (size_t) chans * (floatOutput ? sizeof (float) : sizeof (juce::int16));
        juce::int64 decoded = 0;

        while (decoded < maxSamples)
        {
            DWORD streamIndex = 0, flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;

            if (FAILED (reader->ReadSample ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags,
                                            &timestamp, sample.resetAndGetPointerAddress())))
                return false;

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                break;

            if ((flags & MF_SOURCE_READERF_ERROR) != 0)
                return false;

            if (sample == nullptr)
                continue;   // stream tick / gap / format change without data

            ComPtr<IMFMediaBuffer> buffer;

            if (FAILED (sample->ConvertToContiguousBuffer (buffer.resetAndGetPointerAddress())))
                return false;

            BYTE* data = nullptr;
            DWORD maxLength = 0, currentLength = 0;

            if (FAILED (buffer->Lock (&data, &maxLength, &currentLength)))
                return false;

            auto frames = (juce::int64) (currentLength / bytesPerFrame);
            frames = juce::jmin (frames, maxSamples - decoded);

            if (floatOutput)
            {
                const auto* src = reinterpret_cast<const float*> (data);

                for (UINT32 ch = 0; ch < chans; ++ch)
                {
                    auto& dst = channels[ch];

                    for (juce::int64 i = 0; i < frames; ++i)
                        dst.push_back (src[(size_t) i * chans + ch]);
                }
            }
            else
            {
                const auto* src = reinterpret_cast<const juce::int16*> (data);

                for (UINT32 ch = 0; ch < chans; ++ch)
                {
                    auto& dst = channels[ch];

                    for (juce::int64 i = 0; i < frames; ++i)
                        dst.push_back ((float) src[(size_t) i * chans + ch] * (1.0f / 32768.0f));
                }
            }

            buffer->Unlock();
            decoded += frames;
        }

        sampleRate = (double) rate;
        numChannels = chans;
        lengthInSamples = decoded;
        return decoded > 0;
    }

    static bool setOutputType (IMFSourceReader& reader, const GUID& subtype)
    {
        ComPtr<IMFMediaType> type;

        if (FAILED (MFCreateMediaType (type.resetAndGetPointerAddress())))
            return false;

        if (FAILED (type->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio))
            || FAILED (type->SetGUID (MF_MT_SUBTYPE, subtype)))
            return false;

        return SUCCEEDED (reader.SetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.get()));
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaFoundationReader)
};
}

//==============================================================================
MediaFoundationAudioFormat::MediaFoundationAudioFormat()
    : juce::AudioFormat (TRANS (mfFormatName), juce::StringArray (mfExtensions))
{
}

juce::Array<int> MediaFoundationAudioFormat::getPossibleSampleRates()  { return {}; }
juce::Array<int> MediaFoundationAudioFormat::getPossibleBitDepths()    { return {}; }
bool MediaFoundationAudioFormat::canDoStereo()                          { return true; }
bool MediaFoundationAudioFormat::canDoMono()                            { return true; }
bool MediaFoundationAudioFormat::isCompressed()                         { return true; }

juce::AudioFormatReader* MediaFoundationAudioFormat::createReaderFor (juce::InputStream* sourceStream,
                                                                      bool deleteStreamIfOpeningFails)
{
    auto* fileStream = dynamic_cast<juce::FileInputStream*> (sourceStream);

    if (fileStream == nullptr)
    {
        if (deleteStreamIfOpeningFails)
            delete sourceStream;

        return nullptr;
    }

    std::unique_ptr<MediaFoundationReader> reader (new MediaFoundationReader (sourceStream, fileStream->getFile()));

    if (reader->isValid())
        return reader.release();

    if (! deleteStreamIfOpeningFails)
        reader->input = nullptr;

    return nullptr;
}

std::unique_ptr<juce::AudioFormatWriter> MediaFoundationAudioFormat::createWriterFor (std::unique_ptr<juce::OutputStream>&,
                                                                                      const juce::AudioFormatWriterOptions&)
{
    return nullptr;
}
}
