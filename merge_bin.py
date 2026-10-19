"""
PlatformIO post-build step: ESP8266 Arduino builds already produce a
single, self-contained flash image (the eboot bootloader stub is linked
into firmware.bin itself — unlike ESP32 there's no separate bootloader.bin
/ partitions.bin to merge). This script just copies that image to a
clearly-named file and prints the one-line flash command, so "export a
single .bin" has an unambiguous answer.
"""
Import("env")
import shutil
import os


def after_build(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    src = os.path.join(build_dir, "firmware.bin")
    dst = os.path.join(build_dir, "firmware-merged.bin")
    if os.path.exists(src):
        shutil.copyfile(src, dst)
        print(f"\n[merge_bin] Single flashable image ready: {dst}")
        print(f"[merge_bin] Flash with: esptool.py --port <PORT> --baud 460800 write_flash 0x0 {dst}\n")
    else:
        print(f"[merge_bin] WARNING: expected {src} but it was not found")


env.AddPostAction("$BUILD_DIR/firmware.bin", after_build)
