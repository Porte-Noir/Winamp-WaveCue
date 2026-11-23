# WaveCue — modern DJ tools and waveform plugin for classic Winamp

**Modernize classic Winamp with waveform displays, cue points, BPM and key analysis, and DJ set planning — without replacing your music player.**

WaveCue is a free, open-source **General Purpose plugin for classic 32-bit Winamp 5.x** on Windows. For listeners looking for a Winamp modernization, enhancement or DJ extension, it adds two waveform views, persistent In/Out cues, BPM and musical-key detection, and an integrated **Set Planner**.

**v1.0.0-rc1 is a prerelease.** WaveCue is an independent community extension, not affiliated with Winamp.

## Screenshots

### WaveCue in classic Winamp

![WaveCue running alongside classic Winamp](screenshots/screenshot%20wavecue.jpg)

### WaveCue waveform windows

![WaveCue Actually Playing and Selected Track waveforms](screenshots/screenshot%20wavecue%20seul.jpg)

### Set Planner

![Set Planner for harmonic DJ set preparation](screenshots/screenshot%20planner%20seul.jpg)

## Download and install

1. Download **WaveCue-v1.0.0-rc1-win32.zip** under [GitHub Releases](../../releases). Choose this ZIP, not the automatically generated source ZIP.
2. Close Winamp completely.
3. Extract **gen_wavecue.dll** to the Winamp Plugins folder (commonly C:\Program Files (x86)\Winamp\Plugins\), replacing an older WaveCue DLL if necessary.
4. Restart Winamp. Find the plugin under **Options > Preferences > Plug-ins > General Purpose**.
5. Open **Set Planner** from WaveCue's preferences. It is built into the same DLL.

See [INSTALL.md](INSTALL.md) for updating, removing and troubleshooting.

## What does WaveCue add to Winamp?

WaveCue keeps the familiar Winamp interface and playlist workflow while adding preparation and mixing-oriented tools. It is especially suited to DJs, internet-radio presenters and music streamers who want to keep using classic Winamp rather than switch to a complete DJ application.

## WaveCue features

- Separate **Actually Playing** and **Selected Track** waveform views
- Manual or optional automatic waveform loading and local caching
- Persistent In/Out points, Auto In/Out and protected Seek mode
- Track preview, BPM analysis and musical keys in Camelot notation
- Visual streaming-delay indication

## Set Planner features

- Import Winamp playlists without changing playback
- Sort all or selected tracks by harmonic transitions
- Color-coded transitions, BPM and Camelot key display
- Track locks, harmonic breaks, manual ordering, undo
- Explicit **Export to Winamp** when the set is ready

The project was developed by a French-speaking creator and includes several French-language interface options.

## Compatibility

- Classic Winamp 5.x **32-bit** on Windows, with General Purpose plugin support
- Local audio files, including MP3, FLAC, M4A, OGG and AIFF (subject to system decoder support)
- Cues and caches are stored locally under %APPDATA%\Winamp_WaveCue; original audio files are unchanged
- Windows 7 SP1 was an original target. Please report compatibility results for specific Windows and Winamp versions.

## License and corresponding source

The **combined plugin DLL is distributed under GNU GPL version 3**, because GPL-licensed components are statically linked: see [LICENSE](LICENSE). WaveCue-originated code previously carried MIT terms, preserved in [LICENSES/MIT-WaveCue-original.txt](LICENSES/MIT-WaveCue-original.txt). Third-party licensing and attribution: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Every official binary ZIP is accompanied by a **matching corresponding-source ZIP** with libraries, build scripts and source. See [docs/SOURCE_BUILD.md](docs/SOURCE_BUILD.md). Source for the generated binary is available for download alongside the executable.

## Credits

**Concept, direction, testing and validation:** Porte-Noir  
**AI-assisted development:** ChatGPT (OpenAI)

Read [the story behind WaveCue](docs/PROJECT_HISTORY.md).

## Finding WaveCue

Common search descriptions include **Winamp modernization**, **Winamp enhancement plugin**, **Winamp waveform plugin**, **Winamp cue points**, **Winamp BPM and key detection** and **DJ set planner for Winamp**. WaveCue is a community-built enhancement for classic Winamp 5.x, not a replacement player.

Source and downloads: [GitHub releases](../../releases).
