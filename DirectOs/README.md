# DirectOS

DirectOS is a small BIOS-booted x86 operating system. Its 16-bit boot sector loads a freestanding 32-bit kernel, enters protected mode, and starts a text shell. The project is an educational kernel prototype, not a general-purpose PC operating system.

The build produces a raw 1.44 MB disk image. An optional El Torito ISO wraps that image for BIOS CD/DVD boot. The shell filesystem is volatile RAM: its files and user accounts disappear when the machine reboots.

## Features

- BIOS boot sector with EDD reads, a CHS fallback for floppy emulation, and an E820 usable-memory query.
- 32-bit protected-mode kernel and VGA text console.
- Shell with grouped `help`, command summaries via `man`, and RAM filesystem operations.
- Volatile account switching with `login` and `su`; password entry is masked.
- `fastfetch` reports CPUID CPU branding, PCI display/Ethernet IDs, BIOS-usable RAM, the current user, and RTC date/time.

DirectOS has no disk-storage driver, network stack/NIC driver, graphics acceleration, process isolation, scheduler, or heap allocator. `fastfetch` can identify PCI devices but cannot provide an IP address or a friendly GPU name. Accounts and passwords are in-memory shell metadata, not a security boundary; passwords are stored as plain text in RAM. `free`, `df`, and `du` describe RAMFS capacity, not physical memory or disk space.

## Build Prerequisites

The commands below target Ubuntu 24.04 or a similar Debian-based Linux system. They also work on an ARM Ubuntu server: the compiler cross-compiles the x86 kernel. From the server, install the tools:

```sh
sudo apt update
sudo apt install -y build-essential gcc-i686-linux-gnu binutils-i686-linux-gnu nasm python3 genisoimage file
```

To run QEMU locally, install its x86 system emulator too:

```sh
sudo apt install -y qemu-system-x86
```

Do not run `make` with `sudo`; build as the owner of the source directory so the generated files remain writable.

## Get the Source

Clone your GitHub repository, then enter its directory. Replace the placeholder with your GitHub owner and repository name:

```sh
git clone https://github.com/OWNER/DirectOs.git
cd DirectOs
```

If you already have the files on the Ubuntu server, just `cd` to that directory, for example:

```sh
cd "$HOME/DirectOs"
```

## Build the Raw Image

The Makefile defaults to an `i686-elf` bare-metal toolchain. On Ubuntu ARM, use the packaged `i686-linux-gnu` cross-compiler and override the tool names on the make command:

```sh
make -f Makefile.txt \
	CC=i686-linux-gnu-gcc \
	LD=i686-linux-gnu-ld \
	OBJCOPY=i686-linux-gnu-objcopy \
	AS=nasm \
	PYTHON=python3
```

The default `all` target creates these files:

```text
build/directos.img
build/directos.bin
build/directos.iso
```

`directos.img` is the canonical raw floppy image. `directos.bin` is a byte-for-byte copy with a `.bin` extension for tools that expect that suffix. `directos.iso` is a BIOS El Torito CD/DVD image containing the raw image. Verify the outputs with:

```sh
file build/directos.img build/directos.bin build/directos.iso
ls -lh build/directos.img build/directos.bin build/directos.iso
```

The image must be exactly 1,474,560 bytes. The boot sector loads up to 127 kernel sectors (65,024 bytes); the image builder stops with an error if the kernel is empty or exceeds that limit. To rebuild from scratch, run `make -f Makefile.txt clean` first, then repeat the build command above.

## Create a Bootable CD/DVD ISO

The default `make` build creates `build/directos.iso` using BIOS El Torito floppy emulation. To generate it separately after building the raw image, run:

```sh
make -f Makefile.txt \
	CC=i686-linux-gnu-gcc \
	LD=i686-linux-gnu-ld \
	OBJCOPY=i686-linux-gnu-objcopy \
	AS=nasm \
	PYTHON=python3 \
	ISO=genisoimage \
	build/directos.iso
```

The result is:

```text
build/directos.iso
```

This is an optical-disc ISO, not a hybrid USB installer image. Burn it as a disc image; do not copy the `.iso` file onto a data CD and expect it to boot.

## Run in QEMU

### Boot the raw disk image

IDE is the recommended test path for the raw image:

```sh
qemu-system-i386 -m 64M \
	-drive file=build/directos.img,format=raw,if=ide
```

### Boot the ISO

```sh
qemu-system-i386 -m 64M -boot order=d \
	-cdrom build/directos.iso
```

### Windows with portable QEMU

In PowerShell, adjust the QEMU folder if yours is elsewhere. If the portable package is under a Windows path containing accented characters and QEMU cannot find its firmware, copy its `share` folder to an ASCII-only path and pass that directory with `-L`:

```powershell
Set-Location 'C:\Path\To\qemu-portable'
New-Item -ItemType Directory -Force 'C:\Users\Public\qemu-share' | Out-Null
Copy-Item '.\share\*' 'C:\Users\Public\qemu-share' -Recurse -Force
.\qemu-system-i386.exe -L 'C:\Users\Public\qemu-share' -m 64M -boot order=d -cdrom 'C:\Users\Public\directos.iso' -display sdl
```

For the raw image, replace `-boot order=d -cdrom ...` with:

```powershell
.\qemu-system-i386.exe -L 'C:\Users\Public\qemu-share' -m 64M -drive "file=C:\Users\Public\directos.img,format=raw,if=ide" -display sdl
```

### Headless Linux server

Run QEMU with a VNC display bound to localhost:

```sh
qemu-system-i386 -m 64M -boot order=d \
	-cdrom build/directos.iso \
	-display vnc=127.0.0.1:1
```

Keep QEMU running. From your computer, open an SSH tunnel to the server and connect a VNC viewer to `127.0.0.1:5901`:

```sh
ssh -L 5901:127.0.0.1:5901 USER@SERVER
```

Replace `USER@SERVER` with your Ubuntu account and server address. The SSH command only tunnels the VNC connection; it does not add SSH support to DirectOS.

## Boot on a Physical PC

### CD or DVD

Burn `build/directos.iso` as an image using your disc-burning application (for example, Windows "Burn disc image"), insert the disc, and select the optical drive in the firmware boot menu. The PC must support legacy BIOS/CSM boot and BIOS INT 13h disk services. DirectOS is not UEFI bootable.

### USB drive: experimental

The `.iso` is not hybrid and is not intended to be written to USB as a standard bootable installer. The `.img` is a raw 1.44 MB floppy-style image, not a partitioned USB-disk image. You can try writing `build/directos.img` directly to a USB drive with balenaEtcher, or with Rufus if it accepts the image (choose raw/DD mode if offered), but many modern firmware implementations will not boot it as USB storage. Support depends on legacy BIOS/CSM and whether the firmware exposes that USB device in a compatible way. UEFI-only systems will not boot it.

Writing an image erases the selected drive. Double-check the target device before starting. QEMU is the recommended way to test changes; reliable USB boot would require a dedicated hard-disk/USB boot layout that this project does not currently build.

## Shell Basics

At the prompt, run `help` for the command index and `man COMMAND` for a summary. Useful examples:

```text
help
fastfetch
useradd joan
login joan
whoami
su root
```

`useradd` and `passwd` prompt for a new password twice; password characters are not displayed. Accounts are volatile and reset on reboot. Password masking is for display privacy only, not secure account isolation.

The `edit` command appends lines to a file; enter a single `.` on its own line to save. `format yes` resets RAMFS only and does not format a physical drive. `date` and `time` can read or set the emulated/physical RTC.

## Troubleshooting

- **`gtk initialization failed` on a server:** use QEMU's VNC display option shown above instead of a graphical display.
- **QEMU cannot find `bios-256k.bin`:** pass QEMU's firmware directory with `-L`; for portable Windows QEMU, an ASCII-only copy of the `share` folder avoids path-encoding issues.
- **`Disk read error`:** confirm you built the latest boot sector into the image and are booting the `.img` as IDE or the `.iso` as a CD/DVD. The bootloader has an EDD read with CHS fallback for floppy emulation.
- **Kernel-size error:** the bootloader can load at most 127 sectors; reduce kernel size before increasing the limit, since the boot-sector load strategy and memory layout must also change.
- **No network IP in `fastfetch`:** expected; no NIC driver or network stack is implemented.
