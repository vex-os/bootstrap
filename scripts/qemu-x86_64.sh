#!/bin/sh
# Builds the image and boots it in QEMU
# Usage: scripts/qemu-x86_64.sh [qemu arguments]
# The first serial port of the machine is this terminal, and the kernel log is on the screen.
# Ctrl-A X quits QEMU, Ctrl-A C opens its monitor. The build directory is BUILD_DIR, default: build
# The accelerator is QEMU_ACCEL, default: tcg. Example: QEMU_ACCEL=whpx,kernel-irqchip=off
set -eu

export LANG=C.UTF-8
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

script_dir="$(CDPATH= cd -- "$(dirname -- "${0}")" && pwd)"
source_dir="$(dirname -- "${script_dir}")"

build_dir="${BUILD_DIR:-build}"

case "${build_dir}" in
    /*) ;;
    *) build_dir="${source_dir}/${build_dir}" ;;
esac

# Installing the package of the image puts the disk into the tree
image_tree="${build_dir}/image-tree"

"${script_dir}/jinx-wrap" install -f "${image_tree}" image

disk="${image_tree}/images/disk.img"
firmware="${source_dir}/kernel/contrib/edk2/RELEASEX64_OVMF.fd"

if test -f "/proc/sys/fs/binfmt_misc/WSLInterop"; then
    qemu=qemu-system-x86_64.exe
    disk="$(wslpath -a -w "${disk}")"
    firmware="$(wslpath -a -w "${firmware}")"
else
    qemu=qemu-system-x86_64
fi

# The serial terminal is faster under TCG than under WHPX. Each port access of the guest
# leaves the hypervisor, and TCG does it in process
accel="${QEMU_ACCEL:-tcg}"

# The terminal would otherwise edit the line and echo it before QEMU sees it, and the guest
# echoes it again. Ctrl-C goes to the guest. Enter stays a newline (icrnl), as a lone
# carriage return never reaches the guest through the stdio of QEMU
if test -t 0; then
    saved_terminal="$(stty -g)"
    trap 'stty "${saved_terminal}"' EXIT
    stty -icanon -echo -isig -ixon
fi

"${qemu}" \
    -accel "${accel}" \
    -cpu qemu64,-svm \
    -bios "${firmware}" \
    -drive "format=raw,file=${disk}" \
    -serial mon:stdio \
    -machine type=q35 \
    -vga std \
    -m 512M \
    -smp 4 \
    "${@}"
