FROM ghcr.io/dragonminded/libdragon:preview@sha256:d0076cdba66586d018a57879a2babcc063f751267502394dbbd96776f555f283

ARG LIBDRAGON_COMMIT
ARG TOOLCHAIN_RECIPE
ARG TOOLCHAIN_PLATFORM
LABEL org.director64.libdragon-commit="${LIBDRAGON_COMMIT}" \
      org.director64.toolchain-recipe="${TOOLCHAIN_RECIPE}" \
      org.director64.toolchain-platform="${TOOLCHAIN_PLATFORM}"

COPY libdragon-src/ /opt/director64-libdragon/
RUN cd /opt/director64-libdragon && ./build.sh

WORKDIR /workdir
