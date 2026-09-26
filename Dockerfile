# Dockerfile — arm-none-eabi cross-compilation environment
# for the Landau ER-301 unit.
#
# Uses arm-none-eabi-g++ (bare-metal, no glibc) to match the ER-301 SDK's
# intended toolchain (er-301/scripts/tutorial.mk uses arm-none-eabi).
# arm-linux-gnueabihf injects glibc symbols (__fprintf_chk, __stack_chk_*,
# __gmon_start__, _ITM_*, stderr) that the TI RTOS firmware does not export,
# causing silent dlopen failure.
#
# Build:  docker build -t er301-crosscompile .
# Use:    make docker-build ER301_SDK=~/er-301

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi \
        binutils-arm-none-eabi \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        swig \
        make \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Verify the cross-compiler is present
RUN arm-none-eabi-g++ --version
RUN swig -version

WORKDIR /build
