# Multi-stage build for Speech Bridge (Linux x86_64). Models are NOT baked in —
# mount them at runtime via a volume. Fleshed out in milestone 8.
#
# NOTE: cgo forbids cross-compilation; build this on an x86_64 host.

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git golang ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make cores && make app && make check-symbols

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/speechbridge
COPY --from=build /src/app/sb-server ./sb-server
COPY --from=build /src/lib ./lib
ENV SB_BIND=0.0.0.0:8080 \
    SB_LIB_DIR=/opt/speechbridge/lib \
    SB_MODELS_DIR=/models \
    SB_LOG_FORMAT=json
VOLUME ["/models"]
EXPOSE 8080
ENTRYPOINT ["/opt/speechbridge/sb-server"]
