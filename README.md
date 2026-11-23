# WaveCue + Set Planner for classic Winamp

**Waveforms, cue points, harmonic analysis and DJ set planning — inside Winamp.**

WaveCue is a free, open-source **General Purpose plugin for classic 32-bit Winamp 5.x**. It adds two waveform views, persistent In/Out cues, BPM and key detection, and an integrated **Set Planner**.

**v1.0.0-rc1 is a prerelease.** WaveCue is an independent community extension, not affiliated with Winamp.

## Download and install

1. Download **WaveCue-v1.0.0-rc1-win32.zip** under [GitHub Releases](../../releases). Choose this ZIP, not the automatically generated source ZIP.
2. Close Winamp completely.
3. Extract **gen_wavecue.dll** to the Winamp Plugins folder (commonly C:\Program Files (x86)\Winamp\Plugins\), replacing an older WaveCue DLL if necessary.
4. Restart Winamp. Find the plugin under **Options > Preferences > Plug-ins > General Purpose**.
5. Open **Set Planner** from WaveCue's preferences. It is built into the same DLL.

See [INSTALL.md](INSTALL.md) for updating, removing and troubleshooting.

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

Search terms: Winamp plugin, waveform, DJ set planner, cue points, BPM, Camelot, harmonic mixing, playlist editor, streaming delay, Twitch DJ, VRChat DJ.
