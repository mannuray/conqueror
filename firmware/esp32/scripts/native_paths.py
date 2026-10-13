"""PlatformIO pre-script for env:native: absolute path of protocol/vectors/frames.json for tests."""

import pathlib

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

vectors = (
    pathlib.Path(env.subst("$PROJECT_DIR")).parents[1] / "protocol" / "vectors" / "frames.json"  # type: ignore[name-defined]  # noqa: F821
)
env.Append(CPPDEFINES=[("CQ_VECTORS_PATH", env.StringifyMacro(str(vectors)))])  # type: ignore[name-defined]  # noqa: F821
