#!/usr/bin/env bash

set -eu

repo_root=$(cd "$(dirname "$0")/.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT

bdf=0000:01:00.0
device_path=$fixture/bus/pci/devices/$bdf
driver_path=$fixture/bus/pci/drivers/s3fb
fb0_path=$fixture/class/graphics/fb0
vtcon_path=$fixture/class/vtconsole/vtcon1

mkdir -p "$device_path" "$driver_path" "$fb0_path" "$vtcon_path"
printf '0x5333\n' > "$device_path/vendor"
printf '0x8a01\n' > "$device_path/device"
printf '\n' > "$driver_path/bind"
printf '\n' > "$driver_path/unbind"
printf '(S) frame buffer device\n' > "$vtcon_path/name"
printf '1\n' > "$vtcon_path/bind"
ln -s "$driver_path" "$device_path/driver"
ln -s "$device_path" "$fb0_path/device"

dry_output=$(L10GL_SYSFS_ROOT=$fixture \
    "$repo_root/tools/l10gl-run" --dry-run -- /bin/true)
grep -Fq "selected virge at $bdf" <<< "$dry_output"
grep -Fq "will detach fbcon" <<< "$dry_output"
grep -Fq "will unbind /dev/fb0 driver s3fb from $bdf" <<< "$dry_output"
grep -Fq "dry run; no sysfs state changed" <<< "$dry_output"
[[ $(<"$vtcon_path/bind") == 1 ]]

run_output=$(L10GL_SYSFS_ROOT=$fixture \
    "$repo_root/tools/l10gl-run" -- /bin/true)
grep -Fq "unbinding /dev/fb0 driver s3fb from $bdf" <<< "$run_output"
grep -Fq "rebinding /dev/fb0 driver s3fb to $bdf" <<< "$run_output"
grep -Fq "reattaching fbcon" <<< "$run_output"
[[ $(<"$driver_path/unbind") == "$bdf" ]]
[[ $(<"$driver_path/bind") == "$bdf" ]]
[[ $(<"$vtcon_path/bind") == 1 ]]

set +e
L10GL_SYSFS_ROOT=$fixture "$repo_root/tools/l10gl-run" -- /bin/sh -c 'exit 7' \
    > /dev/null
child_status=$?
set -e
[[ $child_status -eq 7 ]]
[[ $(<"$vtcon_path/bind") == 1 ]]

printf 'l10gl-run fixture test: PASS\n'
