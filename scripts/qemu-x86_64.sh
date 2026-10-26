#!/bin/sh
# Builds the image and boots it in QEMU
# Usage: scripts/qemu-x86_64.sh [qemu arguments]
# The first serial port of the machine is PuTTY when it is installed, else this terminal.
# The kernel log is on the screen. Ctrl-A X quits QEMU, Ctrl-A C opens its monitor.
# The build directory is BUILD_DIR, default: build
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
    putty=putty.exe
    disk="$(wslpath -a -w "${disk}")"
    firmware="$(wslpath -a -w "${firmware}")"
else
    qemu=qemu-system-x86_64
    putty=putty
fi

# The serial terminal is faster under TCG than under WHPX. Each port access of the guest
# leaves the hypervisor, and TCG does it in process
accel="${QEMU_ACCEL:-tcg}"

terminal_port=4444

run_qemu() {
    "${qemu}" \
        -accel "${accel}" \
        -cpu qemu64,-svm \
        -bios "${firmware}" \
        -drive "format=raw,file=${disk}" \
        -serial "${serial}" \
        -machine type=q35 \
        -vga std \
        -m 512M \
        -smp 4 \
        "${@}"
}

# The guest hides the cursor, and nothing shows it again when QEMU closes from outside
restore_terminal() {
    stty "${saved_terminal}"
    printf '\033[?25h'
}

if command -v "${putty}" > /dev/null 2>&1; then
    # Telnet makes PuTTY send each key at once and without local echo. The guest starts only
    # after PuTTY connects, so the terminal gets all of the boot. PuTTY cannot connect before
    # QEMU listens, and QEMU tells that on its standard error
    serial="mon:telnet:127.0.0.1:${terminal_port},server=on,wait=on"

    run_qemu "${@}" 2>&1 | while IFS= read -r line; do
        printf '%s\n' "${line}" >&2

        case "${line}" in
            *"waiting for connection"*)
                "${putty}" -telnet 127.0.0.1 -P "${terminal_port}" > /dev/null 2>&1 &
                ;;
        esac
    done
else
    serial=mon:stdio

    # The terminal would otherwise edit the line and echo it before QEMU sees it, and the guest
    # echoes it again. Ctrl-C goes to the guest. Enter stays a newline (icrnl), as a lone
    # carriage return never reaches the guest through the stdio of QEMU
    if test -t 0; then
        saved_terminal="$(stty -g)"
        trap restore_terminal EXIT
        # The shell runs the EXIT trap on a signal only through exit
        trap 'exit 1' HUP INT TERM
        stty -icanon -echo -isig -ixon
    fi

    run_qemu "${@}"
fi
