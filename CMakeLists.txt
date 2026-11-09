cmake_minimum_required(VERSION 3.20)
project(WinampWaveCue LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Le plugin se compile uniquement sous Windows.")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Winamp 5.x classique necessite une DLL 32 bits. Configurez -A Win32.")
endif()

# Include the MSVC runtime statically; useful for legacy Winamp installations.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_library(gen_wavecue SHARED src/gen_wavecue.cpp)
target_compile_features(gen_wavecue PRIVATE cxx_std_17)
target_compile_definitions(gen_wavecue PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(gen_wavecue PRIVATE mfplat mfreadwrite mfuuid ole32 shell32 gdi32 user32)
if(MSVC)
  target_compile_options(gen_wavecue PRIVATE /W4 /EHsc /utf-8)
endif()
set_target_properties(gen_wavecue PROPERTIES PREFIX "" OUTPUT_NAME "gen_wavecue")

# Export the undecorated name required by the classic Winamp SDK.
if(MSVC)
  target_link_options(gen_wavecue PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/gen_wavecue.def")
endif()
