@echo off
rem ==== Elus OS - build no Windows (NASM + MinGW i686) ====
setlocal

where i686-w64-mingw32-gcc >nul 2>&1 && set CC=i686-w64-mingw32-gcc || set CC=gcc
where i686-w64-mingw32-ld >nul 2>&1 && set LD=i686-w64-mingw32-ld || set LD=ld
where i686-w64-mingw32-objcopy >nul 2>&1 && set OBJCOPY=i686-w64-mingw32-objcopy || set OBJCOPY=objcopy

if not exist build mkdir build

nasm -f bin src\boot.asm -o build\boot.bin || goto :err
nasm -f bin src\stage2.asm -o build\stage2.bin || goto :err
nasm -f win32 src\entry.asm -o build\entry.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\sound.c -o build\sound.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\pci.c -o build\pci.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\net.c -o build\net.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\disk.c -o build\disk.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\fs.c -o build\fs.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\video.c -o build\video.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\video_codec.c -o build\video_codec.o || goto :err
%CC% -m32 -ffreestanding -O2 -Wall -Wno-array-bounds %EXTRA% -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-builtin -c src\kernel.c -o build\kernel.o || goto :err
%LD% -T src\linker.ld --image-base=0 --disable-reloc-section -o build\kernel.pe build\entry.o build\pci.o build\net.o build\sound.o build\disk.o build\fs.o build\video.o build\video_codec.o build\kernel.o || goto :err
%OBJCOPY% -O binary build\kernel.pe build\kernel.bin || goto :err

copy /b build\boot.bin + build\stage2.bin + build\kernel.bin build\os.img >nul || goto :err
powershell -NoProfile -Command "$f=[IO.File]::Open('build\os.img','Open','Write'); $f.SetLength(1MB); $f.Close()" || goto :err

echo.
powershell -NoProfile -Command "$s=(Get-Item 'build\kernel.bin').Length; Write-Host ('OK: build\os.img gerado. Kernel: ' + $s + ' bytes (limite 98304 bytes)')"
exit /b 0

:err
echo.
echo *** ERRO no build ***
exit /b 1
