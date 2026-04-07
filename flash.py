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
FIRMWARE_INFO_FILE = ".ouispy-firmware.json"

BOARD_NON_TOUCH = {
    "id": "lcd-1.47",
    "name": "Waveshare ESP32-C6-LCD-1.47",
    "short": "non-touch ST7789",
    "board_touch": 0,
    "supports_release": True,
}

BOARD_TOUCH = {
    "id": "touch-lcd-1.47",
    "name": "Waveshare ESP32-C6-Touch-LCD-1.47",
    "short": "touch JD9853",
    "board_touch": 1,
    "supports_release": False,
}

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

# ── Map tile defaults ──
MAP_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
MAP_USER_AGENT = "OUI-Spy-C6-Flasher/1.0 (tile prep; https://github.com/hipstereclipse/ouispy-c6)"
MAP_DEFAULT_ZOOM_MIN = 9
MAP_DEFAULT_ZOOM_MAX = 13
MAP_TILE_SIZE = 256
MAP_MAX_TILES = 50000  # safety limit

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

def get_firmware_info_path(build_dir=None):
    return os.path.join(build_dir or get_build_dir(), FIRMWARE_INFO_FILE)

def get_monitor_log_dir():
    return os.path.join(get_project_dir(), "logs")

def firmware_ready():
    bd = get_build_dir()
    return all(os.path.isfile(os.path.join(bd, rel)) for _, _, rel in FLASH_MAP)

def _read_json_file(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None

def _write_json_file(path, payload):
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")
        return True
    except OSError:
        return False

def _get_local_git_description():
    if not shutil.which("git"):
        return None
    try:
        r = subprocess.run(
            ["git", "-C", get_project_dir(), "describe", "--tags", "--always", "--dirty"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        text = (r.stdout or "").strip()
        return text or None
    except OSError:
        return None

def _get_build_project_description(build_dir):
    return _read_json_file(os.path.join(build_dir, "project_description.json")) or {}

def _load_firmware_info(build_dir):
    info = _read_json_file(get_firmware_info_path(build_dir)) or {}
    project = _get_build_project_description(build_dir)

    if project.get("project_version") and not info.get("version"):
        info["version"] = str(project["project_version"])
    if project.get("project_name") and not info.get("project_name"):
        info["project_name"] = str(project["project_name"])
    if project.get("target") and not info.get("target"):
        info["target"] = str(project["target"])

    if not info.get("git"):
        git_desc = _get_local_git_description()
        if git_desc:
            info["git"] = git_desc

    if not info.get("source"):
        info["source"] = "local files"

    return info

def _save_firmware_info(build_dir, info):
    payload = dict(info or {})
    payload.setdefault("saved_at", datetime.now().isoformat(timespec="seconds"))
    _write_json_file(get_firmware_info_path(build_dir), payload)

def _firmware_option_suffix(info):
    version = (info or {}).get("version")
    release_tag = (info or {}).get("release_tag")
    source = (info or {}).get("source")
    bits = []
    if version:
        bits.append(f"v{version}")
    elif release_tag:
        bits.append(str(release_tag))
    if source and source != "local files":
        bits.append(str(source))
    return f" ({', '.join(bits)})" if bits else ""

def board_touch_value(board_variant):
    return int((board_variant or BOARD_NON_TOUCH).get("board_touch", 0))

def board_variant_name(board_variant):
    return (board_variant or BOARD_NON_TOUCH).get("name", BOARD_NON_TOUCH["name"])

def board_variant_from_info(info):
    if str((info or {}).get("board_touch", "")) == "1":
        return BOARD_TOUCH
    return BOARD_NON_TOUCH

def prompt_board_variant(default_variant=None):
    default_variant = default_variant or BOARD_NON_TOUCH

    section("Board variant")
    color_print("    [1] Waveshare ESP32-C6-LCD-1.47        (original, ST7789)", C.CYAN)
    color_print("    [2] Waveshare ESP32-C6-Touch-LCD-1.47  (new variant, JD9853)\n", C.CYAN)

    default_choice = "2" if board_touch_value(default_variant) else "1"
    while True:
        choice = input(f"  {C.BOLD}Select [1/2] (default: {default_choice}): {C.RESET}").strip()
        if not choice:
            choice = default_choice
        if choice == "1":
            return BOARD_NON_TOUCH
        if choice == "2":
            return BOARD_TOUCH
        color_print("  Please choose 1 or 2.", C.YELLOW)

def print_firmware_info(info, heading="Firmware selected"):
    info = info or {}
    section(heading)
    version = info.get("version") or "unknown"
    color_print(f"  Version: {version}", C.CYAN)
    if info.get("source"):
        color_print(f"  Source:  {info['source']}", C.CYAN)
    if info.get("release_tag"):
        color_print(f"  Release: {info['release_tag']}", C.CYAN)
    if info.get("git"):
        color_print(f"  Git:     {info['git']}", C.CYAN)
    if info.get("target"):
        color_print(f"  Target:  {info['target']}", C.CYAN)
    color_print(f"  Board:   {board_variant_name(board_variant_from_info(info))}", C.CYAN)
    if info.get("published_at"):
        color_print(f"  Published: {info['published_at']}", C.CYAN)
    print()

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
            # Important for ESP32-C6 USB-Serial/JTAG on Windows: opening the
            # port with pyserial defaults can assert DTR/RTS momentarily and
            # reset the target on every reconnect. Configure the line states
            # before opening the port so reconnects do not trigger a reboot.
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = baudrate
            ser.timeout = 0.25
            ser.write_timeout = 0.25
            ser.dsrdtr = False
            ser.rtscts = False
            ser.xonxoff = False
            ser.dtr = False
            ser.rts = False
            ser.open()
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

def build_with_idf(idf_path, board_variant):
    """Activate ESP-IDF environment and build the project. Returns True on success."""
    project = get_project_dir()
    build_dir = get_build_dir()
    board_touch = str(board_touch_value(board_variant))

    # idf.py set-target triggers fullclean first. If a stale non-CMake build
    # directory exists, fullclean refuses to proceed and the build aborts.
    if os.path.isdir(build_dir) and not os.path.isfile(os.path.join(build_dir, "CMakeCache.txt")):
        color_print(f"  Removing stale build directory: {build_dir}", C.DIM)
        shutil.rmtree(build_dir, ignore_errors=True)

    color_print(f"\n  ESP-IDF path: {idf_path}", C.DIM)
    color_print(f"  Project:      {project}", C.DIM)
    color_print(f"  Board:        {board_variant_name(board_variant)}", C.DIM)
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
            "&&", "idf.py", f"-DBOARD_TOUCH={board_touch}", "build",
        ], cwd=project, env=env)
        ok = (r.returncode == 0)
    else:
        export = os.path.join(idf_path, "export.sh")
        cmd = (
            f'source "{export}" > /dev/null 2>&1 && '
            f'idf.py set-target esp32c6 && '
            f'idf.py -DBOARD_TOUCH={board_touch} build'
        )
        ok = run_shell(cmd, cwd=project)

    return ok


def _find_idf_and_build(board_variant):
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
    if not build_with_idf(idf_path, board_variant):
        fail("Build failed. Check the errors above.")

    if not firmware_ready():
        fail("Build seemed to succeed but firmware binaries are missing.")

    color_print("\n  Build succeeded!", C.GREEN)
    for _, name, rel in FLASH_MAP:
        sz = os.path.getsize(os.path.join(bd, rel)) / 1024
        color_print(f"    {name:28s}  {sz:>7.1f} kB", C.CYAN)

    info = _load_firmware_info(bd)
    info.update({
        "source": "local source build",
        "git": _get_local_git_description() or info.get("git"),
        "board": board_variant_name(board_variant),
        "board_id": board_variant["id"],
        "board_touch": board_touch_value(board_variant),
        "built_at": datetime.now().isoformat(timespec="seconds"),
    })
    _save_firmware_info(bd, info)
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

    info = {
        "source": "GitHub release",
        "release_tag": release.get("tag_name") or GITHUB_RELEASE_TAG,
        "release_name": release.get("name") or "",
        "version": str(release.get("tag_name") or "").lstrip("vV") or None,
        "published_at": release.get("published_at") or "",
    }
    _save_firmware_info(bd, info)
    return bd


def ensure_firmware(board_variant):
    """Make sure firmware binaries exist. Returns build dir."""
    bd = get_build_dir()

    local_ready = firmware_ready()
    local_info = _load_firmware_info(bd) if local_ready else {}
    local_matches_board = local_ready and board_touch_value(board_variant_from_info(local_info)) == board_touch_value(board_variant)

    if local_ready:
        color_print("  Firmware is already available locally.", C.GREEN)
        for _, name, rel in FLASH_MAP:
            sz = os.path.getsize(os.path.join(bd, rel)) / 1024
            color_print(f"    {name:28s}  {sz:>7.1f} kB", C.CYAN)
        if local_info.get("version") or local_info.get("release_tag"):
            color_print(f"  Detected firmware{_firmware_option_suffix(local_info)}", C.GREEN)
        color_print(f"  Built for: {board_variant_name(board_variant_from_info(local_info))}", C.CYAN)
        if not local_matches_board:
            color_print(f"  Local firmware does not match selected board: {board_variant_name(board_variant)}", C.YELLOW)
        print()
    else:
        color_print("  Firmware has not been built yet.\n", C.YELLOW)

    # CLI flags can skip the interactive prompt
    if os.environ.get("OUISPY_FORCE_BUILD"):
        return _find_idf_and_build(board_variant)
    if os.environ.get("OUISPY_FORCE_DOWNLOAD"):
        if not board_variant.get("supports_release", False):
            color_print("\n  Prebuilt GitHub releases are currently only available for the original non-touch board.", C.YELLOW)
            return _find_idf_and_build(board_variant)
        result = _download_release_binaries()
        if result and firmware_ready():
            info = _load_firmware_info(result)
            info.update({
                "board": board_variant_name(board_variant),
                "board_id": board_variant["id"],
                "board_touch": board_touch_value(board_variant),
            })
            _save_firmware_info(result, info)
            return result
        color_print("\n  Download failed — falling back to local build.", C.YELLOW)
        return _find_idf_and_build(board_variant)

    color_print("  Firmware source options:\n", C.BOLD)
    if local_ready and local_matches_board:
        color_print(f"    [1] Use local firmware already in ./build{_firmware_option_suffix(local_info)}", C.CYAN)
        if board_variant.get("supports_release", False):
            color_print("    [2] Download latest prebuilt release from GitHub", C.CYAN)
        else:
            color_print("    [2] Download latest prebuilt release from GitHub (not available for touch board)", C.DIM)
        color_print("    [3] Build from local source code (ESP-IDF)\n", C.CYAN)
        choice = input(f"  {C.BOLD}Select [1/2/3] (default: 2): {C.RESET}").strip()

        if choice == "1":
            return bd
        if choice == "3":
            return _find_idf_and_build(board_variant)

        if not board_variant.get("supports_release", False):
            color_print("\n  Touch-board prebuilt releases are not published yet — building locally instead.", C.YELLOW)
            return _find_idf_and_build(board_variant)

        result = _download_release_binaries()
        if result and firmware_ready():
            info = _load_firmware_info(result)
            info.update({
                "board": board_variant_name(board_variant),
                "board_id": board_variant["id"],
                "board_touch": board_touch_value(board_variant),
            })
            _save_firmware_info(result, info)
            return result

        color_print("\n  Download failed — keeping local firmware.", C.YELLOW)
        if ask_yes("  Build from source instead?", default_yes=False):
            return _find_idf_and_build(board_variant)
        return bd

    if local_ready and not local_matches_board:
        color_print(f"    [1] Build firmware for {board_variant_name(board_variant)} (recommended)", C.CYAN)
        if board_variant.get("supports_release", False):
            color_print("    [2] Download latest prebuilt release from GitHub", C.CYAN)
            color_print("    [3] Keep local firmware anyway (wrong board, not recommended)\n", C.YELLOW)
            choice = input(f"  {C.BOLD}Select [1/2/3] (default: 1): {C.RESET}").strip()
            if choice == "2":
                result = _download_release_binaries()
                if result and firmware_ready():
                    info = _load_firmware_info(result)
                    info.update({
                        "board": board_variant_name(board_variant),
                        "board_id": board_variant["id"],
                        "board_touch": board_touch_value(board_variant),
                    })
                    _save_firmware_info(result, info)
                    return result
                color_print("\n  Download failed — building locally instead.", C.YELLOW)
                return _find_idf_and_build(board_variant)
            if choice == "3":
                return bd
            return _find_idf_and_build(board_variant)

        color_print("    [2] Keep local firmware anyway (wrong board, not recommended)\n", C.YELLOW)
        choice = input(f"  {C.BOLD}Select [1/2] (default: 1): {C.RESET}").strip()
        if choice == "2":
            return bd
        return _find_idf_and_build(board_variant)

    if board_variant.get("supports_release", False):
        color_print("    [1] Download latest prebuilt release from GitHub (fast, no tools needed)", C.CYAN)
        color_print("    [2] Build from local source code (requires ESP-IDF)\n", C.CYAN)

        choice = input(f"  {C.BOLD}Select [1/2] (default: 1): {C.RESET}").strip()
        if choice == "2":
            return _find_idf_and_build(board_variant)

        result = _download_release_binaries()
        if result and firmware_ready():
            info = _load_firmware_info(result)
            info.update({
                "board": board_variant_name(board_variant),
                "board_id": board_variant["id"],
                "board_touch": board_touch_value(board_variant),
            })
            _save_firmware_info(result, info)
            return result

        color_print("\n  Download failed — falling back to local build.", C.YELLOW)
        return _find_idf_and_build(board_variant)

    color_print("  Touch-board prebuilt releases are not published yet.", C.YELLOW)
    return _find_idf_and_build(board_variant)


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

    base_args = ["--chip", CHIP, "--port", port]

    # Build write list once; each retry profile reuses the same map.
    write_pairs = []

    for offset, name, rel in FLASH_MAP:
        full = os.path.join(build_dir, rel)
        if not os.path.isfile(full):
            color_print(f"  Cannot find {name} — skipping", C.YELLOW)
            continue
        size_kb = os.path.getsize(full) / 1024
        color_print(f"    0x{offset:05X}  {name:28s}  ({size_kb:.1f} kB)", C.CYAN)
        write_pairs.extend([f"0x{offset:x}", full])

    if not write_pairs:
        color_print("\n  No firmware segments available to flash.", C.RED)
        return False

    profiles = [
        {"name": "default reset", "before": "default-reset", "after": "hard-reset", "baud": BAUD},
        {"name": "USB reset",     "before": "usb-reset",     "after": "hard-reset", "baud": BAUD},
        {"name": "safe baud",     "before": "default-reset", "after": "hard-reset", "baud": 115200},
    ]

    if erase:
        color_print("\n  Erasing flash first...", C.YELLOW)
        erased = False
        erase_err = None
        for prof in profiles:
            try:
                erase_args = base_args + [
                    "--before", prof["before"],
                    "--after", prof["after"],
                    "--baud", str(prof["baud"]),
                    "erase-flash",
                ]
                color_print(f"  Erase attempt ({prof['name']} @ {prof['baud']})...", C.DIM)
                esptool.main(erase_args)
                erased = True
                break
            except Exception as e:
                erase_err = e
                continue

        if not erased:
            color_print(f"  Erase failed: {erase_err}", C.RED)
            return False

        color_print("  Erase OK.", C.GREEN)
        time.sleep(1)

    color_print("\n  Writing to flash...", C.YELLOW)
    color_print("  (If it hangs: hold BOOT, press RESET, then release both)\n", C.DIM)

    last_error = None
    for i, prof in enumerate(profiles, start=1):
        flash_args = base_args + [
            "--before", prof["before"],
            "--after", prof["after"],
            "--baud", str(prof["baud"]),
            "write-flash",
            "--flash-mode", FLASH_MODE,
            "--flash-freq", FLASH_FREQ,
            "--flash-size", FLASH_SIZE,
        ] + write_pairs

        color_print(f"  Flash attempt {i}/{len(profiles)} ({prof['name']} @ {prof['baud']})...", C.DIM)
        try:
            esptool.main(flash_args)
            return True
        except Exception as e:
            last_error = e
            msg = str(e)
            color_print(f"  Attempt failed: {msg}", C.YELLOW)
            if "Failed to connect" in msg or "No serial data" in msg:
                color_print("  Tip: hold BOOT, tap RESET, release RESET, then release BOOT.", C.CYAN)
            if i < len(profiles):
                time.sleep(1.0)

    color_print(f"\n  Flash failed: {last_error}", C.RED)
    print()
    color_print("  Troubleshooting:", C.YELLOW)
    color_print("    1. Hold BOOT, press RESET, release RESET, release BOOT", C.CYAN)
    color_print("    2. Try a different USB cable (must be data, not charge-only)", C.CYAN)
    color_print("    3. Check that the correct port is selected", C.CYAN)
    color_print("    4. Install the CH340 / CP2102 USB driver for your OS", C.CYAN)
    color_print("    5. Re-run with --baud 115200 for noisy USB links", C.CYAN)
    return False


# ═════════════════════════════════════════════
#  MICROSD MAP TILE PREPARATION
# ═════════════════════════════════════════════

# Region catalog: (name, bbox=(min_lat, max_lat, min_lon, max_lon), regions)
# Each region has (name, bbox, subdivisions) where subdivisions are (name, bbox).
REGION_CATALOG = [
    {
        "name": "United States",
        "bbox": (24.4, 49.4, -125.0, -66.9),
        "regions": [
            {
                "name": "Northeast",
                "bbox": (38.9, 47.5, -80.5, -66.9),
                "subdivisions": [
                    ("Connecticut",    (40.95, 42.05, -73.73, -71.79)),
                    ("Delaware",       (38.45, 39.84, -75.79, -75.05)),
                    ("Maine",          (43.06, 47.46, -71.08, -66.95)),
                    ("Maryland",       (37.91, 39.72, -79.49, -75.05)),
                    ("Massachusetts",  (41.24, 42.89, -73.51, -69.93)),
                    ("New Hampshire",  (42.70, 45.31, -72.56, -70.70)),
                    ("New Jersey",     (38.93, 41.36, -75.56, -73.89)),
                    ("New York",       (40.50, 45.01, -79.76, -71.86)),
                    ("Pennsylvania",   (39.72, 42.27, -80.52, -74.69)),
                    ("Rhode Island",   (41.15, 42.02, -71.86, -71.12)),
                    ("Vermont",        (42.73, 45.02, -73.44, -71.46)),
                ],
            },
            {
                "name": "Southeast",
                "bbox": (24.4, 39.5, -91.7, -75.4),
                "subdivisions": [
                    ("Alabama",        (30.22, 35.01, -88.47, -84.89)),
                    ("Arkansas",       (33.00, 36.50, -94.62, -89.64)),
                    ("Florida",        (24.40, 31.00, -87.63, -80.03)),
                    ("Georgia",        (30.36, 35.00, -85.61, -80.84)),
                    ("Kentucky",       (36.50, 39.15, -89.57, -81.96)),
                    ("Louisiana",      (28.93, 33.02, -94.04, -89.00)),
                    ("Mississippi",    (30.17, 34.99, -91.66, -88.10)),
                    ("North Carolina", (33.84, 36.59, -84.32, -75.46)),
                    ("South Carolina", (32.05, 35.21, -83.35, -78.54)),
                    ("Tennessee",      (34.98, 36.68, -90.31, -81.65)),
                    ("Virginia",       (36.54, 39.47, -83.68, -75.24)),
                    ("West Virginia",  (37.20, 40.64, -82.64, -77.72)),
                ],
            },
            {
                "name": "Midwest",
                "bbox": (36.0, 49.4, -104.1, -80.5),
                "subdivisions": [
                    ("Illinois",       (36.97, 42.51, -91.51, -87.50)),
                    ("Indiana",        (37.77, 41.76, -88.10, -84.78)),
                    ("Iowa",           (40.38, 43.50, -96.64, -90.14)),
                    ("Kansas",         (37.00, 40.00, -102.05, -94.59)),
                    ("Michigan",       (41.70, 48.26, -90.42, -82.12)),
                    ("Minnesota",      (43.50, 49.38, -97.24, -89.49)),
                    ("Missouri",       (36.00, 40.61, -95.77, -89.10)),
                    ("Nebraska",       (40.00, 43.00, -104.05, -95.31)),
                    ("North Dakota",   (45.94, 49.00, -104.05, -96.55)),
                    ("Ohio",           (38.40, 41.98, -84.82, -80.52)),
                    ("South Dakota",   (42.48, 45.94, -104.06, -96.44)),
                    ("Wisconsin",      (42.49, 47.08, -92.89, -86.25)),
                ],
            },
            {
                "name": "Southwest",
                "bbox": (25.8, 37.0, -114.8, -93.5),
                "subdivisions": [
                    ("Arizona",        (31.33, 37.00, -114.81, -109.04)),
                    ("New Mexico",     (31.33, 37.00, -109.05, -103.00)),
                    ("Oklahoma",       (33.62, 37.00, -103.00, -94.43)),
                    ("Texas",          (25.84, 36.50, -106.65, -93.51)),
                ],
            },
            {
                "name": "Mountain West",
                "bbox": (37.0, 49.0, -117.0, -102.0),
                "subdivisions": [
                    ("Colorado",       (36.99, 41.00, -109.06, -102.04)),
                    ("Idaho",          (42.00, 49.00, -117.24, -111.04)),
                    ("Montana",        (44.36, 49.00, -116.05, -104.04)),
                    ("Nevada",         (35.00, 42.00, -120.00, -114.04)),
                    ("Utah",           (37.00, 42.00, -114.05, -109.04)),
                    ("Wyoming",        (41.00, 45.00, -111.06, -104.05)),
                ],
            },
            {
                "name": "Pacific West",
                "bbox": (32.5, 49.0, -124.8, -114.1),
                "subdivisions": [
                    ("California",     (32.53, 42.01, -124.48, -114.13)),
                    ("Oregon",         (41.99, 46.29, -124.57, -116.46)),
                    ("Washington",     (45.54, 49.00, -124.85, -116.92)),
                ],
            },
            {
                "name": "Non-Contiguous",
                "bbox": (18.9, 71.4, -179.2, -129.9),
                "subdivisions": [
                    ("Alaska",         (51.21, 71.39, -179.15, -129.98)),
                    ("Hawaii",         (18.91, 22.24, -160.24, -154.81)),
                ],
            },
        ],
    },
    {
        "name": "Canada",
        "bbox": (41.7, 83.1, -141.0, -52.6),
        "regions": [
            {
                "name": "Western Canada",
                "bbox": (48.3, 60.0, -139.1, -110.0),
                "subdivisions": [
                    ("British Columbia",  (48.30, 60.00, -139.06, -114.04)),
                    ("Alberta",           (49.00, 60.00, -120.00, -110.00)),
                ],
            },
            {
                "name": "Prairies",
                "bbox": (49.0, 60.0, -110.0, -88.9),
                "subdivisions": [
                    ("Saskatchewan",      (49.00, 60.00, -110.00, -101.36)),
                    ("Manitoba",          (49.00, 60.00, -102.00, -88.90)),
                ],
            },
            {
                "name": "Central Canada",
                "bbox": (42.0, 62.6, -95.2, -57.1),
                "subdivisions": [
                    ("Ontario",           (42.00, 56.90, -95.16, -74.34)),
                    ("Quebec",            (45.00, 62.59, -79.76, -57.10)),
                ],
            },
            {
                "name": "Atlantic Canada",
                "bbox": (43.4, 60.4, -69.1, -52.6),
                "subdivisions": [
                    ("New Brunswick",          (44.60, 48.07, -69.06, -63.77)),
                    ("Newfoundland & Labrador",(46.62, 60.37, -67.80, -52.62)),
                    ("Nova Scotia",            (43.42, 47.03, -66.42, -59.68)),
                    ("Prince Edward Island",   (45.95, 47.07, -64.42, -61.98)),
                ],
            },
            {
                "name": "Northern Canada",
                "bbox": (60.0, 83.1, -141.0, -61.1),
                "subdivisions": [
                    ("Northwest Territories", (60.00, 78.76, -136.45, -101.98)),
                    ("Nunavut",               (51.68, 83.11, -120.40, -61.08)),
                    ("Yukon",                 (60.00, 69.65, -141.00, -123.82)),
                ],
            },
        ],
    },
    {
        "name": "United Kingdom",
        "bbox": (49.9, 60.9, -8.7, 1.8),
        "regions": [
            {"name": "England",          "bbox": (49.9, 55.8, -5.7, 1.8),   "subdivisions": []},
            {"name": "Scotland",         "bbox": (54.6, 60.9, -8.7, -0.7),  "subdivisions": []},
            {"name": "Wales",            "bbox": (51.4, 53.4, -5.3, -2.7),  "subdivisions": []},
            {"name": "Northern Ireland", "bbox": (54.0, 55.4, -8.2, -5.4),  "subdivisions": []},
        ],
    },
    {
        "name": "Germany",
        "bbox": (47.3, 55.1, 5.9, 15.0),
        "regions": [
            {"name": "Northern Germany",  "bbox": (52.0, 55.1, 5.9, 15.0), "subdivisions": []},
            {"name": "Western Germany",   "bbox": (49.0, 52.5, 5.9, 10.0), "subdivisions": []},
            {"name": "Eastern Germany",   "bbox": (50.2, 54.7, 10.0, 15.0),"subdivisions": []},
            {"name": "Southern Germany",  "bbox": (47.3, 50.5, 7.5, 13.8), "subdivisions": []},
        ],
    },
    {
        "name": "France",
        "bbox": (41.3, 51.1, -5.1, 9.6),
        "regions": [
            {"name": "Northern France",  "bbox": (47.5, 51.1, -5.1, 9.6),  "subdivisions": []},
            {"name": "Southern France",  "bbox": (41.3, 47.5, -1.8, 9.6),  "subdivisions": []},
            {"name": "Western France",   "bbox": (43.0, 48.9, -5.1, 0.0),  "subdivisions": []},
        ],
    },
    {
        "name": "Australia",
        "bbox": (-43.6, -10.7, 113.2, 153.6),
        "regions": [
            {
                "name": "Eastern Australia",
                "bbox": (-43.6, -10.7, 138.0, 153.6),
                "subdivisions": [
                    ("New South Wales",     (-37.51, -28.16, 141.00, 153.64)),
                    ("Victoria",            (-39.16, -34.00, 141.00, 149.98)),
                    ("Queensland",          (-29.18, -10.69, 138.00, 153.55)),
                    ("Tasmania",            (-43.64, -39.57, 143.83, 148.51)),
                    ("ACT",                 (-35.92, -35.12, 148.76, 149.40)),
                ],
            },
            {
                "name": "Central & Western Australia",
                "bbox": (-38.1, -10.9, 112.9, 141.0),
                "subdivisions": [
                    ("South Australia",     (-38.06, -26.00, 129.00, 141.00)),
                    ("Western Australia",   (-35.13, -13.69, 112.92, 129.00)),
                    ("Northern Territory",  (-26.00, -10.97, 129.00, 138.00)),
                ],
            },
        ],
    },
    {
        "name": "Japan",
        "bbox": (24.0, 45.5, 122.9, 153.9),
        "regions": [
            {"name": "Hokkaido",       "bbox": (41.3, 45.5, 139.3, 145.8),  "subdivisions": []},
            {"name": "Tohoku",         "bbox": (36.8, 41.6, 139.0, 142.1),  "subdivisions": []},
            {"name": "Kanto",          "bbox": (35.0, 37.0, 138.7, 140.9),  "subdivisions": []},
            {"name": "Chubu",          "bbox": (34.6, 37.8, 136.0, 140.0),  "subdivisions": []},
            {"name": "Kansai",         "bbox": (33.4, 35.8, 134.0, 136.9),  "subdivisions": []},
            {"name": "Chugoku",        "bbox": (33.7, 35.7, 130.9, 134.5),  "subdivisions": []},
            {"name": "Shikoku",        "bbox": (32.7, 34.5, 132.0, 134.8),  "subdivisions": []},
            {"name": "Kyushu/Okinawa", "bbox": (24.0, 34.3, 122.9, 132.0),  "subdivisions": []},
        ],
    },
    {
        "name": "South Korea",
        "bbox": (33.1, 38.6, 124.6, 131.9),
        "regions": [],
    },
    {
        "name": "Italy",
        "bbox": (36.6, 47.1, 6.6, 18.5),
        "regions": [
            {"name": "Northern Italy",  "bbox": (43.5, 47.1, 6.6, 14.0),  "subdivisions": []},
            {"name": "Central Italy",   "bbox": (40.8, 43.5, 9.5, 15.0),  "subdivisions": []},
            {"name": "Southern Italy",  "bbox": (36.6, 41.5, 13.5, 18.5), "subdivisions": []},
        ],
    },
    {
        "name": "Spain",
        "bbox": (36.0, 43.8, -9.3, 3.3),
        "regions": [],
    },
    {
        "name": "Mexico",
        "bbox": (14.5, 32.7, -118.4, -86.7),
        "regions": [],
    },
    {
        "name": "Brazil",
        "bbox": (-33.8, 5.3, -73.9, -34.8),
        "regions": [],
    },
    {
        "name": "India",
        "bbox": (6.7, 35.5, 68.2, 97.4),
        "regions": [],
    },
]


def _latlon_to_tile(lat, lon, zoom):
    """Convert lat/lon to tile x,y at given zoom level (Web Mercator)."""
    import math
    n = 2 ** zoom
    x = int((lon + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n)
    x = max(0, min(n - 1, x))
    y = max(0, min(n - 1, y))
    return x, y


def _count_tiles_bbox(bbox, zoom_min, zoom_max):
    """Count how many tiles would be downloaded for a bounding box (min_lat, max_lat, min_lon, max_lon)."""
    lat_min, lat_max, lon_min, lon_max = bbox
    lat_min = max(lat_min, -85.0511)
    lat_max = min(lat_max, 85.0511)
    total = 0
    for z in range(zoom_min, zoom_max + 1):
        x0, y0 = _latlon_to_tile(lat_max, lon_min, z)
        x1, y1 = _latlon_to_tile(lat_min, lon_max, z)
        total += (x1 - x0 + 1) * (y1 - y0 + 1)
    return total


def _count_tiles(lat, lon, radius_km, zoom_min, zoom_max):
    """Count tiles for a center+radius area (legacy wrapper)."""
    import math
    deg_lat = radius_km / 111.0
    deg_lon = radius_km / (111.0 * max(math.cos(math.radians(lat)), 0.01))
    bbox = (
        max(lat - deg_lat, -85.0511),
        min(lat + deg_lat, 85.0511),
        lon - deg_lon,
        lon + deg_lon,
    )
    return _count_tiles_bbox(bbox, zoom_min, zoom_max)


def _suggest_zoom_range(bbox):
    """Suggest appropriate zoom range based on bounding box size."""
    lat_span = bbox[1] - bbox[0]
    lon_span = bbox[3] - bbox[2]
    area_deg2 = lat_span * lon_span
    if area_deg2 > 800:
        return 5, 8
    if area_deg2 > 200:
        return 6, 10
    if area_deg2 > 50:
        return 8, 11
    if area_deg2 > 10:
        return 9, 13
    return 10, 14


def _download_map_tiles_bbox(bbox, zoom_min, zoom_max, dest_dir, label=""):
    """Download OSM tiles for a bounding box and save to dest_dir/map/{z}/{x}/{y}.png."""
    lat_min, lat_max, lon_min, lon_max = bbox
    lat_min = max(lat_min, -85.0511)
    lat_max = min(lat_max, 85.0511)

    map_dir = os.path.join(dest_dir, "map")
    total = _count_tiles_bbox(bbox, zoom_min, zoom_max)
    downloaded = 0
    skipped = 0
    errors = 0

    color_print(f"\n  Downloading {total} tiles to: {map_dir}", C.CYAN)
    color_print(f"  Zoom levels: {zoom_min} to {zoom_max}", C.CYAN)
    if label:
        color_print(f"  Region: {label}", C.CYAN)
    color_print(f"  Bounds: {lat_min:.4f}°–{lat_max:.4f}° lat, {lon_min:.4f}°–{lon_max:.4f}° lon\n", C.CYAN)

    for z in range(zoom_min, zoom_max + 1):
        x0, y0 = _latlon_to_tile(lat_max, lon_min, z)
        x1, y1 = _latlon_to_tile(lat_min, lon_max, z)

        zoom_tiles = (x1 - x0 + 1) * (y1 - y0 + 1)
        color_print(f"  Zoom {z}: {zoom_tiles} tiles (x={x0}-{x1}, y={y0}-{y1})", C.DIM)

        for tx in range(x0, x1 + 1):
            tile_dir = os.path.join(map_dir, str(z), str(tx))
            os.makedirs(tile_dir, exist_ok=True)

            for ty in range(y0, y1 + 1):
                tile_path = os.path.join(tile_dir, f"{ty}.png")

                if os.path.isfile(tile_path) and os.path.getsize(tile_path) > 100:
                    skipped += 1
                    downloaded += 1
                    continue

                url = MAP_TILE_URL.format(z=z, x=tx, y=ty)
                try:
                    req = Request(url, headers={"User-Agent": MAP_USER_AGENT})
                    with urlopen(req, timeout=15) as resp:
                        data = resp.read()
                    with open(tile_path, "wb") as f:
                        f.write(data)
                    downloaded += 1
                except (URLError, OSError) as e:
                    errors += 1
                    if errors <= 5:
                        color_print(f"    Failed: z={z} x={tx} y={ty}: {e}", C.YELLOW)
                    elif errors == 6:
                        color_print(f"    (suppressing further error messages)", C.DIM)

                # Rate limit: OSM tile usage policy
                time.sleep(0.05)

                if downloaded % 100 == 0 and downloaded > 0:
                    pct = downloaded * 100 // total if total else 100
                    print(f"\r  Progress: {downloaded}/{total} ({pct}%)", end="", flush=True)

    print()
    color_print(f"\n  Downloaded: {downloaded - skipped} new, {skipped} cached, {errors} failed", C.GREEN)
    color_print(f"  Tile directory: {map_dir}", C.CYAN)
    return errors == 0 or (downloaded - skipped) > 0


def _find_removable_drives():
    """Find removable drives (microSD cards) on the system."""
    drives = []
    if sys.platform == "win32":
        try:
            import ctypes
            bitmask = ctypes.windll.kernel32.GetLogicalDrives()
            for i in range(26):
                if bitmask & (1 << i):
                    letter = chr(65 + i)
                    drive = f"{letter}:\\"
                    drive_type = ctypes.windll.kernel32.GetDriveTypeW(drive)
                    # 2 = DRIVE_REMOVABLE
                    if drive_type == 2:
                        # Get volume label (may fail on corrupted/unrecognized cards)
                        vol_buf = ctypes.create_unicode_buffer(256)
                        fs_buf = ctypes.create_unicode_buffer(256)
                        vol_ok = ctypes.windll.kernel32.GetVolumeInformationW(
                            drive, vol_buf, 256, None, None, None, fs_buf, 256)
                        label = vol_buf.value if vol_ok else ""
                        fs_type = fs_buf.value if vol_ok else ""
                        try:
                            total, _, free = shutil.disk_usage(drive)
                            size_mb = total // (1024 * 1024)
                            free_mb = free // (1024 * 1024)
                            desc = f"{label or 'Removable'} ({fs_type}, {size_mb}MB, {free_mb}MB free)"
                        except OSError:
                            desc = f"{label or 'Removable'}"
                            if fs_type:
                                desc += f" ({fs_type})"
                            else:
                                desc += " (unreadable — needs format)"
                        drives.append((drive, desc))
        except Exception:
            pass
    else:
        # Linux/Mac: check /dev/sd* and /media mounts
        import glob as _glob
        for mount in _glob.glob("/media/*/*") + _glob.glob("/mnt/*"):
            if os.path.ismount(mount):
                try:
                    total, _, free = shutil.disk_usage(mount)
                    size_mb = total // (1024 * 1024)
                    free_mb = free // (1024 * 1024)
                    drives.append((mount, f"{os.path.basename(mount)} ({size_mb}MB, {free_mb}MB free)"))
                except OSError:
                    drives.append((mount, os.path.basename(mount)))
    return drives


def _format_sd_card(drive_path):
    """Format the SD card as FAT32. Returns True on success."""
    if sys.platform == "win32":
        drive_letter = drive_path[0]
        color_print(f"\n  Formatting {drive_letter}: as FAT32...", C.YELLOW)
        color_print("  (This will erase ALL data on the card!)\n", C.RED)

        sys32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),"System32")
        format_exe = os.path.join(sys32, "format.com")
        diskpart_exe = os.path.join(sys32, "diskpart.exe")

        try:
            # Try the simple format command first
            r = subprocess.run(
                [format_exe, f"{drive_letter}:", "/FS:FAT32", "/Q", "/V:OUISPY", "/Y"],
                timeout=120, capture_output=True, text=True
            )
            if r.returncode == 0:
                color_print("  Format complete!", C.GREEN)
                return True

            # Simple format failed — card may have corrupt partition table
            # (e.g. non-standard sector size from embedded device).
            # Fall back to diskpart: clean → create partition → format.
            color_print("  Quick format failed, trying full partition reset...", C.YELLOW)
            return _diskpart_format(drive_letter, diskpart_exe)
        except Exception as e:
            color_print(f"  Format error: {e}", C.RED)
            # Try diskpart as last resort
            try:
                return _diskpart_format(drive_letter, diskpart_exe)
            except Exception as e2:
                color_print(f"  diskpart fallback also failed: {e2}", C.RED)
                return False
    else:
        color_print("  Auto-format is only supported on Windows.", C.YELLOW)
        color_print("  Please format your SD card as FAT32 manually,", C.CYAN)
        color_print("  then re-run this tool.", C.CYAN)
        return False


def _diskpart_format(drive_letter, diskpart_exe=None):
    """Use diskpart to clean and reformat a corrupted SD card on Windows."""
    import tempfile

    if diskpart_exe is None:
        sys32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
        diskpart_exe = os.path.join(sys32, "diskpart.exe")

    if not os.path.isfile(diskpart_exe):
        color_print(f"  diskpart not found at {diskpart_exe}", C.RED)
        return False

    # Check admin privileges — diskpart requires elevation
    try:
        import ctypes
        if not ctypes.windll.shell32.IsUserAnAdmin():
            color_print("  diskpart requires Administrator privileges.", C.RED)
            color_print("  Please re-run flash.py as Administrator (right-click → Run as administrator).", C.YELLOW)
            return False
    except Exception:
        pass

    # Map drive letter → volume number → disk number via diskpart
    # Step 1: list volumes to find which volume is on this drive letter
    list_script = "list volume\n"
    vol_num = None
    disk_num = None

    with tempfile.NamedTemporaryFile(mode='w', suffix='.txt', delete=False) as f:
        f.write(list_script)
        script_path = f.name

    try:
        r = subprocess.run(
            [diskpart_exe, "/s", script_path],
            timeout=30, capture_output=True, text=True
        )
        if r.returncode != 0:
            color_print(f"  diskpart list failed: {r.stderr.strip()}", C.RED)
            return False

        # Parse output to find volume with matching drive letter
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[0] == "Volume":
                try:
                    vnum = int(parts[1])
                except ValueError:
                    continue
                if parts[2].upper() == drive_letter.upper():
                    vol_num = vnum
                    break

        if vol_num is None:
            # Also check for volumes with no letter assignment (Removable, RAW)
            for line in r.stdout.splitlines():
                parts = line.split()
                if len(parts) >= 4 and parts[0] == "Volume":
                    try:
                        vnum = int(parts[1])
                    except ValueError:
                        continue
                    rest = line.lower()
                    if "removable" in rest and ("raw" in rest or "0  b" in rest.replace(" ", "")):
                        vol_num = vnum
                        break

        if vol_num is None:
            color_print(f"  Could not find volume for drive {drive_letter}:", C.RED)
            color_print(f"  diskpart output:\n{r.stdout}", C.DIM)
            return False
    finally:
        os.unlink(script_path)

    # Step 2: find which disk this volume is on
    detail_script = f"select volume {vol_num}\ndetail volume\n"
    with tempfile.NamedTemporaryFile(mode='w', suffix='.txt', delete=False) as f:
        f.write(detail_script)
        script_path = f.name

    try:
        r = subprocess.run(
            [diskpart_exe, "/s", script_path],
            timeout=30, capture_output=True, text=True
        )
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0] == "Disk":
                try:
                    disk_num = int(parts[1])
                    break
                except ValueError:
                    continue
        if disk_num is None:
            color_print("  Could not determine disk number for this volume.", C.RED)
            return False
    finally:
        os.unlink(script_path)

    # Safety: refuse to touch disk 0 (usually the system drive)
    if disk_num == 0:
        color_print("  Refusing to format Disk 0 (system drive).", C.RED)
        return False

    color_print(f"  Found: Volume {vol_num} on Disk {disk_num}", C.DIM)
    color_print(f"  Cleaning and reformatting Disk {disk_num}...", C.YELLOW)

    # Step 3: clean, create partition, format
    fmt_script = (
        f"select disk {disk_num}\n"
        f"clean\n"
        f"create partition primary\n"
        f"select partition 1\n"
        f"active\n"
        f"format fs=fat32 quick label=OUISPY\n"
        f"assign letter={drive_letter}\n"
    )

    with tempfile.NamedTemporaryFile(mode='w', suffix='.txt', delete=False) as f:
        f.write(fmt_script)
        script_path = f.name

    try:
        r = subprocess.run(
            [diskpart_exe, "/s", script_path],
            timeout=120, capture_output=True, text=True
        )
        if r.returncode == 0 and "DiskPart successfully" in r.stdout:
            color_print("  Format complete! (via diskpart)", C.GREEN)
            return True
        else:
            color_print(f"  diskpart format failed.", C.RED)
            if r.stdout.strip():
                color_print(f"  Output: {r.stdout.strip()[-200:]}", C.DIM)
            return False
    finally:
        os.unlink(script_path)


def _pick_number(prompt, min_val, max_val):
    """Prompt user to pick a number in range. Returns the 0-based index or None for back."""
    while True:
        raw = input(f"  {C.BOLD}{prompt}: {C.RESET}").strip().lower()
        if raw in ("b", "back", "q", "quit", "0"):
            return None
        try:
            val = int(raw)
            if min_val <= val <= max_val:
                return val - 1  # 0-based index
        except ValueError:
            pass
        color_print(f"  Enter a number {min_val}–{max_val}, or 0 to go back.", C.RED)


def _select_region():
    """Interactive region picker. Returns (bbox, label) or (None, None)."""

    # Step 1: Choose country
    while True:
        print()
        color_print("  Step 1: Choose a country\n", C.BOLD)
        for i, country in enumerate(REGION_CATALOG):
            print(f"    [{i + 1:>2}] {country['name']}")
        manual_idx = len(REGION_CATALOG) + 1
        print(f"    [{manual_idx:>2}] Enter coordinates manually")
        print(f"    [ 0] Cancel\n")

        choice = _pick_number(f"Select country [1-{manual_idx}]", 0, manual_idx)
        if choice is None:
            return None, None

        if choice == manual_idx - 1:
            # Manual coordinate entry
            return _manual_coordinates()

        country = REGION_CATALOG[choice]
        result = _select_country_scope(country)
        if result is not None:
            return result
        # result is None → user chose "back", loop to country list


def _manual_coordinates():
    """Fallback: enter lat/lon + radius manually. Returns (bbox, label) or (None, None)."""
    import math
    color_print("\n  Enter the center coordinates of your area.\n", C.DIM)
    color_print("  Tip: Find coordinates at https://www.openstreetmap.org", C.DIM)
    color_print("       (right-click the map → 'Show address' to see lat/lon)\n", C.DIM)

    while True:
        lat_str = input(f"  {C.BOLD}Latitude (e.g. 40.7128): {C.RESET}").strip()
        lon_str = input(f"  {C.BOLD}Longitude (e.g. -74.0060): {C.RESET}").strip()
        try:
            lat = float(lat_str)
            lon = float(lon_str)
            if -90 <= lat <= 90 and -180 <= lon <= 180:
                break
            color_print("  Latitude must be -90..90, longitude -180..180.", C.RED)
        except ValueError:
            color_print("  Invalid number. Enter decimal degrees.", C.RED)

    radius_str = input(f"  {C.BOLD}Coverage radius in km (default: 15): {C.RESET}").strip()
    radius_km = 15
    if radius_str:
        try:
            radius_km = float(radius_str)
            radius_km = max(1, min(200, radius_km))
        except ValueError:
            pass

    deg_lat = radius_km / 111.0
    deg_lon = radius_km / (111.0 * max(math.cos(math.radians(lat)), 0.01))
    bbox = (
        max(lat - deg_lat, -85.0511),
        min(lat + deg_lat, 85.0511),
        lon - deg_lon,
        lon + deg_lon,
    )
    label = f"Manual ({lat:.4f}, {lon:.4f}) ±{radius_km:.0f} km"
    return bbox, label


def _select_country_scope(country):
    """Let user choose: entire country, a region, or a subdivision. Returns (bbox, label) or None to go back."""
    regions = country.get("regions", [])
    has_regions = bool(regions)
    has_subdivisions = has_regions and any(r.get("subdivisions") for r in regions)

    while True:
        print()
        color_print(f"  {country['name']} — Choose download scope\n", C.BOLD)
        options = []
        options.append(f"Entire country")
        if has_regions:
            options.append(f"Select a region")
        if has_subdivisions:
            options.append(f"Select a state/province")
        options.append("Back")

        for i, opt in enumerate(options):
            print(f"    [{i + 1}] {opt}")
        print()

        choice = _pick_number(f"Select [1-{len(options)}]", 1, len(options))
        if choice is None or options[choice] == "Back":
            return None

        if choice == 0:
            # Entire country
            return country["bbox"], country["name"]

        if options[choice] == "Select a region":
            result = _select_from_regions(country["name"], regions, select_sub=False)
            if result is not None:
                return result
            continue

        if options[choice] == "Select a state/province":
            result = _select_from_regions(country["name"], regions, select_sub=True)
            if result is not None:
                return result
            continue


def _select_from_regions(country_name, regions, select_sub=False):
    """Pick a region, and optionally drill into its subdivisions. Returns (bbox, label) or None to go back."""
    while True:
        print()
        if select_sub:
            color_print(f"  {country_name} — Choose a region to browse states/provinces\n", C.BOLD)
        else:
            color_print(f"  {country_name} — Choose a region\n", C.BOLD)

        for i, r in enumerate(regions):
            extra = ""
            if r.get("subdivisions"):
                extra = f"  ({len(r['subdivisions'])} subdivisions)"
            print(f"    [{i + 1:>2}] {r['name']}{extra}")
        print(f"    [ 0] Back\n")

        choice = _pick_number(f"Select region [1-{len(regions)}]", 0, len(regions))
        if choice is None:
            return None

        region = regions[choice]

        if not select_sub:
            return region["bbox"], f"{country_name} > {region['name']}"

        # Drill into subdivisions
        subdivisions = region.get("subdivisions", [])
        if not subdivisions:
            color_print(f"  No further subdivisions for {region['name']}.", C.YELLOW)
            color_print(f"  Selecting entire region instead.", C.DIM)
            return region["bbox"], f"{country_name} > {region['name']}"

        result = _select_subdivision(country_name, region["name"], subdivisions)
        if result is not None:
            return result
        # None → user chose back, loop to region list


def _select_subdivision(country_name, region_name, subdivisions):
    """Pick a state/province from a region's subdivisions. Returns (bbox, label) or None to go back."""
    while True:
        print()
        color_print(f"  {country_name} > {region_name} — Choose a state/province\n", C.BOLD)
        for i, (name, _bbox) in enumerate(subdivisions):
            print(f"    [{i + 1:>2}] {name}")
        print(f"    [ 0] Back\n")

        choice = _pick_number(f"Select [1-{len(subdivisions)}]", 0, len(subdivisions))
        if choice is None:
            return None

        name, bbox = subdivisions[choice]
        return bbox, f"{country_name} > {region_name} > {name}"


def prepare_map_sd():
    """Interactive wizard to download map tiles and prepare a microSD card."""
    section("MicroSD Map Tile Preparation")
    color_print("  This tool downloads OpenStreetMap tiles for offline use", C.CYAN)
    color_print("  on the OUI-Spy device's map view (Flock You + Sky Spy).\n", C.CYAN)
    color_print("  You'll need:", C.BOLD)
    color_print("    - A microSD card (any size, FAT32 formatted)", C.CYAN)
    color_print("    - Internet connection to download tiles\n", C.CYAN)

    # Step 1: Select region
    bbox, label = _select_region()
    if bbox is None:
        color_print("  Cancelled.", C.YELLOW)
        return

    color_print(f"\n  Selected: {label}", C.GREEN)
    color_print(f"  Bounds:   {bbox[0]:.2f}° to {bbox[1]:.2f}° lat, {bbox[2]:.2f}° to {bbox[3]:.2f}° lon", C.DIM)

    # Step 2: Zoom levels (with smart defaults based on region size)
    default_min, default_max = _suggest_zoom_range(bbox)
    print()
    color_print("  Zoom levels control detail vs. storage:", C.DIM)
    color_print("    5  = ~1500 km view  (continent)", C.DIM)
    color_print("    8  = ~200 km view   (large region)", C.DIM)
    color_print("    9  = ~300 km view   (city region)", C.DIM)
    color_print("    11 = ~75 km view    (city level)", C.DIM)
    color_print("    13 = ~19 km view    (neighborhood)", C.DIM)
    color_print("    15 = ~5 km view     (street level)\n", C.DIM)
    color_print(f"  Suggested for this region: {default_min}-{default_max}", C.CYAN)

    zoom_str = input(f"  {C.BOLD}Zoom range (default: {default_min}-{default_max}): {C.RESET}").strip()
    zoom_min = default_min
    zoom_max = default_max
    if zoom_str:
        parts = re.split(r"[-\u2013,\s]+", zoom_str.strip())
        try:
            if len(parts) == 2:
                zoom_min = int(parts[0])
                zoom_max = int(parts[1])
            elif len(parts) == 1:
                zoom_max = int(parts[0])
            zoom_min = max(1, min(19, zoom_min))
            zoom_max = max(zoom_min, min(19, zoom_max))
        except ValueError:
            color_print("  Using suggested defaults.", C.YELLOW)
            zoom_min = default_min
            zoom_max = default_max

    # Preview tile count
    tile_count = _count_tiles_bbox(bbox, zoom_min, zoom_max)
    est_size_mb = tile_count * 15 / 1024  # ~15 KB average per tile

    print()
    color_print(f"  Region:            {label}", C.CYAN)
    color_print(f"  Tiles to download: {tile_count:,}", C.CYAN)
    color_print(f"  Estimated size:    ~{est_size_mb:.0f} MB", C.CYAN)
    color_print(f"  Zoom levels:       {zoom_min} to {zoom_max}", C.CYAN)

    if tile_count > MAP_MAX_TILES:
        color_print(f"\n  Too many tiles ({tile_count:,} > {MAP_MAX_TILES:,}).", C.RED)
        color_print("  Select a smaller region or reduce the zoom range.", C.CYAN)
        input(f"\n{C.DIM}  Press Enter to go back...{C.RESET}")
        return

    if tile_count == 0:
        color_print("\n  No tiles to download.", C.RED)
        input(f"\n{C.DIM}  Press Enter to go back...{C.RESET}")
        return

    # Step 3: Choose destination
    print()
    drives = _find_removable_drives()

    if drives:
        color_print("  Step 3: Choose destination\n", C.BOLD)
        color_print("    [1] Download to a removable drive (microSD card)", C.CYAN)
        color_print("    [2] Download to a local folder\n", C.CYAN)
        dest_choice = input(f"  {C.BOLD}Select [1/2] (default: 1): {C.RESET}").strip()
    else:
        color_print("  No removable drives detected.", C.YELLOW)
        color_print("  Tiles will be saved to a local folder.", C.DIM)
        dest_choice = "2"

    dest_dir = None

    if dest_choice != "2" and drives:
        print()
        color_print("  Removable drives found:\n", C.BOLD)
        for i, (path, desc) in enumerate(drives):
            print(f"    [{i + 1}] {path}  — {desc}")
        print()
        while True:
            drive_choice = input(f"  {C.BOLD}Select drive [1-{len(drives)}]: {C.RESET}").strip()
            try:
                idx = int(drive_choice) - 1
                if 0 <= idx < len(drives):
                    dest_dir = drives[idx][0]
                    break
            except ValueError:
                pass
            color_print("  Invalid choice.", C.RED)

        print()
        if ask_yes("  Format the SD card as FAT32 first? (erases all data)", default_yes=False):
            if not ask_yes(f"  CONFIRM: Erase ALL data on {dest_dir}?", default_yes=False):
                color_print("  Format cancelled.", C.YELLOW)
            else:
                if not _format_sd_card(dest_dir):
                    if not ask_yes("  Continue without formatting?"):
                        return
    else:
        default_map_dir = os.path.join(get_project_dir(), "sdcard")
        color_print(f"\n  Default folder: {default_map_dir}", C.DIM)
        custom = input(f"  {C.BOLD}Folder path (Enter for default): {C.RESET}").strip()
        dest_dir = custom if custom else default_map_dir

    if not dest_dir:
        return

    # Step 4: Download
    print()
    if not ask_yes(f"  Download {tile_count:,} tiles to {dest_dir}?"):
        color_print("  Cancelled.", C.YELLOW)
        return

    success = _download_map_tiles_bbox(bbox, zoom_min, zoom_max, dest_dir, label)

    if success:
        section("Map preparation complete!")
        color_print("  Your microSD card is ready.\n", C.GREEN)
        color_print(f"  Region: {label}", C.CYAN)
        color_print("  File structure on the SD card:", C.BOLD)
        color_print("    /map/{zoom}/{x}/{y}.png\n", C.CYAN)
        color_print("  To use:", C.BOLD)
        color_print("    1. Insert the microSD card into the OUI-Spy C6 board", C.CYAN)
        color_print("    2. Power on or reset the device", C.CYAN)
        color_print("    3. Triple-click in Flock You or Sky Spy mode to toggle map", C.CYAN)
        color_print("    4. Double-click while in map view to cycle zoom levels", C.CYAN)
    else:
        color_print("\n  Some tiles failed to download. The card may still work", C.YELLOW)
        color_print("  with partial coverage. Re-run to retry failed tiles.", C.CYAN)

    input(f"\n{C.DIM}  Press Enter to continue...{C.RESET}")


# ═════════════════════════════════════════════
#  MAIN WIZARD
# ═════════════════════════════════════════════

def main():
    global BAUD, MONITOR_BAUD

    # Parse optional flags
    specified_port = None
    erase = False
    monitor = True
    run_map_tool = False
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
        if args[i] == "--map":
            run_map_tool = True; i += 1; continue
        if args[i] in ("--help", "-h"):
            print("Usage: python flash.py [--port PORT] [--erase] [--baud RATE] [--monitor-baud RATE] [--no-monitor] [--download] [--build] [--map]")
            print("\n  No arguments needed — the script handles everything automatically.")
            print("  --download   Skip menu, download prebuilt firmware from GitHub")
            print("  --build      Skip menu, build from local source code")
            print("  --map        Skip menu, go directly to map tile SD card preparation")
            print("  It will flash and then open a serial monitor by default.\n")
            sys.exit(0)
        i += 1

    color_print(BANNER, C.PURPLE)

    # Direct launch of map tool via CLI flag
    if run_map_tool:
        prepare_map_sd()
        return

    # ── Top-level menu ──
    section("What would you like to do?")
    color_print("    [1] Flash firmware to the board (default)", C.CYAN)
    color_print("    [2] Prepare microSD card with offline map tiles\n", C.CYAN)
    top_choice = input(f"  {C.BOLD}Select [1/2] (default: 1): {C.RESET}").strip()

    if top_choice == "2":
        prepare_map_sd()
        return

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
    default_board_variant = board_variant_from_info(_load_firmware_info(get_build_dir())) if firmware_ready() else BOARD_NON_TOUCH
    board_variant = prompt_board_variant(default_board_variant)
    build_dir = ensure_firmware(board_variant)
    firmware_info = _load_firmware_info(build_dir)
    firmware_info.setdefault("board", board_variant_name(board_variant))
    firmware_info.setdefault("board_id", board_variant["id"])
    firmware_info.setdefault("board_touch", board_touch_value(board_variant))

    # ── 3. Serial port ──
    section("Detecting board")
    port = select_port(specified_port)

    # ── 4. Flash ──
    print_firmware_info(firmware_info, "Firmware to flash")
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
