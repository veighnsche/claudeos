#!/bin/bash
# ClaudeOS - QEMU Run Script
# Run with: ./run.sh [options]
#
# Options:
#   --debug     Enable input debug output
#   --tablet    Use tablet device instead of mouse (better touch simulation)

set -e

# Configuration
KERNEL="kernel64.bin"
DISK="tinyos_disk.img"
MEMORY="512M"

# Default to tablet for better touch simulation (absolute positioning)
INPUT_DEVICE="virtio-tablet-device"

# Parse arguments
for arg in "$@"; do
    case $arg in
        --debug)
            DEBUG=1
            ;;
        --mouse)
            INPUT_DEVICE="virtio-mouse-device"
            ;;
        --tablet)
            INPUT_DEVICE="virtio-tablet-device"
            ;;
    esac
done

# Check if kernel exists
if [ ! -f "$KERNEL" ]; then
    echo "Kernel not found. Building..."
    if command -v aarch64-elf-gcc &> /dev/null; then
        make -f Makefile.arm64 clean && make -f Makefile.arm64
    elif command -v aarch64-linux-gnu-gcc &> /dev/null; then
        make -f Makefile.arm64 clean && make -f Makefile.arm64 PREFIX=aarch64-linux-gnu-
    else
        echo "Error: No ARM64 cross-compiler found."
        echo "  macOS: brew install aarch64-elf-gcc"
        echo "  Linux: sudo apt install gcc-aarch64-linux-gnu"
        exit 1
    fi
fi

# Create disk image if it doesn't exist
if [ ! -f "$DISK" ]; then
    echo "Creating disk image..."
    qemu-img create -f raw "$DISK" 16M
fi

echo "Starting ClaudeOS..."
echo "  Input device: $INPUT_DEVICE"
echo "  Tip: Click in window to interact. Use Ctrl+Alt+G to release mouse."
echo ""

qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a53 \
    -m "$MEMORY" \
    -kernel "$KERNEL" \
    -device virtio-gpu-device \
    -device virtio-keyboard-device \
    -device "$INPUT_DEVICE" \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0 \
    -drive file="$DISK",format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -serial stdio
