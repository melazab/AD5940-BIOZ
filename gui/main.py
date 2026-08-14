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
import queue
import re
import subprocess
import threading
import tkinter as tk
from collections import deque
from pathlib import Path
from tkinter import ttk, scrolledtext

import serial
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

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

# time-series-bioz prints "sample=N freq=...Hz Z=(...)..." -- checked first
# since it's a superset of the sweep line format below (which just starts
# at "freq="). Sweep firmwares (measure-*-bioz) never print a "sample="
# prefix, so there's no ambiguity between the two.
TIME_SERIES_LINE_RE = re.compile(
    r"sample=(\d+)\s+freq=([\d.eE+-]+)Hz\s+Z=\(([-\d.eE+]+),([-\d.eE+]+)\)ohm\s+"
    r"\|Z\|=([\d.eE+-]+)ohm\s+phase=([-\d.eE+]+)deg"
)
DATA_LINE_RE = re.compile(
    r"freq=([\d.eE+-]+)Hz\s+Z=\(([-\d.eE+]+),([-\d.eE+]+)\)ohm\s+"
    r"\|Z\|=([\d.eE+-]+)ohm\s+phase=([-\d.eE+]+)deg"
)

# Cap how many time-series points are kept/plotted -- it streams forever
# (no natural end, unlike a 40-point sweep), so without a cap both memory
# and per-frame redraw cost would grow without bound on a long recording.
TIME_SERIES_MAXLEN = 300


def find_daplink_device():
    """Returns the DAPLink block device name (e.g. 'sda'), or None."""
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


class SerialReader(threading.Thread):
    """Reads lines from an open serial port into a queue. One line per queue
    item; the GUI thread drains it via Tk's .after() polling loop, since Tk
    isn't thread-safe to touch directly from here."""

    def __init__(self, ser, out_queue):
        super().__init__(daemon=True)
        self.ser = ser
        self.out_queue = out_queue
        self.stop_flag = threading.Event()

    def run(self):
        buf = b""
        while not self.stop_flag.is_set():
            try:
                chunk = self.ser.read(256)
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
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.out_queue.put(line.decode(errors="replace").rstrip("\r"))

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
        self.ts_mag = deque(maxlen=TIME_SERIES_MAXLEN)
        self.ts_phase = deque(maxlen=TIME_SERIES_MAXLEN)

        # Which buffers/axis-scale _redraw() should use; switched
        # automatically the moment a line in the other format arrives (see
        # _poll_queue), not tied to the firmware dropdown -- that only
        # controls which buttons are shown, not what's actually running.
        self.plot_mode = "sweep"

        self._build_widgets()
        self._on_firmware_change()
        self.after(100, self._poll_queue)

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
        self.port_var = tk.StringVar(value="/dev/ttyACM0")
        ttk.Entry(top, textvariable=self.port_var, width=16).pack(
            side="left", padx=(4, 8)
        )
        self.connect_btn = ttk.Button(
            top, text="Connect", command=self._on_connect_toggle
        )
        self.connect_btn.pack(side="left")

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
        self.log.insert("end", text + "\n")
        self.log.see("end")

    # ---- build & flash ----
    def _on_build_flash(self):
        self.build_btn.config(state="disabled")
        threading.Thread(target=self._build_flash_worker, daemon=True).start()

    def _build_flash_worker(self):
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
            self.after(0, lambda: self.build_btn.config(state="normal"))
            return

        device = find_daplink_device()
        if device is None:
            self._log("DAPLink device (label DAPLINK) not found via lsblk.")
            self.after(0, lambda: self.build_btn.config(state="normal"))
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
            self.after(0, lambda: self.build_btn.config(state="normal"))
            return

        bin_path = project_dir / f"{target}.bin"
        self._log(f"Copying {bin_path.name} to {mount} ...")
        subprocess.run(["cp", str(bin_path), mount + "/"], check=False)
        subprocess.run(["sync"], check=False)

        self._log("Resetting target via OpenOCD ...")
        proc = subprocess.run(
            ["openocd", "-f", "openocd/aducm3029.cfg", "-c", "init",
             "-c", "reset run", "-c", "resume", "-c", "shutdown"],
            cwd=project_dir, capture_output=True, text=True,
        )
        self._log(proc.stdout + proc.stderr)
        self._log("=== Flash complete ===")
        self.after(0, lambda: self.build_btn.config(state="normal"))

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

        if is_time_series:
            self.freq_entry.pack(side="left", padx=(0, 4))
            if is_time_series_2wire:
                self.zero_ts_btn.pack(side="left", padx=(0, 4))
            self.continuous_btn.pack(side="left", padx=(0, 4))
            self.stop_btn.pack(side="left", padx=(0, 8))
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
        self.ts_mag.clear()
        self.ts_phase.clear()
        self._redraw()
        self.ser.write((command + "\r\n").encode())
        self.status_var.set(f"Sent '{command}'...")

    # ---- queue draining / plotting ----
    def _poll_queue(self):
        redraw_needed = False
        while True:
            try:
                line = self.line_queue.get_nowait()
            except queue.Empty:
                break
            self._log(line)

            m = TIME_SERIES_LINE_RE.search(line)
            if m:
                sample, _freq, _real, _imag, mag, phase = (
                    float(x) for x in m.groups()
                )
                if self.plot_mode != "timeseries":
                    self.plot_mode = "timeseries"
                    self._configure_axes()
                self.ts_sample.append(sample)
                self.ts_mag.append(mag)
                self.ts_phase.append(phase)
                redraw_needed = True
                continue

            m = DATA_LINE_RE.search(line)
            if m:
                freq, _real, _imag, mag, phase = (float(x) for x in m.groups())
                if self.plot_mode != "sweep":
                    self.plot_mode = "sweep"
                    self._configure_axes()
                self.sweep_freq.append(freq)
                self.sweep_mag.append(mag)
                self.sweep_phase.append(phase)
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
        frequency, log-x) and the impedance time-series view (vs. sample #,
        linear-x). Called whenever the format of incoming data lines
        changes, not tied to the firmware dropdown -- see _poll_queue."""
        if self.plot_mode == "timeseries":
            self.ax_mag.set_ylabel("|Z| (ohm)")
            self.ax_phase.set_ylabel("phase (deg)")
            self.ax_mag.set_xscale("linear")
            self.ax_phase.set_xscale("linear")
            self.ax_phase.set_xlabel("sample #")
        else:
            self.ax_mag.set_ylabel("|Z| (ohm)")
            self.ax_phase.set_ylabel("phase (deg)")
            self.ax_mag.set_xscale("log")
            self.ax_phase.set_xscale("log")
            self.ax_phase.set_xlabel("frequency (Hz)")

    def _redraw(self):
        if self.plot_mode == "timeseries":
            x, mag, phase = self.ts_sample, self.ts_mag, self.ts_phase
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
