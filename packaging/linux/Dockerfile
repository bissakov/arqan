FROM debian:11-slim@sha256:4a2e40d0d34f8f86f60ef0d79c14d3b6b3d2620825dcdf93152535b5efbaf490

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils ca-certificates coreutils file findutils gcc grep gzip \
      libc6-dev libcurl4 libcurl4-openssl-dev make python3 sed tar \
 && rm -rf /var/lib/apt/lists/*
