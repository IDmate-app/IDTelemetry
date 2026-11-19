# PlatformIO extra_script: Build-Kennung aus git in die Firmware (ROADMAP 0.6).
# FW_BUILD = <FW_VERSION>-g<commit>, z. B. "1.7.7-gabc1234-dirty"; "-dirty" heisst:
# aus nicht committetem Code gebaut. Ohne git: "<FW_VERSION>-nogit".
# 1.7.7: vorher "git describe". Der Release-Tag entsteht aber erst nach dem
# Build, so hiess 1.7.6 "1.7.5-1-gb5594b5" — verwirrend im Syslog und auf der
# Seite. Jetzt steht vorn immer die eigene Version.
import re
import subprocess

Import("env")  # noqa: F821  (von PlatformIO bereitgestellt)


def _version():
    try:
        with open(env.subst("$PROJECT_DIR") + "/src/config.h", encoding="utf-8") as f:  # noqa: F821
            m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
            return m.group(1) if m else "0.0.0"
    except OSError:
        return "0.0.0"


def _git(*args):
    return subprocess.check_output(
        ["git", *args], cwd=env.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL  # noqa: F821
    ).decode().strip()


def _build():
    ver = _version()
    try:
        sha = _git("rev-parse", "--short=7", "HEAD")
        dirty = _git("status", "--porcelain", "--untracked-files=no")
        return f"{ver}-g{sha}" + ("-dirty" if dirty else "")
    except Exception:
        return f"{ver}-nogit"


build = _build()[:40]
env.Append(CPPDEFINES=[("FW_BUILD", env.StringifyMacro(build))])  # noqa: F821
print("FW_BUILD =", build)
