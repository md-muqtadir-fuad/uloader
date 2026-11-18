# U-loader

A fast, multi-connection download manager for Windows, written in C.
It has a window, a command line, and a browser extension. There are no
runtimes or DLLs to install: `uloader-gui.exe` is about 400 KB and
`uloader.exe` about 280 KB.

## What it downloads

| Link | How |
|---|---|
| http / https files | up to 32 connections per file, resumable |
| ftp:// files | multiple connections, resumable (plain FTP, not FTPS) |
| HLS streams (`.m3u8`) | parallel segments, AES-128 decryption, picks the best quality |
| YouTube, Facebook, TikTok, X, Vimeo and 1800+ other sites | yt-dlp finds the video links; U-loader downloads them |
| magnet links, `.torrent` files and URLs | its own BitTorrent client: trackers, DHT, magnet metadata |

**Speed.** Each connection takes over half of the largest unfinished part
when its own part is done ("dynamic segmentation", the same idea IDM
uses), so every connection stays busy until the end. On a server that
limits each connection to 1.5 MB/s, a 31 MB file took 21 s with 1
connection and 3 s with 8.

**Resume.** Pause, a crash, a lost network or a reboot all resume from
where they stopped. A file that changed on the server is detected and
started over, never mixed with the old data.

## Getting started

```
build.bat                     builds uloader.exe and uloader-gui.exe (needs MinGW gcc)
uloader-gui.exe               the window
uloader --install             connect the browsers (once)
uloader --get-ytdlp           enable video sites (once); add ffmpeg for 1080p/4K merging:
                              winget install Gyan.FFmpeg
```

### Browser extension (Chrome, Edge, Brave)

1. Run `uloader --install` once. It registers U-loader for your user
   account only; `uloader --uninstall` removes it.
2. Open `chrome://extensions` (Edge: `edge://extensions`) and turn on
   **Developer mode**.
3. Click **Load unpacked** and choose the `extension` folder.

What the extension does:
- Shows a **Download this video** button over videos, like IDM's panel.
  It appears when a video plays or when you move the mouse over it,
  works in fullscreen, offers the file, any detected streams, or the best
  quality via yt-dlp, and can be turned off in the popup.
- Adds **Download with U-loader** to the right-click menu of links, videos and pages.
- **Takes over browser downloads** of the file types listed in its
  settings, like IDM. If U-loader can't be reached, the browser keeps
  the download.
- Detects **videos playing** on a page: the icon shows a count, and the
  popup lists them, with a **Download video on this page** button.
- Passes the page's login cookies along, so downloads that need you to
  be signed in still work. The cookies go only to U-loader on your PC
  and only to the site they belong to.

> If IDM is also installed, its browser integration grabs downloads
> first. Turn it off in IDM's options if you want U-loader to catch them.

### The window

Includes a download list with progress bars, and a detail pane showing the
segment map, speed, connections and a log. Also: a queue (default 3 at a
time), pause and resume, a tray icon with notifications, clipboard
watching, drag & drop of links and `.torrent` files, and per-download
folder, connections and video quality. The list is kept across restarts
in `%APPDATA%\U-loader`.

### Command line

```
uloader                                 interactive: paste links
uloader -c 16 -d D:\Downloads URL       16 connections, into D:\Downloads
uloader -i links.txt                    a queue
uloader -l 2M URL                       speed limit
uloader -q 720 https://youtu.be/...     video, at most 720p
uloader "magnet:?xt=urn:btih:..."       torrent
uloader --cookies cookies.txt URL       use a browser cookies export
uloader --help                          everything else
```

Ctrl+C pauses; running the same command again resumes.

## Safety

- File names from servers, torrents and video titles are cleaned, so
  they can't write outside the download folder or use device names
  like `CON` and `COM1`.
- Cookies and Authorization headers are never sent to another site
  after a redirect.
- Helper programs (yt-dlp, ffmpeg) are only taken from U-loader's
  folder or your PATH, never from the download folder.
- Torrents use outgoing connections only. Nothing listens, so there
  are no firewall prompts.

## Tests

```
python -I tests\run_tests.py uloader.exe        30 checks against a misbehaving local server
node tests\browser_e2e.mjs ...                  the extension in a real browser (see the file header)
```

`tests\ftp`, `tests\torrent`, `tests\gui` and `tests\adversarial` hold
the larger suites used during development. They cover a local FTP
server with misbehaviour switches, a local BitTorrent swarm, GUI
automation, and hostile-server attack scripts. They were written
against a development folder layout, so check the paths at the top of
each script before running them.

## Limits

- No FTPS, no uTP or encrypted BitTorrent, and torrents don't seed
  after finishing.
- DRM-protected video (Netflix, Prime, Spotify, Widevine/FairPlay)
  cannot be downloaded.
- If a server sends neither an ETag nor a date, a file that changes
  while it is downloading can't be detected. A same-named file of the
  same size is then treated as already downloaded.
- YouTube needs yt-dlp. Recent yt-dlp versions also want a JavaScript
  runtime for full YouTube support (`winget install DenoLand.Deno`).

## License

MIT - see [LICENSE](LICENSE).

## Source

| File | |
|---|---|
| `src\engine.c` | multi-connection engine, dynamic segmentation, resume |
| `src\http.c` | HTTP(S) on WinHTTP: redirects, cookie scoping, cancellation |
| `src\ftp.c` | FTP transport plugged into the same engine |
| `src\hls.c` | m3u8 streams, AES-128 |
| `src\torrent.c` | BitTorrent: bencode, trackers, DHT, peer wire, magnet metadata |
| `src\extract.c` | yt-dlp bridge for video sites |
| `src\task.c` | one download, run the same way by the window, the command line and the browser |
| `src\host.c` | browser bridge (native messaging) and `--install` |
| `src\gui.c` | the window |
| `src\main.c` | command line |
| `src\util.c` | strings, UTF-8, JSON, cookies, files, processes |
| `extension\` | the browser extension (Manifest V3) |
