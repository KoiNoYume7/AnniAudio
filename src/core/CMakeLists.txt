add_library(audio_core STATIC
    audio_utils.cpp
    AudioMixerMatrix.cpp
    global_hotkeys.cpp
    GroupBus.cpp
    InputProcessor.cpp
    OutputMixer.cpp
    midi_input.cpp
)
target_include_directories(audio_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_core PUBLIC ole32 avrt winmm audio_dsp)
