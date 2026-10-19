"""PlatformIO pre-script: fail the build when the generated Tank Logo files are stale (Plan N1, LLD §15)."""

import pathlib
import shutil
import subprocess

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

if env.subst("$PIOENV") != "native":  # type: ignore[name-defined]  # noqa: F821
    logo = pathlib.Path(env.subst("$PROJECT_DIR")).parents[1] / "logo"  # type: ignore[name-defined]  # noqa: F821
    if shutil.which("uv") is None:
        print("logogen_check: uv not found (see docs/dev-setup.md)")
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    if subprocess.run(["uv", "run", "logogen", "check"], cwd=logo, check=False).returncode != 0:
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    print("logogen_check: generated files are fresh")
