#!/bin/sh
# Build para Linux/WSL/MSYS2 (mesmos passos do build.bat)
set -e
command -v i686-w64-mingw32-gcc >/dev/null 2>&1 && CC_DEFAULT=i686-w64-mingw32-gcc || CC_DEFAULT=gcc
command -v i686-w64-mingw32-ld >/dev/null 2>&1 && LD_DEFAULT=i686-w64-mingw32-ld || LD_DEFAULT=ld
command -v i686-w64-mingw32-objcopy >/dev/null 2>&1 && OBJCOPY_DEFAULT=i686-w64-mingw32-objcopy || OBJCOPY_DEFAULT=objcopy
CC=${CC:-$CC_DEFAULT}
LD=${LD:-$LD_DEFAULT}
OBJCOPY=${OBJCOPY:-$OBJCOPY_DEFAULT}

mkdir -p build
nasm -f bin src/boot.asm   -o build/boot.bin
nasm -f bin src/stage2.asm -o build/stage2.bin
nasm -f win32 src/entry.asm -o build/entry.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/sound.c -o build/sound.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/pci.c -o build/pci.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/net.c -o build/net.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/disk.c -o build/disk.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/fs.c -o build/fs.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/video.c -o build/video.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/video_codec.c -o build/video_codec.o
$CC -m32 -ffreestanding -O2 -Wall -Wno-array-bounds $EXTRA -mgeneral-regs-only -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src/kernel.c -o build/kernel.o
$LD -T src/linker.ld --image-base=0 --disable-reloc-section -o build/kernel.pe build/entry.o build/pci.o build/net.o build/sound.o build/disk.o build/fs.o build/video.o build/video_codec.o build/kernel.o
$OBJCOPY -O binary build/kernel.pe build/kernel.bin
cat build/boot.bin build/stage2.bin build/kernel.bin > build/os.img
truncate -s 1M build/os.img
echo "OK: build/os.img (kernel: $(stat -c %s build/kernel.bin) bytes, limite 98304)"
