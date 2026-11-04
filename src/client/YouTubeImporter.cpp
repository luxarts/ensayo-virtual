#include "client/YouTubeImporter.h"

#include <juce_events/juce_events.h>
#include <cstring>

namespace ensayo
{
namespace
{
    juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

    bool isRunnable (const juce::File& file)
    {
        if (! file.existsAsFile())
            return false;

        const auto ext = file.getFileExtension().toLowerCase();
        return ext == ".exe" || ext == ".cmd" || ext == ".bat" || ext == ".com";
    }

    // Fallback when yt-dlp's output named no file: the newest audio file in dir (whatever container it chose).
    // No "modified after we started" filter: yt-dlp keeps a file it already has ("has already been downloaded")
    // and may give a fresh file the upload date of the video as its mtime.
    juce::File newestAudioIn (const juce::File& dir)
    {
        juce::File newest;
        juce::Time newestTime;

        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
        {
            const auto ext = entry.getFile().getFileExtension().toLowerCase();

            if (ext != ".m4a" && ext != ".mp4" && ext != ".aac" && ext != ".mp3" && ext != ".webm" && ext != ".opus"
                 && ext != ".ogg" && ext != ".wav" && ext != ".flac")
                continue;

            const auto modified = entry.getModificationTime();

            if (newest == juce::File() || modified > newestTime)
            {
                newest = entry.getFile();
                newestTime = modified;
            }
        }

        return newest;
    }
}

//==============================================================================
class YouTubeImporter::Worker : public juce::Thread
{
public:
    Worker (YouTubeImporter& o, std::shared_ptr<std::atomic<int>> gen, int myGen,
            juce::File exe, juce::String urlToFetch, juce::File dir, juce::File denoExe)
        : juce::Thread ("YouTubeImporter"), owner (o), generation (std::move (gen)), myGeneration (myGen),
          ytDlp (std::move (exe)), url (std::move (urlToFetch)), outDir (std::move (dir)), deno (std::move (denoExe))
    {
    }

    ~Worker() override
    {
        signalThreadShouldExit();
        process.kill();
        stopThread (5000);
    }

    void run() override
    {
        if (! isRunnable (ytDlp))
        {
            fail ("No se encuentra yt-dlp: " + ytDlp.getFullPathName());
            return;
        }

        if (! outDir.createDirectory().wasOk())
        {
            fail ("No se pudo crear la carpeta temporal: " + outDir.getFullPathName());
            return;
        }

        auto command = buildCommand (ytDlp, url, outDir, deno);

        // Batch files need the command interpreter.
        const auto ext = ytDlp.getFileExtension().toLowerCase();

        if (ext == ".cmd" || ext == ".bat")
        {
            command.insert (0, "/c");
            command.insert (0, "cmd.exe");
        }

        if (! process.start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            fail ("No se pudo ejecutar yt-dlp: " + ytDlp.getFullPathName());
            return;
        }

        juce::String pending, lastLines, destination;
        juce::MemoryBlock chunk (4096);

        auto consumeLine = [&] (const juce::String& line)
        {
            const auto trimmed = line.trim();

            if (trimmed.isEmpty())
                return;

            lastLines = (lastLines + "\n" + trimmed).getLastCharacters (2000);

            if (const auto named = parseDestinationLine (trimmed); named.isNotEmpty())
                destination = named;

            if (const auto progress = parseProgressLine (trimmed))
                report (*progress);
        };

        while (! threadShouldExit())
        {
            const int n = process.readProcessOutput (chunk.getData(), (int) chunk.getSize());

            if (n > 0)
            {
                pending += juce::String::fromUTF8 ((const char*) chunk.getData(), n);

                // yt-dlp rewrites its progress line with '\r'; treat both separators as line ends.
                for (;;)
                {
                    const int cr = pending.indexOfChar ('\r');
                    const int lf = pending.indexOfChar ('\n');
                    int cut = -1;

                    if (cr >= 0 && lf >= 0)
                        cut = juce::jmin (cr, lf);
                    else
                        cut = juce::jmax (cr, lf);

                    if (cut < 0)
                        break;

                    consumeLine (pending.substring (0, cut));
                    pending = pending.substring (cut + 1);
                }
            }
            else if (! process.isRunning())
            {
                break;
            }
            else
            {
                wait (20);
            }
        }

        if (threadShouldExit())
        {
            process.kill();
            return;
        }

        consumeLine (pending);
        const auto exitCode = process.getExitCode();
        juce::File produced;

        if (destination.isNotEmpty())
        {
            const juce::File named = juce::File::isAbsolutePath (destination) ? juce::File (destination)
                                                                               : outDir.getChildFile (destination);
            if (named.existsAsFile())
                produced = named;
        }

        if (produced == juce::File())
            produced = newestAudioIn (outDir);

        if (exitCode != 0 || produced == juce::File())
        {
            fail ("yt-dlp " + utf8 ("fall\xc3\xb3") + " (" + juce::String ((int) exitCode) + "):\n" + lastLines.trim());
            return;
        }

        report (1.0f);

        post ([this, produced]
        {
            if (owner.onFinished != nullptr)
                owner.onFinished (produced);
        });
    }

private:
    void post (std::function<void()> fn)
    {
        juce::MessageManager::callAsync ([gen = generation, my = myGeneration, fn = std::move (fn)]
        {
            if (gen->load() == my)
                fn();
        });
    }

    void report (float progress)
    {
        post ([this, progress]
        {
            if (owner.onProgress != nullptr)
                owner.onProgress (progress);
        });
    }

    void fail (const juce::String& message)
    {
        post ([this, message]
        {
            if (owner.onError != nullptr)
                owner.onError (message);
        });
    }

    YouTubeImporter& owner;
    std::shared_ptr<std::atomic<int>> generation;
    const int myGeneration;
    const juce::File ytDlp;
    const juce::String url;
    const juce::File outDir;
    const juce::File deno;
    juce::ChildProcess process;
};

//==============================================================================
YouTubeImporter::YouTubeImporter() : generation (std::make_shared<std::atomic<int>> (0)) {}

YouTubeImporter::~YouTubeImporter()
{
    cancel();
}

namespace
{
    // First runnable <dir>\<name> over the given names, or File().
    juce::File firstRunnableIn (const juce::File& dir, std::initializer_list<const char*> names)
    {
        for (const auto* name : names)
            if (isRunnable (dir.getChildFile (name)))
                return dir.getChildFile (name);

        return {};
    }

    juce::File firstRunnableOnPath (std::initializer_list<const char*> names)
    {
        const auto path = juce::SystemStats::getEnvironmentVariable ("PATH", {});

        for (const auto& entry : juce::StringArray::fromTokens (path, ";", "\""))
        {
            const auto dirName = entry.trim().unquoted();

            if (dirName.isEmpty())
                continue;

            if (const auto found = firstRunnableIn (juce::File (dirName), names); found != juce::File())
                return found;
        }

        return {};
    }
}

juce::File YouTubeImporter::toolsDirectory()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile ("tools");
}

juce::File YouTubeImporter::findYtDlp (const juce::String& configuredPath)
{
    const auto names = { "yt-dlp.exe", "yt-dlp.cmd", "yt-dlp.bat" };

    if (configuredPath.trim().isNotEmpty())
    {
        const juce::File configured (configuredPath.trim().unquoted());

        if (isRunnable (configured))
            return configured;

        if (configured.isDirectory())
            if (const auto found = firstRunnableIn (configured, names); found != juce::File())
                return found;
    }

    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    for (const auto& dir : { toolsDirectory(), exeDir })
        if (const auto found = firstRunnableIn (dir, names); found != juce::File())
            return found;

    return firstRunnableOnPath (names);
}

juce::File YouTubeImporter::findDeno (const juce::File& ytDlp)
{
    const auto names = { "deno.exe" };
    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    for (const auto& dir : { ytDlp.getParentDirectory(), toolsDirectory(), exeDir })
        if (dir != juce::File())
            if (const auto found = firstRunnableIn (dir, names); found != juce::File())
                return found;

    return firstRunnableOnPath (names);
}

bool YouTubeImporter::looksLikeYouTubeUrl (const juce::String& input)
{
    const auto text = input.trim().toLowerCase();

    if (! (text.startsWith ("http://") || text.startsWith ("https://")))
        return false;

    auto host = text.fromFirstOccurrenceOf ("://", false, false);
    host = host.upToFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false)
               .upToFirstOccurrenceOf (":", false, false);

    if (host.contains ("@"))
        host = host.fromLastOccurrenceOf ("@", false, false);

    return host == "youtube.com" || host.endsWith (".youtube.com")
        || host == "youtu.be" || host.endsWith (".youtu.be")
        || host == "youtube-nocookie.com" || host.endsWith (".youtube-nocookie.com");
}

const char* YouTubeImporter::audioFormatSelector() noexcept
{
    // AAC in MP4 (itag 140, DASH container) decodes natively on Windows (Media Foundation); mp3 is rare on YouTube
    // but cheap to accept; 18 is the progressive mp4 (video + AAC) every video has, decodable the same way; then
    // whatever is best (webm/opus may not be decodable, the server reports it). No post-processing → no ffmpeg.
    return "bestaudio[ext=m4a]/bestaudio[ext=mp3]/18/bestaudio";
}

juce::StringArray YouTubeImporter::buildCommand (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir,
                                                 const juce::File& deno)
{
    juce::StringArray command { ytDlp.getFullPathName(), "-f", audioFormatSelector(), "--no-playlist", "--no-update",
                                "-o", outDir.getChildFile ("%(title)s.%(ext)s").getFullPathName() };

    if (deno != juce::File())
        command.addArray ({ "--js-runtimes", "deno:" + deno.getFullPathName() });

    // "--" ends option parsing, so a URL that starts with '-' can never be taken for a yt-dlp option.
    command.addArray ({ "--", url.trim() });
    return command;
}

juce::String YouTubeImporter::parseDestinationLine (const juce::String& line)
{
    const auto trimmed = line.trim();

    for (const auto* prefix : { "[download] Destination: ", "[ExtractAudio] Destination: ", "[Merger] Merging formats into \"" })
        if (trimmed.startsWith (prefix))
            return trimmed.substring ((int) std::strlen (prefix)).trim().unquoted();

    if (trimmed.startsWith ("[download] ") && trimmed.endsWith (" has already been downloaded"))
        return trimmed.substring (11, trimmed.length() - 28).trim();

    return {};
}

std::optional<float> YouTubeImporter::parseProgressLine (const juce::String& line)
{
    const auto trimmed = line.trim();

    if (! trimmed.startsWith ("[download]"))
        return std::nullopt;

    const auto rest = trimmed.fromFirstOccurrenceOf ("[download]", false, false).trim();
    const auto percent = rest.indexOfChar ('%');

    if (percent <= 0)
        return std::nullopt;

    const auto number = rest.substring (0, percent).trim();

    if (number.isEmpty() || ! number.containsOnly ("0123456789."))
        return std::nullopt;

    return juce::jlimit (0.0f, 1.0f, number.getFloatValue() / 100.0f);
}

void YouTubeImporter::start (const juce::File& ytDlp, const juce::String& url, const juce::File& outDir, const juce::File& deno)
{
    cancel();
    const int myGeneration = ++(*generation);

    // Only YouTube URLs reach yt-dlp; anything else is reported the same way a failed download is (asynchronously).
    if (! looksLikeYouTubeUrl (url))
    {
        juce::MessageManager::callAsync ([this, gen = generation, my = myGeneration]
        {
            if (gen->load() == my && onError != nullptr)
                onError ("URL de YouTube no valida");
        });

        return;
    }

    worker = std::make_unique<Worker> (*this, generation, myGeneration, ytDlp, url, outDir, deno);
    worker->startThread();
}

void YouTubeImporter::cancel()
{
    ++(*generation);   // drops callbacks already queued by the old worker
    worker = nullptr;  // kills the process and joins the thread
}
}
