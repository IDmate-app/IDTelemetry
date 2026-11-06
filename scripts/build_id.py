# PlatformIO extra_script: Build-Kennung aus git in die Firmware (ROADMAP 0.6).
# FW_BUILD = git describe, z. B. "1.5.0-2-gabc1234-dirty"; "-dirty" heisst:
# aus nicht committetem Code gebaut. Ohne git: "nogit".
import subprocess

Import("env")  # noqa: F821  (von PlatformIO bereitgestellt)


def _describe():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL)  # noqa: F821
        d = out.decode().strip()
        return d[1:] if d.startswith("v") else d
    except Exception:
        return "nogit"


build = _describe()[:40]
env.Append(CPPDEFINES=[("FW_BUILD", env.StringifyMacro(build))])  # noqa: F821
print("FW_BUILD =", build)
