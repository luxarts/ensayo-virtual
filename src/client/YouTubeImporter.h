#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
namespace ensayo
{
class YouTubeImporter
{
public:
    YouTubeImporter();
    ~YouTubeImporter();

    static juce::File findYtDlp (const juce::String& configuredPath);   // configured → next to exe → PATH; File() if none
    static bool looksLikeYouTubeUrl (const juce::String&);
    static juce::StringArray buildCommand (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir);
      // {ytdlp, "-x", "--audio-format", "mp3", "--audio-quality", "0", "--no-playlist", "-o", outDir/"%(title)s.%(ext)s", "--", url}
    // Background thread; callbacks on the message thread. Progress parsed from "[download]  42.0%".
    // A url for which looksLikeYouTubeUrl is false is refused with onError ("URL de YouTube no valida") and never run.
    void start (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir);
    void cancel();
    std::function<void (float)> onProgress; std::function<void (const juce::File&)> onFinished; std::function<void (const juce::String&)> onError;

    // Parses one line of yt-dlp output; returns the fraction (0..1) when the line carries download progress.
    static std::optional<float> parseProgressLine (const juce::String& line);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
    std::shared_ptr<std::atomic<int>> generation;   // bumped by cancel()/start() so stale callbacks are dropped

    JUCE_DECLARE_NON_COPYABLE (YouTubeImporter)
};
}
