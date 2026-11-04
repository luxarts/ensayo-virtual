#include "BackingTrack.h"
#include "MediaFoundationAudioFormat.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ensayo
{
BackingTrack::BackingTrack()
{
    // Media Foundation first: it is the only one that decodes AAC (.m4a/.mp4 from YouTube). The basic
    // formats still cover wav/flac/ogg/mp3 whenever MF declines a file.
    formats.registerFormat (new MediaFoundationAudioFormat(), false);
    formats.registerBasicFormats();
}

juce::Result BackingTrack::load (const juce::File& file)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("El archivo no existe: " + file.getFullPathName());

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return juce::Result::fail ("Formato de audio no soportado: " + file.getFileName());

    if (reader->sampleRate <= 0.0 || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return juce::Result::fail ("Archivo de audio vacio o invalido: " + file.getFileName());

    const double ratio = reader->sampleRate / (double) sampleRate;
    const int64_t outputLength = (int64_t) std::llround ((double) reader->lengthInSamples / ratio);

    if (outputLength > maxLengthSamples)
        return juce::Result::fail ("La pista supera los 30 minutos: " + file.getFileName());

    if (reader->lengthInSamples > (int64_t) std::numeric_limits<int>::max())
        return juce::Result::fail ("La pista es demasiado larga: " + file.getFileName());

    const int sourceSamples = (int) reader->lengthInSamples;
    const int sourceChannels = (int) juce::jmin (reader->numChannels, 2u);

    juce::AudioBuffer<float> source (sourceChannels, sourceSamples);

    if (! reader->read (&source, 0, sourceSamples, 0, true, sourceChannels > 1))
        return juce::Result::fail ("No se pudo leer la pista: " + file.getFileName());

    // Mono files play centred on both channels.
    if (sourceChannels == 1)
    {
        source.setSize (2, sourceSamples, true, false, true);
        source.copyFrom (1, 0, source, 0, 0, sourceSamples);
    }

    auto result = std::make_shared<Audio>();
    result->name = file.getFileName();

    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        result->buffer = std::move (source);
    }
    else
    {
        result->buffer.setSize (2, (int) outputLength, false, true, false);

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.reset();
            interpolator.process (ratio, source.getReadPointer (ch), result->buffer.getWritePointer (ch),
                                  (int) outputLength, sourceSamples, 0);
        }
    }

    swapIn (std::move (result));
    return juce::Result::ok();
}

void BackingTrack::unload()
{
    swapIn (nullptr);
}

void BackingTrack::swapIn (std::shared_ptr<const Audio> newAudio)
{
    std::shared_ptr<const Audio> old;

    {
        const juce::SpinLock::ScopedLockType scoped (lock);
        playing.store (false);
        position.store (0);
        length.store (newAudio != nullptr ? (int64_t) newAudio->buffer.getNumSamples() : 0);
        loaded.store (newAudio != nullptr);
        current.store (newAudio.get(), std::memory_order_release);
        old = audio.exchange (std::move (newAudio));
    }
    // `old` is released here, outside the lock.
}

void BackingTrack::play() noexcept
{
    if (! loaded.load())
        return;

    // Playing from the end restarts from the beginning.
    if (position.load() >= length.load())
        position.store (0);

    playing.store (true);
}

void BackingTrack::pause() noexcept
{
    playing.store (false);
}

void BackingTrack::stop() noexcept
{
    playing.store (false);
    position.store (0);
}

void BackingTrack::seek (int64_t samples) noexcept
{
    position.store (juce::jlimit ((int64_t) 0, length.load(), samples));
}

void BackingTrack::setLoop (bool shouldLoop) noexcept
{
    loop.store (shouldLoop);
}

TransportState BackingTrack::getState() const noexcept
{
    // Lock-free: never contend with render()'s try-lock.
    TransportState state;
    const auto snapshot = audio.load (std::memory_order_acquire);
    state.loaded = loaded.load();
    state.playing = playing.load();
    state.loop = loop.load();
    state.position = position.load();
    state.length = length.load();

    if (snapshot != nullptr)
        state.name = snapshot->name;

    return state;
}

void BackingTrack::render (float* L, float* R, int numSamples) noexcept
{
    if (! playing.load (std::memory_order_acquire) || numSamples <= 0)
        return;

    const juce::SpinLock::ScopedTryLockType scoped (lock);

    if (! scoped.isLocked())
        return;

    const Audio* source = current.load (std::memory_order_acquire);

    if (source == nullptr)
        return;

    const int total = source->buffer.getNumSamples();
    int64_t pos = position.load (std::memory_order_relaxed);
    int written = 0;

    while (written < numSamples)
    {
        if (pos >= total)
        {
            if (loop.load (std::memory_order_relaxed) && total > 0)
            {
                pos = 0;
            }
            else
            {
                pos = total;
                playing.store (false, std::memory_order_release);
                break;
            }
        }

        const int chunk = (int) std::min<int64_t> (numSamples - written, total - pos);
        juce::FloatVectorOperations::add (L + written, source->buffer.getReadPointer (0, (int) pos), chunk);
        juce::FloatVectorOperations::add (R + written, source->buffer.getReadPointer (1, (int) pos), chunk);
        pos += chunk;
        written += chunk;
    }

    position.store (pos, std::memory_order_release);
}
}
