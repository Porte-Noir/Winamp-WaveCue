# RC1 release checklist

- [ ] Confirm the repository contains the latest WaveCue **and Set Planner** sources
- [ ] Verify latest commit and record its full SHA
- [ ] Verify Winamp 32-bit compatibility and the supported Windows versions
- [ ] Build both components on GitHub Actions from that exact SHA
- [ ] Verify the compiled DLL and any required Set Planner files are in the package
- [ ] Test install, restart, operation and uninstall on a clean Winamp installation
- [ ] Verify waveform, cues, BPM/key, batch load, planner, export and stream delay
- [ ] Review third-party decoder licenses and redistribution notices
- [ ] Add the three screenshots to the repository and link them from README
- [ ] Finalize English README and installation instructions based on verified build
- [ ] Scan the final ZIP and publish its SHA-256 checksum
- [ ] Create GitHub prerelease tagged v1.0.0-rc1 from the verified commit
- [ ] Attach the ZIP and release notes
- [ ] Add repository description and GitHub topics
- [ ] **Repository visibility change to public: reserved for Porte-Noir**
