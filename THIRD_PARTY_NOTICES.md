# Third-party notices

U-loader's own code (everything in this repository) is under the
[PolyForm Noncommercial License 1.0.0](LICENSE).

The installer can also include three separate programs. U-loader starts
them as separate processes and talks to them only through their command
line; they are not linked into U-loader and are not covered by U-loader's
license. **Each keeps its own license, printed below or linked**, and
whoever receives them from us receives these rights too, including
commercial use where their license allows it.

| Program | What U-loader uses it for | License |
|---|---|---|
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | finding the video links on YouTube and 1800+ other sites | The Unlicense (public domain) |
| [FFmpeg](https://ffmpeg.org) | joining separate video and audio streams, converting HLS streams to MP4 | GNU LGPL version 2.1 or later (a small build made by `installer/build-ffmpeg.sh`) |
| [Deno](https://deno.com) | the JavaScript runtime yt-dlp needs to read YouTube in full quality | MIT |

The installer's *Components* page lets you leave any of them out.
U-loader also finds them on your `PATH` or next to `uloader.exe` if you
install them yourself (for example `winget install Gyan.FFmpeg` and
`winget install DenoLand.Deno`; `uloader --get-ytdlp` downloads yt-dlp).

---

## yt-dlp

Source: <https://github.com/yt-dlp/yt-dlp>

```
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
```

The yt-dlp Windows executable bundles Python and other libraries under
their own licenses; see the yt-dlp repository for the full list.

## FFmpeg

FFmpeg is Copyright (c) 2000-2026 the FFmpeg developers.

The `ffmpeg.exe` included is a small build made for U-loader by
[installer/build-ffmpeg.sh](installer/build-ffmpeg.sh): only the
containers, parsers and a few audio decoders U-loader's stream copy needs.
It is built without `--enable-gpl`, `--enable-version3` or
`--enable-nonfree`, so it is licensed under the **GNU Lesser General
Public License, version 2.1 or later**:
<https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>.

**Source code.** Its complete source is the unmodified FFmpeg release
tarball named in the script (for U-loader 1.3.0:
<https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz>), configured with the
options in the script; the installed `ffmpeg-build.txt` lists both. Every
release that includes FFmpeg links to that tarball next to the setup
program. You may redistribute, change and rebuild FFmpeg under the LGPL.

## Deno

Source: <https://github.com/denoland/deno>

```
MIT License

Copyright 2018-2026 the Deno authors

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

The Deno executable includes other open-source components (V8 and Rust
crates) under their own licenses; see the Deno repository.
