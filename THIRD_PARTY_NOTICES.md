# Third-party software and license notices

WaveCue's downloadable DLL statically links libraries under GNU GPL, so the **combined executable is distributed under GPL version 3**. The original MIT license grant for the WaveCue-authored source remains in LICENSES/MIT-WaveCue-original.txt. The full GPLv3 license is in LICENSE.

| Software | Version pinned for RC1 | License | Source |
|---|---|---|---|
| libKeyFinder | 941e517ebf853c2153a8b9d6efcc0c729199aa0b | GPL-3.0-or-later | https://github.com/mixxxdj/libkeyfinder |
| BTrack | 9d6127618a5679e9caa74c594b88f1d74f0e035f | GPLv3 | https://github.com/adamstark/BTrack |
| FFTW3 | 3.3.11 (vcpkg static x86) | GPL-2.0-or-later | https://fftw.org/ |
| libsamplerate | 0.2.2 (vcpkg static x86) | BSD 2-Clause | https://github.com/libsndfile/libsamplerate |
| dr_flac | 4ffbded724cc3e3d2838eebca70858bf5984385a | Public domain / MIT-0 | https://github.com/mackron/dr_libs |
| stb_vorbis | 1ee679ca2ef753a528db5ba6801e1067b40481b8 | Public domain / MIT | https://github.com/nothings/stb |

Copyright and authorship belong to each upstream copyright holder. In particular: libKeyFinder by Ibrahim Sha'ath and Mixxx contributors; BTrack by Adam Stark and contributors; FFTW by Matteo Frigo and Steven G. Johnson and contributors; libsamplerate by Erik de Castro Lopo and contributors; dr_flac by David Reid; stb_vorbis by Sean Barrett and contributors.

The complete GPLv3 license accompanies each binary as LICENSE.txt. The BSD 2-Clause notice for libsamplerate and the original WaveCue MIT license are in LICENSES/. The upstream copyright notices embedded in the source and distributed libraries remain in the corresponding-source archive.

**Build-time changes:** The Windows build workflow adapts some BTrack variable-length arrays to C++ vectors and initializes an SRC_DATA structure. Its exact modifications are in .github/workflows/windows-build.yml and are reflected in the source release archive.

The source archive delivered next to the binary includes the matching WaveCue source, pinned external sources, vcpkg source trees and port patches, and build instructions. See docs/SOURCE_BUILD.md.

Winamp binaries and trademarks are not part of WaveCue's distribution.
