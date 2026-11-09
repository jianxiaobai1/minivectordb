# MiniVectorDB — built from source, run in the foreground over stdin/stdout.
#
#   docker build -t minivectordb .
#   docker run -i --rm -v "$PWD/data:/data" minivectordb
#
# Note the flags: -i keeps stdin attached so the MySQL test driver can pipe SQL
# in. -t (TTY) and -d (detached) are deliberately not used, and no port is
# exposed or mapped: the process speaks on standard streams only.
#
# Reproducibility comes from two places, not from apt version pins. The base
# image can be pinned by digest with --build-arg BASE=ubuntu:24.04@sha256:...
# and the platform locks the resulting image once built. Hard-pinning package
# versions here would be worse: Ubuntu removes older point releases from the
# archive, which turns a rebuild into a failed build.

ARG BASE=ubuntu:24.04
FROM ${BASE} AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && DESTDIR=/stage cmake --install build --prefix /usr/local

# ---------------------------------------------------------------------------

FROM ${BASE} AS runtime

# Nothing is installed: the executable links libstdc++ and libgcc statically
# (see CMakeLists.txt), so the only shared dependency is glibc, which the base
# image already provides. A build-time check below makes any other dependency
# fail here rather than during evaluation with networking disabled.
COPY --from=build /stage/usr/local /usr/local

RUN mkdir -p /data \
 && ldd /usr/local/bin/minivectordb \
 && /usr/local/bin/minivectordb --help

# All database files, the write-ahead log and the index sidecars live under
# /data, which the harness mounts, measures and reuses across restarts.
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir=/data"]
