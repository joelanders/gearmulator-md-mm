#!/bin/sh

set -eu

preset="${1:-ios-simulator}"
case "$preset" in
	ios)
		sdk="iphoneos"
		;;
	ios-simulator)
		sdk="iphonesimulator"
		;;
	*)
		echo "usage: $0 [ios|ios-simulator]" >&2
		exit 2
		;;
esac

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$project_dir/temp/cmake_$preset"

cmake_version=$(cmake --version | sed -n '1s/[^0-9]*\([0-9][0-9.]*\).*/\1/p')
cmake_major=$(echo "$cmake_version" | cut -d. -f1)
cmake_minor=$(echo "$cmake_version" | cut -d. -f2)
use_uv_cmake=false
if [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 30 ]; }; then
	use_uv_cmake=true
fi

run_cmake()
{
	if [ "$use_uv_cmake" = true ] && command -v uv >/dev/null 2>&1; then
		uv run --with cmake cmake "$@"
	else
		cmake "$@"
	fi
}

run_cmake -S "$project_dir" -B "$build_dir" -G Xcode \
	-U CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED \
	-DCMAKE_SYSTEM_NAME=iOS \
	-DCMAKE_OSX_SYSROOT="$sdk" \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
	-DCMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET=17.4 \
	-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic \
	-DBUILD_TESTING=OFF \
	-Dgearmulator_BUILD_JUCEPLUGIN=ON \
	-Dgearmulator_BUILD_FX_PLUGIN=OFF \
	-Dgearmulator_BUILD_JUCEPLUGIN_Standalone=ON \
	-Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF \
	-Dgearmulator_BUILD_JUCEPLUGIN_VST2=OFF \
	-Dgearmulator_BUILD_JUCEPLUGIN_VST3=OFF \
	-Dgearmulator_BUILD_JUCEPLUGIN_AU=OFF \
	-Dgearmulator_BUILD_JUCEPLUGIN_LV2=OFF \
	-Dgearmulator_SYNTH_OSIRUS=OFF \
	-Dgearmulator_SYNTH_OSTIRUS=OFF \
	-Dgearmulator_SYNTH_VAVRA=OFF \
	-Dgearmulator_SYNTH_XENIA=OFF \
	-Dgearmulator_SYNTH_NODALRED2X=OFF \
	-Dgearmulator_SYNTH_JE8086=OFF \
	-Dgearmulator_SYNTH_ELEKTRON=ON

run_cmake --build "$build_dir" --config Release \
	--target mdJucePlugin_Standalone mmJucePlugin_Standalone -- \
	CODE_SIGNING_ALLOWED=NO
