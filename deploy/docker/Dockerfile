# Multi-stage build for Speech Bridge (Linux x86_64). Models are NOT baked in —
# mount them at runtime via a volume.
#
# NOTE: no cross-compilation; build this on an x86_64 host.

FROM debian:bookworm-slim AS build
# nodejs/npm: build-time only, for the Vue frontend (`make web`); the runtime
# image below never installs them. zlib1g-dev: uWebSockets' permessage-deflate.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git nodejs npm zlib1g-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make web
RUN make cores && make app && make check-symbols

FROM debian:bookworm-slim AS runtime
# libgomp1: ggml (all four cores) links OpenMP at runtime.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libgomp1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/speechbridge
COPY --from=build /src/sb-server ./sb-server
COPY --from=build /src/lib ./lib
COPY --from=build /src/web/dist ./web/dist
ENV SB_BIND=0.0.0.0:8080 \
    SB_LIB_DIR=/opt/speechbridge/lib \
    SB_MODELS_DIR=/models \
    SB_WEB_DIST_DIR=/opt/speechbridge/web/dist \
    SB_LOG_FORMAT=json
VOLUME ["/models"]
EXPOSE 8080
ENTRYPOINT ["/opt/speechbridge/sb-server"]
