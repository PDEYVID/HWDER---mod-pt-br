"""Run commands inside the Visual Studio x64 developer environment.

Paths are passed through environment variables (HW_*) so that non-ASCII characters
in the project path survive cmd.exe.
"""
import os
import subprocess
import tempfile

DEFAULT_VS = r"C:\Program Files\Microsoft Visual Studio\2022\Community"
VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")
_env_cache = None
_root_cache = None


def vs_root():
    """Newest Visual Studio / Build Tools install with the C++ tools (any edition or version)."""
    global _root_cache
    if _root_cache is None:
        _root_cache = DEFAULT_VS
        if os.path.exists(VSWHERE):
            out = subprocess.run([VSWHERE, "-all", "-prerelease", "-products", "*", "-sort", "-property",
                                  "installationPath"], capture_output=True, text=True).stdout
            for p in out.splitlines():
                if os.path.exists(os.path.join(p.strip(), "VC", "Auxiliary", "Build", "vcvars64.bat")):
                    _root_cache = p.strip()
                    break
    return _root_cache


def vs_env():
    global _env_cache
    if _env_cache is None:
        vcvars = os.path.join(vs_root(), "VC", "Auxiliary", "Build", "vcvars64.bat")
        out = subprocess.run(f'cmd /c ""{vcvars}" >nul && set"', capture_output=True, text=True,
                             shell=False).stdout
        env = {}
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
        if "INCLUDE" not in env:
            raise RuntimeError("failed to load Visual Studio environment")
        _env_cache = env
    return dict(_env_cache)


def run(args, cwd=None, **kw):
    """Run an argv list (no shell) with the VS environment."""
    env = vs_env()
    exe = args[0]
    for d in env["PATH"].split(";"):
        cand = os.path.join(d, exe if exe.endswith(".exe") else exe + ".exe")
        if os.path.exists(cand):
            args = [cand] + list(args[1:])
            break
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, **kw)
