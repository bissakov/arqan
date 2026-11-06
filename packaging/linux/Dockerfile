FROM debian:11-slim@sha256:4a2e40d0d34f8f86f60ef0d79c14d3b6b3d2620825dcdf93152535b5efbaf490

ENV DEBIAN_FRONTEND=noninteractive
# NOTE: Debian 11 LTS ended and deb.debian.org no longer serves the
# bullseye-security pool; archive.debian.org serves all of bullseye. Its
# Release files are past their Valid-Until date. apt in Debian 11 does not
# retry a failed download by default.
RUN sed -i -e '/^# deb /d' -e 's|http://deb\.debian\.org/|http://archive.debian.org/|' \
      /etc/apt/sources.list \
 && printf '%s\n' 'Acquire::Check-Valid-Until "false";' 'Acquire::Retries "5";' \
      >/etc/apt/apt.conf.d/99archive \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils ca-certificates coreutils file findutils gcc git grep gzip \
      cpio dpkg-dev libarchive-tools libc6-dev libcurl4 libcurl4-openssl-dev \
      make python3 rpm rpm2cpio sed tar zstd \
 && rm -rf /var/lib/apt/lists/*
