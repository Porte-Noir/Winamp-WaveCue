param(
  [Parameter(Mandatory = $true)]
  [string]$BuildCommit
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$version = 'v1.0.0-rc1'
$dist = Join-Path (Get-Location) 'dist'
$stage = Join-Path $env:RUNNER_TEMP 'wavecue-rc1-stage'
$binaryRoot = Join-Path $stage "WaveCue-$version-win32"
$sourceRoot = Join-Path $stage "WaveCue-$version-source"
$vcpkg = $env:VCPKG_INSTALLATION_ROOT

if (-not $vcpkg -or -not (Test-Path $vcpkg)) { throw 'vcpkg installation is missing' }
if (-not (Test-Path 'build/Release/gen_wavecue.dll')) { throw 'Release DLL is missing' }
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dist,$binaryRoot,$sourceRoot | Out-Null

# The end user only needs one DLL and these text documents.
Copy-Item 'build/Release/gen_wavecue.dll' $binaryRoot
Copy-Item 'INSTALL.md' $binaryRoot
Copy-Item 'THIRD_PARTY_NOTICES.md' $binaryRoot
Copy-Item 'LICENSE' (Join-Path $binaryRoot 'LICENSE.txt')
Copy-Item 'LICENSES' $binaryRoot -Recurse
Copy-Item 'docs/PROJECT_HISTORY.md' $binaryRoot
@"
WaveCue + Set Planner $version
================================
Copy gen_wavecue.dll to the Plugins folder of classic 32-bit Winamp,
then restart Winamp. Set Planner is integrated in the same DLL.

See INSTALL.md for details and THIRD_PARTY_NOTICES.md for third-party
copyright and the GNU GPLv3 license terms.

Matching complete source: WaveCue-$version-source.zip, available alongside
this ZIP on the same GitHub release page.
Source commit: $BuildCommit
"@ | Set-Content -Path (Join-Path $binaryRoot 'START_HERE.txt') -Encoding UTF8

# Build a complete source bundle, including the dependencies that CI fetched.
# Git tracks only project files; decoded headers and third_party are added below.
$tracked = @(git ls-files)
foreach ($file in $tracked) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Tracked source missing: $file" }
  $target = Join-Path $sourceRoot $file
  $folder = Split-Path -Parent $target
  New-Item -ItemType Directory -Force -Path $folder | Out-Null
  Copy-Item -LiteralPath $file -Destination $target -Force
}
foreach ($decoder in @('dr_flac.h','stb_vorbis.c')) {
  $file = Join-Path 'src' $decoder
  if (-not (Test-Path $file)) { throw "Fetched decoder source missing: $file" }
  Copy-Item $file (Join-Path $sourceRoot $file) -Force
}
$third = Join-Path $sourceRoot 'third_party'
New-Item -ItemType Directory -Force -Path $third | Out-Null
foreach ($component in @('libkeyfinder','BTrack')) {
  $original = Join-Path 'third_party' $component
  if (-not (Test-Path $original)) { throw "Third-party source missing: $original" }
  Copy-Item $original (Join-Path $third $component) -Recurse -Force
  $embeddedGit = Join-Path (Join-Path $third $component) '.git'
  if (Test-Path $embeddedGit) { Remove-Item $embeddedGit -Recurse -Force }
}

# Capture the *actual* vcpkg input sources and port patches used by this CI run.
# The packages are linked statically, so these are part of Corresponding Source.
$vcpkgSources = Join-Path $sourceRoot 'vcpkg-source'
New-Item -ItemType Directory -Force -Path $vcpkgSources | Out-Null
foreach ($name in @('fftw3','libsamplerate')) {
  $srcDir = Join-Path $vcpkg "buildtrees/$name/src"
  $clean = Get-ChildItem -Path $srcDir -Directory |
    Where-Object { $_.Name.EndsWith('.clean') } | Select-Object -First 1
  if (-not $clean) { throw "Could not locate vcpkg $name source tree" }
  Copy-Item $clean.FullName (Join-Path $vcpkgSources $name) -Recurse -Force
  Copy-Item (Join-Path $vcpkg "ports/$name") (Join-Path $vcpkgSources "$name-port") -Recurse -Force
  $copyright = Join-Path $vcpkg "installed/x86-windows-static/share/$name/copyright"
  if (Test-Path $copyright) {
    Copy-Item $copyright (Join-Path $vcpkgSources "$name-copyright.txt")
  }
}
$vcpkgCommit = (git -C $vcpkg rev-parse HEAD 2>$null | Select-Object -First 1)
if (-not $vcpkgCommit) { $vcpkgCommit = 'Runner-managed checkout (commit unavailable)' }
@"
WaveCue release: $version
WaveCue source commit: $BuildCommit
GitHub Actions runner: windows-2022
Toolchain: Visual Studio 2022, x86 CMake build
vcpkg source revision: $vcpkgCommit
FFTW3: 3.3.11 (x86-windows-static, source and port patches included)
libsamplerate: 0.2.2#1 (x86-windows-static, source and port patches included)
libKeyFinder: 941e517ebf853c2153a8b9d6efcc0c729199aa0b
BTrack: 9d6127618a5679e9caa74c594b88f1d74f0e035f (patched by workflow)
dr_flac: 4ffbded724cc3e3d2838eebca70858bf5984385a
stb_vorbis: 1ee679ca2ef753a528db5ba6801e1067b40481b8

For rebuild instructions and full licensing details, read
docs/SOURCE_BUILD.md and THIRD_PARTY_NOTICES.md.
"@ | Set-Content -Path (Join-Path $sourceRoot 'SOURCE_MANIFEST.txt') -Encoding UTF8

$binaryZip = Join-Path $dist "WaveCue-$version-win32.zip"
$sourceZip = Join-Path $dist "WaveCue-$version-source.zip"
Compress-Archive -Path (Join-Path $binaryRoot '*') -DestinationPath $binaryZip -CompressionLevel Optimal
Compress-Archive -Path $sourceRoot -DestinationPath $sourceZip -CompressionLevel Optimal

$sumLines = @(
  "$( (Get-FileHash $binaryZip -Algorithm SHA256).Hash.ToLowerInvariant() )  $(Split-Path $binaryZip -Leaf)"
  "$( (Get-FileHash $sourceZip -Algorithm SHA256).Hash.ToLowerInvariant() )  $(Split-Path $sourceZip -Leaf)"
)
$sumLines | Set-Content -Path (Join-Path $dist 'SHA256SUMS.txt') -Encoding ASCII

foreach ($path in @($binaryZip,$sourceZip)) {
  if (-not (Test-Path $path) -or (Get-Item $path).Length -lt 100000) {
    throw "Invalid release package: $path"
  }
}
Write-Host 'Created release archives and SHA-256 checksums:'
Get-ChildItem $dist | Select-Object Name,Length | Format-Table
