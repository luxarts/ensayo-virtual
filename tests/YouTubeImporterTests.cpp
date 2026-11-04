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

            const juce::StringArray expected { "C:\\tools\\yt-dlp.exe", "-x", "--audio-format", "mp3",
                                               "--audio-quality", "0", "--no-playlist", "-o",
                                               "C:\\tmp\\ensayo\\%(title)s.%(ext)s", "--", "https://youtu.be/abc" };

            expectEquals (command.size(), expected.size());

            for (int i = 0; i < juce::jmin (command.size(), expected.size()); ++i)
                expectEquals (command[i], expected[i]);

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
                   << "echo fake mp3 > \"" << outDir.getChildFile ("out.mp3").getFullPathName() << "\"\r\n"
                   << "echo [ExtractAudio] Destination: out.mp3\r\n";
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
            expectEquals (finished.getFileName(), juce::String ("out.mp3"));
            expect (finished.existsAsFile());

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
