# WaveCue + Set Planner — v1.0.0-rc1

**First public release candidate** for classic 32-bit Winamp, with integrated Set Planner.

## Highlights

- Waveforms for selected and actually playing tracks
- In/Out cue editing, per-track Auto In/Out and protected Seek
- Key detection in Camelot notation and BPM analysis
- Cached loading, playlist batch preloading and streaming-delay display
- Set Planner: harmonic transitions, locks, manual arrangement, undo, and export to Winamp
- French-language options in WaveCue preferences
- Latest change: **yellow indication of inferred half/double BPM** while preserving the existing sort logic

## Install

Download **WaveCue-v1.0.0-rc1-win32.zip** in the Assets section. Exit Winamp and copy gen_wavecue.dll to the Plugins directory. Restart Winamp. Set Planner is built into WaveCue.

For upgrades and removal, see [INSTALL.md](INSTALL.md).

## License and source

The combined plugin is distributed under **GNU GPL version 3**, because it statically links GPL-licensed libraries. Download the **WaveCue-v1.0.0-rc1-source.zip** alongside the binary: this contains corresponding source code and build recipes, including external libraries not in GitHub's auto-generated source archive.

Third-party credits and license details: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Credits

**Concept, direction, testing:** Porte-Noir  
**AI-assisted development:** ChatGPT (OpenAI)

This is a prerelease. Reproducible bug reports are welcome through GitHub Issues.
