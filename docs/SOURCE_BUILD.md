# Building WaveCue and the Corresponding Source

The official RC1 download offers **two adjacent packages**:

- WaveCue-v1.0.0-rc1-win32.zip — DLL and user-facing license and installation files
- WaveCue-v1.0.0-rc1-source.zip — corresponding sources, upstream dependencies, build recipes and patches

The *corresponding-source ZIP* is required because the source archives generated automatically by GitHub omit the third-party code that the build fetches. The source ZIP contains the actual WaveCue source commit and these third-party sources:

- dr_flac and stb_vorbis at pinned revisions
- libKeyFinder and BTrack at pinned revisions, including the MSVC BTrack source modifications
- FFTW3 3.3.11 and libsamplerate 0.2.2 from vcpkg's unpacked source trees
- the vcpkg port recipes and patches, CMake file, workflow and source manifest

## Compilation

Use Windows with Visual Studio 2022 C++ desktop support, CMake and vcpkg (x86-windows-static). From the source archive root:

1. Install FFTW3 and libsamplerate via vcpkg for x86-windows-static, using the vcpkg revision recorded in SOURCE_MANIFEST.txt.
2. Configure and build:

    cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DFFTW3_INCLUDE_DIR="<vcpkg>/installed/x86-windows-static/include" -DFFTW3_LIBRARY="<vcpkg>/installed/x86-windows-static/lib/fftw3.lib" -DLIBSAMPLERATE_INCLUDE_DIR="<vcpkg>/installed/x86-windows-static/include" -DLIBSAMPLERATE_LIBRARY="<vcpkg>/installed/x86-windows-static/lib/samplerate.lib"
    cmake --build build --config Release

3. The output is build/Release/gen_wavecue.dll.

The GitHub Actions workflow is the authoritative recipe for fetching, patching and building with exact pinned upstream revisions. The release source archive contains both the patched and original third-party notices and includes everything required to modify and rebuild the work, apart from usual Windows system/compiler tools.

Source availability and license terms: GNU GPLv3 section 6; https://www.gnu.org/licenses/gpl-3.0.html

Any redistribution of WaveCue's DLL should preserve access to its corresponding-source archive.
