#!/usr/bin/env bash
set -e

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
	Darwin) PLATFORM="mac" ;;
	Linux)  PLATFORM="linux" ;;
	*)      echo "Unsupported platform: $OS"; exit 1 ;;
esac

echo ""
echo "Select formats to build (space-separated numbers, e.g. 1 3):"
echo ""

if [ "$PLATFORM" = "mac" ]; then
	echo "  1) VST3"
	echo "  2) AU"
	echo "  3) CLAP"
	echo "  4) Standalone"
	echo "  5) All"
else
	echo "  1) VST3"
	echo "  2) CLAP"
	echo "  3) LV2"
	echo "  4) Standalone"
	echo "  5) All"
fi

echo ""
read -rp "Choice: " FORMAT_INPUT

VST3=OFF AU=OFF CLAP=OFF LV2=OFF STANDALONE=OFF
TARGETS=""

add_targets() {
	local suffix="$1"
	TARGETS="$TARGETS mdJucePlugin_$suffix mmJucePlugin_$suffix"
}

if [ "$PLATFORM" = "mac" ]; then
	for n in $FORMAT_INPUT; do
		case $n in
			1) VST3=ON;       add_targets VST3 ;;
			2) AU=ON;         add_targets AU ;;
			3) CLAP=ON;       add_targets CLAP ;;
			4) STANDALONE=ON; add_targets Standalone ;;
			5) VST3=ON; AU=ON; CLAP=ON; STANDALONE=ON
			   add_targets VST3; add_targets AU; add_targets CLAP; add_targets Standalone ;;
		esac
	done
else
	for n in $FORMAT_INPUT; do
		case $n in
			1) VST3=ON;       add_targets VST3 ;;
			2) CLAP=ON;       add_targets CLAP ;;
			3) LV2=ON;        add_targets LV2 ;;
			4) STANDALONE=ON; add_targets Standalone ;;
			5) VST3=ON; CLAP=ON; LV2=ON; STANDALONE=ON
			   add_targets VST3; add_targets CLAP; add_targets LV2; add_targets Standalone ;;
		esac
	done
fi

BUILD_DIR="./temp/cmake"
echo ""
read -rp "Clean build directory? (y/N): " CLEAN_INPUT
if [[ "$CLEAN_INPUT" =~ ^[Yy]$ ]]; then
	echo "Cleaning $BUILD_DIR..."
	rm -rf "$BUILD_DIR"
fi

echo ""
echo "Configuring..."

CMAKE_FLAGS="-Dgearmulator_BUILD_JUCEPLUGIN_VST3=$VST3"
CMAKE_FLAGS="$CMAKE_FLAGS -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=$CLAP"
CMAKE_FLAGS="$CMAKE_FLAGS -Dgearmulator_BUILD_JUCEPLUGIN_LV2=$LV2"
CMAKE_FLAGS="$CMAKE_FLAGS -Dgearmulator_BUILD_JUCEPLUGIN_Standalone=$STANDALONE"
CMAKE_FLAGS="$CMAKE_FLAGS -Dgearmulator_BUILD_JUCEPLUGIN_AU=$AU"
CMAKE_FLAGS="$CMAKE_FLAGS -Dgearmulator_BUILD_JUCEPLUGIN_VST2=OFF"

if [ "$PLATFORM" = "mac" ]; then
	cmake -G Xcode -S . -B "$BUILD_DIR" -DCMAKE_OSX_ARCHITECTURES="$ARCH" $CMAKE_FLAGS
else
	cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release $CMAKE_FLAGS
fi

echo ""
echo "Building..."
TARGET_ARGS=""
for t in $TARGETS; do
	TARGET_ARGS="$TARGET_ARGS --target $t"
done
cmake --build "$BUILD_DIR" --config Release -j $TARGET_ARGS

echo ""
read -rp "Create ZIP package? (y/N): " PACK_INPUT
if [[ "$PACK_INPUT" =~ ^[Yy]$ ]]; then
	cmake -P scripts/pack.cmake
fi

echo ""
echo "Done."
