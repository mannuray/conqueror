"""PlatformIO pre-script: define CQ_GIT_COMMIT for the HELLO frame (design-notes §27).

CQ_GIT_COMMIT = first 8 hex digits of HEAD as a number (git rev-parse --short=8 HEAD);
0 when git or the repository is not available.
"""

import subprocess

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

try:
    rev = subprocess.run(
        ["git", "rev-parse", "--short=8", "HEAD"],
        cwd=env.subst("$PROJECT_DIR"),  # type: ignore[name-defined]  # noqa: F821
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    commit = int(rev[:8], 16)
except (OSError, subprocess.CalledProcessError, ValueError):
    commit = 0
env.Append(CPPDEFINES=[("CQ_GIT_COMMIT", f"0x{commit:08X}UL")])  # type: ignore[name-defined]  # noqa: F821
print(f"git_commit: CQ_GIT_COMMIT=0x{commit:08X}")
