"""PlatformIO post-script: custom targets for the source archive (bridge LLD R2.3 step 9, R2.5).

pio run -e bridge -t upload_source --upload-port /dev/ttyACM0   # USB: chip check, then esptool
pio run -e bridge_ota -t upload_source_ota                     # WiFi: espota -s (CQ_OTA_PASSWORD)
"""

import os
import pathlib
import sys

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

PROJECT = pathlib.Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]  # noqa: F821
sys.path.insert(0, str(PROJECT / "scripts"))
import check_chip  # noqa: E402

SOURCE_BIN = PROJECT / "build" / "source.bin"
SOURCE_OFFSET = "0x610000"


def _upload_usb(target, source, env):  # type: ignore[no-untyped-def]
    port = env.subst("$UPLOAD_PORT")
    if not port:
        env.AutodetectUploadPort()
        port = env.subst("$UPLOAD_PORT")
    pkg = env.PioPlatform().get_package_dir("tool-esptoolpy")
    cmd = check_chip.esptool_cmd(env.subst("$PYTHONEXE"), pkg)
    if check_chip.run_check(cmd, port) != 0:
        return 1
    return env.Execute(
        " ".join(
            [
                *cmd,
                "--chip",
                "esp32s3",
                "--port",
                port,
                "--baud",
                "921600",
                "write_flash",
                SOURCE_OFFSET,
                str(SOURCE_BIN),
            ]
        )
    )


def _upload_ota(target, source, env):  # type: ignore[no-untyped-def]
    password = os.environ.get("CQ_OTA_PASSWORD", "")
    if not password:
        print("upload_source_ota: export CQ_OTA_PASSWORD first")
        return 1
    espota = pathlib.Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
    return env.Execute(
        f'"$PYTHONEXE" "{espota / "tools" / "espota.py"}" -i conqueror-bridge.local -p 3232 '
        f'--auth="{password}" -s -f "{SOURCE_BIN}"'
    )


env.AddCustomTarget(  # type: ignore[name-defined]  # noqa: F821
    name="upload_source",
    dependencies=None,
    actions=[_upload_usb],
    title="Upload source",
    description="write build/source.bin to the source partition (USB)",
)
env.AddCustomTarget(  # type: ignore[name-defined]  # noqa: F821
    name="upload_source_ota",
    dependencies=None,
    actions=[_upload_ota],
    title="Upload source OTA",
    description="write build/source.bin over WiFi (espota -s)",
)
