#!/usr/bin/env python3
"""Build/flash/run the AD5940-BIOZ example firmwares and plot impedance data.

Talks to the board the same way we've been doing by hand all session:
'make' + copy the .bin to the DAPLink mass-storage mount + an OpenOCD reset,
then 'zero'/'start' (sweep firmwares), 'start <Hz>'/'stop' (time-series-bioz),
or 'zero <Hz>'/'start <Hz>'/'stop' (time-series-bioz-2wire, whose 'zero <Hz>'
captures an RLIMIT/isolation-cap offset baseline instead of a sweep-point
table) over the UART console, parsing the same lines the firmware prints.
The plot switches between a frequency sweep view and a vs.-sample-number
time-series view automatically based on which line format is actually
coming in.
"""
import bisect
import csv
import math
import os
import queue
import re
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
from collections import deque
from pathlib import Path
from tkinter import ttk, scrolledtext, filedialog

import serial
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
from serial.tools import list_ports

PROJECT_ROOT = Path(__file__).resolve().parent.parent

# Each directory's Makefile TARGET is its own name with '-' -> '_' (confirmed
# against measure-2wire-bioz/measure-4wire-bioz/time-series-bioz/
# time-series-bioz-2wire).
FIRMWARE_DIRS = [
    "measure-2wire-bioz",
    "measure-4wire-bioz",
    "time-series-bioz",
    "time-series-bioz-2wire",
]

# Both print the same "sample=N freq=...Hz Z=(...)..." line format and
# understand the same "start <Hz>"/"stop" UART commands -- see
# time-series-bioz-2wire/README.md for how the 2-wire one differs (CE0/AIN1
# header). The 2-wire one additionally understands "zero <Hz>", to capture
# an RLIMIT/isolation-cap offset baseline at that frequency the way the
# sweep firmwares' bare "zero" does per sweep point -- see zero_ts_btn.
TIME_SERIES_FIRMWARES = {"time-series-bioz", "time-series-bioz-2wire"}
TIME_SERIES_2WIRE_FIRMWARE = "time-series-bioz-2wire"

# Sweep firmwares (measure-*-bioz) print "freq=...Hz Z=(...)..." lines --
# the time-series firmwares send per-sample data as binary frames instead
# (see SAMPLE_FRAME_SYNC/parse_sample_frame below), not as text.
DATA_LINE_RE = re.compile(
    r"freq=([\d.eE+-]+)Hz\s+Z=\(([-\d.eE+]+),([-\d.eE+]+)\)ohm\s+"
    r"\|Z\|=([\d.eE+-]+)ohm\s+phase=([-\d.eE+]+)deg"
)

# Cap how many time-series points are kept in memory for the *live plot* --
# it streams forever (no natural end, unlike a 40-point sweep), so without a
# cap memory would grow without bound on a long run. This is independent of
# CSV recording (see _on_record_toggle), which streams straight to disk and
# isn't bounded by this. 12000 covers a full 60s of scrollback at the
# 2-wire firmware's 200Hz BIOZODR (the fastest of the two time-series
# firmwares) -- the x-axis window control (ts_window_var) then just slices
# into whatever of that is actually buffered, so the 4-wire firmware's much
# slower 5Hz rate ends up with proportionally more (~40 minutes) of
# scrollback for free.
TIME_SERIES_MAXLEN = 12000


def find_daplink_device():
    """Returns the DAPLink block device name (e.g. 'sda'), or None. Linux only
    -- see find_daplink_mount_windows for the Windows equivalent."""
    out = subprocess.run(
        ["lsblk", "-o", "NAME,LABEL", "-nr"], capture_output=True, text=True
    ).stdout
    for line in out.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].strip() == "DAPLINK":
            return parts[0].strip()
    return None


def find_mountpoint(device):
    out = subprocess.run(
        ["lsblk", "-o", "NAME,MOUNTPOINT", "-nr"], capture_output=True, text=True
    ).stdout
    for line in out.splitlines():
        parts = line.split(None, 1)
        if parts and parts[0].strip() == device and len(parts) == 2:
            return parts[1].strip()
    return None


def find_daplink_mount_windows():
    """Returns the DAPLink drive root (e.g. 'D:\\\\'), or None. DAPLink shows
    up as a normal removable drive on Windows -- no lsblk/udisksctl
    equivalent exists, so this scans drive letters for the volume label
    instead (stdlib ctypes only, no extra dependency)."""
    import ctypes

    kernel32 = ctypes.windll.kernel32
    bitmask = kernel32.GetLogicalDrives()
    for i in range(26):
        if not (bitmask & (1 << i)):
            continue
        drive = f"{chr(65 + i)}:\\"
        name_buf = ctypes.create_unicode_buffer(261)
        # Non-zero return means the call succeeded; GetVolumeInformationW
        # can fail/hang-free-return-False for e.g. an empty CD drive.
        ok = kernel32.GetVolumeInformationW(
            drive, name_buf, len(name_buf), None, None, None, None, 0
        )
        if ok and name_buf.value == "DAPLINK":
            return drive
    return None


def list_serial_ports():
    """Sorted device names (e.g. ['COM3'] on Windows, ['/dev/ttyACM0'] on
    Linux) of currently connected serial ports, via pyserial's own
    cross-platform enumeration."""
    return sorted(p.device for p in list_ports.comports())


# ARM/Mbed's registered USB vendor ID -- what the ADICUP3029's DAPLink CDC
# UART enumerates under. Used to pick a sane default port: alphabetically-
# first isn't good enough on a machine with other serial hardware attached
# (seen in the wild: an unrelated lab instrument claiming the lower COM#).
DAPLINK_HWID_MARKER = "VID:PID=0D28"


def find_daplink_serial_port():
    """Returns the DAPLink board's own serial device name, or None if no
    port matches the expected vendor ID."""
    for p in list_ports.comports():
        if p.hwid and DAPLINK_HWID_MARKER in p.hwid.upper():
            return p.device
    return None


# The two time-series firmwares stream per-sample data as fixed-size binary
# frames instead of a printf'd text line -- see time-series-bioz-2wire/
# main.c's SendSampleBinary for the full rationale (printf's float
# formatting mallocs scratch space per call, which fragmented that
# firmware's 4KB heap and hung it after a few tens of thousands of
# samples). Everything else those firmwares print (banner, prompts, "Zero
# calibration captured...", "Stopped.") is still plain newline-terminated
# text, so the two are told apart by SYNC, which never appears in that text
# output -- see SerialReader.run().
SAMPLE_FRAME_SYNC = b"\xaa\x55"
SAMPLE_FRAME_LEN = 16


def parse_sample_frame(frame):
    """Decodes one SAMPLE_FRAME_LEN-byte binary sample frame (layout
    documented in SendSampleBinary). Returns (sample_num, real, imag,
    apply_baseline), or None if the checksum doesn't match."""
    checksum = 0
    for b in frame[:15]:
        checksum ^= b
    if checksum != frame[15]:
        return None
    sample_num = struct.unpack_from("<I", frame, 2)[0]
    apply_baseline = bool(frame[6] & 1)
    real, imag = struct.unpack_from("<ff", frame, 7)
    return sample_num, real, imag, apply_baseline


class SerialReader(threading.Thread):
    """Reads from an open serial port into a queue, as ("line", text) for
    plain newline-terminated text or ("sample", parsed) for a binary sample
    frame (see parse_sample_frame) -- the two are mixed on the same wire for
    the time-series firmwares. The GUI thread drains the queue via Tk's
    .after() polling loop, since Tk isn't thread-safe to touch directly from
    here."""

    def __init__(self, ser, out_queue):
        super().__init__(daemon=True)
        self.ser = ser
        self.out_queue = out_queue
        self.stop_flag = threading.Event()

    def run(self):
        buf = b""
        while not self.stop_flag.is_set():
            try:
                # Deliberately small: a large read() lets many already-
                # buffered sample frames return in one call, which then get
                # timestamped (see parse loop below) within microseconds of
                # each other in a tight loop -- visible on the time-series
                # plot as samples clustering at nearly the same x with a gap
                # before the next cluster, not reflecting when they actually
                # arrived. 32 bytes (2 sample frames, or a slice of a text
                # line -- either way harmless, since partial data just
                # accumulates in buf across calls) makes read() return
                # closer to real time as bytes trickle in instead of
                # batching up to 16 frames (256 bytes) per call.
                chunk = self.ser.read(32)
            except (serial.SerialException, OSError, TypeError):
                # Closing the port from the main thread while this call is
                # blocked (timeout=0.2 means it can't block for long, but
                # the race is still possible) leaves pyserial's internal fd
                # as None -- os.read(None, ...) raises TypeError, not
                # SerialException. Either way, the port's gone; stop.
                break
            if not chunk:
                continue
            buf += chunk
            while True:
                sync_idx = buf.find(SAMPLE_FRAME_SYNC)
                nl_idx = buf.find(b"\n")
                if sync_idx != -1 and (nl_idx == -1 or sync_idx < nl_idx):
                    if len(buf) < sync_idx + SAMPLE_FRAME_LEN:
                        break  # rest of the frame hasn't arrived yet
                    if sync_idx > 0:
                        # Leftover bytes before the frame with no
                        # terminating newline yet -- firmware only ever
                        # starts a frame right after a complete printf'd
                        # line, so this shouldn't normally happen, but
                        # surface it rather than silently dropping it.
                        leftover = buf[:sync_idx].decode(errors="replace")
                        if leftover:
                            self.out_queue.put(("line", leftover))
                    frame = buf[sync_idx:sync_idx + SAMPLE_FRAME_LEN]
                    buf = buf[sync_idx + SAMPLE_FRAME_LEN:]
                    sample = parse_sample_frame(frame)
                    if sample is not None:
                        # Timestamped here, at actual receipt, rather than
                        # in _poll_queue (a 100ms Tk .after() tick) -- the
                        # latter would batch several frames per tick and
                        # give them near-identical timestamps, making the
                        # time axis bursty instead of reflecting real
                        # arrival spacing.
                        self.out_queue.put(("sample", sample + (time.time(),)))
                    continue
                if nl_idx != -1:
                    line, buf = buf[:nl_idx], buf[nl_idx + 1:]
                    self.out_queue.put(
                        ("line", line.decode(errors="replace").rstrip("\r"))
                    )
                    continue
                break

    def stop(self):
        self.stop_flag.set()


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("AD5940-BIOZ")
        self.geometry("1000x720")

        self.ser = None
        self.reader = None
        self.line_queue = queue.Queue()

        self.sweep_freq = []
        self.sweep_mag = []
        self.sweep_phase = []

        self.ts_sample = deque(maxlen=TIME_SERIES_MAXLEN)
        self.ts_time = deque(maxlen=TIME_SERIES_MAXLEN)
        self.ts_mag = deque(maxlen=TIME_SERIES_MAXLEN)
        self.ts_phase = deque(maxlen=TIME_SERIES_MAXLEN)
        # Wall-clock time of the first sample in the current run -- set the
        # moment ts_time goes from empty to non-empty (see _poll_queue), not
        # when 'start' is sent, so the time axis reads 0 at the first real
        # sample instead of including the variable AD5940 init/RTIA-cal
        # latency before it as a misleading offset.
        self._ts_epoch = None

        # Sample frames don't carry frequency (it's fixed for the whole
        # 'start <Hz>' run) -- set from freq_var whenever a run starts, used
        # to reconstruct a readable log line for each incoming frame.
        self._active_freq = 0.0

        # CSV recording, independent of the (bounded) live-plot buffers
        # above -- writes straight to disk as each row arrives so a
        # multi-hour run doesn't have to sit in Python memory waiting for an
        # "Export" click. None/None while not recording; see
        # _on_record_toggle.
        self.record_file = None
        self.record_writer = None

        # Which buffers/axis-scale _redraw() should use; switched
        # automatically the moment a line in the other format arrives (see
        # _poll_queue), not tied to the firmware dropdown -- that only
        # controls which buttons are shown, not what's actually running.
        self.plot_mode = "sweep"

        self._build_widgets()
        self._on_firmware_change()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(100, self._poll_queue)

    def _on_close(self):
        self._stop_recording()
        self.destroy()

    # ---- UI construction ----
    def _build_widgets(self):
        top = ttk.Frame(self, padding=6)
        top.pack(fill="x")

        ttk.Label(top, text="Firmware:").pack(side="left")
        self.firmware_var = tk.StringVar(value=FIRMWARE_DIRS[0])
        firmware_combo = ttk.Combobox(
            top, textvariable=self.firmware_var, values=FIRMWARE_DIRS,
            state="readonly", width=22,
        )
        firmware_combo.pack(side="left", padx=(4, 12))
        firmware_combo.bind("<<ComboboxSelected>>", self._on_firmware_change)

        self.build_btn = ttk.Button(
            top, text="Build && Flash", command=self._on_build_flash
        )
        self.build_btn.pack(side="left", padx=(0, 20))

        ttk.Label(top, text="Serial port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=16)
        # Editable (not "readonly"): list_serial_ports() covers the normal
        # case, but a manual path/COM# still works if a port doesn't
        # enumerate for some reason. postcommand re-scans right before the
        # dropdown opens, so newly plugged-in boards show up without a
        # separate refresh button.
        self.port_combo.configure(postcommand=self._refresh_ports)
        self.port_combo.pack(side="left", padx=(4, 8))
        self._refresh_ports()
        self.connect_btn = ttk.Button(
            top, text="Connect", command=self._on_connect_toggle
        )
        self.connect_btn.pack(side="left")

        # Streams every incoming row straight to a CSV file as it arrives
        # (see _on_record_toggle) -- independent of the bounded live-plot
        # buffers, so it's the way to keep a run longer than
        # TIME_SERIES_MAXLEN actually holds.
        self.record_btn = ttk.Button(
            top, text="Start Recording", command=self._on_record_toggle,
            state="disabled",
        )
        self.record_btn.pack(side="left", padx=(12, 0))

        controls = ttk.Frame(self, padding=(6, 0))
        controls.pack(fill="x")
        self.zero_btn = ttk.Button(
            controls, text="Zero (S1 all closed)", command=lambda: self._send("zero"),
            state="disabled",
        )
        self.zero_btn.pack(side="left", padx=(0, 8))
        self.start_btn = ttk.Button(
            controls, text="Start Sweep", command=lambda: self._send("start"),
            state="disabled",
        )
        self.start_btn.pack(side="left")

        # time-series-bioz's controls: only shown/enabled when that
        # firmware is selected (see _on_firmware_change) -- it doesn't
        # understand plain 'start' or 'zero', only 'start <Hz>'/'stop'.
        self.freq_var = tk.StringVar(value="50000")
        self.freq_entry = ttk.Entry(controls, textvariable=self.freq_var, width=10)
        # time-series-bioz-2wire only: sends "zero <Hz>" (not plain "zero",
        # which only the sweep firmwares understand) to capture the
        # RLIMIT/isolation-cap offset baseline at freq_var's frequency --
        # see _on_firmware_change for why this is hidden for the 4-wire
        # time-series-bioz (true Kelvin sensing has no such offset to zero).
        self.zero_ts_btn = ttk.Button(
            controls, text="Zero (short) at freq", command=self._on_zero_continuous,
            state="disabled",
        )
        self.continuous_btn = ttk.Button(
            controls, text="Start Continuous", command=self._on_start_continuous,
            state="disabled",
        )
        self.stop_btn = ttk.Button(
            controls, text="Stop", command=lambda: self._send("stop"),
            state="disabled",
        )

        # Time-series-only: how much scrollback (in seconds, ending at the
        # newest sample) _redraw() actually plots, out of whatever
        # TIME_SERIES_MAXLEN has buffered -- lets you zoom into recent data
        # on a long run without that run's full history being redrawn every
        # tick. The trace redraws immediately on every keystroke/arrow-click
        # rather than waiting for the next sample, since typing a new value
        # otherwise wouldn't visibly do anything until one arrived.
        self.ts_window_var = tk.StringVar(value="10")
        self.ts_window_var.trace_add("write", lambda *_a: self._redraw())
        self.window_label = ttk.Label(controls, text="Window (s):")
        self.window_spin = ttk.Spinbox(
            controls, from_=1, to=60, increment=1, width=6,
            textvariable=self.ts_window_var,
        )

        self.status_var = tk.StringVar(value="Not connected.")
        ttk.Label(controls, textvariable=self.status_var).pack(
            side="left", padx=16
        )

        # Sweep plot: |Z| vs frequency on top, phase vs frequency below.
        fig = Figure(figsize=(8, 4.2), dpi=100)
        self.ax_mag = fig.add_subplot(211)
        self.ax_phase = fig.add_subplot(212, sharex=self.ax_mag)
        self.ax_mag.set_ylabel("|Z| (ohm)")
        self.ax_mag.set_xscale("log")
        self.ax_phase.set_ylabel("phase (deg)")
        self.ax_phase.set_xlabel("frequency (Hz)")
        self.ax_phase.set_xscale("log")
        fig.tight_layout()
        self.mag_line, = self.ax_mag.plot([], [], "o-", markersize=3)
        self.phase_line, = self.ax_phase.plot([], [], "o-", markersize=3, color="tab:orange")
        self.canvas = FigureCanvasTkAgg(fig, master=self)
        self.canvas.get_tk_widget().pack(fill="both", expand=True, padx=6, pady=6)
        self.fig = fig

        ttk.Label(self, text="Log:").pack(anchor="w", padx=6)
        self.log = scrolledtext.ScrolledText(self, height=10, font=("monospace", 9))
        self.log.pack(fill="both", expand=False, padx=6, pady=(0, 6))

    # ---- logging helper ----
    def _log(self, text):
        # _build_flash_worker_inner calls this from a background thread, and
        # Tk isn't thread-safe on Windows -- calls made off the main thread
        # observably raced/dropped frames (a "Flash complete" line took
        # several seconds and extra redraws to actually appear). Marshal
        # through .after() whenever we're not already on the main thread.
        if threading.current_thread() is not threading.main_thread():
            self.after(0, self._log, text)
            return
        self.log.insert("end", text + "\n")
        self.log.see("end")

    # ---- build & flash ----
    def _on_build_flash(self):
        self.build_btn.config(state="disabled")
        threading.Thread(target=self._build_flash_worker, daemon=True).start()

    def _build_flash_worker(self):
        try:
            self._build_flash_worker_inner()
        finally:
            self.after(0, lambda: self.build_btn.config(state="normal"))

    def _build_flash_worker_inner(self):
        name = self.firmware_var.get()
        project_dir = PROJECT_ROOT / name
        target = name.replace("-", "_")
        self._log(f"=== make ({name}) ===")
        proc = subprocess.run(
            ["make"], cwd=project_dir, capture_output=True, text=True
        )
        self._log(proc.stdout + proc.stderr)
        if proc.returncode != 0:
            self._log("Build failed, not flashing.")
            return

        if sys.platform == "win32":
            mount = find_daplink_mount_windows()
            if not mount:
                self._log("DAPLink drive (volume label DAPLINK) not found.")
                return
        else:
            device = find_daplink_device()
            if device is None:
                self._log("DAPLink device (label DAPLINK) not found via lsblk.")
                return
            mount = find_mountpoint(device)
            if not mount:
                self._log(f"Mounting /dev/{device} ...")
                r = subprocess.run(
                    ["udisksctl", "mount", "-b", f"/dev/{device}"],
                    capture_output=True, text=True,
                )
                self._log(r.stdout + r.stderr)
                mount = find_mountpoint(device)
            if not mount:
                self._log("Could not determine DAPLink mount point.")
                return
            mount = mount + "/"

        bin_path = project_dir / f"{target}.bin"
        dest_path = Path(mount) / bin_path.name
        self._log(f"Copying {bin_path.name} to {mount} ...")
        # Write + explicit fsync instead of shelling out to cp/sync (the
        # latter doesn't exist on Windows) -- a plain copy can return before
        # the data actually leaves the OS cache over USB, in which case
        # DAPLink never sees a complete file and silently keeps running
        # whatever was flashed before. fsync forces it out for real, on
        # both platforms.
        with open(bin_path, "rb") as fsrc, open(dest_path, "wb") as fdst:
            fdst.write(fsrc.read())
            fdst.flush()
            os.fsync(fdst.fileno())

        self._log("Resetting target via OpenOCD ...")
        try:
            # "reset run" already leaves the core running -- a trailing
            # "resume" on top of that isn't a no-op, it errors ("not
            # halted", "context restore failed, aborting resume") because
            # resume expects a halted core. Confirmed on real hardware: with
            # "resume" included, the target came up unresponsive (no UART
            # output at all) after a flash; dropping it fixed that.
            proc = subprocess.run(
                ["openocd", "-f", "openocd/aducm3029.cfg", "-c", "init",
                 "-c", "reset run", "-c", "shutdown"],
                cwd=project_dir, capture_output=True, text=True,
            )
            self._log(proc.stdout + proc.stderr)
        except FileNotFoundError:
            self._log(
                "openocd not found on PATH -- flash was written, but the "
                "target wasn't reset via SWD. It may still auto-reset on "
                "its own (DAPLink briefly unmounts/remounts on a real "
                "flash); power-cycle or press the board's reset button if "
                "it doesn't come up running the new image."
            )
        self._log("=== Flash complete ===")

    # ---- serial port dropdown ----
    def _refresh_ports(self):
        ports = list_serial_ports()
        self.port_combo["values"] = ports
        if not self.port_var.get() and ports:
            self.port_var.set(find_daplink_serial_port() or ports[0])

    # ---- serial connect/disconnect ----
    def _on_connect_toggle(self):
        if self.ser is None:
            try:
                self.ser = serial.Serial(self.port_var.get(), 230400, timeout=0.2)
            except serial.SerialException as e:
                self._log(f"Could not open {self.port_var.get()}: {e}")
                return
            self.reader = SerialReader(self.ser, self.line_queue)
            self.reader.start()
            self.connect_btn.config(text="Disconnect")
            self.status_var.set(f"Connected to {self.port_var.get()}.")
        else:
            self.reader.stop()
            # Wait for the reader's blocked read() (timeout=0.2s) to notice
            # the stop flag and return before closing the port out from
            # under it -- closing first raced the thread's next read call.
            self.reader.join(timeout=1.0)
            self.ser.close()
            self.ser = None
            self.reader = None
            self.connect_btn.config(text="Connect")
            self.status_var.set("Not connected.")
            self._stop_recording()
        self._update_control_states()

    # ---- firmware selection changes which controls are usable ----
    def _on_firmware_change(self, _event=None):
        fw = self.firmware_var.get()
        is_time_series = fw in TIME_SERIES_FIRMWARES
        is_time_series_2wire = fw == TIME_SERIES_2WIRE_FIRMWARE

        self.freq_entry.pack_forget()
        self.zero_ts_btn.pack_forget()
        self.continuous_btn.pack_forget()
        self.stop_btn.pack_forget()
        self.zero_btn.pack_forget()
        self.start_btn.pack_forget()
        self.window_label.pack_forget()
        self.window_spin.pack_forget()

        if is_time_series:
            self.freq_entry.pack(side="left", padx=(0, 4))
            if is_time_series_2wire:
                self.zero_ts_btn.pack(side="left", padx=(0, 4))
            self.continuous_btn.pack(side="left", padx=(0, 4))
            self.stop_btn.pack(side="left", padx=(0, 8))
            self.window_label.pack(side="left", padx=(0, 4))
            self.window_spin.pack(side="left", padx=(0, 8))
        else:
            self.start_btn.config(text="Start Sweep")
            self.zero_btn.pack(side="left", padx=(0, 8))
            self.start_btn.pack(side="left")
            self.title("AD5940-BIOZ")
        self._update_control_states()

    def _update_control_states(self):
        connected = self.ser is not None
        fw = self.firmware_var.get()
        is_time_series = fw in TIME_SERIES_FIRMWARES
        is_time_series_2wire = fw == TIME_SERIES_2WIRE_FIRMWARE
        state = "normal" if connected else "disabled"
        self.continuous_btn.config(state=state if is_time_series else "disabled")
        self.zero_ts_btn.config(state=state if is_time_series_2wire else "disabled")
        self.stop_btn.config(state=state if is_time_series else "disabled")
        self.zero_btn.config(state=state if not is_time_series else "disabled")
        self.start_btn.config(state=state if not is_time_series else "disabled")
        self.record_btn.config(state=state)

    # ---- CSV recording (independent of the bounded live-plot buffers --
    # see TIME_SERIES_MAXLEN) ----
    RECORD_HEADER = [
        "kind", "sample_num", "freq_hz", "time_s",
        "real_ohm", "imag_ohm", "mag_ohm", "phase_deg", "calibrated",
    ]

    def _on_record_toggle(self):
        if self.record_writer is not None:
            self._stop_recording()
            return
        path = filedialog.asksaveasfilename(
            title="Record to CSV",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
        )
        if not path:
            return
        try:
            self.record_file = open(path, "w", newline="")
        except OSError as e:
            self._log(f"Could not open {path} for recording: {e}")
            return
        self.record_writer = csv.writer(self.record_file)
        self.record_writer.writerow(self.RECORD_HEADER)
        self.record_btn.config(text="Stop Recording")
        self._log(f"Recording to {path}")

    def _stop_recording(self):
        if self.record_file is not None:
            self.record_file.close()
            self._log("Recording stopped.")
        self.record_file = None
        self.record_writer = None
        self.record_btn.config(text="Start Recording")

    def _on_zero_continuous(self):
        try:
            freq = float(self.freq_var.get())
        except ValueError:
            self._log(f"Not a valid frequency: {self.freq_var.get()!r}")
            return
        self._send(f"zero {freq:.0f}")

    def _on_start_continuous(self):
        try:
            freq = float(self.freq_var.get())
        except ValueError:
            self._log(f"Not a valid frequency: {self.freq_var.get()!r}")
            return
        self.title(f"AD5940-BIOZ — {freq:.0f} Hz")
        self._active_freq = freq
        self._send(f"start {freq:.0f}")

    def _send(self, command):
        if self.ser is None:
            return
        # A fresh command means fresh data -- old points would otherwise
        # stay mixed in with the new ones on the plot.
        self.sweep_freq.clear()
        self.sweep_mag.clear()
        self.sweep_phase.clear()
        self.ts_sample.clear()
        self.ts_time.clear()
        self.ts_mag.clear()
        self.ts_phase.clear()
        self._ts_epoch = None
        self._redraw()
        self.ser.write((command + "\r\n").encode())
        self.status_var.set(f"Sent '{command}'...")

    # ---- queue draining / plotting ----
    def _poll_queue(self):
        redraw_needed = False
        while True:
            try:
                kind, payload = self.line_queue.get_nowait()
            except queue.Empty:
                break

            if kind == "sample":
                sample_num, real, imag, apply_baseline, recv_time = payload
                mag = math.hypot(real, imag)
                phase = math.degrees(math.atan2(imag, real))
                suffix = "" if apply_baseline else " (uncalibrated -- run 'zero' first)"
                self._log(
                    f"sample={sample_num} freq={self._active_freq:.1f}Hz "
                    f"Z=({real:.2f},{imag:.2f})ohm |Z|={mag:.2f}ohm "
                    f"phase={phase:.2f}deg{suffix}"
                )
                if self.plot_mode != "timeseries":
                    self.plot_mode = "timeseries"
                    self._configure_axes()
                if self._ts_epoch is None:
                    self._ts_epoch = recv_time
                ts_sec = recv_time - self._ts_epoch
                self.ts_sample.append(sample_num)
                self.ts_time.append(ts_sec)
                self.ts_mag.append(mag)
                self.ts_phase.append(phase)
                if self.record_writer is not None:
                    self.record_writer.writerow([
                        "sample", sample_num, self._active_freq, f"{ts_sec:.6f}",
                        real, imag, mag, phase, int(apply_baseline),
                    ])
                    self.record_file.flush()
                redraw_needed = True
                continue

            line = payload
            self._log(line)

            m = DATA_LINE_RE.search(line)
            if m:
                freq, _real, _imag, mag, phase = (float(x) for x in m.groups())
                if self.plot_mode != "sweep":
                    self.plot_mode = "sweep"
                    self._configure_axes()
                self.sweep_freq.append(freq)
                self.sweep_mag.append(mag)
                self.sweep_phase.append(phase)
                if self.record_writer is not None:
                    self.record_writer.writerow([
                        "sweep", "", freq, "", _real, _imag, mag, phase, "",
                    ])
                    self.record_file.flush()
                redraw_needed = True
            elif "BIOZ Stop Now" in line:
                self.status_var.set("Sweep complete.")
            elif "Zero calibration captured" in line:
                self.status_var.set("Zero calibration captured.")
        if redraw_needed:
            self._redraw()
        self.after(100, self._poll_queue)

    def _configure_axes(self):
        """Switches axis scale/labels between the sweep view (|Z|/phase vs.
        frequency, log-x) and the impedance time-series view (vs. time in
        seconds since the first sample of the run, linear-x). Called
        whenever the format of incoming data lines changes, not tied to the
        firmware dropdown -- see _poll_queue."""
        if self.plot_mode == "timeseries":
            self.ax_mag.set_ylabel("|Z| (ohm)")
            self.ax_phase.set_ylabel("phase (deg)")
            self.ax_mag.set_xscale("linear")
            self.ax_phase.set_xscale("linear")
            self.ax_phase.set_xlabel("time (s)")
        else:
            self.ax_mag.set_ylabel("|Z| (ohm)")
            self.ax_phase.set_ylabel("phase (deg)")
            self.ax_mag.set_xscale("log")
            self.ax_phase.set_xscale("log")
            self.ax_phase.set_xlabel("frequency (Hz)")

    def _windowed_timeseries(self):
        """Returns (time, mag, phase) trimmed to the last ts_window_var
        seconds of self.ts_time, out of whatever TIME_SERIES_MAXLEN has
        actually buffered. Feeding only this slice into the plot (rather
        than the full buffer with set_xlim() applied after) means a small
        window stays cheap to redraw even once the buffer's full, since
        matplotlib never sees the off-screen points at all. Falls back to
        the whole buffer if the window field doesn't parse -- typing a new
        value goes through a transient empty/partial state on every
        keystroke via the StringVar trace, so this has to tolerate that
        rather than erroring."""
        times = list(self.ts_time)
        mags = list(self.ts_mag)
        phases = list(self.ts_phase)
        try:
            window = float(self.ts_window_var.get())
        except ValueError:
            window = None
        if window is not None and window > 0 and times:
            lo = bisect.bisect_left(times, times[-1] - window)
            times, mags, phases = times[lo:], mags[lo:], phases[lo:]
        return times, mags, phases

    def _redraw(self):
        if self.plot_mode == "timeseries":
            x, mag, phase = self._windowed_timeseries()
        else:
            x, mag, phase = self.sweep_freq, self.sweep_mag, self.sweep_phase
        self.mag_line.set_data(x, mag)
        self.phase_line.set_data(x, phase)
        for ax in (self.ax_mag, self.ax_phase):
            ax.relim()
            ax.autoscale_view()
        self.canvas.draw_idle()


if __name__ == "__main__":
    App().mainloop()
