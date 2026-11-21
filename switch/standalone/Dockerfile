# devkitA64 r29.2, libnx 4.12.0 and switch-tools 1.13.1
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
RUN apt-get update && apt-get install -y --no-install-recommends \
    bison ca-certificates cmake curl flex git make meson ninja-build pkg-config \
    python3 python3-jinja2 python3-mako python3-pil unzip \
    && rm -rf /var/lib/apt/lists/*
