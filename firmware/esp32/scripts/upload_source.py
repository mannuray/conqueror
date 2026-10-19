"""PlatformIO post-script: custom targets for the source archive (bridge LLD R2.3 step 9, R2.5).

pio run -e bridge -t upload_source --upload-port /dev/ttyACM0   # USB: chip check, then esptool
pio run -e bridge_ota -t upload_source_ota                     # WiFi: espota -s (CQ_OTA_PASSWORD)
"""

import os
import pathlib
import subprocess
import sys

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

PROJECT = pathlib.Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]  # noqa: F821
sys.path.insert(0, str(PROJECT / "scripts"))
import check_chip  # noqa: E402

SOURCE_BIN = PROJECT / "build" / "source.bin"


def _source_offset() -> str:  # from the partition table, so a new layout can never write the wrong place (Plan N3)
    for line in (PROJECT / "partitions_cq8m.csv").read_text().splitlines():
        cols = [c.strip() for c in line.split(",")]
        if cols and cols[0] == "source":
            return cols[3]
    raise RuntimeError("partitions_cq8m.csv has no 'source' partition")


SOURCE_OFFSET = _source_offset()


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
    host = os.environ.get("CQ_OTA_HOST", "conqueror-bridge.local")  # ota.sh passes the resolved IP
    # Run without SCons echo: the command line holds the OTA password (never print it).
    print(f"upload_source_ota: espota -i {host} -p 3232 --auth=*** -s -f {SOURCE_BIN}")
    cmd = [env.subst("$PYTHONEXE"), str(espota / "tools" / "espota.py"), "-i", host, "-p", "3232",
           f"--auth={password}", "-s", "-f", str(SOURCE_BIN)]
    return subprocess.run(cmd, check=False).returncode


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
