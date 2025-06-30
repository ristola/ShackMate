def after_build(source, target, env):
    project_name = get_project_name(env)
    version = get_version(env)
    build_dir = env['BUILD_DIR']
    pioenv = env['PIOENV']

    firmware_src = os.path.join(build_dir, pioenv, "firmware.bin")
    firmware_dir = os.path.join(env['PROJECT_DIR'], "firmware")
    os.makedirs(firmware_dir, exist_ok=True)
    firmware_dest = os.path.join(firmware_dir, f"{project_name}-v{version}.bin")

    shutil.copyfile(firmware_src, firmware_dest)
    print(f"[copy_firmware.py] Copied firmware to {firmware_dest}")

    fs_filename = None
    for fs_image in ["spiffs.bin", "littlefs.bin"]:
        fs_src = os.path.join(build_dir, pioenv, fs_image)
        if os.path.exists(fs_src):
            fs_dest = os.path.join(firmware_dir, f"{project_name}-fs-v{version}.bin")
            shutil.copyfile(fs_src, fs_dest)
            fs_filename = os.path.basename(fs_dest)
            print(f"[copy_firmware.py] Copied filesystem image to {fs_dest}")
            break

    version_json_path = os.path.join(firmware_dir, "version.json")
    version_data = {
        "project": project_name,
        "version": version,
        "firmware_filename": os.path.basename(firmware_dest),
        "fs_filename": fs_filename
    }
    with open(version_json_path, "w") as f:
        json.dump(version_data, f, indent=2)
    print(f"[copy_firmware.py] Wrote {version_json_path}")