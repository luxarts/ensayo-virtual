cmake_minimum_required(VERSION 3.22)
project(EnsayoVirtual VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(ENSAYO_DEPS_DIR "${CMAKE_SOURCE_DIR}/../../.deps" CACHE PATH "Pre-cloned dependencies")
if(NOT FETCHCONTENT_SOURCE_DIR_JUCE AND EXISTS "${ENSAYO_DEPS_DIR}/JUCE/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_JUCE "${ENSAYO_DEPS_DIR}/JUCE")
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_OPUS AND EXISTS "${ENSAYO_DEPS_DIR}/opus/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_OPUS "${ENSAYO_DEPS_DIR}/opus")
endif()

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.17-security
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

set(OPUS_CUSTOM_MODES ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.6.1)
FetchContent_MakeAvailable(opus)

# Sources shared by the app, the server and the test runner. Each marker is owned by one task.
set(ENSAYO_CORE_SOURCES
    # T2
    src/core/Protocol.cpp
    src/core/MixMath.cpp
    # T3
    src/core/OpusCodec.cpp
    src/core/JitterBuffer.cpp
    # T4
    src/server/BackingTrack.cpp
    src/server/MixServer.cpp
    # T6
    src/client/PluginSlot.cpp
    src/client/FakeAudioDevice.cpp
    src/client/AudioEngine.cpp
    src/client/Settings.cpp
    # T7
    src/client/SessionLink.cpp
    src/client/SessionClient.cpp
    src/client/YouTubeImporter.cpp
    # T8
    src/client/HeadlessClient.cpp
)
set(ENSAYO_SERVER_SOURCES
    # T5
    src/server/ServerHost.cpp
    src/server/HighResClock.cpp
    src/server/MediaFoundationAudioFormat.cpp
)
set(ENSAYO_UI_SOURCES
    # T9
    src/ui/LobbyView.cpp
    src/ui/SessionView.cpp
    src/ui/MainComponent.cpp
    # T10
    src/ui/Screenshot.cpp
)
set(ENSAYO_TEST_SOURCES
    tests/TestMain.cpp
    tests/BuildConfigTests.cpp
    # T2-tests
    tests/ProtocolTests.cpp
    tests/MixMathTests.cpp
    # T3-tests
    tests/OpusCodecTests.cpp
    tests/JitterBufferTests.cpp
    tests/SpscQueueTests.cpp
    # T4-tests
    tests/BackingTrackTests.cpp
    tests/MediaFoundationAudioFormatTests.cpp
    tests/MixServerTests.cpp
    # T5-tests
    tests/ServerHostTests.cpp
    # T6-tests
    tests/PluginSlotTests.cpp
    tests/FakeAudioDeviceTests.cpp
    tests/AudioEngineTests.cpp
    tests/SettingsTests.cpp
    # T7-tests
    tests/SessionLinkTests.cpp
    tests/SessionClientTests.cpp
    tests/YouTubeImporterTests.cpp
    # T8-tests
    tests/HeadlessClientTests.cpp
    tests/UiTests.cpp
    # T10-tests
    tests/ScreenshotTests.cpp
)

set(ENSAYO_MODULES
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_devices
    juce::juce_audio_formats juce::juce_audio_basics juce::juce_gui_extra
    juce::juce_events juce::juce_core)

set(ENSAYO_DEFINITIONS
    JUCE_ASIO=1 JUCE_WASAPI=0 JUCE_DIRECTSOUND=0 JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1 CUSTOM_MODES=1 JUCE_DISPLAY_SPLASH_SCREEN=0)

set(ENSAYO_SYSTEM_LIBS ws2_32 avrt winmm mf mfplat mfreadwrite mfuuid)

juce_add_gui_app(EnsayoVirtual PRODUCT_NAME "Ensayo Virtual" COMPANY_NAME "EnsayoVirtual")
target_sources(EnsayoVirtual PRIVATE src/Main.cpp ${ENSAYO_UI_SOURCES} ${ENSAYO_SERVER_SOURCES} ${ENSAYO_CORE_SOURCES})
target_include_directories(EnsayoVirtual PRIVATE src)
target_compile_definitions(EnsayoVirtual PRIVATE ${ENSAYO_DEFINITIONS})
target_compile_options(EnsayoVirtual PRIVATE /utf-8)
target_link_libraries(EnsayoVirtual PRIVATE ${ENSAYO_MODULES} Opus::opus ${ENSAYO_SYSTEM_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

juce_add_console_app(EnsayoServer PRODUCT_NAME "EnsayoServer")
target_sources(EnsayoServer PRIVATE src/server/ServerMain.cpp ${ENSAYO_SERVER_SOURCES} ${ENSAYO_CORE_SOURCES})
target_include_directories(EnsayoServer PRIVATE src)
target_compile_definitions(EnsayoServer PRIVATE ${ENSAYO_DEFINITIONS})
target_link_libraries(EnsayoServer PRIVATE ${ENSAYO_MODULES} Opus::opus ${ENSAYO_SYSTEM_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

juce_add_console_app(EnsayoTests PRODUCT_NAME "EnsayoTests")
target_sources(EnsayoTests PRIVATE ${ENSAYO_TEST_SOURCES} ${ENSAYO_UI_SOURCES} ${ENSAYO_SERVER_SOURCES} ${ENSAYO_CORE_SOURCES})
target_include_directories(EnsayoTests PRIVATE src)
target_compile_definitions(EnsayoTests PRIVATE ${ENSAYO_DEFINITIONS} ENSAYO_TESTS=1)
target_compile_options(EnsayoTests PRIVATE /utf-8)
target_link_libraries(EnsayoTests PRIVATE ${ENSAYO_MODULES} Opus::opus ${ENSAYO_SYSTEM_LIBS}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
