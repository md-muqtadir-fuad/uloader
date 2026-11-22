# Building the setup program

`build-installer.bat` makes `installer\out\U-loader-Setup-<version>.exe`:
an *I agree -> Next -> Install* setup that installs U-loader for the
current Windows user (no administrator rights) into
`%LOCALAPPDATA%\Programs\U-loader`.

## Once

```
winget install JRSoftware.InnoSetup     the setup compiler (Inno Setup 6)
winget install Gyan.FFmpeg              optional part: FFmpeg
winget install DenoLand.Deno            optional part: Deno
uloader --get-ytdlp                     optional part: yt-dlp (next to uloader.exe)
```

## Each release

1. Set the version in `src\uloader.h` (`UL_VERSION`), `src\gui.rc` (both
   `VERSIONINFO` numbers), `extension\manifest.json` and `uloader.iss`
   (`AppVersion`), and add the release to `CHANGELOG.md`.
2. Run `installer\build-installer.bat`. It builds U-loader, copies the
   helper programs it finds into `installer\bin\` (a missing one is simply
   left out of the setup) and compiles `uloader.iss`.
3. Try the setup on a clean account: install, download a file and a
   video, uninstall.
4. Publish the setup on the GitHub release page. **If it includes FFmpeg**
   (GPL v3), publish the FFmpeg source with it: a link to the exact build's
   source listed at <https://www.gyan.dev/ffmpeg/builds/> (or an archive of
   it), as [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) promises.

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
