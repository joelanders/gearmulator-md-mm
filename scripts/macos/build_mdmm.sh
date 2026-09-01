#!/bin/bash

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "${1:-${script_dir}/../..}" && pwd)"
build_dir="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "${2:-${source_dir}/build/macos-mdmm-universal}")"
output_dir="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "${3:-${source_dir}/artifacts/macos-mdmm-universal}")"

if [[ -z "${build_dir}" || -z "${output_dir}" || "${build_dir}" == "/" || "${output_dir}" == "/" ]]; then
  echo "Refusing unsafe build or output directory" >&2
  exit 2
fi
if [[ "${build_dir}" == "${source_dir}" || "${output_dir}" == "${source_dir}" ]]; then
  echo "Build and output directories must not be the source directory" >&2
  exit 2
fi

python3 "${script_dir}/write_mdmm_receipt.py" \
  --source "${source_dir}" \
  --check-source-only

rm -rf "${build_dir}" "${output_dir}"
mkdir -p "${build_dir}" "${output_dir}"

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

cmake --build "${build_dir}" --parallel 4 --target \
  mdJucePlugin_VST3 \
  mmJucePlugin_VST3 \
  mdJucePlugin_Standalone \
  mmJucePlugin_Standalone \
  pluginTester \
  mdLibTest \
  mdStandalonePlusDrivePersistenceTest

ctest --test-dir "${build_dir}" -C Release --output-on-failure \
  --tests-regex '^(mdLibTests|mdStandalonePlusDrivePersistenceTest)$'

artifact_root="${source_dir}/bin/plugins/Release"
md_app="${artifact_root}/Standalone/Gearmulator MD.app"
mm_app="${artifact_root}/Standalone/Gearmulator MM.app"
md_vst3="${artifact_root}/VST3/Gearmulator MD.vst3"
mm_vst3="${artifact_root}/VST3/Gearmulator MM.vst3"
plugin_tester="${build_dir}/source/framework/tools/pluginTester/pluginTester_artefacts/Release/pluginTester"

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
empty_home="${build_dir}/empty-home"
mkdir -p "${empty_home}"
HOME="${empty_home}" "${plugin_tester}" -blocks 16 -plugin "${md_vst3}"
HOME="${empty_home}" "${plugin_tester}" -blocks 16 -plugin "${mm_vst3}"

if find "${md_app}" "${mm_app}" "${md_vst3}" "${mm_vst3}" -type f \
    \( -iname '*.bin' -o -iname '*.rom' -o -iname '*.nvram' -o \
       -iname '*.syx' -o -iname '*.wav' \) -print -quit | grep -q .; then
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
"${script_dir}/verify_mdmm_package.sh" "${archive}"

receipt="${output_dir}/Gearmulator-Elektron-macOS-Universal-receipt.json"
python3 "${script_dir}/write_mdmm_receipt.py" \
  --source "${source_dir}" \
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
