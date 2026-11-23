# WaveCue RC1 release checklist

## Completed preparations
- [x] Identified latest successful experimental build: run 45255239217, commit historical-private-commit
- [x] Confirmed integrated Set Planner source (src/set_planner.inc)
- [x] Prepared English project history, installation, README, release notes and search metadata
- [x] Reviewed static third-party library licenses
- [x] Prepared GNU GPLv3 text and retained WaveCue's previous MIT license notice
- [x] Prepared packaging workflow to ship DLL and corresponding-source ZIPs

## Automatic CI verifications
- [ ] Build release commit in Win32 GitHub Actions
- [ ] Package binary with license and installation files
- [ ] Package source with fetched libraries, vcpkg source trees, build scripts and patches
- [ ] Compute and upload SHA-256 checksums
- [ ] Publish prerelease v1.0.0-rc1 with both archives attached

## Human acceptance
- [ ] Test archive on a clean Winamp installation
- [ ] Run antivirus check on target Windows machine
- [ ] Upload and place the three supplied screenshots in README (currently stored outside GitHub)
- [ ] **Only Porte-Noir will switch GitHub repository visibility to public**
