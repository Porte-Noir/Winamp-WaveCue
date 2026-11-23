# Installing WaveCue + Set Planner

## Install or update

1. Obtain **WaveCue-v1.0.0-rc1-win32.zip** from the release's Assets.
2. Exit Winamp completely.
3. Extract **gen_wavecue.dll** from the archive into your Winamp **Plugins** folder (commonly C:\Program Files (x86)\Winamp\Plugins\). Replace the previous WaveCue DLL if updating.
4. Restart classic **32-bit Winamp 5.x**.
5. Open **Options > Preferences > Plug-ins > General Purpose** to find WaveCue. Open its preferences to access Set Planner.

Set Planner is integrated into the WaveCue DLL. It does not require a second program.

## Uninstall

1. Close Winamp.
2. Remove **gen_wavecue.dll** from Winamp's Plugins folder.
3. Optional: to erase saved cue points and analysis cache, remove **%APPDATA%\Winamp_WaveCue**. This permanently removes WaveCue's saved local data.

## Troubleshooting

Ensure your Winamp version is 32-bit, and verify the plugin appears under General Purpose plugins after restart. Media Foundation and other Windows components affect format support. Compare the ZIP's SHA-256 digest with SHA256SUMS.txt in Releases and report unexpected antivirus alerts; avoid disabling antivirus protections.
