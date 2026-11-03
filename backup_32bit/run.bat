@echo off
set QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
%QEMU% -cpu max -accel tcg,thread=multi -drive format=raw,file=build\os.img -m 2048 -vga std -rtc base=localtime -netdev user,id=u1 -device rtl8139,netdev=u1 -audiodev dsound,id=snd0 -machine pc,pcspk-audiodev=snd0 -device sb16,audiodev=snd0
