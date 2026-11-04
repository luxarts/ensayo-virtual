#pragma once
// Runs the JUCE message loop for a while from code that owns the message thread but is not inside
// JUCEApplication's own loop (tests, the headless client). runDispatchLoopUntil only exists when
// JUCE_MODAL_LOOPS_PERMITTED is on, so without it the Win32 queue behind JUCE's hidden message
// window is pumped directly (JUCE posts its callAsync messages and timers there).

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#if JUCE_WINDOWS && ! JUCE_MODAL_LOOPS_PERMITTED
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace ensayo
{
inline void pumpMessages (int milliseconds)
{
   #if JUCE_MODAL_LOOPS_PERMITTED
    juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
   #else
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

    for (;;)
    {
        MSG message;

        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessageW (&message);
        }

        const auto now = juce::Time::getMillisecondCounter();

        if (now >= end)
            break;

        MsgWaitForMultipleObjects (0, nullptr, FALSE, (DWORD) (end - now), QS_ALLINPUT);
    }
   #endif
}
}
