#!/usr/bin/env python3
"""
OUI-Spy C6 — Fully Automated Firmware Flasher

Handles EVERYTHING end-to-end:
  1. Installs Python dependencies (esptool, pyserial)
  2. Finds or installs Git
  3. Finds or installs ESP-IDF
  4. Builds the firmware
  5. Detects serial port and flashes the board

Just run:
    python flash.py

SPDX-License-Identifier: MIT
"""

import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from collections import deque
from datetime import datetime
from urllib.request import urlopen, Request
from urllib.error import URLError

# ── Flash layout (must match partitions.csv) ──
CHIP = "esp32c6"
FLASH_SIZE = "4MB"
FLASH_MODE = "dio"
FLASH_FREQ = "80m"
BAUD = 460800
MONITOR_BAUD = 115200
IDF_VERSION = "v5.3.2"
MONITOR_RETRY_SECONDS = 15
GITHUB_REPO = "hipstereclipse/ouispy-c6"
GITHUB_RELEASE_TAG = "latest"

CRASH_MARKERS = (
    "guru meditation",
    "backtrace:",
    "abort() was called",
    "assert failed",
    "stack overflow",
    "esp_error_check failed",
    "panic'ed",
    "panic handler",
    "exception was unhandled",
)

RESET_MARKERS = (
    "rst:",
    "rebooting...",
    "ets jun",
)

FLASH_MAP = [
    (0x0000,   "bootloader.bin",     "bootloader/bootloader.bin"),
    (0x8000,   "partition-table.bin", "partition_table/partition-table.bin"),
    (0x10000,  "ouispy-c6.bin",      "ouispy-c6.bin"),
]

BANNER = r"""
  ╔═══════════════════════════════════════════╗
  ║         OUI-SPY C6 Firmware Flasher       ║
  ║   Waveshare ESP32-C6-LCD-1.47             ║
  ║   Open Source RF Intelligence Tool        ║
  ╚═══════════════════════════════════════════╝
"""

# ── ANSI colors ──
class C:
    RED     = "\033[91m"
    GREEN   = "\033[92m"
    YELLOW  = "\033[93m"
    BLUE    = "\033[94m"
    PURPLE  = "\033[95m"
    CYAN    = "\033[96m"
    BOLD    = "\033[1m"
    DIM     = "\033[2m"
    RESET   = "\033[0m"

if sys.platform == "win32":
    os.system("")  # enable VT100 escape sequences

def color_print(msg, color=C.RESET):
    print(f"{color}{msg}{C.RESET}")

def section(title):
    color_print(f"\n{'─' * 46}", C.PURPLE)
    color_print(f"  {title}", C.BOLD)
    color_print(f"{'─' * 46}", C.PURPLE)

def ask_yes(prompt, default_yes=True):
    hint = "Y/n" if default_yes else "y/N"
    answer = input(f"{C.BOLD}{prompt} [{hint}]: {C.RESET}").strip().lower()
    if answer == "":
        return default_yes
    return answer in ("y", "yes")

def fail(msg):
    color_print(f"\n  {msg}", C.RED)
    print()
    input(f"{C.DIM}  Press Enter to exit...{C.RESET}")
    sys.exit(1)

def get_project_dir():
    return os.path.dirname(os.path.abspath(__file__))

def get_build_dir():
    return os.path.join(get_project_dir(), "build")

def get_monitor_log_dir():
    return os.path.join(get_project_dir(), "logs")

def firmware_ready():
    bd = get_build_dir()
    return all(os.path.isfile(os.path.join(bd, rel)) for _, _, rel in FLASH_MAP)

def run_shell(cmd, cwd=None, env=None):
    """Run a command through the platform shell. Returns (success, output)."""
    if sys.platform == "win32":
        r = subprocess.run(["cmd", "/c", cmd], cwd=cwd, env=env)
    else:
        r = subprocess.run(["bash", "-c", cmd], cwd=cwd, env=env)
    return r.returncode == 0

def _timestamp():
    return datetime.now().strftime("%H:%M:%S")

def _make_monitor_log_path():
    os.makedirs(get_monitor_log_dir(), exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return os.path.join(get_monitor_log_dir(), f"serial-monitor-{stamp}.log")

def _contains_marker(text, markers):
    lower = text.lower()
    return any(marker in lower for marker in markers)

def _open_serial_with_retry(port, baudrate, retry_seconds):
    import serial

    deadline = time.time() + retry_seconds
    last_error = None
    while time.time() < deadline:
        try:
            ser = serial.Serial(port=port, baudrate=baudrate, timeout=0.25)
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            try:
                ser.reset_input_buffer()
            except Exception:
                pass
            return ser
        except Exception as exc:
            last_error = exc
            time.sleep(0.5)
    raise last_error if last_error else RuntimeError(f"Could not open {port}")

def monitor_serial(port, baudrate):
    import serial

    log_path = _make_monitor_log_path()
    recent_lines = deque(maxlen=80)
    crash_detected = False
    reset_seen = False
    crash_count = 0

    section("Serial monitor")
    color_print(f"  Port:      {port}", C.CYAN)
    color_print(f"  Baud:      {baudrate}", C.CYAN)
    color_print(f"  Log file:  {log_path}", C.CYAN)
    color_print("  Press Ctrl+C to stop monitoring.\n", C.DIM)

    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        log_file.write(f"OUI-Spy C6 serial monitor\n")
        log_file.write(f"Port: {port}\n")
        log_file.write(f"Baud: {baudrate}\n")
        log_file.write(f"Started: {datetime.now().isoformat()}\n\n")
        log_file.flush()

        ser = None
        try:
            while True:
                if ser is None:
                    color_print("  Connecting serial monitor...", C.YELLOW)
                    try:
                        ser = _open_serial_with_retry(port, baudrate, MONITOR_RETRY_SECONDS)
                    except Exception as exc:
                        color_print(f"  Could not open {port}: {exc}", C.RED)
                        color_print(f"  Serial log saved to: {log_path}", C.CYAN)
                        return False
                    color_print("  Serial monitor connected. Waiting for device output...\n", C.GREEN)

                try:
                    raw = ser.readline()
                except serial.SerialException as exc:
                    color_print(f"\n  Serial connection lost: {exc}", C.YELLOW)
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None
                    time.sleep(0.5)
                    continue

                if not raw:
                    continue

                text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                entry = f"[{_timestamp()}] {text}"
                print(entry)
                log_file.write(entry + "\n")
                log_file.flush()
                recent_lines.append(entry)

                if _contains_marker(text, RESET_MARKERS):
                    reset_seen = True

                if _contains_marker(text, CRASH_MARKERS):
                    crash_detected = True
                    crash_count += 1
                    color_print("\n  Crash signature detected in serial output.", C.RED)
                    color_print(f"  Capture saved to: {log_path}", C.CYAN)
                    if reset_seen:
                        color_print("  A reset marker was also seen, so this likely explains the reboot loop.", C.YELLOW)
                    print()
        except KeyboardInterrupt:
            print()
            color_print("  Serial monitor stopped by user.", C.YELLOW)
            if crash_detected:
                color_print(f"  Crash signatures seen: {crash_count}", C.RED)
                color_print("  Last captured lines:", C.BOLD)
                for line in recent_lines:
                    print(f"    {line}")
            else:
                color_print("  No crash signatures detected during this session.", C.GREEN)
            color_print(f"  Serial log saved to: {log_path}", C.CYAN)
            return not crash_detected
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass


# ═════════════════════════════════════════════
#  PYTHON DEPENDENCIES
# ═════════════════════════════════════════════

def ensure_pip_package(name, import_name=None):
    """Import a package; install via pip if missing. Returns True on success."""
    mod = import_name or name
    try:
        __import__(mod)
        return True
    except ImportError:
        pass
    color_print(f"  Installing {name}...", C.YELLOW)
    r = subprocess.run(
        [sys.executable, "-m", "pip", "install", name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if r.returncode != 0:
        # retry without --quiet so user can see what went wrong
        subprocess.run([sys.executable, "-m", "pip", "install", name])
    try:
        __import__(mod)
        return True
    except ImportError:
        color_print(f"  Failed to install {name}.", C.RED)
        return False


# ═════════════════════════════════════════════
#  GIT
# ═════════════════════════════════════════════

def ensure_git():
    """Make sure git is available. Try to install it if not."""
    if shutil.which("git"):
        return True

    color_print("  Git is not installed (needed to download ESP-IDF).", C.YELLOW)

    if sys.platform == "win32":
        # Try winget first (built into Windows 10/11)
        if shutil.which("winget"):
            color_print("  Installing Git via winget...", C.CYAN)
            r = subprocess.run(
                ["winget", "install", "--id", "Git.Git", "-e",
                 "--accept-package-agreements", "--accept-source-agreements"],
            )
            if r.returncode == 0:
                # winget installs to Program Files; refresh PATH
                git_paths = [
                    os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Git", "cmd"),
                    os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Git", "cmd"),
                    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Git", "cmd"),
                ]
                for gp in git_paths:
                    if os.path.isfile(os.path.join(gp, "git.exe")):
                        os.environ["PATH"] = gp + os.pathsep + os.environ["PATH"]
                        break
                if shutil.which("git"):
                    color_print("  Git installed successfully.", C.GREEN)
                    return True
    elif shutil.which("apt-get"):
        color_print("  Installing Git via apt...", C.CYAN)
        subprocess.run(["sudo", "apt-get", "install", "-y", "git"])
        if shutil.which("git"):
            return True
    elif shutil.which("brew"):
        color_print("  Installing Git via Homebrew...", C.CYAN)
        subprocess.run(["brew", "install", "git"])
        if shutil.which("git"):
            return True

    if not shutil.which("git"):
        color_print("  Could not install Git automatically.", C.RED)
        color_print("  Please install Git from: https://git-scm.com/downloads", C.CYAN)
        color_print("  Then run this script again.", C.CYAN)
        return False
    return True


# ═════════════════════════════════════════════
#  ESP-IDF — FIND / INSTALL / ACTIVATE
# ═════════════════════════════════════════════

def _has_idf_py(path):
    """Check if a directory is a valid ESP-IDF root."""
    return path and os.path.isfile(os.path.join(path, "tools", "idf.py"))

def find_idf_installation():
    """Search the system for an existing ESP-IDF installation. Returns path or None."""
    # 1. Already-activated environment
    if shutil.which("idf.py"):
        idf = os.environ.get("IDF_PATH", "")
        if _has_idf_py(idf):
            return idf

    # 2. IDF_PATH env var
    idf = os.environ.get("IDF_PATH", "")
    if _has_idf_py(idf):
        return idf

    home = os.path.expanduser("~")

    # 3. Platform-specific common locations
    candidates = []
    if sys.platform == "win32":
        # ESP-IDF Tools Installer on Windows
        espressif = os.path.join(home, ".espressif")
        frameworks_dir = r"C:\Espressif\frameworks"
        for base in [frameworks_dir, espressif]:
            if os.path.isdir(base):
                try:
                    for entry in sorted(os.listdir(base), reverse=True):
                        candidates.append(os.path.join(base, entry))
                except OSError:
                    pass
        candidates += [
            os.path.join(home, "esp", "esp-idf"),
            os.path.join(home, "esp-idf"),
            r"C:\esp\esp-idf",
        ]
    else:
        candidates += [
            os.path.join(home, "esp", "esp-idf"),
            os.path.join(home, "esp-idf"),
            "/opt/esp-idf",
        ]

    for c in candidates:
        if _has_idf_py(c):
            return c

    return None

def _default_idf_dir():
    return os.path.join(os.path.expanduser("~"), "esp", "esp-idf")

def _python_is_idf_compatible(version_str):
    """Prefer CPython 3.8-3.11 for ESP-IDF v5.3 on Windows."""
    m = re.match(r"^(\d+)\.(\d+)", version_str.strip())
    if not m:
        return False
    major = int(m.group(1))
    minor = int(m.group(2))
    return major == 3 and 8 <= minor <= 11

def _try_install_python311_windows():
    """Attempt one-time Python 3.11 install via winget."""
    if sys.platform != "win32" or not shutil.which("winget"):
        return False
    color_print("  Installing Python 3.11 via winget for ESP-IDF compatibility...", C.CYAN)
    r = subprocess.run([
        "winget", "install", "--id", "Python.Python.3.11", "-e",
        "--accept-package-agreements", "--accept-source-agreements",
    ])
    return r.returncode == 0

def select_idf_python():
    """Pick a Python executable compatible with ESP-IDF tool install on this host."""
    # Running interpreter first (fast path).
    if _python_is_idf_compatible(platform.python_version()):
        return sys.executable

    # Windows: prefer Python launcher if available.
    if sys.platform == "win32" and shutil.which("py"):
        r = subprocess.run(["py", "-0p"], capture_output=True, text=True)
        if r.returncode == 0:
            candidates = []
            for line in r.stdout.splitlines():
                # Example: " -V:3.12 * C:\\Python312\\python.exe"
                m = re.search(r"-V:(\d+\.\d+).*?([A-Za-z]:\\.*python\.exe)", line)
                if not m:
                    continue
                ver = m.group(1)
                exe = m.group(2).strip()
                if _python_is_idf_compatible(ver) and os.path.isfile(exe):
                    mm = re.match(r"^(\d+)\.(\d+)$", ver)
                    candidates.append((int(mm.group(2)), exe))
            if candidates:
                # Highest compatible minor first (prefer 3.12 over 3.11 ...)
                candidates.sort(key=lambda x: x[0], reverse=True)
                return candidates[0][1]

    # Fallback: try versioned executables on PATH.
    for name in ["python3.12", "python3.11", "python3.10", "python3.9", "python3.8", "python"]:
        exe = shutil.which(name)
        if not exe:
            continue
        r = subprocess.run([exe, "-c", "import sys; print(f'{sys.version_info[0]}.{sys.version_info[1]}')"], capture_output=True, text=True)
        if r.returncode == 0 and _python_is_idf_compatible(r.stdout.strip()):
            return exe

    # Last resort on Windows: install Python 3.11 automatically, then retry once.
    if _try_install_python311_windows():
        return select_idf_python_no_install()

    return None

def select_idf_python_no_install():
    """Same selection logic as select_idf_python() but without auto-install side effects."""
    if _python_is_idf_compatible(platform.python_version()):
        return sys.executable

    if sys.platform == "win32" and shutil.which("py"):
        r = subprocess.run(["py", "-0p"], capture_output=True, text=True)
        if r.returncode == 0:
            candidates = []
            for line in r.stdout.splitlines():
                m = re.search(r"-V:(\d+\.\d+).*?([A-Za-z]:\\.*python\.exe)", line)
                if not m:
                    continue
                ver = m.group(1)
                exe = m.group(2).strip()
                if _python_is_idf_compatible(ver) and os.path.isfile(exe):
                    mm = re.match(r"^(\d+)\.(\d+)$", ver)
                    candidates.append((int(mm.group(2)), exe))
            if candidates:
                candidates.sort(key=lambda x: x[0], reverse=True)
                return candidates[0][1]

    for name in ["python3.11", "python3.10", "python3.9", "python3.8", "python"]:
        exe = shutil.which(name)
        if not exe:
            continue
        r = subprocess.run([exe, "-c", "import sys; print(f'{sys.version_info[0]}.{sys.version_info[1]}')"], capture_output=True, text=True)
        if r.returncode == 0 and _python_is_idf_compatible(r.stdout.strip()):
            return exe

    return None

def _env_with_python_first(python_exe):
    env = os.environ.copy()
    python_dir = os.path.dirname(os.path.abspath(python_exe))
    path_parts = [python_dir]
    cargo_bin = os.path.join(os.path.expanduser("~"), ".cargo", "bin")
    if os.path.isfile(os.path.join(cargo_bin, "cargo.exe")):
        path_parts.append(cargo_bin)
    env["PATH"] = os.pathsep.join(path_parts) + os.pathsep + env.get("PATH", "")

    # Keep ESP-IDF bound to a virtualenv matching the selected Python version.
    r = subprocess.run(
        [python_exe, "-c", "import sys; print(f'{sys.version_info[0]}.{sys.version_info[1]}')"],
        capture_output=True,
        text=True,
    )
    if r.returncode == 0:
        py_mm = r.stdout.strip()
        idf_mm = ".".join(IDF_VERSION.lstrip("v").split(".")[:2])
        env["IDF_PYTHON_ENV_PATH"] = os.path.join(
            os.path.expanduser("~"), ".espressif", "python_env", f"idf{idf_mm}_py{py_mm}_env"
        )

    return env

def install_esp_idf():
    """Clone and install ESP-IDF. Returns the install path or None."""
    dest = _default_idf_dir()

    if os.path.isdir(dest):
        # Directory exists but isn't a valid IDF — don't clobber it
        if _has_idf_py(dest):
            return dest
        color_print(f"  {dest} exists but doesn't look like ESP-IDF.", C.YELLOW)
        color_print(f"  Please remove it manually and retry.", C.YELLOW)
        return None

    color_print(f"\n  Downloading ESP-IDF {IDF_VERSION}...", C.CYAN)
    color_print(f"  Destination: {dest}", C.DIM)
    color_print(f"  (This is a one-time download, ~1.5 GB)\n", C.DIM)

    r = subprocess.run([
        "git", "clone",
        "-b", IDF_VERSION,
        "--recursive",
        "--depth", "1",
        "--shallow-submodules",
        "--jobs", "8",
        "https://github.com/espressif/esp-idf.git",
        dest,
    ])
    if r.returncode != 0:
        color_print("  Git clone failed.", C.RED)
        return None

    color_print(f"\n  Installing ESP-IDF toolchain for {CHIP}...", C.CYAN)
    color_print("  (This downloads the compiler + tools, ~800 MB)\n", C.DIM)

    if sys.platform == "win32":
        install_bat = os.path.join(dest, "install.bat")
        # Run via cmd with tokenized arguments to avoid Windows quote escaping issues.
        py = select_idf_python()
        if not py:
            color_print("  No compatible Python found for ESP-IDF (need Python 3.8-3.11).", C.RED)
            color_print("  Install Python 3.11 and re-run this script.", C.CYAN)
            return None
        color_print(f"  Using Python for ESP-IDF install: {py}", C.DIM)
        env = _env_with_python_first(py)
        r = subprocess.run(["cmd", "/d", "/c", "call", install_bat, CHIP], cwd=dest, env=env)
        ok = (r.returncode == 0)
    else:
        install_sh = os.path.join(dest, "install.sh")
        ok = run_shell(f'"{install_sh}" esp32c6', cwd=dest)

    if not ok:
        color_print("  ESP-IDF tool installation failed.", C.RED)
        return None

    color_print("  ESP-IDF installed successfully!", C.GREEN)
    return dest

def ensure_idf_tools_installed(idf_path):
    """Ensure ESP-IDF toolchain is installed for the selected chip."""
    color_print(f"\n  Ensuring ESP-IDF toolchain for {CHIP} is installed...", C.CYAN)
    color_print("  (This may take a while on first run.)\n", C.DIM)

    if sys.platform == "win32":
        install_bat = os.path.join(idf_path, "install.bat")
        py = select_idf_python()
        if not py:
            color_print("  No compatible Python found for ESP-IDF (need Python 3.8-3.11).", C.RED)
            color_print("  Install Python 3.11 and re-run this script.", C.CYAN)
            return False
        color_print(f"  Using Python for ESP-IDF tools: {py}", C.DIM)
        env = _env_with_python_first(py)
        r = subprocess.run(["cmd", "/d", "/c", "call", install_bat, CHIP], cwd=idf_path, env=env)
        return r.returncode == 0

    install_sh = os.path.join(idf_path, "install.sh")
    return run_shell(f'"{install_sh}" {CHIP}', cwd=idf_path)

def build_with_idf(idf_path):
    """Activate ESP-IDF environment and build the project. Returns True on success."""
    project = get_project_dir()

    color_print(f"\n  ESP-IDF path: {idf_path}", C.DIM)
    color_print(f"  Project:      {project}", C.DIM)
    color_print("  Building firmware (first build may take several minutes)...\n", C.YELLOW)

    if sys.platform == "win32":
        py = select_idf_python()
        if not py:
            color_print("  No compatible Python found for ESP-IDF build (need Python 3.8-3.11).", C.RED)
            return False
        color_print(f"  Using Python for ESP-IDF build: {py}", C.DIM)
        env = _env_with_python_first(py)
        export = os.path.join(idf_path, "export.bat")
        r = subprocess.run([
            "cmd", "/d", "/c",
            "call", export,
            "&&", "idf.py", "set-target", CHIP,
            "&&", "idf.py", "build",
        ], cwd=project, env=env)
        ok = (r.returncode == 0)
    else:
        export = os.path.join(idf_path, "export.sh")
        cmd = (
            f'source "{export}" > /dev/null 2>&1 && '
            f'idf.py set-target esp32c6 && '
            f'idf.py build'
        )
        ok = run_shell(cmd, cwd=project)

    return ok


def _find_idf_and_build():
    """Locate (or install) ESP-IDF and build the project. Returns build dir or calls fail()."""
    bd = get_build_dir()

    idf_path = find_idf_installation()

    if idf_path:
        color_print(f"  Found ESP-IDF: {idf_path}", C.GREEN)
    else:
        color_print("  ESP-IDF is not installed on this system.", C.YELLOW)
        color_print("  It's needed to compile the firmware (one-time setup).", C.DIM)
        print()

        if not ensure_git():
            fail("Cannot continue without Git.")

        idf_path = install_esp_idf()
        if not idf_path:
            fail("ESP-IDF installation failed.")

    if not ensure_idf_tools_installed(idf_path):
        fail("ESP-IDF tool installation failed.")

    section("Building firmware")
    if not build_with_idf(idf_path):
        fail("Build failed. Check the errors above.")

    if not firmware_ready():
        fail("Build seemed to succeed but firmware binaries are missing.")

    color_print("\n  Build succeeded!", C.GREEN)
    for _, name, rel in FLASH_MAP:
        sz = os.path.getsize(os.path.join(bd, rel)) / 1024
        color_print(f"    {name:28s}  {sz:>7.1f} kB", C.CYAN)
    return bd


def _download_release_binaries():
    """Download prebuilt firmware from the latest GitHub release. Returns build dir or None."""
    bd = get_build_dir()
    api_url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/tags/{GITHUB_RELEASE_TAG}"

    color_print(f"\n  Fetching release info from GitHub...", C.CYAN)
    try:
        req = Request(api_url, headers={"Accept": "application/vnd.github+json"})
        with urlopen(req, timeout=15) as resp:
            release = json.loads(resp.read())
    except (URLError, OSError, json.JSONDecodeError) as e:
        color_print(f"  Could not reach GitHub: {e}", C.RED)
        return None

    assets = {a["name"]: a["browser_download_url"] for a in release.get("assets", [])}
    needed = {name: url for _, name, _ in FLASH_MAP if (url := assets.get(name))}

    if len(needed) != len(FLASH_MAP):
        missing = [name for _, name, _ in FLASH_MAP if name not in assets]
        color_print(f"  Release is missing: {', '.join(missing)}", C.RED)
        return None

    # Ensure build dir structure exists
    for _, _, rel in FLASH_MAP:
        os.makedirs(os.path.join(bd, os.path.dirname(rel)), exist_ok=True)

    for _, name, rel in FLASH_MAP:
        url = needed[name]
        dest = os.path.join(bd, rel)
        color_print(f"  Downloading {name}...", C.YELLOW)
        try:
            with urlopen(url, timeout=30) as resp:
                data = resp.read()
            with open(dest, "wb") as f:
                f.write(data)
            sz = len(data) / 1024
            color_print(f"    {name:28s}  {sz:>7.1f} kB", C.CYAN)
        except (URLError, OSError) as e:
            color_print(f"  Download failed for {name}: {e}", C.RED)
            return None

    color_print("\n  Downloaded prebuilt firmware from GitHub!", C.GREEN)
    return bd


def ensure_firmware():
    """Make sure firmware binaries exist. Returns build dir."""
    bd = get_build_dir()

    local_ready = firmware_ready()

    if local_ready:
        color_print("  Firmware is already available locally.", C.GREEN)
        for _, name, rel in FLASH_MAP:
            sz = os.path.getsize(os.path.join(bd, rel)) / 1024
            color_print(f"    {name:28s}  {sz:>7.1f} kB", C.CYAN)
        print()
    else:
        color_print("  Firmware has not been built yet.\n", C.YELLOW)

    # CLI flags can skip the interactive prompt
    if os.environ.get("OUISPY_FORCE_BUILD"):
        return _find_idf_and_build()
    if os.environ.get("OUISPY_FORCE_DOWNLOAD"):
        result = _download_release_binaries()
        if result and firmware_ready():
            return result
        color_print("\n  Download failed — falling back to local build.", C.YELLOW)
        return _find_idf_and_build()

    color_print("  Firmware source options:\n", C.BOLD)
    if local_ready:
        color_print("    [1] Use local firmware already in ./build", C.CYAN)
        color_print("    [2] Download latest prebuilt release from GitHub", C.CYAN)
        color_print("    [3] Build from local source code (ESP-IDF)\n", C.CYAN)
        choice = input(f"  {C.BOLD}Select [1/2/3] (default: 2): {C.RESET}").strip()

        if choice == "1":
            return bd
        if choice == "3":
            return _find_idf_and_build()

        result = _download_release_binaries()
        if result and firmware_ready():
            return result

        color_print("\n  Download failed — keeping local firmware.", C.YELLOW)
        if ask_yes("  Build from source instead?", default_yes=False):
            return _find_idf_and_build()
        return bd

    color_print("    [1] Download latest prebuilt release from GitHub (fast, no tools needed)", C.CYAN)
    color_print("    [2] Build from local source code (requires ESP-IDF)\n", C.CYAN)

    choice = input(f"  {C.BOLD}Select [1/2] (default: 1): {C.RESET}").strip()
    if choice == "2":
        return _find_idf_and_build()

    # Try download; fall back to build on failure
    result = _download_release_binaries()
    if result and firmware_ready():
        return result

    color_print("\n  Download failed — falling back to local build.", C.YELLOW)
    return _find_idf_and_build()


# ═════════════════════════════════════════════
#  SERIAL PORT
# ═════════════════════════════════════════════

def find_serial_ports():
    ports = []
    if sys.platform == "win32":
        import serial.tools.list_ports
        for p in serial.tools.list_ports.comports():
            desc = (p.description or "").lower()
            vid = p.vid or 0
            if any(k in desc for k in ["cp210", "ch340", "ch9102", "usb serial",
                                        "silicon labs", "uart bridge", "esp"]):
                ports.append((p.device, p.description))
            elif vid in (0x1A86, 0x10C4, 0x303A, 0x0403):
                ports.append((p.device, p.description))
    else:
        patterns = ["/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usbserial*",
                    "/dev/cu.usbmodem*", "/dev/cu.SLAB*"]
        for pat in patterns:
            for p in glob.glob(pat):
                ports.append((p, ""))
    return ports

def select_port(specified=None):
    if specified:
        color_print(f"  Using specified port: {specified}", C.GREEN)
        return specified

    ports = find_serial_ports()

    if not ports:
        color_print("  No serial ports detected.", C.YELLOW)
        print()
        color_print("  Make sure:", C.CYAN)
        color_print("    - The board is plugged in with a USB data cable", C.CYAN)
        color_print("    - USB drivers are installed (CH340 / CP2102)", C.CYAN)
        color_print("    - Try: hold BOOT, press RESET, then release both", C.CYAN)
        print()
        port = input(f"  {C.BOLD}Enter port manually (e.g. COM3): {C.RESET}").strip()
        if port:
            return port
        fail("No serial port specified.")

    if len(ports) == 1:
        name, desc = ports[0]
        color_print(f"  Found: {name}  ({desc})", C.GREEN)
        return name

    color_print("  Multiple ports found:\n", C.YELLOW)
    for i, (name, desc) in enumerate(ports):
        print(f"    [{i + 1}] {name}  — {desc}")
    print()
    while True:
        choice = input(f"  {C.BOLD}Select port [1-{len(ports)}]: {C.RESET}").strip()
        try:
            idx = int(choice) - 1
            if 0 <= idx < len(ports):
                return ports[idx][0]
        except ValueError:
            pass
        color_print("  Invalid choice.", C.RED)


# ═════════════════════════════════════════════
#  FLASH
# ═════════════════════════════════════════════

def flash_firmware(port, build_dir, erase=False):
    import esptool

    args = ["--chip", CHIP, "--port", port, "--baud", str(BAUD)]

    if erase:
        color_print("\n  Erasing flash first...", C.YELLOW)
        try:
            esptool.main(args + ["erase_flash"])
            color_print("  Erase OK.", C.GREEN)
        except Exception as e:
            color_print(f"  Erase failed: {e}", C.RED)
            return False
        time.sleep(1)

    flash_args = args + [
        "write_flash",
        "--flash_mode", FLASH_MODE,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", FLASH_SIZE,
    ]

    for offset, name, rel in FLASH_MAP:
        full = os.path.join(build_dir, rel)
        if not os.path.isfile(full):
            color_print(f"  Cannot find {name} — skipping", C.YELLOW)
            continue
        size_kb = os.path.getsize(full) / 1024
        color_print(f"    0x{offset:05X}  {name:28s}  ({size_kb:.1f} kB)", C.CYAN)
        flash_args.extend([f"0x{offset:x}", full])

    color_print("\n  Writing to flash...", C.YELLOW)
    color_print("  (If it hangs: hold BOOT, press RESET, then release both)\n", C.DIM)

    try:
        esptool.main(flash_args)
    except Exception as e:
        color_print(f"\n  Flash failed: {e}", C.RED)
        print()
        color_print("  Troubleshooting:", C.YELLOW)
        color_print("    1. Hold BOOT, press RESET, release RESET, release BOOT", C.CYAN)
        color_print("    2. Try a different USB cable (must be data, not charge-only)", C.CYAN)
        color_print("    3. Check that the correct port is selected", C.CYAN)
        color_print("    4. Install the CH340 / CP2102 USB driver for your OS", C.CYAN)
        return False

    return True


# ═════════════════════════════════════════════
#  MAIN WIZARD
# ═════════════════════════════════════════════

def main():
    global BAUD, MONITOR_BAUD

    # Parse optional flags
    specified_port = None
    erase = False
    monitor = True
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ("--port", "-p") and i + 1 < len(args):
            specified_port = args[i + 1]; i += 2; continue
        if args[i] in ("--baud",) and i + 1 < len(args):
            BAUD = int(args[i + 1]); i += 2; continue
        if args[i] in ("--monitor-baud",) and i + 1 < len(args):
            MONITOR_BAUD = int(args[i + 1]); i += 2; continue
        if args[i] in ("--erase", "-e"):
            erase = True; i += 1; continue
        if args[i] == "--no-monitor":
            monitor = False; i += 1; continue
        if args[i] == "--build":
            os.environ["OUISPY_FORCE_BUILD"] = "1"; i += 1; continue
        if args[i] == "--download":
            os.environ["OUISPY_FORCE_DOWNLOAD"] = "1"; i += 1; continue
        if args[i] in ("--help", "-h"):
            print("Usage: python flash.py [--port PORT] [--erase] [--baud RATE] [--monitor-baud RATE] [--no-monitor] [--download] [--build]")
            print("\n  No arguments needed — the script handles everything automatically.")
            print("  --download   Skip menu, download prebuilt firmware from GitHub")
            print("  --build      Skip menu, build from local source code")
            print("  It will flash and then open a serial monitor by default.\n")
            sys.exit(0)
        i += 1

    color_print(BANNER, C.PURPLE)

    # ── 1. Python dependencies ──
    section("Checking Python dependencies")
    if not ensure_pip_package("esptool"):
        fail("Cannot install esptool.")
    color_print("  esptool: OK", C.GREEN)
    if not ensure_pip_package("pyserial", "serial"):
        fail("Cannot install pyserial.")
    color_print("  pyserial: OK", C.GREEN)

    # ── 2. Firmware (find IDF / install IDF / build — all automatic) ──
    section("Checking firmware")
    build_dir = ensure_firmware()

    # ── 3. Serial port ──
    section("Detecting board")
    port = select_port(specified_port)

    # ── 4. Flash ──
    section("Flashing firmware")
    color_print(f"  Chip:  {CHIP}", C.CYAN)
    color_print(f"  Port:  {port}", C.CYAN)
    color_print(f"  Baud:  {BAUD}", C.CYAN)
    color_print(f"  Erase: {'Yes' if erase else 'No'}", C.CYAN)
    color_print(f"  Monitor after flash: {'Yes' if monitor else 'No'}", C.CYAN)
    if monitor:
        color_print(f"  Monitor baud: {MONITOR_BAUD}", C.CYAN)
    print()

    if not ask_yes("  Ready — flash now?"):
        color_print("  Aborted.", C.YELLOW)
        input(f"\n{C.DIM}  Press Enter to exit...{C.RESET}")
        sys.exit(0)

    if not flash_firmware(port, build_dir, erase=erase):
        fail("Flashing failed. See troubleshooting above.")

    color_print("\n  Firmware flashed successfully!", C.GREEN)

    if monitor:
        time.sleep(1.0)
        monitor_serial(port, MONITOR_BAUD)

    # ── 5. Done ──
    section("Done!")
    print()
    color_print("  Next steps:", C.BOLD)
    color_print("    1. Connect to WiFi shown on device/web:", C.CYAN)
    color_print("       - UniSpy-C6 / ouispy123 (if Single AP Name is ON)", C.CYAN)
    color_print("       - or mode AP (flockyou-c6 / foxhunt-c6 / skyspy-c6)", C.CYAN)
    color_print("    2. Open in browser:  https://192.168.4.1  (or http://192.168.4.1)", C.CYAN)
    color_print("    3. If it still crashes, check the latest file in ./logs/", C.CYAN)
    print()
    color_print("  Button controls:", C.BOLD)
    color_print("    Single click  = next item", C.CYAN)
    color_print("    Double click  = previous item", C.CYAN)
    color_print("    Triple click  = back / cancel (mode-specific)", C.CYAN)
    color_print("    Hold ~0.5 sec = select / activate", C.CYAN)
    color_print("    Hold ~3.5 sec = warning flash", C.CYAN)
    color_print("    Hold ~5 sec   = back to mode select", C.CYAN)
    color_print("    Flock mode: 5 rapid clicks = jump to Fox Hunter and track detected camera", C.CYAN)
    input(f"\n{C.DIM}  Press Enter to exit...{C.RESET}")


if __name__ == "__main__":
    main()
