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
# libKeyFinder is GPL-3.0-or-later; FFTW3 is GPL-2.0-or-later.
# Both are statically linked in the experimental DLL.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libkeyfinder)
# BTrack GPLv3, pinned and fetched by the Windows CI. FFTW is reused from
# libKeyFinder, while libsamplerate is statically linked for the 32-bit target.
add_library(wavecue_btrack STATIC
  third_party/BTrack/src/BTrack.cpp
  third_party/BTrack/src/OnsetDetectionFunction.cpp)
target_include_directories(wavecue_btrack PUBLIC
  third_party/BTrack/src ${FFTW3_INCLUDE_DIR}
  ${LIBSAMPLERATE_INCLUDE_DIR})
# BTrack.h conditionally adds FFTW pointers to its class layout.
# Consumers must see the same definition to avoid heap corruption.
target_compile_definitions(wavecue_btrack PUBLIC USE_FFTW PRIVATE _USE_MATH_DEFINES)
target_link_libraries(wavecue_btrack PUBLIC
  ${FFTW3_LIBRARY} ${LIBSAMPLERATE_LIBRARY})
add_library(gen_wavecue SHARED src/gen_wavecue.cpp)
target_compile_features(gen_wavecue PRIVATE cxx_std_17)
target_compile_definitions(gen_wavecue PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(gen_wavecue PRIVATE mfplat mfreadwrite mfuuid ole32 shell32 gdi32 user32 comctl32 keyfinder wavecue_btrack)
if(MSVC)
  target_compile_options(gen_wavecue PRIVATE /W4 /EHsc /utf-8)
endif()
set_target_properties(gen_wavecue PROPERTIES PREFIX "" OUTPUT_NAME "gen_wavecue")

# Export the undecorated name required by the classic Winamp SDK.
if(MSVC)
  target_link_options(gen_wavecue PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/gen_wavecue.def")
endif()
