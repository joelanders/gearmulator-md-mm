#!/bin/bash

set -u

# A browser download may quarantine this script along with the plug-in bundles.
/usr/bin/xattr -d com.apple.quarantine "$0" >/dev/null 2>&1 || true

current_location="$(cd "$(dirname "$0")" && pwd)"
products=("Gearmulator MD" "Gearmulator MM")
formats=("app" "vst3")
found=0
failed=0

echo
echo "Clearing extended attributes for Gearmulator MD/MM"
echo "Location: ${current_location}"
echo

for product in "${products[@]}"; do
    for format in "${formats[@]}"; do
        target="${current_location}/${product}.${format}"
        if [[ ! -e "${target}" ]]; then
            continue
        fi

        found=1
        echo "Clearing attributes for: ${target}"
        if ! /usr/bin/xattr -cr "${target}"; then
            failed=1
        fi
    done
done

echo
if [[ ${found} -eq 0 ]]; then
    echo "No Gearmulator MD/MM applications or VST3 plug-ins were found beside this script."
    exit 1
fi
if [[ ${failed} -ne 0 ]]; then
    echo "One or more bundles could not be prepared."
    exit 1
fi

echo "Done. You can now copy the applications and VST3 plug-ins to their destinations."
