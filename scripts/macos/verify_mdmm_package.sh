#!/bin/bash

set -euo pipefail

archive="${1:?usage: verify_mdmm_package.sh ARCHIVE}"
if [[ ! -f "${archive}" ]]; then
  echo "Package archive does not exist: ${archive}" >&2
  exit 2
fi

verification_root="$(mktemp -d "${TMPDIR:-/tmp}/gearmulator-mdmm-package.XXXXXX")"
trap 'rm -rf "${verification_root}"' EXIT

/usr/bin/ditto -x -k "${archive}" "${verification_root}"
package_dir="$(find "${verification_root}" -mindepth 1 -maxdepth 1 -type d -name 'Gearmulator-Elektron-macOS-*' -print -quit)"
if [[ -z "${package_dir}" ]]; then
  echo "Extracted package directory was not found" >&2
  exit 3
fi

setup_command="${package_dir}/macsetup_Gearmulator-Elektron.command"
install_guide="${package_dir}/INSTALL-macOS.txt"
bundles=(
  "${package_dir}/Gearmulator MD.app"
  "${package_dir}/Gearmulator MM.app"
  "${package_dir}/Gearmulator MD.vst3"
  "${package_dir}/Gearmulator MM.vst3"
)

if [[ ! -x "${setup_command}" ]]; then
  echo "Setup command is missing or is not executable: ${setup_command}" >&2
  exit 3
fi
if [[ ! -f "${install_guide}" ]]; then
  echo "Installation guide is missing: ${install_guide}" >&2
  exit 3
fi

quarantined_paths=("${setup_command}")
for bundle in "${bundles[@]}"; do
  if [[ ! -d "${bundle}" ]]; then
    echo "Expected bundle is missing from extracted package: ${bundle}" >&2
    exit 3
  fi
  executable="${bundle}/Contents/MacOS/$(basename "${bundle}" | sed -E 's/\.(app|vst3)$//')"
  if [[ ! -f "${executable}" ]]; then
    echo "Expected bundle executable is missing: ${executable}" >&2
    exit 3
  fi
  quarantined_paths+=("${bundle}" "${executable}")
done

for path in "${quarantined_paths[@]}"; do
  /usr/bin/xattr -w com.apple.quarantine "0081;00000000;GearmulatorPackageTest;" "${path}"
done

"${setup_command}"

for path in "${quarantined_paths[@]}"; do
  if /usr/bin/xattr -p com.apple.quarantine "${path}" >/dev/null 2>&1; then
    echo "Setup command left quarantine metadata on: ${path}" >&2
    exit 4
  fi
done

for bundle in "${bundles[@]}"; do
  /usr/bin/codesign --verify --deep --strict "${bundle}"
done

echo "Verified extracted MD/MM package setup flow: ${archive}"
