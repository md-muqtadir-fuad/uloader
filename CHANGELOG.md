# Changelog

## 1.3.0 (not released yet)

### New
- **Categories** ([#1](https://github.com/md-muqtadir-fuad/uloader/issues/1)): downloads are sorted into
  `Video`, `Music`, `Documents`, `Compressed`, `Programs` and `Pictures` folders by file type. On by
  default in the window (Settings), `--categories` on the command line. A folder you choose for one
  download is used as it is.
- **Speed limiter** ([#2](https://github.com/md-muqtadir-fuad/uloader/issues/2)): one limit for all
  downloads together, switched on and off with the *Speed limiter* toolbar button or the tray menu.
  Running downloads follow a change at once.
- **Video quality per download** ([#3](https://github.com/md-muqtadir-fuad/uloader/issues/3)): the
  video button's menu and the extension popup offer Best, 2160p, 1440p, 1080p, 720p, 480p, 360p and
  Audio only. Also in the Add dialog and as `-q audio` on the command line.
- The browser keeps a download that U-loader cannot get (for example when the site answers
  "403 Forbidden" to anything but the browser), and says why.

### Changed
- **License** ([#8](https://github.com/md-muqtadir-fuad/uloader/issues/8)): U-loader is now under the
  PolyForm Noncommercial License 1.0.0 - free to use, study and change, but not for commercial
  purposes. Earlier versions were released under MIT.
- **Minimize** ([#7](https://github.com/md-muqtadir-fuad/uloader/issues/7)) keeps the window on the
  taskbar. Only closing the window keeps U-loader running in the notification area.
- Downloaded files are dated with the time they were downloaded, not the server's date.
- The video button is a small "U-loader" chip (just the icon on small videos) and never wraps.
- One version number for the program and the extension.

### Fixed
- Streams (HLS): two downloads that got the same name (for example from a site that calls every
  video `1080p.av1.mp4.m3u8`) shared a folder and broke each other, ending in
  "could not write ... (disk full?)". Each download now owns its folder, and the error messages say
  what really happened. No more `name.mp4.mp4`.
- A file that changes on a server without a version tag (ETag) is no longer saved as a mix of the
  old and the new file; a different file of the same name and size is no longer taken for
  "already downloaded".
- Video titles with line breaks can no longer make yt-dlp's output point to the wrong link.
- Cookies and login headers stay with the site they belong to (redirects to another site,
  HTTPS-to-HTTP and port changes).
- The window no longer freezes when a download is paused and resumed quickly, and plain *Del* no
  longer deletes the partial data of a paused download.
- A handle leak of about one Windows handle per download, which mattered in a window left open for
  weeks.
- FTP: no more tight reconnect loops against servers that answer "421 too many connections".
- Torrents: many robustness fixes (bad peers, lying metadata, read-only files); the tracker is told
  "stopped" when you press Ctrl+C.
- About 75 smaller findings of a full code audit.

## 1.1.0 (2026-11-18)

First public version: multi-connection downloads with resume, the window, the command line, the
browser extension with download capture and the video button, video sites through yt-dlp, HLS
streams, FTP and BitTorrent.
