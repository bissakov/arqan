FROM debian:11-slim@sha256:4a2e40d0d34f8f86f60ef0d79c14d3b6b3d2620825dcdf93152535b5efbaf490

ENV DEBIAN_FRONTEND=noninteractive
# NOTE: Debian 11 LTS ended and deb.debian.org no longer serves the
# bullseye-security pool. The image ships commented snapshot.debian.org lines
# for its own build date; use them so the security updates it already has stay
# installable. Snapshot Release files are past their Valid-Until date.
RUN sed -i -e '/^deb /d' -e 's/^# deb http:\/\/snapshot/deb http:\/\/snapshot/' \
      /etc/apt/sources.list \
 && echo 'Acquire::Check-Valid-Until "false";' >/etc/apt/apt.conf.d/99snapshot \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils ca-certificates coreutils file findutils gcc grep gzip \
      cpio dpkg-dev libarchive-tools libc6-dev libcurl4 libcurl4-openssl-dev \
      make python3 rpm rpm2cpio sed tar zstd \
 && rm -rf /var/lib/apt/lists/*
