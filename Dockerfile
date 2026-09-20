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

# SBOM placeholder: the statically linked lws and nlohmann/json versions are
# invisible to package scanners (DESIGN.md §13), so CI must emit an SBOM here.
LABEL org.opencontainers.image.title="mod_audio_fork" \
      org.opencontainers.image.description="Bidirectional FreeSWITCH audio fork over WebSockets" \
      audiofork.vendored.libwebsockets="4.3.3" \
      audiofork.vendored.nlohmann_json="3.11.3"
