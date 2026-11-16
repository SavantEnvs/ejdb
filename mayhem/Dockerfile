# syntax=docker/dockerfile:1.7
#
# Commit-image Dockerfile for ejdb, on top of the org C/C++ base image.
#
# Build (from the repo root):
#   DOCKER_BUILDKIT=1 docker build -f mayhem/Dockerfile \
#       --build-arg COMMIT=$(git rev-parse HEAD) -t ejdb-mayhem .
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
# SOURCE_DATE_EPOCH stays an ARG: an empty ENV value breaks clang. build.sh unsets it when empty.
ARG SOURCE_DATE_EPOCH
# No default on purpose: the base ENV flows through unless --build-arg overrides it.
ARG SANITIZER_FLAGS
ARG COVERAGE_FLAGS=
ARG MAYHEM_JOBS=

# ejdb's Autark build downloads iwnet (HTTP/WebSocket server framework), iowow (the storage/JSON
# engine) and binn-c (its binary format) as source archives. Pre-fetch all three from pinned tags into the in-image dependency cache so
# mayhem/build.sh runs offline; build.sh hands them to Autark as dir:// URLs.
USER root
ARG IOWOW_VER=1.7.3
ARG IOWOW_SHA256=5f0cbe955d5a0ed423e44a40f29b254ad5e30fea6d31f3c76d31cb9570b72c33
ARG BINNC_VER=0.2.0
ARG BINNC_SHA256=3c6d9d330699e09bbbeb91145faaa3bb9fb9286dac6d1b51f6a06984bd7ae5cb
ARG IWNET_VER=1.6.2
ARG IWNET_SHA256=61a76f64d6d9395eb6d90afff8c2697d04b66e2fdc4b7eb3f71de1e6beaf5277
# libcurl: only upstream's jbr_test1 (the HTTP server test run by mayhem/test.sh) links it.
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
RUN set -eux; \
    fetch() { name="$1"; ver="$2"; url="$3"; sha="$4"; d="/opt/toolchains/deps/${name}-${ver}"; \
      mkdir -p "$d/src" /tmp/dep && curl -fsSL -o /tmp/dep/src.zip "$url" \
      && echo "${sha}  /tmp/dep/src.zip" | sha256sum -c - \
      && unzip -q /tmp/dep/src.zip -d /tmp/dep/x && cp -a /tmp/dep/x/*/. "$d/src/" \
      && rm -rf /tmp/dep && chmod -R a+rX "$d"; }; \
    fetch iowow "${IOWOW_VER}" "https://github.com/Softmotions/iowow/archive/refs/tags/v${IOWOW_VER}.zip" "${IOWOW_SHA256}"; \
    fetch binn-c "${BINNC_VER}" "https://github.com/Softmotions/binn-c/archive/refs/tags/v${BINNC_VER}.zip" "${BINNC_SHA256}"; \
    fetch iwnet "${IWNET_VER}" "https://github.com/Softmotions/iwnet/archive/refs/tags/v${IWNET_VER}.zip" "${IWNET_SHA256}"

# Source enters from the build CONTEXT (the CI checkout): the FULL repo (incl. .git) is copied to
# /mayhem; build.sh builds in place.
USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Optional compile-cache pre-fill (SPEC 6.2 item 18); falls back to the plain build on older bases.
RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

# Run the project's functional tests at build time: a failing suite fails the image build.
RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
