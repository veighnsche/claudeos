# ClaudeOS - Bare-Metal ARM64 Operating System

A bare-metal ARM64 operating system with GUI, networking, and filesystem - built entirely with Claude Code.

Created by [@khaterdev](https://x.com/khaterdev)

## Features

- **Graphical Home Screen** - Animated logo with app launcher dock
- **Terminal App** - Full shell with command history, soft keyboard support
- **File Manager App** - Browse, create, edit, and delete files
- **Networking** - DHCP, TCP/IP stack, HTTP client (curl command)
- **Filesystem** - TinyFS with FAT-like structure on virtio-blk
- **Touch & Keyboard** - Both hardware keyboard and on-screen soft keyboard

## Quick Start

### Build

```bash
make -f Makefile.arm64 clean
make -f Makefile.arm64
```

### Run with QEMU

```bash
# Create disk image (first time only)
qemu-img create -f raw tinyos_disk.img 16M

# Run with QEMU
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a53 \
    -m 512M \
    -kernel kernel64.bin \
    -device virtio-gpu-device \
    -device virtio-keyboard-device \
    -device virtio-mouse-device \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0 \
    -drive file=tinyos_disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -serial stdio
```

## Requirements

- **Toolchain**: `aarch64-elf-gcc` (or `aarch64-none-elf-gcc`)
- **QEMU**: `qemu-system-aarch64`
- **macOS**: `brew install aarch64-elf-gcc qemu`
- **Linux**: Install `gcc-aarch64-linux-gnu` and `qemu-system-arm`

## Project Structure

```
kernel/
├── main.c                    # Kernel entry point & main loop
├── home.c                    # Home screen with app launcher
├── terminal.c                # Terminal app with shell
├── filemanager.c             # File manager app
├── keyboard.c                # Soft keyboard
├── fs.c                      # TinyFS filesystem
├── font.c                    # 8x16 bitmap font
├── memory.c                  # Heap allocator
├── tcp.c                     # TCP/IP stack
├── http.c                    # HTTP client
├── drivers/
│   ├── goldfish/fb.c         # Framebuffer driver
│   ├── virtio/gpu.c          # virtio-GPU driver
│   ├── virtio/input.c        # virtio-input (touch/keyboard)
│   ├── virtio/net.c          # virtio-net driver
│   ├── virtio/blk.c          # virtio-blk driver
│   └── gic.c                 # ARM GIC interrupt controller
├── net/net.c                 # Network stack (DHCP, ARP, etc.)
├── arch/arm64/
│   ├── boot.S                # ARM64 boot code
│   ├── vectors.S             # Exception vectors
│   └── linker.ld             # Memory layout
└── include/                  # Headers
```

## Terminal Commands

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `clear` | Clear screen |
| `heap` | Show heap memory statistics |
| `echo <text>` | Echo text back |
| `net` | Show network configuration |
| `curl <url>` | HTTP GET request |
| `disk` | Show disk information |
| `format` | Format filesystem |
| `ls` | List files |
| `cat <file>` | Display file contents |
| `write <file> <text>` | Write text to file |
| `rm <file>` | Delete file |
| `reboot` | Soft reboot |

## Technical Details

### Memory Map

| Address | Usage |
|---------|-------|
| 0x40200000 | Kernel load address |
| 0x42000000 | Framebuffer |
| 0x46000000 | Virtqueue (GPU) |
| 0x46200000 | Virtqueue (Input) |
| 0x47000000 | Virtqueue (Net) |
| 0x47100000 | Virtqueue (Block) |

### Filesystem (TinyFS)

- Sector 0: Superblock
- Sectors 1-8: FAT (File Allocation Table)
- Sectors 9-16: Root directory (64 entries max)
- Sectors 17+: Data clusters (2KB each)

### Build Outputs

- `kernel64.bin` - Raw binary kernel for QEMU
- `kernel64.elf` - ELF with debug symbols
- `kernel64.dump` - Disassembly for debugging

## Troubleshooting

**Black screen?**
- Ensure virtio-gpu-device is included
- Check serial output for errors

**No network?**
- Network initializes after GUI is up (check "Connected to Internet" on home screen)
- DHCP should assign IP automatically

**Filesystem errors?**
- Run `format` command in terminal to initialize disk
- Ensure disk image exists and is attached
