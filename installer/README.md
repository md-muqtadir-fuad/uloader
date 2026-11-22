# Building the setup program

`build-installer.bat` makes `installer\out\U-loader-Setup-<version>.exe`:
an *I agree -> Next -> Install* setup that installs U-loader for the
current Windows user (no administrator rights) into
`%LOCALAPPDATA%\Programs\U-loader`.

## Once

```
winget install JRSoftware.InnoSetup     the setup compiler (Inno Setup 6)
winget install DenoLand.Deno            optional part: Deno
uloader --get-ytdlp                     optional part: yt-dlp (next to uloader.exe)
```

FFmpeg is built from source by `installer\build-ffmpeg.sh` (run it from Git
Bash; it needs MinGW-w64 gcc). It is a 3 MB build with just what U-loader's
stream copy needs, under the LGPL; the full builds are over 200 MB.

## Each release

1. Set the version in `src\uloader.h` (`UL_VERSION`), `src\gui.rc` (both
   `VERSIONINFO` numbers), `extension\manifest.json` and `uloader.iss`
   (`AppVersion`), and add the release to `CHANGELOG.md`.
2. `sh installer/build-ffmpeg.sh` (Git Bash; once per FFmpeg version), then
   check it: `python -I tests\ffmpeg_e2e.py installer\bin\ffmpeg.exe`.
3. Run `installer\build-installer.bat`. It builds U-loader, copies the
   helper programs it finds into `installer\bin\` (a missing one is simply
   left out of the setup) and compiles `uloader.iss`.
4. Try the setup on a clean account: install, download a file and a
   video, uninstall.
5. Publish a GitHub release named `v<version>` with the setup attached as
   `U-loader-Setup-<version>.exe` - installed copies find it there and offer
   the update (see below). Also attach the FFmpeg source tarball named in
   `build-ffmpeg.sh` (and its `.asc`) and link it in the release notes, as
   [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) promises (LGPL;
   FFmpeg asks for the source next to the binary).
6. Point the download links at the new setup: the button and version line in
   `docs/index.html` (and its `downloadUrl`), and the Install section of
   `README.md`.

## Updates

Once a day, installed copies of U-loader ask the GitHub API for the latest
release. If it is newer, U-loader asks the user; on *Yes* it downloads the
attached `U-loader-Setup-<version>.exe` in the background, checks its size
against the one GitHub announces, and runs it silently
(`/VERYSILENT /RESTARTAPP`) once no download is running - the setup
replaces the program and starts it again. yt-dlp updates itself the same
day without asking. Users can turn both off in Settings.

`installer\bin\` and `installer\out\` are not committed (the helper programs
are large and have their own release cycles).

## What the setup does

- Licence page: U-loader's licence followed by the third-party notices.
- Components: U-loader (always), yt-dlp, FFmpeg, Deno.
- Options: connect the browsers (`uloader --install`, on by default), a
  desktop icon, start with Windows in the notification area.
- Start-menu entry; at the end it can start U-loader and show the
  extension folder for *Load unpacked* in Chrome or Edge.
- Uninstall: closes U-loader, disconnects the browsers
  (`uloader --uninstall`) and removes the program. Downloads and the
  settings in `%APPDATA%\U-loader` are kept.
