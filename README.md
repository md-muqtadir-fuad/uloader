<p align="center">
  <img src="extension/icons/icon128.png" width="96" height="96" alt="U-loader icon">
</p>

<h1 align="center">U-loader</h1>

<p align="center">
  A multi-connection download manager for Windows, with a browser extension and video-site support.
</p>

---

## Features

- Up to 32 connections per file, with resume after pause, crash or reboot
- Catches downloads from Chrome, Edge and Brave
- Video button on web pages: pick the quality (only the ones the video has) or audio only
- YouTube and 1800+ other sites through yt-dlp
- HLS streams (`.m3u8`), FTP, magnet links and `.torrent` files
- Sorts files into folders by type: Video, Music, Documents, Compressed, Programs, Pictures
- One speed limit for all downloads, switched on and off from the toolbar
- Window, command line and tray icon; no runtime needed (about 430 KB)

## Install

Download `U-loader-Setup-<version>.exe` from
[Releases](https://github.com/md-muqtadir-fuad/uloader/releases) and run it.
It installs for your account only and can include yt-dlp, FFmpeg and Deno.

Windows may say *"Windows protected your PC"* for a new program: choose
**More info → Run anyway**.

To build it yourself, see [Building](#building).

## Using U-loader

### Window

| To | Do |
|---|---|
| Add a download | **Add link** (Ctrl+N), paste (Ctrl+V), drop a link or `.torrent` file, or copy a link anywhere |
| Pause / resume | Toolbar or right-click menu |
| Remove from the list | **Del** (files are kept) |
| Remove and delete the file | **Shift+Del** (goes to the Recycle Bin) |
| Limit the speed | **Speed limiter** button; set the value in **Settings** |
| Quit | Right-click the tray icon → **Exit** (closing the window keeps downloads running) |

Settings and the download list are stored in `%APPDATA%\U-loader`.

### Browser extension

The installer connects the browsers. To add the extension:

1. Open `edge://extensions` or `chrome://extensions` and turn on **Developer mode**.
2. Click **Load unpacked** and choose the `extension` folder
   (in U-loader: **Settings → Open folder**).

Then:

- Hover a video and click the **U-loader** button to download it, choosing a quality.
- Downloads of the file types listed in the popup go to U-loader. If a site only
  allows the browser (for example, it answers *403 Forbidden*), the browser keeps
  the download.
- Right-click a link, video or page → **Download with U-loader**.

If IDM is installed, turn off its browser integration, or it takes the downloads first.

### Command line

```
uloader [options] LINK...
```

| Option | Meaning |
|---|---|
| `-c N` | connections per file (1–32, default 8) |
| `-d DIR` | save into DIR |
| `-o NAME` | file name (single link) |
| `-i FILE` | download the links in a text file, one per line |
| `-l RATE` | speed limit, e.g. `500K`, `2M` |
| `-q H` | video quality: `1080`, `720`, ... or `audio` |
| `--categories` | sort into folders by type |
| `--cookies FILE` | use a `cookies.txt` export |
| `-x HOST:PORT` | proxy |
| `--get-ytdlp` | install or update yt-dlp |
| `--install` | connect the browsers (`--uninstall` to remove) |

`uloader --help` lists every option. Ctrl+C pauses; run the same command again to resume.

```
uloader -c 16 -d D:\Downloads https://example.com/file.iso
uloader -q 720 https://www.youtube.com/watch?v=...
uloader -i links.txt --categories
```

## Troubleshooting

| Problem | Fix |
|---|---|
| Video sites fail or show low qualities | **Settings → Install / update yt-dlp**, and install Deno (`winget install DenoLand.Deno`) |
| Video and audio saved separately | Install FFmpeg (`winget install Gyan.FFmpeg`) |
| Browser downloads are not caught | The popup must say *connected* (run `uloader --install`); check the file type list; turn off IDM's integration |
| Files go into `Video\`, `Music\`, ... | Turn off **Sort into folders by type** in Settings |

Not supported: FTPS, DRM-protected video (Netflix, Spotify, ...), seeding torrents.

## Building

Needs 64-bit MinGW-w64 gcc ([WinLibs](https://winlibs.com) or [MSYS2](https://www.msys2.org)).

```
build.bat
```

This builds `uloader.exe` (command line and browser bridge) and `uloader-gui.exe`
(the window). The installer is built with Inno Setup; see
[installer/README.md](installer/README.md).

## Tests

```
python -I tests\run_tests.py uloader.exe
```

The `tests` folder also has unit, FTP, BitTorrent, window, browser and
hostile-server suites. They are isolated from an installed U-loader; each
file's header explains how to run it.

## Project layout

| Path | Contents |
|---|---|
| `src/` | the program: engine, HTTP, FTP, HLS, BitTorrent, yt-dlp bridge, window, command line |
| `extension/` | browser extension (Manifest V3) |
| `installer/` | Inno Setup script |
| `tests/` | test suites |

## License

Free for non-commercial use under the
[PolyForm Noncommercial License 1.0.0](LICENSE).
Versions before 1.3.0 were released under MIT.
Bundled tools keep their own licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
