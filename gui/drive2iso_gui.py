#!/usr/bin/env python3
"""Drive2Iso GUI — a thin, Etcher-styled front-end over the drive2iso CLI.

It builds no logic of its own: every action shells out to the verified
`drive2iso` binary and streams its output live. Capture runs as a dry-run
preview until you press Build (which adds --commit behind a confirmation).

Pure Python standard library (tkinter). Run:
    python drive2iso_gui.py          # or pythonw on Windows for no console
"""
import os
import re
import sys
import queue
import shutil
import threading
import subprocess
import tkinter as tk
from tkinter import filedialog, messagebox

# --- Etcher-ish palette (matches the CLI's teal accent) ---
BG     = "#0d1b1e"
PANEL  = "#12262a"
FIELD  = "#0a1517"
TEAL   = "#2ad1d1"
TEXT   = "#eafffb"
DIM    = "#7fa6a6"
DANGER = "#ff6b6b"
WARN   = "#e0a800"
OK     = "#5fd38a"

MONO = ("Consolas", 10) if os.name == "nt" else ("DejaVu Sans Mono", 10)
UI   = ("Segoe UI", 10) if os.name == "nt" else ("DejaVu Sans", 10)
UIB  = (UI[0], 10, "bold")

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(s: str) -> str:
    return _ANSI.sub("", s)


def find_binary() -> str:
    """Locate drive2iso(.exe): next to the GUI (exe when frozen, else repo root),
    then the cwd, then PATH."""
    exe = "drive2iso.exe" if os.name == "nt" else "drive2iso"
    dirs = []
    if getattr(sys, "frozen", False):
        dirs.append(os.path.dirname(sys.executable))          # beside the bundled exe
    else:
        dirs.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    dirs.append(os.getcwd())
    for d in dirs:
        cand = os.path.abspath(os.path.join(d, exe))
        if os.path.isfile(cand):
            return cand
    return shutil.which(exe) or shutil.which("drive2iso") or ""


def is_admin():
    if os.name != "nt":
        return os.geteuid() == 0 if hasattr(os, "geteuid") else None
    try:
        import ctypes
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except Exception:
        return None


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Drive2Iso")
        self.configure(bg=BG)
        self.minsize(820, 660)
        self.q = queue.Queue()
        self.busy = False
        self.actions = []

        self.bin_var  = tk.StringVar(value=find_binary())
        self.dev_var  = tk.StringVar()
        self.pick_var = tk.StringVar(value="— pick —")
        self.iso_var  = tk.StringVar()
        self.work_var = tk.StringVar()
        self.uefi_var = tk.BooleanVar(value=True)
        self.bios_var = tk.BooleanVar(value=True)
        self.hyb_var  = tk.BooleanVar(value=True)
        self.comp_var = tk.StringVar(value="0")

        self._build()
        self.after(80, self._drain)

    # ---------- widget helpers ----------
    def _panel(self, parent, title):
        outer = tk.Frame(parent, bg=BG)
        outer.pack(fill="x", padx=14, pady=(0, 10))
        tk.Label(outer, text=title, bg=BG, fg=TEAL, font=UIB).pack(anchor="w", pady=(0, 3))
        f = tk.Frame(outer, bg=PANEL, highlightbackground="#1d3a3f", highlightthickness=1)
        f.pack(fill="x")
        return f

    def _entry(self, parent, var, width=48):
        return tk.Entry(parent, textvariable=var, width=width, bg=FIELD, fg=TEXT,
                        insertbackground=TEAL, relief="flat", font=MONO,
                        disabledbackground=FIELD, disabledforeground=DIM)

    def _btn(self, parent, text, cmd, primary=False, danger=False):
        bg = TEAL if primary else (DANGER if danger else "#1d3a3f")
        fg = BG if primary else TEXT
        b = tk.Button(parent, text=text, command=cmd, bg=bg, fg=fg, font=UIB,
                      relief="flat", activebackground=bg, activeforeground=fg,
                      padx=12, pady=5, cursor="hand2", bd=0)
        return b

    def _check(self, parent, text, var):
        return tk.Checkbutton(parent, text=text, variable=var, bg=PANEL, fg=TEXT,
                              selectcolor=FIELD, activebackground=PANEL,
                              activeforeground=TEAL, font=UI, bd=0, highlightthickness=0)

    # ---------- layout ----------
    def _build(self):
        # banner
        head = tk.Frame(self, bg=BG)
        head.pack(fill="x", padx=14, pady=(12, 4))
        tk.Label(head, text="Drive2Iso", bg=BG, fg=TEAL,
                 font=(UI[0], 17, "bold")).pack(side="left")
        tk.Label(head, text="  capture a partition into a bootable ISO",
                 bg=BG, fg=DIM, font=UI).pack(side="left", pady=(6, 0))
        tk.Label(self, text="①  SOURCE   ›   ②  CAPTURE   ›   ③  IMAGE",
                 bg=BG, fg=DIM, font=UI).pack(anchor="w", padx=16, pady=(0, 8))

        # binary
        bar = tk.Frame(self, bg=BG)
        bar.pack(fill="x", padx=14, pady=(0, 8))
        tk.Label(bar, text="drive2iso", bg=BG, fg=DIM, font=UI, width=9, anchor="w").pack(side="left")
        self._entry(bar, self.bin_var, width=60).pack(side="left", fill="x", expand=True)
        self._btn(bar, "…", self._browse_bin).pack(side="left", padx=(6, 0))

        # SOURCE
        p = self._panel(self, "① Source partition")
        r1 = tk.Frame(p, bg=PANEL); r1.pack(fill="x", padx=10, pady=(10, 4))
        tk.Label(r1, text="device", bg=PANEL, fg=DIM, font=UI, width=9, anchor="w").pack(side="left")
        self._entry(r1, self.dev_var, width=30).pack(side="left", fill="x", expand=True)
        self.pick = tk.OptionMenu(r1, self.pick_var, "— pick —")
        self.pick.configure(bg=FIELD, fg=TEXT, font=UI, relief="flat",
                            activebackground="#1d3a3f", highlightthickness=0, width=12)
        self.pick["menu"].configure(bg=FIELD, fg=TEXT)
        self.pick.pack(side="left", padx=(6, 0))
        r2 = tk.Frame(p, bg=PANEL); r2.pack(fill="x", padx=10, pady=(0, 10))
        self._track(self._btn(r2, "List partitions", self.list_parts)).pack(side="left")
        self._track(self._btn(r2, "Probe", self.probe)).pack(side="left", padx=(6, 0))
        tk.Label(r2, text="Linux: /dev/sdXN or a mountpoint · Windows: C:",
                 bg=PANEL, fg=DIM, font=(UI[0], 9)).pack(side="left", padx=(10, 0))

        # OUTPUT
        p = self._panel(self, "Output")
        r = tk.Frame(p, bg=PANEL); r.pack(fill="x", padx=10, pady=(10, 4))
        tk.Label(r, text="ISO", bg=PANEL, fg=DIM, font=UI, width=9, anchor="w").pack(side="left")
        self._entry(r, self.iso_var).pack(side="left", fill="x", expand=True)
        self._btn(r, "Save as…", self._browse_iso).pack(side="left", padx=(6, 0))
        r = tk.Frame(p, bg=PANEL); r.pack(fill="x", padx=10, pady=(0, 10))
        tk.Label(r, text="work dir", bg=PANEL, fg=DIM, font=UI, width=9, anchor="w").pack(side="left")
        self._entry(r, self.work_var).pack(side="left", fill="x", expand=True)
        self._btn(r, "Browse…", self._browse_work).pack(side="left", padx=(6, 0))

        # OPTIONS
        p = self._panel(self, "Options")
        r = tk.Frame(p, bg=PANEL); r.pack(fill="x", padx=10, pady=10)
        self._check(r, "UEFI", self.uefi_var).pack(side="left")
        self._check(r, "BIOS", self.bios_var).pack(side="left", padx=(8, 0))
        self._check(r, "isohybrid (dd/USB bootable)", self.hyb_var).pack(side="left", padx=(8, 0))
        tk.Label(r, text="compression", bg=PANEL, fg=DIM, font=UI).pack(side="left", padx=(16, 4))
        tk.Spinbox(r, from_=0, to=22, textvariable=self.comp_var, width=4, bg=FIELD,
                   fg=TEXT, buttonbackground=PANEL, relief="flat", font=MONO,
                   insertbackground=TEAL).pack(side="left")

        # ACTIONS
        act = tk.Frame(self, bg=BG)
        act.pack(fill="x", padx=14, pady=(2, 8))
        self._track(self._btn(act, "Dry-run (preview plan)", self.dry_run)).pack(side="left")
        self._track(self._btn(act, "Build ISO  ▶", self.build, primary=True)).pack(side="left", padx=(8, 0))
        self._track(self._btn(act, "Write to USB…", self.write_usb_dialog, danger=True)).pack(side="right")

        # LOG
        lf = tk.Frame(self, bg=BG)
        lf.pack(fill="both", expand=True, padx=14, pady=(0, 6))
        self.log = tk.Text(lf, bg="#07100f", fg=TEXT, font=MONO, relief="flat",
                           wrap="word", height=12, insertbackground=TEAL)
        sb = tk.Scrollbar(lf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.log.pack(side="left", fill="both", expand=True)
        for tag, col in (("err", DANGER), ("warn", WARN), ("info", DIM),
                         ("ok", OK), ("cmd", TEAL)):
            self.log.tag_config(tag, foreground=col)
        self.log.configure(state="disabled")

        # STATUS
        self.status = tk.Label(self, text="", bg="#081113", fg=DIM, font=(UI[0], 9),
                               anchor="w", padx=10)
        self.status.pack(fill="x")
        self._status_ready()

    def _track(self, btn):
        self.actions.append(btn)
        return btn

    def _status_ready(self):
        adm = is_admin()
        note = ""
        if adm is False:
            note = "   ·   not elevated (capture --commit / write-usb need admin/root)"
        b = self.bin_var.get() or "not found — set the drive2iso path above"
        self.status.configure(text=f"binary: {b}{note}")

    # ---------- file pickers ----------
    def _browse_bin(self):
        f = filedialog.askopenfilename(title="Locate drive2iso")
        if f:
            self.bin_var.set(f); self._status_ready()

    def _browse_iso(self):
        f = filedialog.asksaveasfilename(title="Output ISO", defaultextension=".iso",
                                         filetypes=[("ISO image", "*.iso"), ("All", "*.*")])
        if f:
            self.iso_var.set(f)

    def _browse_work(self):
        d = filedialog.askdirectory(title="Working / staging directory")
        if d:
            self.work_var.set(d)

    # ---------- log helpers ----------
    def _emit(self, line, tag=None):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n", tag or ())
        self.log.see("end")
        self.log.configure(state="disabled")

    def _tag_for(self, line):
        low = line.lower()
        if line.startswith("[err") or "error" in low or "refus" in low:
            return "err"
        if line.startswith("[warn") or "warn" in low:
            return "warn"
        if line.startswith("+ ") or line.startswith("$ "):
            return "cmd"
        if line.startswith("[info"):
            return "info"
        return None

    # ---------- command runner ----------
    def _validate_bin(self):
        b = self.bin_var.get().strip()
        if not b or not (os.path.isfile(b) or shutil.which(b)):
            messagebox.showerror("Drive2Iso", "drive2iso binary not found.\n"
                                 "Build it (make) and set its path at the top.")
            return None
        return b

    def _run(self, argv, heading):
        if self.busy:
            return
        b = self._validate_bin()
        if not b:
            return
        self.busy = True
        for a in self.actions:
            a.configure(state="disabled")
        self.status.configure(text="running: " + " ".join(argv))
        self._emit("", None)
        self._emit("$ drive2iso " + " ".join(argv) + "   (" + heading + ")", "cmd")
        threading.Thread(target=self._worker, args=([b] + argv,), daemon=True).start()

    def _worker(self, cmd):
        kwargs = {}
        if os.name == "nt":
            kwargs["creationflags"] = 0x08000000  # CREATE_NO_WINDOW
        try:
            # drive2iso writes UTF-8 (banner em-dash, ①②③) straight to the pipe;
            # decode it as such rather than the platform locale codec.
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, bufsize=1, encoding="utf-8",
                                    errors="replace", **kwargs)
            for line in proc.stdout:
                self.q.put(("line", strip_ansi(line.rstrip("\n"))))
            proc.wait()
            self.q.put(("done", proc.returncode))
        except Exception as e:
            self.q.put(("line", f"[err ] failed to launch: {e}"))
            self.q.put(("done", -1))

    def _drain(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "line":
                    self._emit(payload, self._tag_for(payload))
                elif kind == "done":
                    self.busy = False
                    for a in self.actions:
                        a.configure(state="normal")
                    ok = payload == 0
                    self._emit(("✓ done" if ok else f"✗ exit {payload}"),
                               "ok" if ok else "err")
                    self._status_ready()
        except queue.Empty:
            pass
        self.after(80, self._drain)

    # ---------- argv assembly ----------
    def _need(self, dev, iso):
        if not dev:
            messagebox.showwarning("Drive2Iso", "Enter a source device (e.g. C: or /dev/sda2).")
            return False
        if not iso:
            messagebox.showwarning("Drive2Iso", "Choose an output ISO path.")
            return False
        return True

    def _capture_argv(self, commit):
        argv = ["capture", self.dev_var.get().strip(), self.iso_var.get().strip()]
        if not self.uefi_var.get(): argv.append("--no-uefi")
        if not self.bios_var.get(): argv.append("--no-bios")
        if not self.hyb_var.get():  argv.append("--no-isohybrid")
        try:
            if int(self.comp_var.get()) > 0:
                argv += ["--comp", self.comp_var.get()]
        except ValueError:
            pass
        w = self.work_var.get().strip()
        if w:
            argv += ["--work", w]
        if commit:
            argv.append("--commit")
        return argv

    # ---------- actions ----------
    def list_parts(self):
        self._run(["list-parts"], "enumerate volumes")

    def probe(self):
        dev = self.dev_var.get().strip()
        if not dev:
            messagebox.showwarning("Drive2Iso", "Enter a device to probe.")
            return
        self._run(["probe", dev], "inspect source")

    def dry_run(self):
        if not self._need(self.dev_var.get().strip(), self.iso_var.get().strip()):
            return
        self._run(self._capture_argv(False), "dry-run preview")

    def build(self):
        dev, iso = self.dev_var.get().strip(), self.iso_var.get().strip()
        if not self._need(dev, iso):
            return
        msg = (f"Build a bootable ISO from {dev}?\n\n"
               f"→ {iso}\n\n"
               "This reads the source (a VSS/RO snapshot is used) and writes the ISO. "
               "On Windows, capturing needs an elevated prompt.")
        if is_admin() is False:
            msg += "\n\nWARNING: this session is not elevated."
        if messagebox.askokcancel("Build ISO (--commit)", msg, icon="warning"):
            self._run(self._capture_argv(True), "COMMIT — build ISO")

    def write_usb_dialog(self):
        WriteUsbDialog(self)


class WriteUsbDialog(tk.Toplevel):
    """Small modal for the raw dd-style flash of a produced ISO."""
    def __init__(self, app: App):
        super().__init__(app)
        self.app = app
        self.title("Write ISO to USB")
        self.configure(bg=BG)
        self.resizable(False, False)
        self.transient(app)
        self.grab_set()

        self.iso = tk.StringVar(value=app.iso_var.get())
        self.dev = tk.StringVar()
        self.verify = tk.BooleanVar(value=True)
        self.force = tk.BooleanVar(value=False)

        pad = dict(padx=12, pady=4)
        tk.Label(self, text="Raw-write an ISO onto a whole device (destroys it).",
                 bg=BG, fg=WARN, font=UIB).grid(row=0, column=0, columnspan=3, sticky="w", **pad)

        tk.Label(self, text="ISO", bg=BG, fg=DIM, font=UI).grid(row=1, column=0, sticky="w", **pad)
        app._entry(self, self.iso, width=42).grid(row=1, column=1, sticky="we", **pad)
        tk.Button(self, text="…", command=self._pick, bg="#1d3a3f", fg=TEXT,
                  relief="flat").grid(row=1, column=2, **pad)

        tk.Label(self, text="device", bg=BG, fg=DIM, font=UI).grid(row=2, column=0, sticky="w", **pad)
        app._entry(self, self.dev, width=42).grid(row=2, column=1, columnspan=2, sticky="we", **pad)
        tk.Label(self, text="Windows: drive number / PhysicalDriveN   ·   Linux: /dev/sdX",
                 bg=BG, fg=DIM, font=(UI[0], 9)).grid(row=3, column=1, columnspan=2, sticky="w", padx=12)

        opts = tk.Frame(self, bg=BG)
        opts.grid(row=4, column=0, columnspan=3, sticky="w", padx=8, pady=(4, 0))
        c1 = tk.Checkbutton(opts, text="verify (read back)", variable=self.verify, bg=BG, fg=TEXT,
                            selectcolor=FIELD, activebackground=BG, activeforeground=TEAL, font=UI)
        c1.pack(side="left")
        c2 = tk.Checkbutton(opts, text="force (allow non-removable)", variable=self.force, bg=BG,
                            fg=DANGER, selectcolor=FIELD, activebackground=BG, activeforeground=DANGER, font=UI)
        c2.pack(side="left", padx=(8, 0))

        btns = tk.Frame(self, bg=BG)
        btns.grid(row=5, column=0, columnspan=3, sticky="e", padx=12, pady=10)
        tk.Button(btns, text="Preview (dry-run)", command=lambda: self._go(False),
                  bg="#1d3a3f", fg=TEXT, font=UIB, relief="flat", padx=10, pady=4).pack(side="left")
        tk.Button(btns, text="Flash  ▶", command=lambda: self._go(True),
                  bg=DANGER, fg=TEXT, font=UIB, relief="flat", padx=10, pady=4).pack(side="left", padx=(8, 0))

    def _pick(self):
        f = filedialog.askopenfilename(title="ISO to flash",
                                       filetypes=[("ISO image", "*.iso"), ("All", "*.*")])
        if f:
            self.iso.set(f)

    def _go(self, commit):
        iso, dev = self.iso.get().strip(), self.dev.get().strip()
        if not iso or not dev:
            messagebox.showwarning("Write to USB", "ISO and device are both required.", parent=self)
            return
        argv = ["write-usb", iso, dev]
        if self.verify.get(): argv.append("--verify")
        if self.force.get():  argv.append("--force")
        if commit:
            if not messagebox.askokcancel(
                    "Flash device",
                    f"IRREVERSIBLY overwrite {dev} with {os.path.basename(iso)}?\n\n"
                    "All data on the target is destroyed.", icon="warning", parent=self):
                return
            argv.append("--commit")
        self.destroy()
        self.app._run(argv, "flash ISO" if commit else "flash dry-run")


if __name__ == "__main__":
    App().mainloop()
