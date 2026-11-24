Import("env")
import os
import shutil

TARGET_MAP = {
    "freenove_esp32_s3_n16r8": "chronoring_freenove_esp32s3_display.bin",
    "esp32_classic": "chronoring_esp32_classic_headless.bin",
    "esp32_c3": "chronoring_esp32c3_headless.bin",
}


def export_firmware_bin(source, target, env):
    pio_env = env.subst("$PIOENV")
    out_name = TARGET_MAP.get(pio_env)
    if not out_name:
        return
    src_bin = str(target[0])
    project_dir = env.subst("$PROJECT_DIR")
    out_dir = os.path.join(project_dir, "firmware")
    os.makedirs(out_dir, exist_ok=True)
    dst_bin = os.path.join(out_dir, out_name)
    shutil.copy2(src_bin, dst_bin)
    size_kb = os.path.getsize(dst_bin) / 1024.0
    print(f"[EXPORT] {pio_env} -> firmware/{out_name} ({size_kb:.1f} KB)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", export_firmware_bin)
