"""PlatformIO pre-script: fail the build when generated protocol files are stale (LLD R2.3/2)."""

import pathlib
import shutil
import subprocess

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

if env.subst("$PIOENV") != "native":  # type: ignore[name-defined]  # noqa: F821
    protocol = pathlib.Path(env.subst("$PROJECT_DIR")).parents[1] / "protocol"  # type: ignore[name-defined]  # noqa: F821
    if shutil.which("uv") is None:
        print("protogen_check: uv not found (see docs/dev-setup.md)")
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    result = subprocess.run(["uv", "run", "protogen", "check"], cwd=protocol, check=False)
    if result.returncode != 0:
        print("protogen_check: stale generated files - run: cd protocol && uv run protogen write")
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    print("protogen_check: generated files are fresh")
