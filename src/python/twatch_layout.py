# twatch-elrs: attach the T-Watch S3 hardware layout and options to firmware.bin.
#
# Unified targets get theirs from python/UnifiedConfiguration.py, which picks a layout out of the ExpressLRS/targets
# repo (hardware/targets.json). The watch is not in that repo, so this script appends the layout from
# custom_twatch_layout itself - with the same appendToFirmware() and so the same on-flash format (product name,
# Lua name, options JSON, hardware JSON) that options.cpp / hardware.cpp read back.
#
# platformio env options:
#   custom_twatch_layout          path of the hardware layout JSON, relative to the project dir
#   custom_twatch_product         product name (128 bytes max)
#   custom_twatch_lua_name        Lua device name (16 bytes max)
#   custom_twatch_binding_phrase  binding phrase -> options "uid", hashed exactly like user_defines.txt's
#                                 -DMY_BINDING_PHRASE="..." (python/build_flags.py) and the web UI (config.cpp)
#
# Also writes firmware-merged.bin (bootloader + partitions + boot_app0 + firmware, flashable at 0x0).
Import("env")

import hashlib
import json
import os
import subprocess
import sys

import UnifiedConfiguration


def _opt(name, default=None):
    return env.GetProjectOption(name, default)


phrase = _opt("custom_twatch_binding_phrase", "")
if phrase:
    uid = list(hashlib.md5(('-DMY_BINDING_PHRASE="' + phrase + '"').encode()).digest()[0:6])
    env["OPTIONS_JSON"]["uid"] = uid
    sys.stdout.write("twatch: binding phrase -> UID %s\n" % ",".join(map(str, uid)))


def append_layout(source, target, env):
    layout = os.path.join(env.subst("$PROJECT_DIR"), _opt("custom_twatch_layout"))
    product = _opt("custom_twatch_product", "T-Watch S3")
    lua_name = _opt("custom_twatch_lua_name", "TWatch S3")
    defines = json.JSONEncoder().encode(env["OPTIONS_JSON"])
    firmware = str(target[0])
    with open(firmware, "r+b") as f:
        UnifiedConfiguration.appendToFirmware(f, product, lua_name, defines, {}, layout, None)
    sys.stdout.write("twatch: appended %s and options %s to %s\n" % (layout, defines, firmware))

    build_dir = env.subst("$BUILD_DIR")
    esptool = os.path.join(env.subst("$PROJECT_DIR"), "python", "external", "esptool", "esptool.py")
    merged = os.path.join(build_dir, "firmware-merged.bin")
    cmd = [env.subst("$PYTHONEXE"), esptool, "--chip", "esp32s3", "merge_bin", "-o", merged,
           "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "keep",
           "0x0", os.path.join(build_dir, "bootloader.bin"),
           "0x8000", os.path.join(build_dir, "partitions.bin"),
           "0xe000", os.path.join(build_dir, "boot_app0.bin"),
           "0x10000", firmware]
    subprocess.check_call(cmd)
    sys.stdout.write("twatch: wrote %s\n" % merged)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", append_layout)
