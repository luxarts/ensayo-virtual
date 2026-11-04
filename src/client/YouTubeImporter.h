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

    // The app ships yt-dlp.exe and deno.exe in <exe dir>\tools so nothing has to be installed.
    static juce::File toolsDirectory();                                  // <exe dir>\tools
    static juce::File findYtDlp (const juce::String& configuredPath);   // configured → tools → next to exe → PATH; File() if none
    static juce::File findDeno (const juce::File& ytDlp);               // next to yt-dlp → tools → next to exe → PATH; File() if none
    static bool looksLikeYouTubeUrl (const juce::String&);
    // {ytdlp, "-f", audioFormatSelector, "--no-playlist", "--no-update", "-o", outDir/"%(title)s.%(ext)s",
    //  ["--js-runtimes", "deno:<deno>"], "--", url}. No ffmpeg: the audio is downloaded as it is (m4a/AAC preferred,
    // decoded server-side by Media Foundation; mp3 as the second choice).
    static juce::StringArray buildCommand (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir,
                                           const juce::File& deno = juce::File());
    static const char* audioFormatSelector() noexcept;   // "bestaudio[ext=m4a]/bestaudio[ext=mp3]/18/bestaudio"
    // Background thread; callbacks on the message thread. Progress parsed from "[download]  42.0%".
    // A url for which looksLikeYouTubeUrl is false is refused with onError ("URL de YouTube no valida") and never run.
    // deno (optional) is handed to yt-dlp with --js-runtimes; without it YouTube may offer fewer formats.
    void start (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir, const juce::File& deno = juce::File());
    void cancel();
    std::function<void (float)> onProgress; std::function<void (const juce::File&)> onFinished; std::function<void (const juce::String&)> onError;

    // Parses one line of yt-dlp output; returns the fraction (0..1) when the line carries download progress.
    static std::optional<float> parseProgressLine (const juce::String& line);
    // The file yt-dlp names in "[download] Destination: X", "[download] X has already been downloaded" (a repeat of
    // the same video keeps the old file, with its old mtime) or a post-processor line; empty for any other line.
    // The finished file is that one when it exists, else the newest audio file in the output folder.
    static juce::String parseDestinationLine (const juce::String& line);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
    std::shared_ptr<std::atomic<int>> generation;   // bumped by cancel()/start() so stale callbacks are dropped

    JUCE_DECLARE_NON_COPYABLE (YouTubeImporter)
};
}
