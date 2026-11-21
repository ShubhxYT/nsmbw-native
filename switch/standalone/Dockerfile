# devkitA64 r29.2, libnx 4.12.0 and switch-tools 1.13.1
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282 AS toolchain
RUN apt-get update && apt-get install -y --no-install-recommends \
    bison ca-certificates cmake curl flex git make meson ninja-build pkg-config \
    python3 python3-jinja2 python3-mako python3-pil unzip \
    && rm -rf /var/lib/apt/lists/*

FROM mcr.microsoft.com/dotnet/sdk@sha256:1b955f179b02df4f0eb5fadedaa898cbe44d620a64aab52eec72c8e91d1355b5 AS dotnet-sdk
FROM toolchain AS windows-builder
RUN apt-get update && apt-get install -y --no-install-recommends libicu72 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir /recipe /input /work /pack
COPY --from=dotnet-sdk /usr/share/dotnet /usr/share/dotnet
ENV PATH="/usr/share/dotnet:${PATH}" DOTNET_CLI_TELEMETRY_OPTOUT=1
