# Build stage: Alpine packages freeswitch-dev at the same 1.10.x the module
# targets, so the module compiles against real FreeSWITCH headers.
FROM alpine:edge AS build

RUN echo "https://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories && \
    apk add --no-cache \
      build-base cmake ninja git pkgconf linux-headers \
      freeswitch-dev openssl-dev zlib-dev

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY core core
COPY net net
COPY module module

# AUDIOFORK_STRIP=0 builds RelWithDebInfo and skips the strip, so a crash in a
# rig container has a symbolised backtrace. The shipped image keeps the default.
ARG AUDIOFORK_STRIP=1

# lws and nlohmann/json are fetched and statically linked here (DESIGN.md §13)
RUN if [ "$AUDIOFORK_STRIP" = "0" ]; then BUILD_TYPE=RelWithDebInfo; else BUILD_TYPE=Release; fi && \
    cmake -S . -B /build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=OFF \
      -DAUDIOFORK_BUILD_MODULE=ON && \
    cmake --build /build --parallel && \
    { [ "$AUDIOFORK_STRIP" = "0" ] || strip /build/module/mod_audio_fork.so; }

# Runtime stage: only the module and its config land in the image. Point this at
# whatever FreeSWITCH base image you deploy; the module resolves switch_*
# symbols from the running binary at dlopen time.
FROM alpine:edge AS runtime

RUN echo "https://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories && \
    apk add --no-cache freeswitch

COPY --from=build /build/module/mod_audio_fork.so /usr/lib/freeswitch/mod/mod_audio_fork.so
COPY conf/audio_fork.conf.xml /etc/freeswitch/autoload_configs/audio_fork.conf.xml

# git describe --tags --always of the source tree; the SBOM has no other way to
# name this build.
ARG AUDIOFORK_VERSION=0.0.0-dev

# The SBOM is generated here and not in the build stage because the OpenSSL
# package version is a property of this image; the vendored pins are read from
# the CMake files the build stage actually compiled.
COPY packaging/sbom.sh /tmp/sbom.sh
COPY --from=build /src/net/CMakeLists.txt /tmp/pins/net/CMakeLists.txt
COPY --from=build /src/core/CMakeLists.txt /tmp/pins/core/CMakeLists.txt
RUN mkdir -p /usr/share/doc/mod_audio_fork && \
    AUDIOFORK_VERSION="$AUDIOFORK_VERSION" sh /tmp/sbom.sh /tmp/pins \
      > /usr/share/doc/mod_audio_fork/sbom.cdx.json && \
    rm -rf /tmp/sbom.sh /tmp/pins

LABEL org.opencontainers.image.title="mod_audio_fork" \
      org.opencontainers.image.description="Bidirectional FreeSWITCH audio fork over WebSockets" \
      org.opencontainers.image.version="$AUDIOFORK_VERSION"
