#!/bin/bash
set -x

. scripts/lib.sh

cd "$GITHUB_WORKSPACE" || exit 1

# "booo, bash feature!", -- posix sh users, probably
declare -A BASE_BUILD_PACKAGES SDL_BUILD_PACKAGES APPIMAGETOOL

# bzip2 and opus are added from submodules, freetype replaced by stb_truetype in this build, so it's just compiler toolchain
BASE_BUILD_PACKAGES[common]="desktop-file-utils"
BASE_BUILD_PACKAGES[amd64]="build-essential"
BASE_BUILD_PACKAGES[i386]="gcc-multilib g++-multilib"
BASE_BUILD_PACKAGES[arm64]="crossbuild-essential-arm64"
BASE_BUILD_PACKAGES[armhf]="crossbuild-essential-armhf"
BASE_BUILD_PACKAGES[riscv64]="crossbuild-essential-riscv64"
BASE_BUILD_PACKAGES[ppc64el]="crossbuild-essential-ppc64el"

SDL_BUILD_PACKAGES[common]="cmake ninja-build"
# TODO: add libpipewire-0.3-dev and libdecor-0-dev after we migrate from 20.04
# TODO: figure out how to install fcitx and ibus dev in cross compile environment on gha
# In theory, we better run this in limited container. Right now, some preinstalled PHP shit breaks libpcre builds
# and prevents us from installing crosscompiling packages
SDL_BUILD_PACKAGES[amd64]="libasound2-dev libpulse-dev \
	libaudio-dev libjack-dev libsndio-dev libsamplerate0-dev libx11-dev libxext-dev \
	libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libwayland-dev \
	libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
	libegl1-mesa-dev libdbus-1-dev libudev-dev"
SDL_BUILD_PACKAGES[i386]="${SDL_BUILD_PACKAGES[amd64]//-dev/-dev:i386} libjack0:i386" # test
SDL_BUILD_PACKAGES[arm64]=${SDL_BUILD_PACKAGES[amd64]//-dev/-dev:arm64}
SDL_BUILD_PACKAGES[armhf]=${SDL_BUILD_PACKAGES[amd64]//-dev/-dev:armhf}
SDL_BUILD_PACKAGES[riscv64]=${SDL_BUILD_PACKAGES[amd64]//-dev/-dev:riscv64}
SDL_BUILD_PACKAGES[ppc64el]=${SDL_BUILD_PACKAGES[amd64]//-dev/-dev:ppc64el}

APPIMAGETOOL[amd64]=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
APPIMAGETOOL[i386]=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-i686.AppImage

# can't run AppImageTool yet because it's compiled for these platforms natively and don't support cross compilation yet
# uncomment when we will enable qemu-user for tests
# APPIMAGETOOL[arm64]=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-aarch64.AppImage
# APPIMAGETOOL[armhf]=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-armhf.AppImage

regenerate_sources_list()
{
	# this is evil but to speed up update, specify all repositories manually
	sudo rm /etc/apt/sources.list
	sudo rm -rf /etc/apt/sources.list.d

	for i in focal focal-updates focal-backports focal-security; do
		echo "deb [arch=$GH_CPU_ARCH] http://azure.ports.ubuntu.com/ubuntu-ports $i main universe" | sudo tee -a /etc/apt/sources.list
		echo "deb [arch=amd64] http://azure.archive.ubuntu.com/ubuntu $i main universe" | sudo tee -a /etc/apt/sources.list
	done
}

if [ "$GH_CPU_ARCH" != "amd64" ] && [ -n "$GH_CPU_ARCH" ]; then
	if [ "$GH_CPU_ARCH" != "i386" ]; then
		regenerate_sources_list
	fi
	sudo dpkg --add-architecture "$GH_CPU_ARCH"
fi

sudo apt update || die
sudo apt install aptitude || die # aptitude is just more reliable at resolving dependencies

# shellcheck disable=SC2086 # splitting is intended here
sudo aptitude install -y ${BASE_BUILD_PACKAGES[common]} ${BASE_BUILD_PACKAGES[$GH_CPU_ARCH]} ${SDL_BUILD_PACKAGES[common]} ${SDL_BUILD_PACKAGES[$GH_CPU_ARCH]} || die

if [ -n "${APPIMAGETOOL[$GH_CPU_ARCH]}" ]; then
	wget -O appimagetool.AppImage "${APPIMAGETOOL[$GH_CPU_ARCH]}"
	chmod +x appimagetool.AppImage
fi

wget "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" -qO- | tar -xzf -
mv "SDL2-$SDL_VERSION" SDL2_src

# ref_vk requires Vulkan SDK
wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo apt-key add -
sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-${VULKAN_SDK_VERSION}-jammy.list https://packages.lunarg.com/vulkan/${VULKAN_SDK_VERSION}/lunarg-vulkan-${VULKAN_SDK_VERSION}-jammy.list
sudo apt update
sudo apt install -y vulkan-sdk
