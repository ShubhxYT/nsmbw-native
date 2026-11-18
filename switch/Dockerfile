# Switch toolchain for the NSMBW port: devkitA64 plus the switch-dev group (libnx, deko3d,
# switch-tools). Built once; every build after that runs offline in this image.
FROM devkitpro/devkita64:latest
RUN dkp-pacman -Sy --noconfirm --needed switch-dev
