#!/bin/sh
# DirectorySize — Copyright (c) 2026 META.vn
# Author: Nguyễn Đức Sơn
# SPDX-License-Identifier: GPL-2.0-only
# Regenerates dirsize.ico and png/*.png from dirsize.svg / dirsize-small.svg using
# Side-by-side previews (light/dark background) go to preview/ (git-ignored).
# rsvg-convert + ImageMagick inside Docker.   Usage: sh assets/icon/build-icons.sh
set -eu
cd "$(dirname "$0")"
docker build -q -t dirsize-icons . >/dev/null
export MSYS_NO_PATHCONV=1
HERE="$(pwd -W 2>/dev/null || pwd)"
docker run --rm -v "$HERE:/icon" dirsize-icons sh -euc '
  rm -rf png tmp preview && mkdir -p png tmp preview
  # 16/24 px from the simplified SVG, the rest from the full SVG.
  for s in 16 24; do rsvg-convert -w $s -h $s dirsize-small.svg -o png/dirsize-$s.png; done
  for s in 20; do rsvg-convert -w $s -h $s dirsize-small.svg -o tmp/dirsize-$s.png; done
  for s in 32 40 48 64 128 256 512; do rsvg-convert -w $s -h $s dirsize.svg -o png/dirsize-$s.png; done
  for s in 40; do cp png/dirsize-$s.png tmp/ 2>/dev/null || true; done
  cp png/dirsize-16.png png/dirsize-24.png png/dirsize-32.png png/dirsize-48.png png/dirsize-64.png png/dirsize-128.png png/dirsize-256.png tmp/
  [ -f tmp/dirsize-40.png ] || rsvg-convert -w 40 -h 40 dirsize.svg -o tmp/dirsize-40.png
  # ICO: 16,20,24,32,40,48,64,128,256 (ImageMagick stores the 256 entry PNG-compressed).
  convert tmp/dirsize-16.png tmp/dirsize-20.png tmp/dirsize-24.png tmp/dirsize-32.png tmp/dirsize-40.png \
          tmp/dirsize-48.png tmp/dirsize-64.png tmp/dirsize-128.png tmp/dirsize-256.png dirsize.ico
  # Side-by-side preview on light and dark backgrounds.
  for bg in white "#1e1e1e"; do
    n=$( [ "$bg" = white ] && echo light || echo dark )
    convert -background "$bg" -gravity center \
      \( tmp/dirsize-16.png -extent 40x40 \) \( tmp/dirsize-24.png -extent 40x40 \) \( tmp/dirsize-32.png -extent 48x48 \) \
      \( tmp/dirsize-48.png -extent 64x64 \) \( tmp/dirsize-64.png -extent 80x80 \) \( tmp/dirsize-128.png -extent 144x144 \) \
      +append preview/preview-$n.png
  done
  rm -rf tmp
  identify dirsize.ico | sed "s/^/ico: /"
'
