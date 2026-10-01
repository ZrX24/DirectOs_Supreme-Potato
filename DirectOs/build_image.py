import pathlib
import sys

SECTOR_SIZE = 512
FLOPPY_SIZE = 1474560
MAX_KERNEL_SIZE = 127 * SECTOR_SIZE


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: build_image.py BOOT.BIN KERNEL.BIN OUTPUT.IMG")

    boot_path, kernel_path, output_path = map(pathlib.Path, sys.argv[1:])
    boot = boot_path.read_bytes()
    kernel = kernel_path.read_bytes()

    if len(boot) != SECTOR_SIZE or boot[510:512] != b"\x55\xaa":
        raise SystemExit("boot sector must be exactly 512 bytes and end with 55 AA")
    if not kernel or len(kernel) > MAX_KERNEL_SIZE:
        raise SystemExit("kernel must be between 1 byte and 127 sectors")

    image = bytearray(FLOPPY_SIZE)
    image[:SECTOR_SIZE] = boot
    image[SECTOR_SIZE:SECTOR_SIZE + len(kernel)] = kernel
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(image)
    print(f"Created {output_path} ({len(image)} bytes, kernel {len(kernel)} bytes)")


if __name__ == "__main__":
    main()
