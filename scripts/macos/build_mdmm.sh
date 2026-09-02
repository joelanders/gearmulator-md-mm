#!/bin/bash

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "${1:-${script_dir}/../..}" && pwd)"
build_dir_input="${2:-${source_dir}/build/macos-mdmm-universal}"
output_dir_input="${3:-${source_dir}/artifacts/macos-mdmm-universal}"
build_dir="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "${build_dir_input}")"
output_dir="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "${output_dir_input}")"
require_firmware_tests="${GEARMULATOR_REQUIRE_FIRMWARE_TESTS:-1}"
md_firmware_bin="${GEARMULATOR_MD_FIRMWARE_BIN:-}"
mm_firmware_bin="${GEARMULATOR_MM_FIRMWARE_BIN:-}"
readonly md_firmware_bin_sha256="68542e30917b9918ccaee2b2237df62c8a00479938680b85aca93ce4fbca44c8"
readonly mm_firmware_bin_sha256="369849175602e20a9dd2b6e0ad8ac404b76f82718b14afbf1cbc01b7acabec7e"

validate_firmware_bin() {
  local label="$1"
  local path="$2"
  local expected_sha256="$3"
  if [[ ! -f "${path}" ]]; then
    echo "Required ${label} complete firmware image is missing: ${path}" >&2
    return 1
  fi
  local bytes
  bytes="$(/usr/bin/stat -f '%z' "${path}")"
  if [[ "${bytes}" != "8388608" ]]; then
    echo "${label} firmware image must be exactly 8 MiB; found ${bytes} bytes" >&2
    return 1
  fi
  local actual_sha256
  actual_sha256="$(env LC_ALL=C LANG=C /usr/bin/shasum -a 256 "${path}" | /usr/bin/awk '{print $1}')"
  if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    echo "${label} firmware image hash mismatch: expected ${expected_sha256}, found ${actual_sha256}" >&2
    return 1
  fi
}

python3 "${script_dir}/write_mdmm_receipt.py" \
  --source "${source_dir}" \
  --validate-build-root "${build_dir_input}" \
  --validate-output-root "${output_dir_input}"
if [[ "${require_firmware_tests}" != "0" && "${require_firmware_tests}" != "1" ]]; then
  echo "GEARMULATOR_REQUIRE_FIRMWARE_TESTS must be 0 or 1" >&2
  exit 2
fi
if [[ "${require_firmware_tests}" == "1" ]]; then
  if [[ -z "${md_firmware_bin}" || -z "${mm_firmware_bin}" ]]; then
    echo "Release verification requires GEARMULATOR_MD_FIRMWARE_BIN and GEARMULATOR_MM_FIRMWARE_BIN" >&2
    exit 2
  fi
  validate_firmware_bin "MD" "${md_firmware_bin}" "${md_firmware_bin_sha256}"
  validate_firmware_bin "MM" "${mm_firmware_bin}" "${mm_firmware_bin_sha256}"
fi

python3 "${script_dir}/write_mdmm_receipt.py" \
  --source "${source_dir}" \
  --prepare-build-root "${build_dir_input}" \
  --prepare-output-root "${output_dir_input}"

source_tuple_before="$(python3 "${script_dir}/write_mdmm_receipt.py" \
  --source "${source_dir}" \
  --check-source-only \
  --allow-untracked-root "${build_dir}/.gearmulator-mdmm-release-root" \
  --allow-untracked-root "${output_dir}/.gearmulator-mdmm-release-root")"

artifact_root="${source_dir}/bin/plugins/Release"
md_app="${artifact_root}/Standalone/Gearmulator MD.app"
mm_app="${artifact_root}/Standalone/Gearmulator MM.app"
md_vst3="${artifact_root}/VST3/Gearmulator MD.vst3"
mm_vst3="${artifact_root}/VST3/Gearmulator MM.vst3"
build_runtime_home="${build_dir}/build-runtime-home"
build_runtime_data="${build_runtime_home}/Documents"

cleanup_build_runtime_home() {
  rm -rf -- "${build_runtime_home}"
}

# The staged firmware is private test material. Remove it after success, after
# any failing command, and when an interactive build is interrupted.
trap cleanup_build_runtime_home EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# JUCE executes each VST3 while generating its metadata. Keep that build-time
# process out of the caller's real Gearmulator folders. For a release build,
# give it only the same pinned firmware images used by the package smoke.
mkdir -p "${build_runtime_data}"
if [[ "${require_firmware_tests}" == "1" ]]; then
  md_build_rom_dir="${build_runtime_data}/Gearmulator Preview/Machinedrum/roms"
  mm_build_rom_dir="${build_runtime_data}/Gearmulator Preview/Monomachine/roms"
  mkdir -p "${md_build_rom_dir}" "${mm_build_rom_dir}"
  /usr/bin/ditto "${md_firmware_bin}" "${md_build_rom_dir}/validated-md.bin"
  /usr/bin/ditto "${mm_firmware_bin}" "${mm_build_rom_dir}/validated-mm.bin"
  validate_firmware_bin "staged MD" "${md_build_rom_dir}/validated-md.bin" \
    "${md_firmware_bin_sha256}"
  validate_firmware_bin "staged MM" "${mm_build_rom_dir}/validated-mm.bin" \
    "${mm_firmware_bin_sha256}"
fi

# JUCE places products in a shared ignored source-tree directory rather than in
# build_dir. Remove only this release's four exact bundles so stale resources
# from an older build cannot enter the archive.
rm -rf "${md_app}" "${mm_app}" "${md_vst3}" "${mm_vst3}"

cmake -S "${source_dir}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13 \
  -DXCODE_VERSION="${XCODE_VERSION:-16}" \
  -DBUILD_TESTING=ON \
  -Dgearmulator_BUILD_JUCEPLUGIN=ON \
  -Dgearmulator_BUILD_FX_PLUGIN=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN_VST2=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN_VST3=ON \
  -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN_LV2=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN_AU=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN_Standalone=ON \
  -Dgearmulator_SYNTH_ELEKTRON=ON \
  -Dgearmulator_SYNTH_OSIRUS=OFF \
  -Dgearmulator_SYNTH_OSTIRUS=OFF \
  -Dgearmulator_SYNTH_VAVRA=OFF \
  -Dgearmulator_SYNTH_XENIA=OFF \
  -Dgearmulator_SYNTH_NODALRED2X=OFF \
  -Dgearmulator_SYNTH_JE8086=OFF

HOME="${build_runtime_home}" GEARMULATOR_DATA_ROOT="${build_runtime_data}" \
cmake --build "${build_dir}" --parallel 4 --target \
  mdJucePlugin_VST3 \
  mmJucePlugin_VST3 \
  mdJucePlugin_Standalone \
  mmJucePlugin_Standalone \
  pluginTester \
  mdLibTest \
  mdFirmwareImageTest \
  mc68kColdFireDivideTest

# The helper no longer needs the private fixtures once all plugin targets have
# finished. Do not leave firmware copies behind in the persistent build tree.
cleanup_build_runtime_home
trap - EXIT HUP INT TERM

for test_name in mdLibTests mdFirmwareImageTest mc68kColdFireDivideTest; do
  ctest --test-dir "${build_dir}" -C Release --output-on-failure \
    --no-tests=error --tests-regex "^${test_name}$"
done

plugin_tester="${build_dir}/source/pluginTester/pluginTester_artefacts/Release/pluginTester"

for bundle in "${md_vst3}" "${mm_vst3}"; do
  rm -f "${bundle}/Contents/Resources/moduleinfo.json"
done

for bundle in "${md_app}" "${mm_app}" "${md_vst3}" "${mm_vst3}"; do
  if [[ ! -d "${bundle}" ]]; then
    echo "Expected bundle is missing: ${bundle}" >&2
    exit 3
  fi
  codesign --force --deep --sign - "${bundle}"
  codesign --verify --deep --strict "${bundle}"
  executable="${bundle}/Contents/MacOS/$(basename "${bundle}" | sed -E 's/\.(app|vst3)$//')"
  archs="$(lipo -archs "${executable}")"
  [[ " ${archs} " == *" arm64 "* && " ${archs} " == *" x86_64 "* ]] || {
    echo "Bundle is not universal: ${bundle} (${archs})" >&2
    exit 4
  }
done

if [[ ! -x "${plugin_tester}" ]]; then
  echo "Expected VST3 host is missing: ${plugin_tester}" >&2
  exit 4
fi

if find "${md_app}" "${mm_app}" "${md_vst3}" "${mm_vst3}" -type f \
    \( -iname '*.bin' -o -iname '*.rom' -o -iname '*.nvram' -o \
       -iname '*.syx' -o -iname '*.wav' -o -iname '*.cache' -o \
       -iname '*.mdpd' \) -print -quit | grep -q .; then
  echo "Firmware or private runtime material found in final bundles" >&2
  exit 5
fi

package_dir="${output_dir}/Gearmulator-Elektron-macOS-Universal"
mkdir -p "${package_dir}"
/usr/bin/ditto "${md_app}" "${package_dir}/Gearmulator MD.app"
/usr/bin/ditto "${mm_app}" "${package_dir}/Gearmulator MM.app"
/usr/bin/ditto "${md_vst3}" "${package_dir}/Gearmulator MD.vst3"
/usr/bin/ditto "${mm_vst3}" "${package_dir}/Gearmulator MM.vst3"
/usr/bin/ditto "${source_dir}/LICENSE.md" "${package_dir}/LICENSE.md"
/usr/bin/ditto "${script_dir}/macsetup_Gearmulator-Elektron.command" \
  "${package_dir}/macsetup_Gearmulator-Elektron.command"
/usr/bin/ditto "${script_dir}/INSTALL-macOS.txt" \
  "${package_dir}/INSTALL-macOS.txt"

archive="${output_dir}/Gearmulator-Elektron-macOS-Universal.zip"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "${package_dir}" "${archive}"
if [[ "${require_firmware_tests}" == "1" ]]; then
  "${script_dir}/verify_mdmm_package.sh" "${archive}" "${plugin_tester}" \
    "${md_firmware_bin}" "${mm_firmware_bin}"
else
  "${script_dir}/verify_mdmm_package.sh" "${archive}" "${plugin_tester}"
fi

receipt="${output_dir}/Gearmulator-Elektron-macOS-Universal-receipt.json"
python3 "${script_dir}/write_mdmm_receipt.py" \
  --source "${source_dir}" \
  --expected-source-tuple "${source_tuple_before}" \
  --allow-untracked-root "${package_dir}" \
  --allow-untracked-root "${archive}" \
  --allow-untracked-root "${output_dir}/.gearmulator-mdmm-release-root" \
  --firmware-tests-required "${require_firmware_tests}" \
  --output "${receipt}" \
  --archive "${archive}" \
  --artifact "${package_dir}/Gearmulator MD.app" \
  --artifact "${package_dir}/Gearmulator MM.app" \
  --artifact "${package_dir}/Gearmulator MD.vst3" \
  --artifact "${package_dir}/Gearmulator MM.vst3" \
  --package-file "${package_dir}/macsetup_Gearmulator-Elektron.command" \
  --package-file "${package_dir}/INSTALL-macOS.txt"

echo "MACOS_MDMM_ZIP=${archive}"
echo "MACOS_MDMM_RECEIPT=${receipt}"
env LC_ALL=C LANG=C /usr/bin/shasum -a 256 "${archive}"
