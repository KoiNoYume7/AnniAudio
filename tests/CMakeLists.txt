# Tests / Phase-0 POCs.
#
# These are manual verification tools and proof-of-concept programs, not an
# automated test suite. They are grouped here so the main src/ tree stays
# focused on the production libraries.

include_directories(
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/dsp
    ${CMAKE_SOURCE_DIR}/third_party
)

add_executable(poc_eq poc_eq.cpp)

add_executable(poc_rnnoise poc_rnnoise.cpp)
target_link_libraries(poc_rnnoise PRIVATE rnnoise)

add_executable(poc_hrtf poc_hrtf.cpp)
target_link_libraries(poc_hrtf PRIVATE audio_dsp)

add_executable(poc_wasapi poc_wasapi.cpp)
target_link_libraries(poc_wasapi PRIVATE ole32 avrt)

add_executable(test_routing test_routing.cpp)
target_link_libraries(test_routing PRIVATE audio_core)

add_executable(test_mixer_live_edit test_mixer_live_edit.cpp)
target_link_libraries(test_mixer_live_edit PRIVATE audio_core)

add_executable(test_input_processor test_input_processor.cpp)
target_link_libraries(test_input_processor PRIVATE audio_core)

add_executable(test_new_pipeline test_new_pipeline.cpp)
target_link_libraries(test_new_pipeline PRIVATE audio_core)
