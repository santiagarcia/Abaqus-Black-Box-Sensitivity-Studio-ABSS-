#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Abaqus ODB Viewer  (single file)
================================

HOW TO USE
    1. Install Python 3.8 or newer from python.org (default options are fine).
       Abaqus must be installed on this computer.
    2. Double-click this file, or run:   python abaqus_odb_viewer.py
    3. The first run downloads what it needs (1 to 3 minutes, needs internet).
    4. In the window: pick the .odb file, press "Read file", press "Open viewer".

WHAT IT DOES
    - Reads the mesh, the element types and every field output stored in the .odb
    - Opens an interactive Plotly viewer in your web browser
    - Dropdown to choose the variable used for the colors
    - Slider (and Play button) to move through every saved increment
    - Click on the model to plot the history of the variable at that point

ONLY THE .odb IS NEEDED. It already contains the mesh, the element types, the
outputs and the increment/time information, so the .inp and .sta are not required.

Everything this program creates lives in one folder (shown in the window log),
normally  C:\\Users\\<you>\\.abaqus_odb_viewer . Delete that folder to uninstall.
If something fails, the file  setup_log.txt  or  error_log.txt  in that folder
has the details.
"""
from __future__ import print_function
# pyright: reportMissingImports=false, reportMissingModuleSource=false
# (numpy, plotly and dash are installed by this file on its first run, so editors may not see them)

import os
import sys
import subprocess


def _this_file():
    try:
        return os.path.abspath(__file__)
    except NameError:          # pasted into an interactive window
        return os.path.abspath(sys.argv[0]) if sys.argv and sys.argv[0] else ""


def _safe_console():
    """Consoles that cannot show some characters (Windows, VS Code) must not crash the program."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass

APP_NAME = "Abaqus ODB Viewer"
REQUIREMENTS = ["numpy>=1.22", "plotly>=5.20", "dash>=3.0,<5"]
IS_WIN = os.name == "nt"
NO_WINDOW = 0x08000000 if IS_WIN else 0


# =============================================================================
# PART 1.  First-run setup and error handling
#          The packages are installed with pip into a private folder that only
#          this program uses. No virtual environment, no admin rights, nothing
#          is changed in the Python installation.
# =============================================================================
def _plain(path):
    """True for paths that every program (also Abaqus Python 2) handles well."""
    try:
        path.encode("ascii")
    except Exception:
        return False
    return not (IS_WIN and " " in path)


def _writable(path):
    try:
        if not os.path.isdir(path):
            os.makedirs(path)
        test = os.path.join(path, "write_test.tmp")
        with open(test, "w") as f:
            f.write("ok")
        os.remove(test)
        return True
    except Exception:
        return False


def _pick_app_dir():
    import tempfile
    cands = []
    if os.environ.get("ODBVIEW_HOME"):
        cands.append(os.environ["ODBVIEW_HOME"])
    home = os.path.join(os.path.expanduser("~"), ".abaqus_odb_viewer")
    if IS_WIN:
        pub = os.path.join(os.environ.get("PUBLIC") or r"C:\Users\Public", "abaqus_odb_viewer")
        loc = os.path.join(os.environ.get("LOCALAPPDATA") or home, "abaqus_odb_viewer")
        good = [p for p in (home, pub, r"C:\abaqus_odb_viewer", loc) if _plain(p)]
        cands += good + [home, loc]
    else:
        cands.append(home)
    cands.append(os.path.join(tempfile.gettempdir(), "abaqus_odb_viewer"))
    for c in cands:
        if _writable(c):
            return c
    return cands[-1]


APP_DIR = _pick_app_dir()
CACHE_DIR = os.path.join(APP_DIR, "cache")
PKG_ROOT = os.path.join(APP_DIR, "packages")
PY_TAG = "py%d%d-%s" % (sys.version_info[0], sys.version_info[1], "64" if sys.maxsize > 2 ** 32 else "32")
POINTER = os.path.join(PKG_ROOT, "current-%s.txt" % PY_TAG)
SETUP_LOG = os.path.join(APP_DIR, "setup_log.txt")
ERROR_LOG = os.path.join(APP_DIR, "error_log.txt")


def python_exe():
    """The python.exe running this file (python.exe, not pythonw.exe, on Windows)."""
    exe = sys.executable or "python"
    d, b = os.path.split(exe)
    if IS_WIN and b.lower() == "pythonw.exe" and os.path.exists(os.path.join(d, "python.exe")):
        return os.path.join(d, "python.exe")
    return exe


def clean_env(drop_python_vars=False):
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    for k in ("PIP_USER", "PIP_REQUIRE_VIRTUALENV", "PIP_TARGET", "PIP_PREFIX", "PIP_NO_INDEX",
              "PYTHONSTARTUP", "PYTHONNOUSERSITE"):
        env.pop(k, None)
    env["PIP_DISABLE_PIP_VERSION_CHECK"] = "1"
    env["PIP_NO_INPUT"] = "1"
    env["PIP_USER"] = "0"
    env["PIP_REQUIRE_VIRTUALENV"] = "0"
    if drop_python_vars:
        for k in ("PYTHONPATH", "PYTHONHOME", "PYTHONUTF8", "PYTHONEXECUTABLE"):
            env.pop(k, None)
    return env


def run_cmd(cmd, on_line=None, env=None, cwd=None):
    """Runs a command and returns (exit code, output lines). Never raises."""
    lines = []
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             env=env, cwd=cwd, creationflags=NO_WINDOW,
                             universal_newlines=True, encoding="utf-8", errors="replace")
    except Exception as exc:
        return 1, ["Could not start %s: %s" % (cmd[0], exc)]
    for line in p.stdout:
        line = line.rstrip()
        if line:
            lines.append(line)
            if on_line:
                on_line(line)
    return p.wait(), lines


def _marker_text():
    return "\n".join(REQUIREMENTS + [sys.version.split()[0]])


def _current_pkg_dir():
    try:
        with open(POINTER) as f:
            lines = f.read().split("\n", 1)
        if len(lines) == 2 and lines[1] == _marker_text() and os.path.isdir(lines[0]):
            return lines[0]
    except Exception:
        pass
    return None


def _cleanup_old(keep):
    import shutil
    try:
        for name in os.listdir(PKG_ROOT):
            p = os.path.join(PKG_ROOT, name)
            if os.path.isdir(p) and p != keep and name.startswith(PY_TAG + "-"):
                shutil.rmtree(p, ignore_errors=True)
    except Exception:
        pass


def _install(report):
    """Installs the packages. report(text) gets progress lines. Returns error text or None."""
    import time
    if not os.path.isdir(PKG_ROOT):
        os.makedirs(PKG_ROOT)
    log = open(SETUP_LOG, "w", encoding="utf-8")

    def step(text):
        report(text)
        log.write("\n### " + text + "\n")
        log.flush()

    def line(text):
        report("    " + text[:170])
        log.write(text + "\n")
        log.flush()

    def run(cmd, env):
        log.write("> " + " ".join(cmd) + "\n")
        rc, out = run_cmd(cmd, line, env)
        log.write("exit code %d\n" % rc)
        return rc, out

    import shutil
    shutil.rmtree(os.path.join(APP_DIR, "venv"), ignore_errors=True)   # left by older versions
    py = python_exe()
    env = clean_env()
    env.pop("PYTHONPATH", None)   # editors such as VS Code may set it from a .env file
    log.write("Python: %s\n%s\nApp folder: %s\n" % (py, sys.version, APP_DIR))

    # 1. pip must exist
    step("Checking the Python installer (pip)...")
    rc, _ = run([py, "-m", "pip", "--version"], env)
    if rc != 0:
        step("pip is missing. Installing it...")
        run([py, "-m", "ensurepip", "--user", "--default-pip"], env)
        rc, _ = run([py, "-m", "pip", "--version"], env)
    if rc != 0:
        step("Downloading pip...")
        try:
            import urllib.request
            gp = os.path.join(APP_DIR, "get-pip.py")
            urllib.request.urlretrieve("https://bootstrap.pypa.io/get-pip.py", gp)
            run([py, gp, "--user", "--no-warn-script-location"], env)
            rc, _ = run([py, "-m", "pip", "--version"], env)
        except Exception as exc:
            line("Download failed: %s" % exc)
    if rc != 0:
        log.close()
        return ("This Python installation has no package installer (pip) and it could not be added.\n\n"
                "Fix: reinstall Python from python.org and keep the option 'pip' checked.")

    # 2. install the packages into a new private folder
    target = os.path.join(PKG_ROOT, "%s-%d" % (PY_TAG, int(time.time())))
    base = [py, "-m", "pip", "install", "--target", target, "--upgrade", "--prefer-binary",
            "--no-warn-script-location", "--disable-pip-version-check", "--no-input"]
    trusted = ["--trusted-host", "pypi.org", "--trusted-host", "files.pythonhosted.org"]
    attempts = [
        ("Downloading and installing numpy, plotly and dash...", base, env),
        ("Second try, relaxing the secure-connection check (common on university networks)...",
         base + trusted, env),
    ]
    ok = False
    out = []
    for i, (title, cmd, e) in enumerate(attempts):
        step(title)
        rc, out = run(cmd + REQUIREMENTS, e)
        if rc == 0:
            ok = True
            break
        if i == 0:
            # an old pip can fail to find packages for a new Python. Get a new pip on the side.
            step("Getting a newer pip on the side...")
            side = os.path.join(PKG_ROOT, "newpip-%s" % PY_TAG)
            rc2, _ = run([py, "-m", "pip", "install", "--target", side, "--upgrade", "pip"] + trusted, env)
            if rc2 == 0:
                e2 = dict(env)
                e2["PYTHONPATH"] = side + os.pathsep + env.get("PYTHONPATH", "")
                attempts.insert(1, ("Trying again with the newer pip...", base, e2))
                attempts.append(("Last try with the newer pip...", base + trusted, e2))
    if not ok:
        log.close()
        hint = "\n".join(out[-8:])
        return ("The packages could not be downloaded.\n\nLast messages:\n%s\n\n"
                "Check the internet connection. If a firewall or VPN is active, try another network."
                % hint[-900:])

    # 3. confirm that they import
    step("Checking the installation...")
    code = "import sys; sys.path.insert(0, %r); import numpy, plotly, dash; print('numpy', numpy.__version__)" % target
    rc, out = run([py, "-c", code], env)
    if rc != 0:
        log.close()
        return "The packages were installed but do not load.\n\n" + "\n".join(out[-8:])[-900:]
    with open(POINTER, "w") as f:
        f.write(target + "\n" + _marker_text())
    _cleanup_old(target)
    step("Done.")
    log.close()
    return None


# ---- messages that work with or without a window -----------------------------
def _has_tk():
    try:
        import tkinter  # noqa: F401
        return True
    except Exception:
        return False


def show_error(text, log_path=None):
    """Shows an error the user cannot miss."""
    full = text + ("\n\nFull details are in:\n" + log_path if log_path else "")
    shown = False
    if _has_tk():
        try:
            import tkinter as tk
            from tkinter import messagebox
            r = tk.Tk()
            r.withdraw()
            r.attributes("-topmost", True)
            if log_path and os.path.exists(log_path):
                if messagebox.askyesno(APP_NAME + " - problem", full + "\n\nOpen the details file now?",
                                       icon="error", parent=r):
                    open_path(log_path)
            else:
                messagebox.showerror(APP_NAME + " - problem", full, parent=r)
            r.destroy()
            shown = True
        except Exception:
            shown = False
    if not shown:
        print("\n" + "=" * 70 + "\nPROBLEM\n" + "=" * 70 + "\n" + full + "\n")
        pause()


def pause():
    try:
        if sys.stdin and sys.stdin.isatty():
            input("Press Enter to close...")
    except Exception:
        pass


def open_path(p):
    try:
        if IS_WIN:
            os.startfile(p)
        elif sys.platform == "darwin":
            subprocess.Popen(["open", p])
        else:
            subprocess.Popen(["xdg-open", p])
    except Exception:
        pass


def write_error_log(text):
    try:
        with open(ERROR_LOG, "w", encoding="utf-8") as f:
            f.write(text)
        return ERROR_LOG
    except Exception:
        return None


def _setup_with_window():
    """Installs the packages, showing progress. Returns True on success."""
    if not _has_tk():
        print("First-time setup. This takes 1 to 3 minutes...")
        err = _install(print)
        if err:
            show_error(err, SETUP_LOG)
            return False
        return True

    import threading
    import queue
    import tkinter as tk
    from tkinter import ttk
    q = queue.Queue()
    result = {}

    def worker():
        try:
            result["err"] = _install(q.put)
        except Exception:
            import traceback
            result["err"] = "Unexpected problem during setup.\n\n" + traceback.format_exc()[-900:]
        q.put(None)

    root = tk.Tk()
    root.title(APP_NAME + " - first-time setup")
    root.geometry("680x380")
    root.attributes("-topmost", True)
    root.after(1500, lambda: root.attributes("-topmost", False))
    ttk.Label(root, text="First-time setup. This happens only once and takes 1 to 3 minutes.",
              font=("", 11, "bold")).pack(padx=12, pady=(12, 2), anchor="w")
    ttk.Label(root, text="Please keep this window open. The viewer starts by itself when it finishes.").pack(
        padx=12, anchor="w")
    bar = ttk.Progressbar(root, mode="indeterminate")
    bar.pack(fill="x", padx=12, pady=6)
    bar.start(12)
    txt = tk.Text(root, height=14, wrap="none", font=("Courier", 9))
    txt.pack(fill="both", expand=True, padx=12, pady=(4, 12))

    def poll():
        try:
            while True:
                item = q.get_nowait()
                if item is None:
                    root.destroy()
                    return
                txt.insert("end", item + "\n")
                txt.see("end")
        except queue.Empty:
            pass
        root.after(100, poll)

    threading.Thread(target=worker, daemon=True).start()
    root.after(100, poll)
    root.protocol("WM_DELETE_WINDOW", lambda: None)  # closing mid-install would leave a half setup
    root.mainloop()
    err = result.get("err", "Setup was interrupted.")
    if err:
        show_error(err, SETUP_LOG)
        return False
    return True


def _activate(pkg_dir):
    if pkg_dir not in sys.path:
        sys.path.insert(0, pkg_dir)
    import importlib
    importlib.invalidate_caches()
    import numpy  # noqa: F401
    import plotly  # noqa: F401
    import dash  # noqa: F401


def bootstrap():
    """Makes sure numpy, plotly and dash can be imported. Installs them the first time."""
    if os.environ.get("ODBVIEW_SKIP_SETUP"):
        return
    if sys.version_info < (3, 8):
        show_error("This program needs Python 3.8 or newer. This computer runs Python %s.\n\n"
                   "Install the latest Python from python.org and open this file again."
                   % sys.version.split()[0])
        sys.exit(1)
    pkg = _current_pkg_dir()
    if pkg:
        try:
            _activate(pkg)
            return
        except Exception:
            pass   # broken install: make a fresh one below
        if os.environ.get("ODBVIEW_RETRIED"):
            show_error("The packages are installed but do not load.\n\nDelete this folder and try again:\n"
                       + APP_DIR, SETUP_LOG)
            sys.exit(1)
        try:
            os.remove(POINTER)
        except Exception:
            pass
        if not _setup_with_window():
            sys.exit(1)
        # modules from the broken copy may already be loaded: start fresh
        env = dict(os.environ)
        env["ODBVIEW_RETRIED"] = "1"
        sys.exit(subprocess.call([sys.executable, _this_file()] + sys.argv[1:], env=env))
    if not _setup_with_window():
        sys.exit(1)
    try:
        _activate(_current_pkg_dir())
    except Exception:
        import traceback
        show_error("The packages were installed but do not load.\n\n" + traceback.format_exc()[-800:],
                   write_error_log(traceback.format_exc()))
        sys.exit(1)

# =============================================================================
# PART 2.  ODB extractor.  This text is saved to a file and executed with
#          "abaqus python", because only Abaqus can open .odb files.
#          It must stay compatible with Python 2.7 (Abaqus <= 2023) and
#          Python 3 (Abaqus >= 2024). It writes plain .npy and .json files.
# =============================================================================
EXTRACTOR = r'''# -*- coding: utf-8 -*-
# Generated by abaqus_odb_viewer.py. Do not edit.
from __future__ import print_function, division
import sys, os, json, glob
import numpy as np
from numpy.lib.format import open_memmap


def say(msg):
    print("@@MSG " + str(msg))
    sys.stdout.flush()


def progress(i, n):
    print("@@P %d %d" % (i, n))
    sys.stdout.flush()


def S(x):
    try:
        return str(x)
    except Exception:
        return x.encode(sys.getfilesystemencoding() or "utf-8")


def load_json(p):
    f = open(p, "r")
    try:
        return json.load(f)
    finally:
        f.close()


def save_json(p, obj):
    f = open(p, "w")
    try:
        json.dump(obj, f)
    finally:
        f.close()


# ----------------------------------------------------------------------------
# Small fake ODB with the same interface as the Abaqus one (demo mode only)
# ----------------------------------------------------------------------------
class _O(object):
    def __init__(self, **kw):
        self.__dict__.update(kw)


class _Repo(object):
    def __init__(self):
        self._k = []
        self._d = {}

    def add(self, k, v):
        self._k.append(k)
        self._d[k] = v

    def keys(self):
        return list(self._k)

    def __getitem__(self, k):
        return self._d[k]

    def __len__(self):
        return len(self._k)


class _Frame(object):
    def __init__(self, builder, a, amax, inc, value, desc):
        self._b = builder
        self._a = a
        self._amax = amax
        self.incrementNumber = inc
        self.frameValue = value
        self.description = desc

    def _get(self):
        return self._b(self._a, self._amax)

    fieldOutputs = property(_get)


def make_demo_odb():
    L, H, W = 120.0, 30.0, 30.0
    insts = _Repo()

    # solid block, C3D8
    nx, ny, nz = 24, 6, 6
    nodes, lab, k = [], {}, 1
    for kk in range(nz + 1):
        for jj in range(ny + 1):
            for ii in range(nx + 1):
                nodes.append(_O(label=k, coordinates=(L * ii / nx, H * jj / ny, W * kk / nz)))
                lab[(ii, jj, kk)] = k
                k += 1
    elems, e = [], 1
    for kk in range(nz):
        for jj in range(ny):
            for ii in range(nx):
                c = (lab[(ii, jj, kk)], lab[(ii + 1, jj, kk)], lab[(ii + 1, jj + 1, kk)], lab[(ii, jj + 1, kk)],
                     lab[(ii, jj, kk + 1)], lab[(ii + 1, jj, kk + 1)], lab[(ii + 1, jj + 1, kk + 1)],
                     lab[(ii, jj + 1, kk + 1)])
                elems.append(_O(label=e, type="C3D8", connectivity=c))
                e += 1
    block = _O(name="BLOCK-1", nodes=nodes, elements=elems, embeddedSpace="THREE_D")
    insts.add("BLOCK-1", block)

    # shell plate, S4R plus a row of S3
    sx, sz = 12, 4
    nodes, lab, k = [], {}, 1001
    for kk in range(sz + 1):
        for ii in range(sx + 1):
            nodes.append(_O(label=k, coordinates=(L * ii / sx, 60.0, W * kk / sz)))
            lab[(ii, kk)] = k
            k += 1
    elems, e = [], 1
    for kk in range(sz):
        for ii in range(sx):
            a, b, c, d = lab[(ii, kk)], lab[(ii + 1, kk)], lab[(ii + 1, kk + 1)], lab[(ii, kk + 1)]
            if ii == sx - 1:
                elems.append(_O(label=e, type="S3", connectivity=(a, b, c)))
                e += 1
                elems.append(_O(label=e, type="S3", connectivity=(a, c, d)))
            else:
                elems.append(_O(label=e, type="S4R", connectivity=(a, b, c, d)))
            e += 1
    plate = _O(name="PLATE-1", nodes=nodes, elements=elems, embeddedSpace="THREE_D")
    insts.add("PLATE-1", plate)

    # truss, T3D2, labels with gaps
    nodes = [_O(label=5 * (i + 1), coordinates=(L * i / 10.0, -25.0, W / 2)) for i in range(11)]
    elems = [_O(label=i + 1, type="T3D2", connectivity=(5 * (i + 1), 5 * (i + 2))) for i in range(10)]
    truss = _O(name="TRUSS-1", nodes=nodes, elements=elems, embeddedSpace="THREE_D")
    insts.add("TRUSS-1", truss)

    def arrays(inst):
        nl = np.array([n.label for n in inst.nodes])
        xyz = np.array([n.coordinates for n in inst.nodes], dtype=float)
        pos = dict((int(l), i) for i, l in enumerate(nl))
        el = np.array([q.label for q in inst.elements])
        cen = np.array([xyz[[pos[c] for c in q.connectivity]].mean(axis=0) for q in inst.elements])
        return nl, xyz, el, cen

    A = dict((nm, arrays(insts[nm])) for nm in insts.keys())

    def defl(x, a):
        xi = x / L
        return -a * 15.0 * xi * xi * (3.0 - xi) / 2.0

    def slope(x, a):
        xi = x / L
        return -a * 15.0 * (6.0 * xi - 3.0 * xi * xi) / (2.0 * L)

    def blk(pos, inst, data, comps, nl=None, el=None, sp=None, mises=None):
        return _O(position=pos, instance=inst, data=data, componentLabels=comps, nodeLabels=nl,
                  elementLabels=el, sectionPoint=sp, mises=mises)

    def fo(comps, typ, inv, pos, desc, blocks):
        return _O(componentLabels=comps, type=typ, validInvariants=inv, locations=[_O(position=pos)],
                  description=desc, bulkDataBlocks=blocks)

    def builder(a, amax):
        rep = _Repo()
        # U
        blocks = []
        for nm, yc in (("BLOCK-1", H / 2), ("PLATE-1", 60.0), ("TRUSS-1", -25.0)):
            nl, xyz, el, cen = A[nm]
            u = np.zeros((len(nl), 3))
            u[:, 1] = defl(xyz[:, 0], a)
            u[:, 0] = -(xyz[:, 1] - yc) * slope(xyz[:, 0], a)
            blocks.append(blk("NODAL", insts[nm], u, ("U1", "U2", "U3"), nl=nl))
        rep.add("U", fo(("U1", "U2", "U3"), "VECTOR", ("MAGNITUDE",), "NODAL", "Spatial displacement", blocks))
        # RF only at the clamped end
        nl, xyz, el, cen = A["BLOCK-1"]
        m = xyz[:, 0] == 0.0
        rf = np.zeros((int(m.sum()), 3))
        rf[:, 1] = 50.0 * a
        rep.add("RF", fo(("RF1", "RF2", "RF3"), "VECTOR", ("MAGNITUDE",), "NODAL", "Reaction force",
                         [blk("NODAL", block, rf, ("RF1", "RF2", "RF3"), nl=nl[m])]))
        # S
        blocks = []
        eta = (cen[:, 1] - H / 2) / (H / 2)
        s = np.zeros((len(el), 6))
        s[:, 0] = a * 300.0 * (1.0 - cen[:, 0] / L) * eta
        s[:, 3] = a * 20.0 * (1.0 - eta ** 2)
        mis = np.sqrt(s[:, 0] ** 2 + 3.0 * s[:, 3] ** 2)
        blocks.append(blk("INTEGRATION_POINT", block, np.repeat(s, 8, axis=0),
                          ("S11", "S22", "S33", "S12", "S13", "S23"), el=np.repeat(el, 8),
                          mises=np.repeat(mis, 8)))
        nl2, xyz2, el2, cen2 = A["PLATE-1"]
        for spn, sign in ((1, -1.0), (5, 1.0)):
            s = np.zeros((len(el2), 4))
            s[:, 0] = sign * a * 150.0 * (1.0 - cen2[:, 0] / L)
            blocks.append(blk("INTEGRATION_POINT", plate, s, ("S11", "S22", "S33", "S12"), el=el2,
                              sp=_O(number=spn, description="sp"), mises=np.abs(s[:, 0])))
        nl3, xyz3, el3, cen3 = A["TRUSS-1"]
        s = (a * 100.0 * (1.0 - cen3[:, 0] / L)).reshape(-1, 1)
        blocks.append(blk("INTEGRATION_POINT", truss, s, ("S11",), el=el3, mises=np.abs(s[:, 0])))
        rep.add("S", fo(("S11", "S22", "S33", "S12", "S13", "S23"), "TENSOR_3D_FULL", ("MISES",),
                        "INTEGRATION_POINT", "Stress components", blocks))
        # PEEQ
        pe = np.maximum(0.0, amax * np.abs(300.0 * (1.0 - cen[:, 0] / L) * eta) - 150.0) * 1.0e-4
        rep.add("PEEQ", fo((), "SCALAR", (), "INTEGRATION_POINT", "Equivalent plastic strain",
                           [blk("INTEGRATION_POINT", block, np.repeat(pe, 8).reshape(-1, 1), (),
                                el=np.repeat(el, 8))]))
        return rep

    steps = _Repo()
    f1 = [_Frame(builder, i / 20.0, i / 20.0, i, i / 20.0, "Increment %d: Step Time = %.3f" % (i, i / 20.0))
          for i in range(21)]
    f2 = [_Frame(builder, 1.0 - 0.6 * i / 10.0, 1.0, i, i / 10.0,
                 "Increment %d: Step Time = %.3f" % (i, i / 10.0)) for i in range(11)]
    steps.add("Load", _O(name="Load", totalTime=0.0, frames=f1))
    steps.add("Unload", _O(name="Unload", totalTime=1.0, frames=f2))
    return _O(rootAssembly=_O(instances=insts), steps=steps)


# ----------------------------------------------------------------------------
def open_odb(cfg):
    if cfg.get("demo"):
        return make_demo_odb()
    import odbAccess
    path = S(cfg["odb"])
    try:
        if odbAccess.isUpgradeRequiredForOdb(upgradeRequiredOdbPath=path):
            up = os.path.join(S(cfg["out"]), "upgraded.odb")
            if not os.path.exists(up):
                say("This ODB comes from an older Abaqus version. Upgrading a copy...")
                odbAccess.upgradeOdb(existingOdbPath=path, upgradedOdbPath=up)
            path = up
    except Exception as exc:
        say("Upgrade check skipped (%s)" % exc)
    return odbAccess.openOdb(path=path, readOnly=True)


def list_frames(odb):
    out = []
    si = 0
    for sname in list(odb.steps.keys()):
        step = odb.steps[sname]
        try:
            t0 = float(step.totalTime)
        except Exception:
            t0 = 0.0
        nf = len(step.frames)
        for fi in range(nf):
            fr = step.frames[fi]
            val = float(fr.frameValue)
            out.append({"step": S(sname), "si": si, "fi": fi, "inc": int(fr.incrementNumber),
                        "value": val, "total": t0 + val, "desc": S(fr.description)})
        si += 1
    return out


def list_fields(odb):
    seen = {}
    order = []
    for sname in list(odb.steps.keys()):
        step = odb.steps[sname]
        nf = len(step.frames)
        if nf == 0:
            continue
        for fi in sorted(set([0, nf - 1])):
            fos = step.frames[fi].fieldOutputs
            for key in list(fos.keys()):
                key = S(key)
                if key in seen:
                    continue
                try:
                    f = fos[key]
                    pos = []
                    try:
                        for loc in f.locations:
                            pos.append(S(loc.position))
                    except Exception:
                        pass
                    seen[key] = {"key": key, "comps": [S(c) for c in f.componentLabels],
                                 "type": S(f.type), "positions": pos,
                                 "invariants": [S(v) for v in f.validInvariants],
                                 "desc": S(f.description)}
                    order.append(key)
                except Exception as exc:
                    say("Skipping field %s (%s)" % (key, exc))
    return [seen[k] for k in order]


def scan(cfg):
    out = S(cfg["out"])
    say("Opening the ODB...")
    odb = open_odb(cfg)
    asm = odb.rootAssembly
    instances, groups, coords_all = [], [], []
    node_start = 0
    elem_start = 0
    ii = 0
    gi = 0
    for iname in list(asm.instances.keys()):
        inst = asm.instances[iname]
        nn = len(inst.nodes)
        ne = len(inst.elements)
        if nn == 0:
            continue
        say("Reading mesh of %s: %d nodes, %d elements" % (S(iname), nn, ne))
        labels = np.zeros(nn, dtype=np.int64)
        xyz = np.zeros((nn, 3), dtype=np.float64)
        k = 0
        for nd in inst.nodes:
            labels[k] = nd.label
            c = nd.coordinates
            xyz[k, 0] = c[0]
            xyz[k, 1] = c[1]
            if len(c) > 2:
                xyz[k, 2] = c[2]
            k += 1
        np.save(os.path.join(out, "nlab_%d.npy" % ii), labels)
        coords_all.append(xyz)
        maxlab = int(labels.max())
        nlut = -np.ones(maxlab + 2, dtype=np.int64)
        nlut[labels] = np.arange(nn, dtype=np.int64) + node_start

        bytype = {}
        order = []
        for el in inst.elements:
            conn = el.connectivity
            key = (S(el.type), len(conn))
            g = bytype.get(key)
            if g is None:
                g = [[], []]
                bytype[key] = g
                order.append(key)
            g[0].append(el.label)
            g[1].append(tuple(conn))
        for key in order:
            g = bytype[key]
            if key[1] == 0:
                continue
            labs = np.array(g[0], dtype=np.int64)
            conn = np.array(g[1], dtype=np.int64).reshape(len(labs), key[1])
            bad = (conn > maxlab) | (conn < 0)
            gidx = nlut[np.where(bad, 0, conn)]
            gidx[bad] = -1
            keep = (gidx >= 0).all(axis=1)
            labs = labs[keep]
            gidx = gidx[keep]
            if len(labs) == 0:
                continue
            np.save(os.path.join(out, "conn_%d.npy" % gi), gidx)
            np.save(os.path.join(out, "elab_%d.npy" % gi), labs)
            groups.append({"instance": S(iname), "ii": ii, "etype": key[0], "nn": key[1],
                           "n": int(len(labs)), "start": elem_start})
            elem_start += int(len(labs))
            gi += 1
        emb = ""
        try:
            emb = S(inst.embeddedSpace)
        except Exception:
            pass
        instances.append({"name": S(iname), "ii": ii, "n_nodes": nn, "node_start": node_start,
                          "embedded": emb})
        node_start += nn
        ii += 1
    if node_start == 0:
        raise RuntimeError("No mesh was found in the part instances of this ODB.")
    np.save(os.path.join(out, "nodes.npy"), np.vstack(coords_all))
    say("Reading the list of steps, frames and outputs...")
    meta = {"version": 1, "odb": S(cfg.get("odb", "demo")), "instances": instances, "groups": groups,
            "n_nodes": node_start, "n_elems": elem_start,
            "frames": list_frames(odb), "fields": list_fields(odb)}
    save_json(os.path.join(out, "meta.json"), meta)
    say("Found %d nodes, %d elements, %d frames, %d output variables"
        % (node_start, elem_start, len(meta["frames"]), len(meta["fields"])))


def build_luts(out, meta):
    nlut = {}
    elut = {}
    for inst in meta["instances"]:
        labels = np.load(os.path.join(out, "nlab_%d.npy" % inst["ii"]))
        lut = -np.ones(int(labels.max()) + 2, dtype=np.int64)
        lut[labels] = np.arange(len(labels), dtype=np.int64) + int(inst["node_start"])
        nlut[S(inst["name"])] = lut
    labs = []
    for gi in range(len(meta["groups"])):
        labs.append(np.load(os.path.join(out, "elab_%d.npy" % gi)))
    for inst in meta["instances"]:
        mx = -1
        for gi, g in enumerate(meta["groups"]):
            if g["ii"] == inst["ii"]:
                mx = max(mx, int(labs[gi].max()))
        lut = -np.ones(mx + 2, dtype=np.int64)
        for gi, g in enumerate(meta["groups"]):
            if g["ii"] == inst["ii"]:
                lut[labs[gi]] = np.arange(len(labs[gi]), dtype=np.int64) + int(g["start"])
        elut[S(inst["name"])] = lut
    return nlut, elut


def mises_from(data, labels):
    col = {}
    for i, c in enumerate(labels):
        col[S(c)[-2:]] = data[:, i]
    z = np.zeros(data.shape[0])
    s11, s22, s33 = col.get("11", z), col.get("22", z), col.get("33", z)
    s12, s13, s23 = col.get("12", z), col.get("13", z), col.get("23", z)
    return np.sqrt(0.5 * ((s11 - s22) ** 2 + (s22 - s33) ** 2 + (s33 - s11) ** 2)
                   + 3.0 * (s12 ** 2 + s13 ** 2 + s23 ** 2))


def process_field(f, key, j, nsel, n_nodes, n_elems, nlut, elut, vars_, run, ids):
    names = [S(c) for c in f.componentLabels]
    if len(names) == 0:
        names = [key]
    nc = len(names)
    want_mises = "MISES" in [S(v) for v in f.validInvariants]
    nct = nc + (1 if want_mises else 0)
    acc = {}
    for b in f.bulkDataBlocks:
        inst = b.instance
        if inst is None:
            continue
        iname = S(inst.name)
        if S(b.position) == "NODAL":
            kind, lut, labels, n = "N", nlut.get(iname), b.nodeLabels, n_nodes
        else:
            kind, lut, labels, n = "E", elut.get(iname), b.elementLabels, n_elems
        if lut is None or labels is None or n == 0:
            continue
        labels = np.asarray(labels).ravel().astype(np.int64)
        data = np.asarray(b.data, dtype=np.float64)
        if data.ndim == 1:
            data = data.reshape(-1, 1)
        if data.shape[0] != labels.size:
            continue
        spn = 0
        try:
            sp = b.sectionPoint
            if sp is not None:
                spn = int(sp.number)
        except Exception:
            spn = 0
        bl = []
        try:
            if b.componentLabels is not None:
                bl = [S(c) for c in b.componentLabels]
        except Exception:
            bl = []
        if len(bl) != data.shape[1]:
            bl = names[:data.shape[1]]
            cols = list(range(len(bl)))
        else:
            cols = [names.index(c) if c in names else -1 for c in bl]
        good = (labels >= 0) & (labels < lut.size)
        idx = -np.ones(labels.size, dtype=np.int64)
        idx[good] = lut[labels[good]]
        m = idx >= 0
        if not m.any():
            continue
        idx = idx[m]
        data = data[m]
        a = acc.get((kind, spn))
        if a is None:
            a = [np.zeros((n, nct)), np.zeros((n, nct))]
            acc[(kind, spn)] = a
        cnt = np.bincount(idx, minlength=n).astype(np.float64)
        for c in range(len(cols)):
            t = cols[c]
            if t < 0:
                continue
            a[0][:, t] += np.bincount(idx, weights=data[:, c], minlength=n)
            a[1][:, t] += cnt
        if want_mises:
            mis = None
            try:
                bm = b.mises
                if bm is not None:
                    bm = np.asarray(bm, dtype=np.float64).ravel()
                    if bm.size == labels.size:
                        mis = bm[m]
            except Exception:
                mis = None
            if mis is None:
                mis = mises_from(data, bl)
            a[0][:, nc] += np.bincount(idx, weights=mis, minlength=n)
            a[1][:, nc] += cnt
    for kk in list(acc.keys()):
        kind, spn = kk
        a = acc[kk]
        vkey = "%s|%s|%d" % (key, kind, spn)
        v = vars_.get(vkey)
        if v is None:
            vid = ids[0]
            ids[0] += 1
            n = a[0].shape[0]
            mm = open_memmap(os.path.join(run, "var_%d.npy" % vid), mode="w+", dtype=np.float32,
                             shape=(nsel, n, nct))
            nanrow = np.empty((n, nct), dtype=np.float32)
            nanrow.fill(np.nan)
            for r in range(nsel):
                mm[r] = nanrow
            name = key
            if spn:
                name = "%s [section point %d]" % (key, spn)
            rec = {"id": vid, "name": name, "field": key, "kind": kind, "sp": spn,
                   "comps": names + (["Mises"] if want_mises else []), "type": S(f.type),
                   "file": "var_%d.npy" % vid, "def": "def_%d.npy" % vid, "desc": S(f.description)}
            v = {"rec": rec, "mm": mm, "defined": np.zeros((n, nct), dtype=bool)}
            vars_[vkey] = v
        old = np.seterr(divide="ignore", invalid="ignore")
        row = a[0] / a[1]
        np.seterr(**old)
        v["mm"][j] = row.astype(np.float32)
        v["defined"] |= (a[1] > 0)


def extract(cfg):
    out = S(cfg["out"])
    run = S(cfg["run"])
    meta = load_json(os.path.join(out, "meta.json"))
    sel = [int(i) for i in cfg["frames"]]
    want = [S(k) for k in cfg["fields"]]
    res_path = os.path.join(run, "results.json")
    res = None
    if os.path.exists(res_path):
        try:
            res = load_json(res_path)
            if [int(i) for i in res["frames"]] != sel:
                res = None
        except Exception:
            res = None
    if res is None:
        res = {"frames": sel, "vars": [], "fields_done": []}
    done = [S(k) for k in res["fields_done"]]
    todo = [k for k in want if k not in done]
    if len(todo) == 0:
        say("All requested variables are already cached.")
        return
    say("Opening the ODB...")
    odb = open_odb(cfg)
    nlut, elut = build_luts(out, meta)
    n_nodes = int(meta["n_nodes"])
    n_elems = int(meta["n_elems"])
    ids = [0]
    for r in res["vars"]:
        ids[0] = max(ids[0], int(r["id"]) + 1)
    vars_ = {}
    nsel = len(sel)
    say("Reading %d variables in %d frames..." % (len(todo), nsel))
    for j in range(nsel):
        fm = meta["frames"][sel[j]]
        frame = odb.steps[S(fm["step"])].frames[int(fm["fi"])]
        fos = frame.fieldOutputs
        keys = [S(k) for k in fos.keys()]
        for key in todo:
            if key not in keys:
                continue
            try:
                process_field(fos[key], key, j, nsel, n_nodes, n_elems, nlut, elut, vars_, run, ids)
            except Exception as exc:
                say("Warning: could not read %s in frame %d (%s)" % (key, sel[j], exc))
        progress(j + 1, nsel)
    for vkey in sorted(vars_.keys(), key=lambda q: vars_[q]["rec"]["id"]):
        v = vars_[vkey]
        v["mm"].flush()
        np.save(os.path.join(run, v["rec"]["def"]), v["defined"])
        res["vars"].append(v["rec"])
        v["mm"] = None
    res["fields_done"] = done + todo
    save_json(res_path, res)


def main():
    cfg = load_json(sys.argv[1])
    if cfg["mode"] == "scan":
        scan(cfg)
    else:
        extract(cfg)
    print("@@DONE")
    sys.stdout.flush()


main()
'''


# =============================================================================
# PART 3.  Running the extractor
# =============================================================================
def find_abaqus():
    """Looks for the Abaqus command on this computer. Returns '' if not found."""
    import shutil
    import glob
    names = ["abaqus", "abq"] + ["abq%d" % y for y in range(2032, 2015, -1)] + ["abq6144", "abq6141", "abq6132"]
    for n in names:
        try:
            p = shutil.which(n)
        except Exception:
            p = None
        if p:
            return p
    if IS_WIN:
        drives = ["%s:\\" % d for d in "CDEFGH" if os.path.exists("%s:\\" % d)]
        pats = []
        for d in drives:
            pats += [d + r"SIMULIA\Commands\abaqus.bat", d + r"SIMULIA\*\Commands\abaqus.bat",
                     d + r"SIMULIA\Commands\abq*.bat", d + r"SIMULIA\*\Commands\abq*.bat",
                     d + r"Program Files\Dassault Systemes\SIMULIA\Commands\abaqus.bat",
                     d + r"Program Files\Dassault Systemes\SIMULIA*\Commands\abaqus.bat",
                     d + r"Program Files\SIMULIA\Commands\abaqus.bat",
                     d + r"SIMULIA\EstProducts\*\win_b64\code\bin\ABQLauncher.exe",
                     d + r"Program Files\Dassault Systemes\SimulationServices\*\win_b64\code\bin\ABQLauncher.exe",
                     d + r"Program Files\Dassault Systemes\SIMULIA\*\EstProducts\*\win_b64\code\bin\ABQLauncher.exe"]
        for pat in pats:
            try:
                hits = sorted(glob.glob(pat))
            except Exception:
                hits = []
            if hits:
                return hits[-1]
    else:
        for pat in ("/opt/abaqus/Commands/abaqus", "/usr/SIMULIA/Commands/abaqus", "/opt/SIMULIA/Commands/abaqus",
                    "/var/DassaultSystemes/SIMULIA/Commands/abaqus", "/usr/local/bin/abaqus",
                    "/opt/DassaultSystemes/SIMULIA/Commands/abaqus"):
            if os.path.exists(pat):
                return pat
    return ""


def cache_dir_for(odb_path):
    import hashlib
    import re
    st = os.stat(odb_path)
    key = "%s|%d|%d" % (os.path.abspath(odb_path), st.st_size, int(st.st_mtime))
    h = hashlib.md5(key.encode("utf-8", "replace")).hexdigest()[:10]
    base = re.sub(r"[^A-Za-z0-9_-]", "_", os.path.splitext(os.path.basename(odb_path))[0])[:40]
    return os.path.join(CACHE_DIR, base + "_" + h)


def odb_for_abaqus(odb, cdir, on_msg=None):
    """Older Abaqus versions cannot open paths with accents or special characters.
    In that case a link (or a copy) with a plain name is made in the cache folder."""
    odb = os.path.abspath(odb)
    try:
        odb.encode("ascii")
        return odb
    except Exception:
        pass
    import shutil
    if not os.path.isdir(cdir):
        os.makedirs(cdir)
    plain = os.path.join(cdir, "model.odb")
    if os.path.exists(plain) and os.path.getsize(plain) == os.path.getsize(odb):
        return plain
    try:
        if os.path.exists(plain):
            os.remove(plain)
        os.link(odb, plain)
        return plain
    except Exception:
        pass
    if on_msg:
        on_msg("The file path has special characters. Making a copy with a plain name (one time only)...")
    shutil.copyfile(odb, plain)
    return plain


def _explain_failure(tail):
    text = "\n".join(tail).lower()
    if ".lck" in text or "locked" in text:
        return ("The ODB is locked. The job is still running, or Abaqus/CAE has it open.\n"
                "Wait for the job to finish (or close Abaqus/CAE) and try again.")
    if ("not recognized as an internal or external command" in text or "no such file" in text
            or "could not start" in text):
        return "The Abaqus command did not start. Check the 'Abaqus command' box in the window."
    if "newer version" in text or "later version" in text or "cannot be opened by this version" in text:
        return ("The ODB was made with a newer Abaqus than the one selected.\n"
                "Select the Abaqus command of the newest version installed.")
    if "license" in text:
        return "Abaqus reported a license problem. Check the Abaqus license server connection."
    if "memoryerror" in text or "out of memory" in text:
        return "Not enough memory. Load fewer outputs or a larger 'Load one frame every' value."
    if "no space left" in text or "errno 28" in text:
        return "The disk is full. Free some space or press 'Clear cache'."
    return ""


def run_extractor(cfg, abaqus_cmd, on_msg=None, on_progress=None):
    """Runs the extractor. Raises RuntimeError with a readable message if it fails."""
    import json
    out = cfg["out"]
    for d in (APP_DIR, out, cfg.get("run", out)):
        if not os.path.isdir(d):
            os.makedirs(d)
    script = os.path.join(APP_DIR, "odb_extractor.py")
    with open(script, "w") as f:
        f.write(EXTRACTOR)
    cfg_path = os.path.join(out, "job_%s.json" % cfg["mode"])
    with open(cfg_path, "w") as f:
        json.dump(cfg, f)
    if cfg.get("demo"):
        cmds = [[python_exe(), script, cfg_path]]
        env = clean_env()
        pkg = _current_pkg_dir()
        if pkg:
            env["PYTHONPATH"] = pkg + os.pathsep + env.get("PYTHONPATH", "")
    else:
        abaqus_cmd = (abaqus_cmd or "").strip().strip('"')
        if not abaqus_cmd or not os.path.exists(abaqus_cmd):
            found = find_abaqus()
            if found and not abaqus_cmd:
                abaqus_cmd = found
            elif not (abaqus_cmd and shutil_which(abaqus_cmd)):
                raise RuntimeError(
                    "The Abaqus command was not found.\n\nAbaqus must be installed on this computer to read "
                    ".odb files.\nPress the Browse button next to 'Abaqus command' and select the file\n"
                    "abaqus.bat  (usually in C:\\SIMULIA\\Commands)  on Windows, or  abaqus  on Linux.")
        cmds = [[abaqus_cmd, "python", script, cfg_path]]
        if IS_WIN:
            cmds.append(["cmd", "/d", "/c", "call", abaqus_cmd, "python", script, cfg_path])
        env = clean_env(drop_python_vars=True)
    tail, done = [], [False]

    def handle(line):
        if line.startswith("@@P "):
            parts = line.split()
            if on_progress:
                on_progress(int(parts[1]), int(parts[2]))
        elif line.startswith("@@MSG "):
            if on_msg:
                on_msg(line[6:])
        elif line.startswith("@@DONE"):
            done[0] = True
        else:
            tail.append(line)

    for cmd in cmds:
        del tail[:]
        rc, lines = run_cmd(cmd, handle, env=env, cwd=out)
        if done[0]:
            return
        if len(lines) == 1 and lines[0].startswith("Could not start"):
            tail.append(lines[0])
            continue       # try the next way of starting Abaqus
        break
    hint = _explain_failure(tail)
    raise RuntimeError("Reading the ODB failed.\n\n" + (hint + "\n\n" if hint else "") +
                       "Last messages from Abaqus:\n" + "\n".join(tail[-15:]))


def shutil_which(cmd):
    import shutil
    try:
        return shutil.which(cmd)
    except Exception:
        return None


def frame_selection(n_frames, stride):
    stride = max(1, int(stride))
    sel = list(range(0, n_frames, stride))
    if sel and sel[-1] != n_frames - 1:
        sel.append(n_frames - 1)
    return sel


def estimate_bytes(meta, field, n_sel):
    nodal = field["positions"] == ["NODAL"]
    n = meta["n_nodes"] if nodal else meta["n_elems"]
    nc = max(1, len(field["comps"])) + (1 if "MISES" in field["invariants"] else 0)
    return 4.0 * n * nc * n_sel


# =============================================================================
# PART 4.  Mesh handling: element type recognition and outer surface
# =============================================================================
# Faces of the solid elements in Abaqus node order (corner nodes only, 0-based)
SOLID_FACES = {
    "tet": ([(0, 1, 2), (0, 3, 1), (1, 3, 2), (2, 3, 0)], []),
    "pyramid": ([(0, 4, 1), (1, 4, 2), (2, 4, 3), (3, 4, 0)], [(0, 1, 2, 3)]),
    "wedge": ([(0, 1, 2), (3, 5, 4)], [(0, 3, 4, 1), (1, 4, 5, 2), (2, 5, 3, 0)]),
    "hex": ([], [(0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)]),
}
SOLID_BY_NODES = {4: "tet", 10: "tet", 5: "pyramid", 13: "pyramid", 6: "wedge", 15: "wedge",
                  8: "hex", 20: "hex", 27: "hex"}
SOLID_PREFIX = ("C3D", "DC3D", "AC3D", "SC6", "SC8", "CSS8", "COH3D", "GK3D", "Q3D", "EMC3D", "DCC3D")
LINE_PREFIX = ("B2", "B3", "T2D", "T3D", "PIPE", "ELBOW", "CONN", "SPRING", "DASHPOT", "SAX", "MAX",
               "FRAME", "GAP", "ITS", "JOINTC", "R2D2", "RAX2", "DC1D", "AC1D", "SFMAX", "DCOUP", "FLINK")
SURF_PREFIX = ("S3", "S4", "S8", "S9", "STRI", "M3D", "R3D", "SFM3D", "DS", "CPE", "CPS", "CAX", "CGAX",
               "DC2D", "DCAX", "AC2D", "ACAX", "COH2D", "COHAX", "WARP2D", "DCC2D", "DCCAX", "Q2D",
               "EMC2D", "GK2D", "GKPS", "GKPE", "GKAX")


def classify(etype, nn):
    """Returns (class, shape) from the Abaqus element name and its number of nodes.
    class is 'solid', 'surface', 'line' or 'skip'."""
    t = etype.upper()
    if nn <= 1:
        return "skip", "point"
    if t.startswith(SOLID_PREFIX) and nn in SOLID_BY_NODES:
        return "solid", SOLID_BY_NODES[nn]
    if t.startswith(LINE_PREFIX) or nn == 2:
        return "line", "line"
    if t.startswith(SURF_PREFIX):
        return "surface", ("tri" if nn in (3, 6, 7) else "quad")
    # unknown name: decide with the number of nodes
    if "3D" in t and nn in SOLID_BY_NODES:
        return "solid", SOLID_BY_NODES[nn]
    if nn in (3, 6, 7):
        return "surface", "tri"
    if nn in (4, 8, 9):
        return "surface", "quad"
    return "skip", "unknown"


CLASS_TEXT = {"solid": "3D solid", "surface": "shell / 2D", "line": "beam / truss / connector",
              "skip": "not drawn"}


def _exterior(faces, elems, n_nodes):
    """Keeps the faces that belong to one element only (the outer skin)."""
    import numpy as np
    if len(faces) == 0:
        return np.zeros((0, faces.shape[1] if faces.ndim == 2 else 3), dtype=np.int64), np.zeros(0, np.int64)
    key = np.sort(faces, axis=1).astype(np.int64)
    N = np.int64(n_nodes)
    k1 = key[:, 0] * N + key[:, 1]
    k2 = key[:, 2] if key.shape[1] == 3 else key[:, 2] * N + key[:, 3]
    order = np.lexsort((k2, k1))
    s1, s2 = k1[order], k2[order]
    new = np.ones(len(order), dtype=bool)
    new[1:] = (s1[1:] != s1[:-1]) | (s2[1:] != s2[:-1])
    nxt = np.ones(len(order), dtype=bool)
    nxt[:-1] = new[1:]
    keep = np.sort(order[new & nxt])
    return faces[keep], elems[keep]


def build_surface(groups, conns, n_nodes):
    """groups: list of dicts (etype, nn, n, start). conns: list of connectivity arrays.
    Returns triangles, their parent element, mesh edges, line segments, their parent element."""
    import numpy as np
    e3 = np.zeros((0, 3), np.int64)
    e4 = np.zeros((0, 4), np.int64)
    e2 = np.zeros((0, 2), np.int64)
    st, st_e, sq, sq_e = [e3], [np.zeros(0, np.int64)], [e4], [np.zeros(0, np.int64)]
    ft, ft_e, fq, fq_e = [e3], [np.zeros(0, np.int64)], [e4], [np.zeros(0, np.int64)]
    sg, sg_e = [e2], [np.zeros(0, np.int64)]
    for g, conn in zip(groups, conns):
        conn = np.asarray(conn, dtype=np.int64)
        eid = g["start"] + np.arange(conn.shape[0], dtype=np.int64)
        cls, shape = classify(g["etype"], g["nn"])
        if cls == "solid":
            tf, qf = SOLID_FACES[shape]
            for f in tf:
                st.append(conn[:, list(f)])
                st_e.append(eid)
            for f in qf:
                sq.append(conn[:, list(f)])
                sq_e.append(eid)
        elif cls == "surface":
            if shape == "tri":
                ft.append(conn[:, :3])
                ft_e.append(eid)
            else:
                fq.append(conn[:, :4])
                fq_e.append(eid)
        elif cls == "line":
            pairs = [(0, 1), (1, 2)] if g["nn"] == 3 else [(0, 1)]
            for a, b in pairs:
                sg.append(conn[:, [a, b]])
                sg_e.append(eid)
    xt, xt_e = _exterior(np.vstack(st), np.concatenate(st_e), n_nodes)
    xq, xq_e = _exterior(np.vstack(sq), np.concatenate(sq_e), n_nodes)
    T = np.vstack([xt] + ft)
    T_e = np.concatenate([xt_e] + ft_e)
    Q = np.vstack([xq] + fq)
    Q_e = np.concatenate([xq_e] + fq_e)
    tris = np.vstack([T, Q[:, [0, 1, 2]], Q[:, [0, 2, 3]]])
    tri_elem = np.concatenate([T_e, Q_e, Q_e])
    ed = np.vstack([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]],
                    Q[:, [0, 1]], Q[:, [1, 2]], Q[:, [2, 3]], Q[:, [3, 0]]])
    if len(ed):
        ed = np.sort(ed, axis=1)
        code = np.unique(ed[:, 0] * np.int64(n_nodes) + ed[:, 1])
        ed = np.stack([code // n_nodes, code % n_nodes], axis=1)
    return tris, tri_elem, ed, np.vstack(sg), np.concatenate(sg_e)


# =============================================================================
# PART 5.  The data model used by the viewer
# =============================================================================
def _enc(a, dt="f4"):
    """Packs a numpy array the way plotly.js reads it fastest."""
    import base64
    import numpy as np
    a = np.ascontiguousarray(a, dtype=np.dtype("<" + dt))
    return {"dtype": dt, "bdata": base64.b64encode(a.tobytes()).decode("ascii")}


class Model(object):
    def __init__(self, cdir, run):
        import json
        import numpy as np
        self.np = np
        self.dir = cdir
        with open(os.path.join(cdir, "meta.json")) as f:
            self.meta = json.load(f)
        with open(os.path.join(run, "results.json")) as f:
            self.res = json.load(f)
        self.name = os.path.basename(self.meta.get("odb", "model"))
        self.frames = [self.meta["frames"][i] for i in self.res["frames"]]
        self.nf = len(self.frames)
        nodes = np.load(os.path.join(cdir, "nodes.npy"))
        self.groups = self.meta["groups"]
        conns = [np.load(os.path.join(cdir, "conn_%d.npy" % i)) for i in range(len(self.groups))]
        tris, tri_elem, edges, segs, seg_elem = build_surface(self.groups, conns, len(nodes))
        # keep only the nodes that are drawn
        used = np.unique(np.concatenate([tris.ravel(), segs.ravel()]))
        remap = -np.ones(len(nodes), dtype=np.int64)
        remap[used] = np.arange(len(used))
        self.used = used
        self.xyz0 = nodes[used].astype(np.float64)
        self.tris = remap[tris]
        self.tri_elem = tri_elem
        self.edges = remap[edges]
        self.segs = remap[segs]
        self.seg_elem = seg_elem
        self.summary = []
        for g in self.groups:
            cls, shape = classify(g["etype"], g["nn"])
            self.summary.append((g["etype"], g["n"], CLASS_TEXT[cls], g["instance"]))
        # variables
        self.vars = []
        for rec in self.res["vars"]:
            v = dict(rec)
            v["mm"] = np.load(os.path.join(run, rec["file"]), mmap_mode="r")
            v["defined"] = np.load(os.path.join(run, rec["def"]))
            v["nreal"] = len(rec["comps"]) - (1 if "Mises" in rec["comps"] else 0)
            self.vars.append(v)
        names = [v["name"] for v in self.vars]
        for v in self.vars:
            if names.count(v["name"]) > 1:
                v["name"] += " (nodes)" if v["kind"] == "N" else " (elements)"
        self.U = None
        for v in self.vars:
            if v["field"] == "U" and v["kind"] == "N" and v["sp"] == 0:
                self.U = v
        self.is2d = bool(np.ptp(self.xyz0[:, 2]) == 0.0 and (self.U is None or self.U["nreal"] < 3))
        # dropdown options
        self.options = []
        self.default = None
        for vi, v in enumerate(self.vars):
            comps = v["comps"]
            if v["type"] == "VECTOR" and v["nreal"] > 1:
                self.options.append({"label": v["name"] + ": Magnitude", "value": "%d:mag" % vi})
            if "Mises" in comps:
                self.options.append({"label": v["name"] + ": Mises", "value": "%d:%d" % (vi, comps.index("Mises"))})
            for ci, c in enumerate(comps):
                if c == "Mises" or not v["defined"][:, ci].any():
                    continue
                label = v["name"] if (v["nreal"] == 1 and c == v["field"]) else v["name"] + ": " + c
                self.options.append({"label": label, "value": "%d:%d" % (vi, ci)})
        labels = dict((o["label"], o["value"]) for o in self.options)
        for want in ("S: Mises", "U: Magnitude"):
            if want in labels:
                self.default = labels[want]
                break
        if self.default is None and self.options:
            self.default = self.options[0]["value"]
        self._range = {}
        self._split = {}
        self._umax = None

    # ---- values ------------------------------------------------------------
    def parse(self, sel):
        vi, c = sel.split(":")
        return self.vars[int(vi)], c

    def label(self, sel):
        for o in self.options:
            if o["value"] == sel:
                return o["label"]
        return sel

    def values(self, sel, frame):
        np = self.np
        v, c = self.parse(sel)
        row = v["mm"][frame]
        if c == "mag":
            a = np.asarray(row[:, :v["nreal"]], dtype=np.float64)
            return np.sqrt((a * a).sum(axis=1))
        return np.asarray(row[:, int(c)], dtype=np.float64)

    def defined(self, sel):
        v, c = self.parse(sel)
        if c == "mag":
            return v["defined"][:, :v["nreal"]].all(axis=1)
        return v["defined"][:, int(c)]

    def series(self, sel, idx):
        np = self.np
        v, c = self.parse(sel)
        if c == "mag":
            a = np.asarray(v["mm"][:, idx, :v["nreal"]], dtype=np.float64)
            return np.sqrt((a * a).sum(axis=1))
        return np.asarray(v["mm"][:, idx, int(c)], dtype=np.float64)

    def split(self, sel):
        """Faces that have data for this variable, and faces that do not."""
        np = self.np
        if sel not in self._split:
            v, _ = self.parse(sel)
            d = self.defined(sel)
            if v["kind"] == "E":
                has = d[self.tri_elem] if len(self.tri_elem) else np.zeros(0, bool)
            else:
                has = d[self.used][self.tris].all(axis=1) if len(self.tris) else np.zeros(0, bool)
            if v["kind"] == "E":
                shas = d[self.seg_elem] if len(self.seg_elem) else np.zeros(0, bool)
            else:
                shas = d[self.used][self.segs].all(axis=1) if len(self.segs) else np.zeros(0, bool)
            if len(self._split) > 30:
                self._split.clear()
            self._split[sel] = (np.nonzero(has)[0], np.nonzero(~has)[0],
                                np.nonzero(shas)[0], np.nonzero(~shas)[0])
        return self._split[sel]

    def global_range(self, sel):
        np = self.np
        if sel not in self._range:
            lo, hi = np.inf, -np.inf
            for f in range(self.nf):
                a = self.values(sel, f)
                a = a[np.isfinite(a)]
                if a.size:
                    lo, hi = min(lo, float(a.min())), max(hi, float(a.max()))
            self._range[sel] = (lo, hi)
        return self._range[sel]

    def color_range(self, sel, mode, frame, cmin, cmax):
        np = self.np
        if mode == "manual" and cmin is not None and cmax is not None:
            lo, hi = float(cmin), float(cmax)
        elif mode == "frame":
            a = self.values(sel, frame)
            a = a[np.isfinite(a)]
            lo, hi = (float(a.min()), float(a.max())) if a.size else (0.0, 1.0)
        else:
            lo, hi = self.global_range(sel)
        if not (np.isfinite(lo) and np.isfinite(hi)):
            lo, hi = 0.0, 1.0
        if hi <= lo:
            pad = abs(lo) * 1e-6 + 1e-12
            lo, hi = lo - pad, hi + pad
        return lo, hi

    # ---- geometry ----------------------------------------------------------
    def coords(self, frame, deform, scale):
        np = self.np
        xyz = self.xyz0
        if deform and self.U is not None:
            k = min(3, self.U["nreal"])
            u = np.nan_to_num(np.asarray(self.U["mm"][frame][self.used, :k], dtype=np.float64))
            xyz = xyz.copy()
            xyz[:, :k] += float(scale) * u
        return xyz

    def bbox(self, deform, scale):
        np = self.np
        lo = self.xyz0.min(axis=0)
        hi = self.xyz0.max(axis=0)
        if deform and self.U is not None:
            # exact envelope of the deformed shape over all frames, so the box never moves
            key = float(scale)
            if self._umax is None or self._umax[0] != key:
                for f in range(self.nf):
                    c = self.coords(f, True, key)
                    lo = np.minimum(lo, c.min(axis=0))
                    hi = np.maximum(hi, c.max(axis=0))
                self._umax = (key, lo, hi)
            lo, hi = self._umax[1], self._umax[2]
        span = float((hi - lo).max()) or 1.0
        pad = 0.01 * span
        return lo - pad, hi + pad

    def pair_xyz(self, xyz, pairs):
        """x, y, z arrays for separate line segments (NaN between segments)."""
        np = self.np
        out = np.full((len(pairs), 3, 3), np.nan)
        out[:, 0, :] = xyz[pairs[:, 0]]
        out[:, 1, :] = xyz[pairs[:, 1]]
        out = out.reshape(-1, 3)
        return out[:, 0], out[:, 1], out[:, 2]

    def face_values(self, sel, frame):
        """Returns (intensity for the colored mesh, colors for line elements)."""
        np = self.np
        v, _ = self.parse(sel)
        vals = self.values(sel, frame)
        idx0, _, s0, _ = self.split(sel)
        line = np.full((len(s0), 3), np.nan)
        if v["kind"] == "E":
            inten = vals[self.tri_elem[idx0]]
            if len(s0):
                line[:, 0] = vals[self.seg_elem[s0]]
                line[:, 1] = line[:, 0]
        else:
            nv = vals[self.used]
            inten = nv
            if len(s0):
                line[:, 0] = nv[self.segs[s0, 0]]
                line[:, 1] = nv[self.segs[s0, 1]]
        return inten, line.ravel()

    def frame_text(self, i):
        f = self.frames[i]
        return "Frame %d of %d   |   Step: %s   |   Increment %d   |   Step time %.6g   |   Total time %.6g" % (
            i + 1, self.nf, f["step"], f["inc"], f["value"], f["total"])

    # ---- picking -----------------------------------------------------------
    def node_name(self, gnode):
        np = self.np
        for inst in self.meta["instances"]:
            s = inst["node_start"]
            if s <= gnode < s + inst["n_nodes"]:
                lab = np.load(os.path.join(self.dir, "nlab_%d.npy" % inst["ii"]), mmap_mode="r")
                return "node %d (%s)" % (int(lab[gnode - s]), inst["name"])
        return "node"

    def elem_name(self, gelem):
        np = self.np
        for gi, g in enumerate(self.groups):
            if g["start"] <= gelem < g["start"] + g["n"]:
                lab = np.load(os.path.join(self.dir, "elab_%d.npy" % gi), mmap_mode="r")
                return "element %d (%s, %s)" % (int(lab[gelem - g["start"]]), g["etype"], g["instance"])
        return "element"

    def pick(self, sel, curve, number):
        """Node or element that was clicked. curve = trace number, number = point number
        reported by plotly (vertex for nodal colors, triangle for element colors).
        Returns (index, name) or (None, "")."""
        v, _ = self.parse(sel)
        if number is None or number < 0:
            return None, ""
        if curve == 3:                      # line elements: 3 points per segment
            s0 = self.split(sel)[2]
            end = min(number % 3, 1)
            if number // 3 >= len(s0):
                return None, ""
            s = s0[number // 3]
            if v["kind"] == "N":
                g = int(self.used[self.segs[s, end]])
                return g, self.node_name(g)
            e = int(self.seg_elem[s])
            return e, self.elem_name(e)
        if curve != 0:
            return None, ""
        if v["kind"] == "N":
            if number >= len(self.used):
                return None, ""
            g = int(self.used[number])
            return g, self.node_name(g)
        idx0 = self.split(sel)[0]
        if number >= len(idx0):
            return None, ""
        e = int(self.tri_elem[idx0[number]])
        return e, self.elem_name(e)


# =============================================================================
# PART 6.  The Plotly / Dash viewer
# =============================================================================
SCENE_X0 = 0.07
BOX_HALF = 0.5     # plotly draws the 3D box from -aspectratio/2 to +aspectratio/2
FIT_MARGIN = 1.12   # left part of the 3D panel kept for the color bar

# Runs in the browser before every redraw of the 3D view. It keeps the camera the user set
# (rotation, zoom, pan), and fits the model to the window on the first draw or when a view
# button is pressed.
CAMERA_JS = """
function(fig, nFit, nIso, nXY, nXZ, nYZ) {
    var NO = window.dash_clientside.no_update;
    if (!fig || !fig.layout) { return NO; }
    var C = window.dash_clientside.callback_context;
    var trig = (C && C.triggered) ? C.triggered.map(function (t) { return t.prop_id.split('.')[0]; }) : [];
    var views = {
        'view-iso': [[1.3, 0.9, 1.5], [0, 1, 0]], 'view-xy': [[0, 0, 1], [0, 1, 0]],
        'view-xz': [[0, 1, 0], [0, 0, -1]], 'view-yz': [[1, 0, 0], [0, 1, 0]]
    };
    var box = document.getElementById('g3d');
    var gd = box ? box.querySelector('.js-plotly-plot') : null;
    var sc = (gd && gd._fullLayout && gd._fullLayout.scene) ? gd._fullLayout.scene._scene : null;
    var want = null;
    for (var i = 0; i < trig.length; i++) {
        if (trig[i] === 'view-fit' || views[trig[i]]) { want = trig[i]; }
    }
    function fitCam(dir, up, ar, dom, proj) {
        var W = box ? box.clientWidth : 1000, H = box ? box.clientHeight : 600;
        dom = dom || {x: [0, 1], y: [0, 1]};
        var vw = Math.max(50, W * (dom.x[1] - dom.x[0])), vh = Math.max(50, H * (dom.y[1] - dom.y[0]));
        return {eye: odbFitEye(dir, up, ar, vw / vh, proj), up: {x: up[0], y: up[1], z: up[2]},
                center: {x: 0, y: 0, z: 0}, projection: {type: proj}};
    }
    // view buttons: move the camera with relayout and leave the figure alone
    if (want) {
        if (!sc) { return NO; }
        var fl = gd._fullLayout.scene, c0 = sc.getCamera(), dir, up;
        if (views[want]) { dir = views[want][0]; up = views[want][1]; }
        else {
            dir = [c0.eye.x - c0.center.x, c0.eye.y - c0.center.y, c0.eye.z - c0.center.z];
            up = [c0.up.x, c0.up.y, c0.up.z];
        }
        Plotly.relayout(gd, {'scene.camera': fitCam(dir, up, fl.aspectratio, fl.domain, c0.projection.type)});
        return NO;
    }
    // New data. Plotly loses the mouse rotation if a redraw changes the 3D scene, so the redraw
    // reuses the scene exactly as it is now, and any change of box size is applied afterwards
    // with relayout, which is safe.
    if (sc && window._odbCamReady && gd.layout && gd.layout.scene) {
        try { sc.saveLayout(gd.layout); } catch (e) {}
        var cur = JSON.parse(JSON.stringify(gd.layout.scene));
        var srv = fig.layout.scene, upd = {}, n = 0;
        function differ(a, b) { return JSON.stringify(a) !== JSON.stringify(b); }
        if (differ(srv.aspectratio, window._odbAR)) { upd['scene.aspectratio'] = srv.aspectratio; n++; }
        ['xaxis', 'yaxis', 'zaxis'].forEach(function (ax) {
            ['range', 'nticks', 'visible'].forEach(function (k) {
                var v = srv[ax] ? srv[ax][k] : undefined;
                if (v !== undefined && differ(v, (window._odbAX || {})[ax + k])) { upd['scene.' + ax + '.' + k] = v; n++; }
            });
        });
        odbRemember(srv);
        if (n) {
            var done = false;
            var apply = function () { if (!done) { done = true; Plotly.relayout(gd, upd); } };
            gd.once('plotly_afterplot', function () { setTimeout(apply, 0); });
            setTimeout(apply, 400);
        }
        var lay1 = Object.assign({}, fig.layout, {scene: cur});
        return Object.assign({}, fig, {layout: lay1});
    }
    // first drawing: fit the model to the window
    var sc0 = fig.layout.scene, e0 = sc0.camera.eye, u0 = sc0.camera.up;
    var cam = fitCam([e0.x, e0.y, e0.z], [u0.x, u0.y, u0.z], sc0.aspectratio, sc0.domain,
                     (sc0.camera.projection || {}).type || 'perspective');
    window._odbCamReady = true;
    odbRemember(sc0);
    var lay = Object.assign({}, fig.layout);
    lay.scene = Object.assign({}, sc0, {camera: cam});
    return Object.assign({}, fig, {layout: lay});
}
"""

# Distance that makes the whole bounding box visible (plotly 3D uses a 45 degree field of view
# and a box that spans -aspectratio..+aspectratio around the center).
FIT_JS = """
(function () {
    document.addEventListener('mousedown', function (e) {
        window._odbDown = [e.clientX, e.clientY]; window._odbMouseDown = true; window._odbDragged = false;
    }, true);
    document.addEventListener('mousemove', function (e) {
        var d = window._odbDown;
        if (window._odbMouseDown && d && Math.abs(e.clientX - d[0]) + Math.abs(e.clientY - d[1]) > 4) {
            window._odbDragged = true;
        }
    }, true);
    document.addEventListener('mouseup', function () { window._odbMouseDown = false; }, true);
})();
window.odbRemember = function (srv) {
    window._odbAR = JSON.parse(JSON.stringify(srv.aspectratio));
    window._odbAX = {};
    ['xaxis', 'yaxis', 'zaxis'].forEach(function (ax) {
        ['range', 'nticks', 'visible'].forEach(function (k) {
            if (srv[ax] && srv[ax][k] !== undefined) { window._odbAX[ax + k] = JSON.parse(JSON.stringify(srv[ax][k])); }
        });
    });
};
window.odbFitEye = function (dir, up, ar, aspect, proj) {
    function norm(v) { var n = Math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) || 1; return [v[0]/n, v[1]/n, v[2]/n]; }
    function cross(a, b) { return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]; }
    function dot(a, b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    var f = norm(dir);
    var r = cross(up, f);
    if (Math.sqrt(dot(r, r)) < 1e-6) { r = cross([0, 0, 1], f); }
    r = norm(r);
    var u = cross(f, r);
    var tv = Math.tan(Math.PI / 8), th = tv * aspect;
    var h = [ar.x * BOXHALF, ar.y * BOXHALF, ar.z * BOXHALF];
    var d = 0.1;
    for (var i = 0; i < 8; i++) {
        var p = [(i & 1 ? 1 : -1) * h[0], (i & 2 ? 1 : -1) * h[1], (i & 4 ? 1 : -1) * h[2]];
        var depth = proj === 'orthographic' ? 0 : dot(p, f);
        d = Math.max(d, depth + Math.abs(dot(p, u)) / tv, depth + Math.abs(dot(p, r)) / th);
    }
    d = d * FITMARGIN;
    return {x: f[0] * d, y: f[1] * d, z: f[2] * d};
};
"""

COLORMAPS = ["Turbo", "Jet", "Rainbow", "Viridis", "Plasma", "Cividis", "RdBu_r", "Greys"]


def _colorscale(name):
    try:
        import plotly.colors as pc
        return [[float(p), c] for p, c in pc.get_colorscale(name or "Turbo")]
    except Exception:
        return "Jet"


def build_figure(m, frame, sel, deform, scale, show_edges, cmap, rmode, cmin, cmax):
    np = m.np
    v, _ = m.parse(sel)
    xyz = m.coords(frame, deform, scale)
    idx0, idx1, s0, s1 = m.split(sel)
    inten, linecol = m.face_values(sel, frame)
    lo, hi = m.color_range(sel, rmode, frame, cmin, cmax)
    X, Y, Z = _enc(xyz[:, 0], "f8"), _enc(xyz[:, 1], "f8"), _enc(xyz[:, 2], "f8")
    light = {"ambient": 0.85, "diffuse": 0.35, "specular": 0.02, "roughness": 0.9, "fresnel": 0.0}
    t0 = m.tris[idx0]
    t1 = m.tris[idx1]
    name = m.label(sel)
    data = [
        {"type": "mesh3d", "x": X, "y": Y, "z": Z,
         "i": _enc(t0[:, 0], "i4"), "j": _enc(t0[:, 1], "i4"), "k": _enc(t0[:, 2], "i4"),
         "intensity": _enc(inten), "intensitymode": "cell" if v["kind"] == "E" else "vertex",
         "coloraxis": "coloraxis", "flatshading": True, "lighting": light, "name": name,
         "hovertemplate": name + "<br>%{intensity:.5g}<extra></extra>",
         "visible": bool(len(t0))},
        {"type": "mesh3d", "x": X, "y": Y, "z": Z,
         "i": _enc(t1[:, 0], "i4"), "j": _enc(t1[:, 1], "i4"), "k": _enc(t1[:, 2], "i4"),
         "color": "#c9ccd1", "flatshading": True, "lighting": light, "hoverinfo": "skip",
         "name": "no data", "visible": bool(len(t1))},
    ]
    ex, ey, ez = m.pair_xyz(xyz, m.edges) if show_edges else ([], [], [])
    data.append({"type": "scatter3d", "mode": "lines", "x": _enc(ex, "f8"), "y": _enc(ey, "f8"),
                 "z": _enc(ez, "f8"), "line": {"color": "#30343a", "width": 1.5},
                 "hoverinfo": "skip", "visible": bool(show_edges and len(m.edges))})
    lx, ly, lz = m.pair_xyz(xyz, m.segs[s0])
    data.append({"type": "scatter3d", "mode": "lines", "x": _enc(lx, "f8"), "y": _enc(ly, "f8"),
                 "z": _enc(lz, "f8"), "line": {"color": _enc(linecol), "coloraxis": "coloraxis", "width": 7},
                 "name": name, "hovertemplate": name + "<br>%{line.color:.5g}<extra></extra>",
                 "visible": bool(len(s0))})
    lx, ly, lz = m.pair_xyz(xyz, m.segs[s1])
    data.append({"type": "scatter3d", "mode": "lines", "x": _enc(lx, "f8"), "y": _enc(ly, "f8"),
                 "z": _enc(lz, "f8"), "line": {"color": "#b4b8bf", "width": 5},
                 "name": "no data", "hoverinfo": "skip", "visible": bool(len(s1))})
    blo, bhi = m.bbox(deform, scale)
    span = np.maximum(bhi - blo, 1e-30)
    ratio = np.maximum(span / span.max(), 0.02)
    axis = {"showbackground": False, "gridcolor": "#e3e5e8", "zerolinecolor": "#c9ccd1"}
    scene = {
        "xaxis": dict(axis, range=[float(blo[0]), float(bhi[0])], title={"text": "X"},
                      nticks=int(max(2, round(8 * ratio[0])))),
        "yaxis": dict(axis, range=[float(blo[1]), float(bhi[1])], title={"text": "Y"},
                      nticks=int(max(2, round(8 * ratio[1])))),
        "zaxis": dict(axis, range=[float(blo[2]), float(bhi[2])], title={"text": "Z"},
                      nticks=int(max(2, round(8 * ratio[2])))),
        "aspectmode": "manual", "dragmode": "orbit",
        "aspectratio": {"x": float(ratio[0]), "y": float(ratio[1]), "z": float(ratio[2])},
    }
    scene["domain"] = {"x": [SCENE_X0, 1.0], "y": [0.0, 1.0]}
    if m.is2d:
        scene["camera"] = {"eye": {"x": 0, "y": 0, "z": 2.2}, "up": {"x": 0, "y": 1, "z": 0},
                           "projection": {"type": "orthographic"}}
        scene["zaxis"]["visible"] = False
    else:
        scene["camera"] = {"eye": {"x": 1.3, "y": 0.9, "z": 1.5}, "up": {"x": 0, "y": 1, "z": 0},
                           "projection": {"type": "perspective"}}
    layout = {
        "uirevision": "keep", "autosize": True, "margin": {"l": 0, "r": 0, "t": 0, "b": 0}, "showlegend": False,
        "paper_bgcolor": "white", "scene": scene,
        "coloraxis": {"colorscale": _colorscale(cmap), "cmin": lo, "cmax": hi,
                      "colorbar": {"title": {"text": name, "side": "top"}, "len": 0.8, "thickness": 18,
                                   "tickformat": ".4g", "x": 0.005, "xanchor": "left"}},
    }
    return {"data": data, "layout": layout}


def make_app(m):
    import logging
    import numpy as np
    from dash import Dash, dcc, html, Input, Output, State, Patch, ctx, no_update
    logging.getLogger("werkzeug").setLevel(logging.ERROR)
    app = Dash(__name__, title="%s - %s" % (m.name, APP_NAME), update_title=None)
    nf = m.nf
    step = max(1, nf // 10)
    marks = dict((i, str(i + 1)) for i in sorted(set(list(range(0, nf, step)) + [nf - 1])))
    lab = {"fontWeight": 600, "fontSize": "13px", "marginTop": "14px", "marginBottom": "4px", "display": "block"}
    small = {"fontSize": "12px", "color": "#555"}
    btn = {"padding": "6px 12px", "border": "1px solid #b8bcc4", "borderRadius": "6px",
           "background": "#f5f6f8", "cursor": "pointer", "fontSize": "14px"}
    vbtn = dict(btn, padding="4px 0", fontSize="13px", flex="1")
    rows = [html.Tr([html.Th("Type"), html.Th("Count"), html.Th("Kind")])]
    for etype, n, kind, inst in m.summary:
        rows.append(html.Tr([html.Td(etype), html.Td("%d" % n), html.Td(kind)], title=inst))
    opts0 = ["edges"] if len(m.edges) <= 150000 else []
    side = html.Div(style={"width": "300px", "minWidth": "300px", "padding": "14px 16px", "overflowY": "auto", "boxSizing": "border-box",
                           "borderRight": "1px solid #dde0e5", "background": "#fafbfc"}, children=[
        html.Div(m.name, style={"fontWeight": 700, "fontSize": "16px", "wordBreak": "break-all"}),
        html.Div("%d nodes, %d elements, %d frames" % (m.meta["n_nodes"], m.meta["n_elems"], nf), style=small),
        html.Label("Variable for the colors", style=lab),
        dcc.Dropdown(id="var", options=m.options, value=m.default, clearable=False,
                     optionHeight=30, maxHeight=420),
        html.Label("Display", style=lab),
        dcc.Checklist(id="opts", value=opts0, options=[
            {"label": " Deformed shape", "value": "deform", "disabled": m.U is None},
            {"label": " Mesh edges", "value": "edges"}], labelStyle={"display": "block"}),
        html.Div([html.Button(t, id=i, style=vbtn, title=h) for t, i, h in (
            ("Fit", "view-fit", "Zoom to fit the model in the window"),
            ("Iso", "view-iso", "Isometric view"), ("XY", "view-xy", "Look along Z"),
            ("XZ", "view-xz", "Look along Y"), ("YZ", "view-yz", "Look along X"))],
            style={"display": "flex", "gap": "4px", "marginTop": "8px"}),
        html.Div([html.Span("Deformation scale  ", style={"fontSize": "13px"}),
                  dcc.Input(id="scale", type="number", value=1, debounce=True, style={"width": "90px"})],
                 style={"marginTop": "6px"}),
        html.Label("Color map", style=lab),
        dcc.Dropdown(id="cmap", options=COLORMAPS, value="Turbo", clearable=False),
        html.Label("Color limits", style=lab),
        dcc.RadioItems(id="rmode", value="global", options=[
            {"label": " Min/max of all frames", "value": "global"},
            {"label": " Min/max of current frame", "value": "frame"},
            {"label": " Manual", "value": "manual"}], labelStyle={"display": "block"}),
        html.Div([dcc.Input(id="cmin", type="number", placeholder="min", debounce=True, style={"width": "110px"}),
                  html.Span("  "),
                  dcc.Input(id="cmax", type="number", placeholder="max", debounce=True, style={"width": "110px"})],
                 style={"marginTop": "6px"}),
        html.Label("Element types in this model", style=lab),
        html.Table(rows, style={"fontSize": "12px", "width": "100%", "textAlign": "left"}),
        html.Div("Drag to rotate. Scroll to zoom. Right-drag to pan. "
                 "Click on the model to plot the history of the variable at that point.",
                 style=dict(small, marginTop="16px")),
    ])
    main = html.Div(style={"flex": "1", "display": "flex", "flexDirection": "column", "minWidth": "0"}, children=[
        dcc.Graph(id="g3d", style={"height": "calc(100vh - 330px)", "minHeight": "260px"},
                  config={"displaylogo": False, "scrollZoom": True}),
        html.Div(style={"padding": "6px 18px 2px 18px", "borderTop": "1px solid #dde0e5"}, children=[
            html.Div(id="frame-label", style={"fontSize": "13px", "marginBottom": "6px"}),
            html.Div(style={"display": "flex", "alignItems": "center", "gap": "8px"}, children=[
                html.Button("\u25C0", id="prev", style=btn, title="Previous frame"),
                html.Button("\u25B6 Play", id="play", style=dict(btn, minWidth="90px")),
                html.Button("\u25B6", id="next", style=btn, title="Next frame"),
                dcc.Dropdown(id="speed", clearable=False, value=500, style={"width": "110px"}, options=[
                    {"label": "Slow", "value": 1000}, {"label": "Normal", "value": 500},
                    {"label": "Fast", "value": 200}]),
                html.Div(dcc.Slider(id="frame", min=0, max=max(nf - 1, 0), step=1, value=0, marks=marks,
                                    updatemode="drag" if len(m.tris) <= 150000 else "mouseup"),
                         style={"flex": "1"}),
            ]),
        ]),
        dcc.Graph(id="hist", style={"height": "240px"}, config={"displaylogo": False}),
        dcc.Interval(id="tick", interval=500, disabled=True),
        dcc.Store(id="fig"),
        dcc.Store(id="pick"),
    ])
    app.index_string = app.index_string.replace(
        "{%scripts%}", "{%scripts%}<script>" + FIT_JS.replace("BOXHALF", "%g" % BOX_HALF)
        .replace("FITMARGIN", "%g" % FIT_MARGIN) + "</script>")
    app.clientside_callback(
        """function(c) {
            var NO = window.dash_clientside.no_update;
            if (!c) { return NO; }
            // wait until the button is released, then ignore it if the mouse moved (that was a rotation)
            return new Promise(function (resolve) {
                (function check() {
                    if (window._odbMouseDown) { setTimeout(check, 40); }
                    else { resolve(window._odbDragged ? NO : c); }
                })();
            });
        }""",
        Output("pick", "data"), Input("g3d", "clickData"))
    app.clientside_callback(CAMERA_JS, Output("g3d", "figure"), Input("fig", "data"),
                            Input("view-fit", "n_clicks"), Input("view-iso", "n_clicks"),
                            Input("view-xy", "n_clicks"), Input("view-xz", "n_clicks"),
                            Input("view-yz", "n_clicks"))
    app.layout = html.Div([side, main], style={"display": "flex", "height": "100vh", "margin": "-8px", "fontFamily":
                                               "Segoe UI, Helvetica, Arial, sans-serif", "color": "#1c1e21"})

    @app.callback(Output("fig", "data"), Output("frame-label", "children"),
                  Input("frame", "value"), Input("var", "value"), Input("opts", "value"),
                  Input("scale", "value"), Input("cmap", "value"), Input("rmode", "value"),
                  Input("cmin", "value"), Input("cmax", "value"))
    def update(frame, sel, opts, scale, cmap, rmode, cmin, cmax):
        if sel is None:
            return no_update, "No variables were loaded."
        frame = int(frame or 0)
        opts = opts or []
        deform = "deform" in opts and m.U is not None
        scale = 1.0 if scale is None else float(scale)
        text = m.frame_text(frame)
        only_frame = set(t["prop_id"] for t in ctx.triggered) == {"frame.value"}
        if not only_frame:
            return build_figure(m, frame, sel, deform, scale, "edges" in opts, cmap, rmode, cmin, cmax), text
        # only the frame changed: send just the arrays that change
        p = Patch()
        inten, linecol = m.face_values(sel, frame)
        p["data"][0]["intensity"] = _enc(inten)
        s0, s1 = m.split(sel)[2:]
        if len(s0):
            p["data"][3]["line"]["color"] = _enc(linecol)
        if deform:
            xyz = m.coords(frame, deform, scale)
            X, Y, Z = _enc(xyz[:, 0], "f8"), _enc(xyz[:, 1], "f8"), _enc(xyz[:, 2], "f8")
            for t in (0, 1):
                p["data"][t]["x"], p["data"][t]["y"], p["data"][t]["z"] = X, Y, Z
            if "edges" in opts and len(m.edges):
                ex, ey, ez = m.pair_xyz(xyz, m.edges)
                p["data"][2]["x"], p["data"][2]["y"], p["data"][2]["z"] = _enc(ex, "f8"), _enc(ey, "f8"), _enc(ez, "f8")
            for t, ss in ((3, s0), (4, s1)):
                if len(ss):
                    lx, ly, lz = m.pair_xyz(xyz, m.segs[ss])
                    p["data"][t]["x"], p["data"][t]["y"], p["data"][t]["z"] = \
                        _enc(lx, "f8"), _enc(ly, "f8"), _enc(lz, "f8")
        if rmode == "frame":
            lo, hi = m.color_range(sel, rmode, frame, cmin, cmax)
            p["layout"]["coloraxis"]["cmin"] = lo
            p["layout"]["coloraxis"]["cmax"] = hi
        return p, text

    @app.callback(Output("frame", "value"), Input("tick", "n_intervals"), Input("prev", "n_clicks"),
                  Input("next", "n_clicks"), State("frame", "value"), prevent_initial_call=True)
    def step_frame(_t, _p, _n, cur):
        cur = int(cur or 0)
        if ctx.triggered_id == "prev":
            return max(0, cur - 1)
        if ctx.triggered_id == "next":
            return min(nf - 1, cur + 1)
        return (cur + 1) % nf

    @app.callback(Output("tick", "disabled"), Output("play", "children"), Input("play", "n_clicks"),
                  State("tick", "disabled"), prevent_initial_call=True)
    def toggle_play(_n, disabled):
        now = not disabled
        return now, ("\u25B6 Play" if now else "\u23F8 Pause")

    @app.callback(Output("tick", "interval"), Input("speed", "value"))
    def set_speed(ms):
        return ms or 500

    def empty_hist(text):
        return {"data": [], "layout": {
            "margin": {"l": 70, "r": 20, "t": 30, "b": 45}, "paper_bgcolor": "white", "plot_bgcolor": "white",
            "xaxis": {"visible": False}, "yaxis": {"visible": False},
            "annotations": [{"text": text, "showarrow": False, "xref": "paper", "yref": "paper",
                             "x": 0.5, "y": 0.5, "font": {"size": 14, "color": "#777"}}]}}

    @app.callback(Output("hist", "figure"), Input("pick", "data"), Input("var", "value"))
    def history(click, sel):
        hint = "Click on the model to plot the history at that point"
        if sel is None or not click or ctx.triggered_id != "pick":
            return empty_hist(hint)
        pt = click["points"][0]
        idx, name = m.pick(sel, pt.get("curveNumber", 0), pt.get("pointNumber"))
        if idx is None:
            return empty_hist(hint)
        y = m.series(sel, idx)
        t = np.array([f["total"] for f in m.frames])
        if len(t) > 1 and np.all(np.diff(t) >= 0) and t[-1] > t[0]:
            x, xt = t, "Total time"
        else:
            x, xt = np.arange(1, nf + 1), "Frame"
        grid = {"gridcolor": "#e3e5e8", "zerolinecolor": "#c9ccd1", "linecolor": "#888"}
        return {"data": [{"type": "scatter", "mode": "lines+markers", "x": x.tolist(),
                          "y": [None if not np.isfinite(q) else float(q) for q in y],
                          "line": {"color": "#1f5fbf", "width": 2}, "marker": {"size": 5}}],
                "layout": {"title": {"text": "%s at %s" % (m.label(sel), name), "font": {"size": 14}},
                           "margin": {"l": 70, "r": 20, "t": 36, "b": 45}, "paper_bgcolor": "white",
                           "plot_bgcolor": "white", "xaxis": dict(grid, title={"text": xt}),
                           "yaxis": dict(grid, title={"text": m.label(sel)}, tickformat=".4g")}}

    return app


def serve(model, port=None, block=False):
    """Starts the viewer. Returns its address."""
    import socket
    import threading
    app = make_app(model)
    if not port:
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
    url = "http://127.0.0.1:%d" % port

    def run():
        app.run(host="127.0.0.1", port=port, debug=False)

    if block:
        print("Viewer running at " + url + "   (Ctrl+C to stop)")
        run()
    else:
        threading.Thread(target=run, daemon=True).start()
    return url


def run_dir_for(cdir, stride):
    return os.path.join(cdir, "frames_every_%d" % stride)


def load_demo(on_msg=None, on_progress=None):
    """Builds the built-in example (does not need Abaqus) and returns its Model."""
    import json
    import shutil
    cdir = os.path.join(CACHE_DIR, "demo_%d" % os.getpid())
    shutil.rmtree(cdir, ignore_errors=True)
    run = run_dir_for(cdir, 1)
    run_extractor({"mode": "scan", "demo": True, "out": cdir}, None, on_msg, on_progress)
    with open(os.path.join(cdir, "meta.json")) as f:
        meta = json.load(f)
    run_extractor({"mode": "extract", "demo": True, "out": cdir, "run": run,
                   "fields": [f["key"] for f in meta["fields"]],
                   "frames": list(range(len(meta["frames"])))}, None, on_msg, on_progress)
    return Model(cdir, run)


# =============================================================================
# PART 7.  The window where the user picks the file
# =============================================================================
class Gui(object):
    def __init__(self):
        import queue
        import tkinter as tk
        from tkinter import ttk
        self.tk = tk
        self.ttk = ttk
        self.q = queue.Queue()
        self.meta = None
        self.cdir = None
        self.scanned = None
        self.busy = False
        root = self.root = tk.Tk()
        root.title(APP_NAME)
        root.geometry("760x720")
        root.minsize(640, 600)
        root.report_callback_exception = self.on_tk_error
        root.lift()
        root.attributes("-topmost", True)
        root.after(1500, lambda: root.attributes("-topmost", False))
        pad = {"padx": 12, "pady": 4}
        bold = ("", 10, "bold")

        ttk.Label(root, text="1.  Choose the Abaqus result file (.odb)", font=bold).pack(anchor="w", padx=12, pady=(12, 2))
        row = ttk.Frame(root)
        row.pack(fill="x", **pad)
        self.odb = tk.StringVar()
        ttk.Entry(row, textvariable=self.odb).pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="Browse...", command=self.browse_odb).pack(side="left", padx=(6, 0))

        row = ttk.Frame(root)
        row.pack(fill="x", **pad)
        ttk.Label(row, text="Abaqus command:").pack(side="left")
        self.abq = tk.StringVar(value=find_abaqus())
        ttk.Entry(row, textvariable=self.abq).pack(side="left", fill="x", expand=True, padx=(6, 0))
        ttk.Button(row, text="Browse...", command=self.browse_abq).pack(side="left", padx=(6, 0))
        self.abq_hint = ttk.Label(root, text="", foreground="#666")
        self.abq_hint.pack(anchor="w", padx=12)
        self.update_abq_hint()

        ttk.Label(root, text="2.  Read the file", font=bold).pack(anchor="w", padx=12, pady=(12, 2))
        self.b_scan = ttk.Button(root, text="Read file", command=self.on_scan)
        self.b_scan.pack(anchor="w", **pad)

        ttk.Label(root, text="3.  Choose the outputs to load (all are selected by default)", font=bold).pack(
            anchor="w", padx=12, pady=(12, 2))
        box = ttk.Frame(root)
        box.pack(fill="both", expand=True, **pad)
        sb = ttk.Scrollbar(box)
        sb.pack(side="right", fill="y")
        self.lb = tk.Listbox(box, selectmode="multiple", exportselection=False, height=8,
                             yscrollcommand=sb.set, font=("Courier", 10))
        self.lb.pack(side="left", fill="both", expand=True)
        sb.config(command=self.lb.yview)
        self.lb.bind("<<ListboxSelect>>", lambda e: self.update_estimate())
        row = ttk.Frame(root)
        row.pack(fill="x", **pad)
        ttk.Button(row, text="Select all", command=lambda: self.select(True)).pack(side="left")
        ttk.Button(row, text="Select none", command=lambda: self.select(False)).pack(side="left", padx=6)
        ttk.Label(row, text="   Load one frame every").pack(side="left")
        self.stride = tk.StringVar(value="1")
        sp = ttk.Spinbox(row, from_=1, to=10000, width=5, textvariable=self.stride, command=self.update_estimate)
        sp.pack(side="left", padx=4)
        sp.bind("<KeyRelease>", lambda e: self.update_estimate())
        self.est = ttk.Label(row, text="")
        self.est.pack(side="left", padx=8)

        ttk.Label(root, text="4.  Open the viewer (opens in your web browser)", font=bold).pack(
            anchor="w", padx=12, pady=(12, 2))
        row = ttk.Frame(root)
        row.pack(fill="x", **pad)
        self.b_open = ttk.Button(row, text="Open viewer", command=self.on_open, state="disabled")
        self.b_open.pack(side="left")
        self.b_demo = ttk.Button(row, text="Try the built-in example (no Abaqus needed)", command=self.on_demo)
        self.b_demo.pack(side="left", padx=6)
        self.b_clear = ttk.Button(row, text="Clear cache", command=self.on_clear)
        self.b_clear.pack(side="right")

        self.bar = ttk.Progressbar(root, mode="determinate")
        self.bar.pack(fill="x", padx=12, pady=(10, 4))
        self.log = tk.Text(root, height=8, wrap="word", state="disabled", font=("Courier", 9))
        self.log.pack(fill="both", expand=True, padx=12, pady=(0, 12))
        self.say("Ready. Pick an .odb file and press 'Read file'.")
        self.say("(Program folder: %s)" % APP_DIR)
        root.after(100, self.poll)

    # ---- helpers -----------------------------------------------------------
    def say(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")

    def set_busy(self, busy):
        self.busy = busy
        st = "disabled" if busy else "normal"
        for b in (self.b_scan, self.b_demo, self.b_clear):
            b.config(state=st)
        self.b_open.config(state=st if self.meta else "disabled")
        if busy:
            self.bar.config(mode="indeterminate")
            self.bar.start(12)
        else:
            self.bar.stop()
            self.bar.config(mode="determinate", value=0)

    def job(self, fn):
        import threading
        import traceback

        def run():
            try:
                fn()
            except RuntimeError as exc:
                self.q.put(("error", str(exc), write_error_log(str(exc))))
            except MemoryError:
                self.q.put(("error", "Not enough memory. Load fewer outputs or a larger "
                            "'Load one frame every' value.", None))
            except Exception:
                tb = traceback.format_exc()
                self.q.put(("error", "Unexpected problem:\n\n" + tb[-700:], write_error_log(tb)))
            self.q.put(("idle",))

        self.set_busy(True)
        threading.Thread(target=run, daemon=True).start()

    def msg(self, text):
        self.q.put(("log", text))

    def prog(self, i, n):
        self.q.put(("prog", i, n))

    def poll(self):
        import queue
        import webbrowser
        try:
            while True:
                item = self.q.get_nowait()
                kind = item[0]
                if kind == "log":
                    self.say(item[1])
                elif kind == "prog":
                    self.bar.stop()
                    self.bar.config(mode="determinate", maximum=item[2], value=item[1])
                elif kind == "scan":
                    self.show_meta(item[1])
                elif kind == "view":
                    self.say("Viewer running at " + item[1] + "  (keep this window open)")
                    print("Viewer running at " + item[1] + "   (keep the window open)", flush=True)
                    webbrowser.open(item[1])
                elif kind == "error":
                    self.say("ERROR: " + item[1])
                    self.error_box(item[1], item[2])
                elif kind == "idle":
                    self.set_busy(False)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def error_box(self, text, log_path):
        from tkinter import messagebox
        text = text[-1500:]
        if log_path and os.path.exists(log_path):
            if messagebox.askyesno(APP_NAME + " - problem", text + "\n\nOpen the details file?",
                                   icon="error", parent=self.root):
                open_path(log_path)
        else:
            messagebox.showerror(APP_NAME + " - problem", text, parent=self.root)

    def on_tk_error(self, *exc):
        import traceback
        tb = "".join(traceback.format_exception(*exc))
        self.say("ERROR: " + tb.strip().splitlines()[-1])
        try:
            self.set_busy(False)
        except Exception:
            pass
        self.error_box("Unexpected problem:\n\n" + tb[-700:], write_error_log(tb))

    def update_abq_hint(self):
        p = self.abq.get().strip().strip('"')
        if p and (os.path.exists(p) or shutil_which(p)):
            self.abq_hint.config(text="Abaqus found. Nothing to do here.", foreground="#2a7a2a")
        else:
            self.abq_hint.config(text="Abaqus was not found automatically. Press Browse and select abaqus.bat "
                                      "(usually in C:\\SIMULIA\\Commands).", foreground="#b03030")

    # ---- actions -----------------------------------------------------------
    def browse_odb(self):
        from tkinter import filedialog
        p = filedialog.askopenfilename(title="Choose the .odb file",
                                       filetypes=[("Abaqus output database", "*.odb"), ("All files", "*.*")])
        if p:
            self.odb.set(p)
            self.meta = None
            self.lb.delete(0, "end")
            self.b_open.config(state="disabled")

    def browse_abq(self):
        from tkinter import filedialog
        start = r"C:\SIMULIA\Commands" if IS_WIN and os.path.isdir(r"C:\SIMULIA\Commands") else None
        p = filedialog.askopenfilename(title="Choose the Abaqus command (abaqus.bat or abaqus)", initialdir=start)
        if p:
            self.abq.set(os.path.normpath(p))
            self.update_abq_hint()

    def check_odb(self):
        from tkinter import messagebox
        p = self.odb.get().strip().strip('"').strip("'")
        if not p or not os.path.isfile(p):
            messagebox.showwarning(APP_NAME, "Choose an existing .odb file first (press Browse).")
            return None
        if not p.lower().endswith(".odb"):
            if not messagebox.askyesno(APP_NAME, "This file does not end in .odb. Use it anyway?"):
                return None
        return os.path.abspath(p)

    def on_scan(self):
        odb = self.check_odb()
        if not odb:
            return
        abq = self.abq.get().strip()

        def work():
            cdir = cache_dir_for(odb)
            if os.path.exists(os.path.join(cdir, "meta.json")):
                self.msg("This file was read before. Using the saved copy.")
            else:
                src = odb_for_abaqus(odb, cdir, self.msg)
                run_extractor({"mode": "scan", "odb": src, "out": cdir}, abq, self.msg, self.prog)
            self.scanned = odb
            self.q.put(("scan", cdir))

        self.job(work)

    def show_meta(self, cdir):
        import json
        with open(os.path.join(cdir, "meta.json")) as f:
            self.meta = json.load(f)
        self.cdir = cdir
        self.lb.delete(0, "end")
        for f in self.meta["fields"]:
            where = "nodes" if f["positions"] == ["NODAL"] else "elements"
            self.lb.insert("end", "%-14s %-9s %s" % (f["key"][:40], where, f["desc"]))
        self.lb.select_set(0, "end")
        # pick a frame step that keeps the data below about 2 GB
        stride = 1
        nfr = len(self.meta["frames"])
        while stride < nfr and sum(estimate_bytes(self.meta, f, len(frame_selection(nfr, stride)))
                                   for f in self.meta["fields"]) > 2e9:
            stride += 1
        self.stride.set(str(stride))
        self.update_estimate()
        if stride > 1:
            self.say("Large model. 'Load one frame every' was set to %d to keep it fast. "
                     "Set it to 1 to load all frames." % stride)
        kinds = {}
        for g in self.meta["groups"]:
            kinds[g["etype"]] = kinds.get(g["etype"], 0) + g["n"]
        self.say("Element types: " + ", ".join(
            "%s x%d (%s)" % (k, n, CLASS_TEXT[classify(k, [g["nn"] for g in self.meta["groups"]
                                                           if g["etype"] == k][0])[0]])
            for k, n in kinds.items()))
        self.say("%d frames and %d output variables found. Press 'Open viewer'."
                 % (len(self.meta["frames"]), len(self.meta["fields"])))
        self.b_open.config(state="normal")

    def select(self, on):
        if on:
            self.lb.select_set(0, "end")
        else:
            self.lb.select_clear(0, "end")
        self.update_estimate()

    def get_stride(self):
        try:
            return max(1, int(self.stride.get()))
        except ValueError:
            return 1

    def update_estimate(self):
        if not self.meta:
            self.est.config(text="")
            return
        sel = frame_selection(len(self.meta["frames"]), self.get_stride())
        total = sum(estimate_bytes(self.meta, self.meta["fields"][i], len(sel)) for i in self.lb.curselection())
        size = "%.1f GB" % (total / 1e9) if total > 1e9 else "%.0f MB" % (total / 1e6 + 1)
        self.est.config(text="%d frames, about %s on disk" % (len(sel), size))

    def on_open(self):
        from tkinter import messagebox
        odb = self.check_odb()
        if not odb or not self.meta:
            return
        if odb != self.scanned:
            messagebox.showwarning(APP_NAME, "The file changed. Press 'Read file' first.")
            return
        fields = [self.meta["fields"][i]["key"] for i in self.lb.curselection()]
        if not fields:
            messagebox.showwarning(APP_NAME, "Select at least one output.")
            return
        stride = self.get_stride()
        sel = frame_selection(len(self.meta["frames"]), stride)
        cdir, abq = self.cdir, self.abq.get().strip()
        run = run_dir_for(cdir, stride)

        need = sum(estimate_bytes(self.meta, self.meta["fields"][i], len(sel)) for i in self.lb.curselection())
        try:
            import shutil
            free = shutil.disk_usage(APP_DIR).free
            if need * 1.1 > free:
                messagebox.showwarning(APP_NAME, "Not enough free disk space (%.1f GB needed, %.1f GB free).\n"
                                       "Select fewer outputs or a larger 'Load one frame every' value."
                                       % (need / 1e9, free / 1e9))
                return
        except Exception:
            pass

        def work():
            src = odb_for_abaqus(odb, cdir, self.msg)
            run_extractor({"mode": "extract", "odb": src, "out": cdir, "run": run, "fields": fields,
                           "frames": sel}, abq, self.msg, self.prog)
            self.msg("Building the outer surface of the mesh...")
            model = Model(cdir, run)
            self.q.put(("view", serve(model)))

        self.job(work)

    def on_demo(self):
        def work():
            model = load_demo(self.msg, self.prog)
            self.q.put(("view", serve(model)))

        self.job(work)

    def on_clear(self):
        import shutil
        from tkinter import messagebox
        if not messagebox.askyesno(APP_NAME, "Delete the saved copies of all the files read so far?"):
            return
        shutil.rmtree(CACHE_DIR, ignore_errors=True)
        self.meta = None
        self.lb.delete(0, "end")
        self.b_open.config(state="disabled")
        self.say("Cache cleared. (Files used by an open viewer are removed after you close this window.)")


def console_main():
    """Used only when the window toolkit is missing. Asks the questions in the console."""
    import webbrowser
    print(APP_NAME + "\n" + "=" * len(APP_NAME))
    print("Drag the .odb file into this window (or type its path) and press Enter.")
    print("Type  demo  to open the built-in example.")
    odb = input("> ").strip().strip('"').strip("'")
    if odb.lower() == "demo":
        model = load_demo(print)
    else:
        if not os.path.isfile(odb):
            raise RuntimeError("File not found: " + odb)
        abq = find_abaqus()
        if not abq:
            print("Abaqus was not found. Drag abaqus.bat here (usually in C:\\SIMULIA\\Commands) and press Enter.")
            abq = input("> ").strip().strip('"').strip("'")
        odb = os.path.abspath(odb)
        cdir = cache_dir_for(odb)
        src = odb_for_abaqus(odb, cdir, print)
        if not os.path.exists(os.path.join(cdir, "meta.json")):
            run_extractor({"mode": "scan", "odb": src, "out": cdir}, abq, print)
        import json
        with open(os.path.join(cdir, "meta.json")) as f:
            meta = json.load(f)
        nfr = len(meta["frames"])
        stride = 1
        while stride < nfr and sum(estimate_bytes(meta, f, len(frame_selection(nfr, stride)))
                                   for f in meta["fields"]) > 2e9:
            stride += 1
        sel = frame_selection(nfr, stride)
        run = run_dir_for(cdir, stride)
        last = [0]

        def prog(i, n):
            if i == n or i - last[0] >= max(1, n // 20):
                last[0] = i
                print("  %d of %d frames" % (i, n))

        run_extractor({"mode": "extract", "odb": src, "out": cdir, "run": run,
                       "fields": [f["key"] for f in meta["fields"]], "frames": sel}, abq, print, prog)
        model = Model(cdir, run)
    url = serve(model)
    print("\nViewer running at " + url + "\nKeep this window open. Close it to stop the viewer.")
    webbrowser.open(url)
    input("Press Enter to stop...\n")


def main():
    import argparse
    ap = argparse.ArgumentParser(description=APP_NAME)
    ap.add_argument("--demo", action="store_true", help="open the built-in example without the window")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--no-browser", action="store_true")
    args, _unknown = ap.parse_known_args()
    if args.demo:
        model = load_demo(print)
        port = args.port or 8050
        if not args.no_browser:
            import threading
            import webbrowser
            threading.Timer(1.5, lambda: webbrowser.open("http://127.0.0.1:%d" % port)).start()
        serve(model, port, block=True)
        return
    if not _has_tk():
        console_main()
        return
    Gui().root.mainloop()


if __name__ == "__main__":
    _safe_console()
    try:
        bootstrap()
        main()
    except SystemExit:
        raise
    except KeyboardInterrupt:
        pass
    except RuntimeError as exc:
        show_error(str(exc), write_error_log(str(exc)))
    except Exception:
        import traceback
        tb = traceback.format_exc()
        show_error("Unexpected problem:\n\n" + tb[-800:], write_error_log(tb))
