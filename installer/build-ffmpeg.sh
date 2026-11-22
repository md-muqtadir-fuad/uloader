#!/bin/sh
# Builds the small FFmpeg that the setup includes: installer/bin/ffmpeg.exe (#12).
#
# U-loader uses FFmpeg only to join a video and an audio stream and to turn
# an HLS (MPEG-TS) stream into MP4, always with stream copy (-c copy). So this
# build has no video codecs, no encoders, no network and no devices - only the
# containers U-loader reads and writes, the parsers and bitstream filters stream
# copy needs, and file input/output. The few audio decoders are there because
# in MPEG-TS FFmpeg must decode a little audio to learn its sample rate and
# channels, even for stream copy. About 3 MB instead of 217 MB.
#
# No --enable-gpl / --enable-version3 / --enable-nonfree: the result is under
# the LGPL 2.1. Its source is the FFmpeg release tarball named below,
# configured with exactly the options in this file.
#
# Needs MinGW-w64 gcc, GNU make, sh, curl, tar and xz (Git Bash has all but
# gcc; gcc e.g. from WinLibs). Run from Git Bash:   sh installer/build-ffmpeg.sh
#   FFMPEG_VERSION=9.0.2   the FFmpeg release to build
#   FFMPEG_WORK=DIR        where to download and build (default installer/ffmpeg-build)
set -e
VERSION=${FFMPEG_VERSION:-9.0.2}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${FFMPEG_WORK:-$HERE/ffmpeg-build}
mkdir -p "$WORK" "$HERE/bin"
cd "$WORK"
[ -f "ffmpeg-$VERSION.tar.xz" ] || curl -fL -o "ffmpeg-$VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
[ -d "ffmpeg-$VERSION" ] || tar -xJf "ffmpeg-$VERSION.tar.xz"
cd "ffmpeg-$VERSION"

OPTIONS="--disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug
 --disable-programs --enable-ffmpeg --disable-avdevice --disable-x86asm --enable-small
 --enable-protocol=file,pipe
 --enable-demuxer=mov,matroska,mpegts,aac,mp3,ogg,flac,wav,h264,hevc
 --enable-muxer=mp4,ipod,mov,matroska,webm,mpegts,adts,mp3,ogg,opus,flac,wav
 --enable-parser=h264,hevc,av1,vp8,vp9,aac,aac_latm,opus,vorbis,mpegaudio,ac3,flac
 --enable-bsf=aac_adtstoasc,h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,vp9_superframe,vp9_superframe_split,av1_frame_merge,av1_frame_split,setts,null
 --enable-decoder=aac,aac_latm,mp3,mp3float,opus,vorbis,ac3,flac
 --enable-filter=null,anull,format,aformat,trim,atrim,copy,acopy
 --cc=gcc --extra-ldflags=-static"

# shellcheck disable=SC2086
./configure $OPTIONS
make -j"${JOBS:-4}"
strip ffmpeg.exe
cp ffmpeg.exe "$HERE/bin/ffmpeg.exe"
{
    echo "FFmpeg $VERSION (https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz), LGPL 2.1, built by installer/build-ffmpeg.sh"
    echo "configure $OPTIONS" | tr '\n' ' '
    echo
} > "$HERE/bin/ffmpeg-build.txt"
ls -l "$HERE/bin/ffmpeg.exe"
