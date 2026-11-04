#include "client/YouTubeImporter.h"
#include "MessagePump.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace ensayo
{
class YouTubeImporterTests : public juce::UnitTest
{
public:
    YouTubeImporterTests() : juce::UnitTest ("YouTubeImporter", "EnsayoVirtual") {}

    static void pump (int ms) { pumpMessages (ms); }

    void runTest() override
    {
        beginTest ("looksLikeYouTubeUrl");
        {
            expect (YouTubeImporter::looksLikeYouTubeUrl ("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
            expect (YouTubeImporter::looksLikeYouTubeUrl ("http://youtube.com/watch?v=abc"));
            expect (YouTubeImporter::looksLikeYouTubeUrl ("https://youtu.be/dQw4w9WgXcQ"));
            expect (YouTubeImporter::looksLikeYouTubeUrl ("https://music.youtube.com/watch?v=abc"));
            expect (YouTubeImporter::looksLikeYouTubeUrl ("  https://m.youtube.com/watch?v=abc  "));
            expect (! YouTubeImporter::looksLikeYouTubeUrl ("https://vimeo.com/12345"));
            expect (! YouTubeImporter::looksLikeYouTubeUrl ("https://notyoutube.com/watch?v=abc"));
            expect (! YouTubeImporter::looksLikeYouTubeUrl ("C:\\music\\song.mp3"));
            expect (! YouTubeImporter::looksLikeYouTubeUrl (""));
        }

        beginTest ("buildCommand");
        {
            const juce::File ytDlp ("C:\\tools\\yt-dlp.exe");
            const juce::File outDir ("C:\\tmp\\ensayo");
            const auto command = YouTubeImporter::buildCommand (ytDlp, "https://youtu.be/abc", outDir);

            // No "-x" / "--audio-format": nothing is post-processed, so ffmpeg is never needed.
            const juce::StringArray expected { "C:\\tools\\yt-dlp.exe", "-f", "bestaudio[ext=m4a]/bestaudio[ext=mp3]/18/bestaudio",
                                               "--no-playlist", "--no-update", "-o",
                                               "C:\\tmp\\ensayo\\%(title)s.%(ext)s", "--", "https://youtu.be/abc" };

            expectEquals (command.size(), expected.size());

            for (int i = 0; i < juce::jmin (command.size(), expected.size()); ++i)
                expectEquals (command[i], expected[i]);

            // With a bundled deno, yt-dlp is told where it is (it would otherwise only look on the PATH).
            const auto withDeno = YouTubeImporter::buildCommand (ytDlp, "https://youtu.be/abc", outDir, juce::File ("C:\\tools\\deno.exe"));
            expectEquals (withDeno.size(), expected.size() + 2);
            expectEquals (withDeno[withDeno.size() - 4], juce::String ("--js-runtimes"));
            expectEquals (withDeno[withDeno.size() - 3], juce::String ("deno:C:\\tools\\deno.exe"));
            expectEquals (withDeno[withDeno.size() - 2], juce::String ("--"));

            // The URL always follows "--", so it can never be parsed as an option.
            const auto hostile = YouTubeImporter::buildCommand (ytDlp, "--exec=calc", outDir);
            expectEquals (hostile[hostile.size() - 2], juce::String ("--"));
            expectEquals (hostile[hostile.size() - 1], juce::String ("--exec=calc"));
        }

        beginTest ("start refuses a URL that is not YouTube");
        {
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("ytdlp-refuse");
            temp.deleteRecursively();
            temp.createDirectory();

            // A fake yt-dlp that leaves a marker: it must never run.
            const auto marker = temp.getChildFile ("ran.txt");
            const auto fake = temp.getChildFile ("yt-dlp.cmd");
            expect (fake.replaceWithText ("@echo off\r\necho ran > \"" + marker.getFullPathName() + "\"\r\n"));

            for (const auto* url : { "--exec=calc", "https://vimeo.com/12345", "C:\\music\\song.mp3", "" })
            {
                YouTubeImporter importer;
                juce::String error;
                bool finished = false, done = false;
                importer.onFinished = [&] (const juce::File&) { finished = true; done = true; };
                importer.onError = [&] (const juce::String& e) { error = e; done = true; };

                importer.start (fake, url, temp.getChildFile ("out"));

                const auto deadline = juce::Time::getMillisecondCounter() + 5000;

                while (! done && juce::Time::getMillisecondCounter() < deadline)
                    pump (20);

                expect (done && ! finished, juce::String ("no error for url '") + url + "'");
                expectEquals (error, juce::String ("URL de YouTube no valida"));
            }

            pump (100);
            expect (! marker.existsAsFile(), "yt-dlp was executed for a refused URL");
            temp.deleteRecursively();
        }

        beginTest ("findYtDlp");
        {
            const auto found = YouTubeImporter::findYtDlp ("C:\\definitely\\not\\here\\yt-dlp.exe");
            // Without a configured copy the lookup falls back to the exe folder and PATH; whatever it
            // returns must either be nothing or an existing file.
            expect (found == juce::File() || found.existsAsFile(), "returned a file that does not exist");

            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("ytdlp-find");
            temp.createDirectory();
            const auto configured = temp.getChildFile ("yt-dlp.cmd");
            configured.replaceWithText ("@echo off\r\n");
            expect (YouTubeImporter::findYtDlp (configured.getFullPathName()) == configured, "configured .cmd not accepted");
            temp.deleteRecursively();
        }

        beginTest ("start with a fake yt-dlp");
        {
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("ytdlp-run");
            temp.deleteRecursively();
            temp.createDirectory();

            const auto outDir = temp.getChildFile ("out");
            outDir.createDirectory();

            const auto fake = temp.getChildFile ("yt-dlp.cmd");
            juce::String script;
            script << "@echo off\r\n"
                   << "echo [youtube] abc: Downloading webpage\r\n"
                   << "echo [download]  50.0%% of 3.00MiB at 1.00MiB/s ETA 00:01\r\n"
                   << "echo [download] 100%% of 3.00MiB\r\n"
                   << "echo fake m4a > \"" << outDir.getChildFile ("out.m4a").getFullPathName() << "\"\r\n"
                   << "echo [download] Destination: out.m4a\r\n";
            expect (fake.replaceWithText (script));

            YouTubeImporter importer;
            float maxProgress = -1.0f;
            juce::File finished;
            juce::String error;
            bool done = false;

            importer.onProgress = [&] (float p) { maxProgress = juce::jmax (maxProgress, p); };
            importer.onFinished = [&] (const juce::File& f) { finished = f; done = true; };
            importer.onError = [&] (const juce::String& e) { error = e; done = true; };

            importer.start (fake, "https://www.youtube.com/watch?v=abc", outDir);

            const auto deadline = juce::Time::getMillisecondCounter() + 15000;

            while (! done && juce::Time::getMillisecondCounter() < deadline)
                pump (20);

            expect (done, "importer never finished");
            expect (error.isEmpty(), "importer failed: " + error);
            expect (maxProgress >= 0.5f, "progress never reached 0.5 (got " + juce::String (maxProgress) + ")");
            expectEquals (finished.getFileName(), juce::String ("out.m4a"));
            expect (finished.existsAsFile());

            temp.deleteRecursively();
        }

        beginTest ("parseDestinationLine");
        {
            expectEquals (YouTubeImporter::parseDestinationLine ("[download] Destination: C:\\tmp\\Song.m4a"), juce::String ("C:\\tmp\\Song.m4a"));
            expectEquals (YouTubeImporter::parseDestinationLine ("[download] C:\\tmp\\Song.m4a has already been downloaded"), juce::String ("C:\\tmp\\Song.m4a"));
            expectEquals (YouTubeImporter::parseDestinationLine ("[ExtractAudio] Destination: out.mp3"), juce::String ("out.mp3"));
            expectEquals (YouTubeImporter::parseDestinationLine ("[download]  42.0% of 3.00MiB"), juce::String());
            expectEquals (YouTubeImporter::parseDestinationLine ("[youtube] abc: Downloading webpage"), juce::String());
        }

        beginTest ("a video yt-dlp already had (old file, 'already been downloaded') is still delivered");
        {
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("ytdlp-cached");
            temp.deleteRecursively();
            temp.createDirectory();
            const auto outDir = temp.getChildFile ("out");
            outDir.createDirectory();

            // The "download" is a file that already exists, with a modification time long before the run.
            const auto cached = outDir.getChildFile ("Cached song.m4a");
            expect (cached.replaceWithText ("old"));
            expect (cached.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::days (400)));

            const auto fake = temp.getChildFile ("yt-dlp.cmd");
            juce::String script;
            script << "@echo off\r\n"
                   << "echo [download] " << cached.getFullPathName() << " has already been downloaded\r\n"
                   << "echo [download] 100%% of 3.00MiB\r\n";
            expect (fake.replaceWithText (script));

            YouTubeImporter importer;
            juce::File finished;
            juce::String error;
            bool done = false;
            importer.onFinished = [&] (const juce::File& f) { finished = f; done = true; };
            importer.onError = [&] (const juce::String& e) { error = e; done = true; };
            importer.start (fake, "https://www.youtube.com/watch?v=abc", outDir);

            const auto deadline = juce::Time::getMillisecondCounter() + 15000;

            while (! done && juce::Time::getMillisecondCounter() < deadline)
                pump (20);

            expect (done, "importer never finished");
            expect (error.isEmpty(), "importer failed: " + error);
            expect (finished == cached, "delivered " + finished.getFullPathName());
            temp.deleteRecursively();
        }

        beginTest ("findDeno looks next to yt-dlp first");
        {
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("deno-find");
            temp.deleteRecursively();
            temp.createDirectory();

            const auto ytDlp = temp.getChildFile ("yt-dlp.exe");
            const auto deno = temp.getChildFile ("deno.exe");
            expect (ytDlp.replaceWithText ("x"));
            expect (deno.replaceWithText ("x"));
            expect (YouTubeImporter::findDeno (ytDlp) == deno, "deno next to yt-dlp not found");

            deno.deleteFile();
            const auto fallback = YouTubeImporter::findDeno (ytDlp);
            expect (fallback == juce::File() || fallback.existsAsFile(), "returned a deno that does not exist");
            expect (YouTubeImporter::toolsDirectory().getFileName() == "tools");

            temp.deleteRecursively();
        }

        beginTest ("start with a missing yt-dlp reports an error");
        {
            YouTubeImporter importer;
            bool done = false;
            juce::String error;
            importer.onFinished = [&] (const juce::File&) { done = true; };
            importer.onError = [&] (const juce::String& e) { error = e; done = true; };

            importer.start (juce::File ("C:\\nowhere\\yt-dlp.exe"), "https://youtu.be/abc", juce::File ("C:\\nowhere\\out"));

            const auto deadline = juce::Time::getMillisecondCounter() + 5000;

            while (! done && juce::Time::getMillisecondCounter() < deadline)
                pump (20);

            expect (done && error.isNotEmpty(), "missing yt-dlp did not produce an error");
        }
    }
};

static YouTubeImporterTests youTubeImporterTests;
}
