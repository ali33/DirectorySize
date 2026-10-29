# DirectorySize — Copyright (c) 2026 META.vn
# Author: Nguyễn Đức Sơn
# SPDX-License-Identifier: GPL-2.0-only
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends imagemagick librsvg2-bin \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /icon
