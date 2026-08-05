#!/usr/bin/env python3
"""
test_bridge.py — Serial bridge for Clip-Boy automated testing.

Communicates with the firmware test harness (test_harness.h) over USB-CDC serial.
Protocol: STX (0x02) prefix + command + newline → STX + JSON response + newline [+ binary payload]

Usage:
    py -3 scripts/test_bridge.py ping
    py -3 scripts/test_bridge.py screenshot --file output/screen.bmp
    py -3 scripts/test_bridge.py tap 160 120
    py -3 scripts/test_bridge.py nav 1 2
    py -3 scripts/test_bridge.py state
    py -3 scripts/test_bridge.py heap
    py -3 scripts/test_bridge.py tree
    py -3 scripts/test_bridge.py text
    py -3 scripts/test_bridge.py sensor_mock 200,200,...  (64 values)
    py -3 scripts/test_bridge.py sensor_real
    py -3 scripts/test_bridge.py audio --frames 4096 --file output/audio.wav
    py -3 scripts/test_bridge.py neopixel_state
    py -3 scripts/test_bridge.py skip_boot
    py -3 scripts/test_bridge.py reboot

Requires: pip install pyserial
"""

import sys
import os
import json
import time
import struct
import argparse
import serial
import serial.tools.list_ports

STX = 0x02
DEFAULT_TIMEOUT = 5.0
RETRY_COUNT = 1

# ─── Port detection ────────────────────────────────────────────────────────

ESP32_S3_VID = 0x303A  # Espressif VID

def find_esp32_port():
    """Auto-detect ESP32-S3 USB-CDC port."""
    for p in serial.tools.list_ports.comports():
        if p.vid == ESP32_S3_VID:
            return p.device
    # Fallback: look for common descriptions
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        if "esp32" in desc or "usb-serial" in desc or "usb serial" in desc:
            return p.device
    return None

# ─── Serial communication ─────────────────────────────────────────────────

class Bridge:
    def __init__(self, port=None, timeout=DEFAULT_TIMEOUT):
        if port is None:
            port = find_esp32_port()
            if port is None:
                raise RuntimeError("No ESP32-S3 device found. Is it connected?")
        # ESP32-S3 native USB-CDC: opening the port with DTR asserted pulls the
        # chip's reset line and REBOOTS the badge (losing runtime state). Create
        # the port unopened, drop DTR/RTS, THEN open -- no reset pulse. CDC data
        # flows fine without DTR (hr_diag.py has always done this).
        self.ser = serial.Serial(baudrate=115200, timeout=timeout, write_timeout=3)
        self.ser.port = port
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self._ensure_ready()

    def _ensure_ready(self):
        """Verify the test harness responds. If not, wait for boot."""
        # Quick probe: try a ping with short timeout
        old_timeout = self.ser.timeout
        self.ser.timeout = 1.5
        try:
            self._expect_cmd = "ping"
            self.ser.reset_input_buffer()
            self.ser.write(bytes([STX]) + b"ping\n")
            self.ser.flush()
            resp = self._read_response()
            if resp.get("ok"):
                return  # Already responsive
        except (TimeoutError, serial.SerialTimeoutException):
            pass
        finally:
            self.ser.timeout = old_timeout

        # Device likely rebooting — wait for boot (up to 15s)
        sys.stderr.write("Waiting for device boot...\n")
        deadline = time.time() + 15
        while time.time() < deadline:
            time.sleep(1)
            self.ser.timeout = 2
            try:
                self.ser.reset_input_buffer()
                self._expect_cmd = "ping"
                self.ser.write(bytes([STX]) + b"ping\n")
                self.ser.flush()
                resp = self._read_response()
                if resp.get("ok"):
                    self.ser.timeout = old_timeout
                    return
            except (TimeoutError, serial.SerialTimeoutException):
                continue
        self.ser.timeout = old_timeout
        sys.stderr.write("WARNING: device may not be responsive\n")

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_command(self, cmd_str):
        """Send an STX-prefixed command and return the JSON response dict."""
        # Extract the command name for response matching
        self._expect_cmd = cmd_str.split()[0] if cmd_str else ""
        self.ser.reset_input_buffer()
        payload = bytes([STX]) + cmd_str.encode("utf-8") + b"\n"
        self.ser.write(payload)
        self.ser.flush()
        return self._read_response()

    def _read_response(self):
        """Read lines until we get an STX-prefixed JSON command response."""
        expect = getattr(self, "_expect_cmd", "")
        deadline = time.time() + self.ser.timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            # Look for STX byte anywhere in the line (may follow other output)
            stx_pos = raw.find(bytes([STX]))
            if stx_pos >= 0:
                json_str = raw[stx_pos + 1:].decode("utf-8", errors="replace").strip()
                try:
                    obj = json.loads(json_str)
                except json.JSONDecodeError:
                    # A stray STX inside corrupted/binary/log bytes is NOT a real
                    # response. Returning it here (the old behavior) made ONE
                    # corrupted frame desync EVERY subsequent command by one
                    # (off-by-one), which is what turned a transient collision into
                    # a permanent session wedge. Skip and keep scanning for the real
                    # STX + cmd-matching JSON reply (or time out cleanly).
                    continue
                # Must have "cmd" matching what we sent, or "ok" key
                cmd_match = obj.get("cmd", "") == expect if expect else True
                if ("ok" in obj) and cmd_match:
                    return obj
                # Response for different command or event — skip
                continue
            # Skip non-STX lines (normal Serial.println output)
        raise TimeoutError(f"No response for '{expect}' from test harness (timeout)")

    def read_binary(self, size):
        """Read exactly `size` bytes of binary data after a JSON header."""
        data = b""
        # Full-frame screenshots transfer slowly over USB-CDC (firmware flushes
        # every 1KB chunk + an 80ms tail delay), so give a generous floor on top
        # of the base port timeout rather than the optimistic ~500KB/s estimate
        # that occasionally truncated big captures (DC34-104).
        deadline = time.time() + self.ser.timeout + max(5.0, size / 30000)
        while len(data) < size and time.time() < deadline:
            chunk = self.ser.read(size - len(data))
            if chunk:
                data += chunk
        if len(data) < size:
            # Flush leftover offset bytes so a truncated binary frame can't bleed
            # into the next command's response (keeps a bad frame from wedging the
            # session).
            try:
                self.ser.reset_input_buffer()
            except Exception:
                pass
            raise TimeoutError(f"Binary read incomplete: got {len(data)}/{size} bytes")
        return data

    def read_stream_until_end(self, cmd_name):
        """Read streaming lines (tree, text) until end marker."""
        lines = []
        deadline = time.time() + self.ser.timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            # Check for STX byte anywhere in line (end marker)
            stx_pos = raw.find(bytes([STX]))
            if stx_pos >= 0:
                json_str = raw[stx_pos + 1:].decode("utf-8", errors="replace").strip()
                try:
                    obj = json.loads(json_str)
                    if obj.get("end"):
                        return lines
                except json.JSONDecodeError:
                    pass
            else:
                # Non-STX line is a data line (widget tree entries)
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    try:
                        lines.append(json.loads(line))
                    except json.JSONDecodeError:
                        lines.append({"raw": line})
        return lines


# ─── BMP writer (RGB565 → 24-bit BMP, no PIL needed) ──────────────────────

def rgb565_to_bmp(data, width, height, stride):
    """Convert RGB565 raw data to 24-bit BMP file bytes."""
    row_size = width * 3
    row_pad = (4 - (row_size % 4)) % 4
    bmp_row = row_size + row_pad
    pixel_data_size = bmp_row * height
    file_size = 54 + pixel_data_size

    # BMP header (14 bytes) + DIB header (40 bytes)
    header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII",
                      40, width, -height,  # negative height = top-down
                      1, 24, 0, pixel_data_size, 2835, 2835, 0, 0)

    rows = []
    for y in range(height):
        row_start = y * stride
        row_bytes = b""
        for x in range(width):
            offset = row_start + x * 2
            if offset + 1 < len(data):
                pixel = data[offset] | (data[offset + 1] << 8)
            else:
                pixel = 0
            r = ((pixel >> 11) & 0x1F) * 255 // 31
            g = ((pixel >> 5) & 0x3F) * 255 // 63
            b = (pixel & 0x1F) * 255 // 31
            row_bytes += bytes([b, g, r])  # BMP is BGR
        row_bytes += b"\x00" * row_pad
        rows.append(row_bytes)

    return header + dib + b"".join(rows)


# ─── WAV writer ────────────────────────────────────────────────────────────

def pcm_to_wav(data, sample_rate=44100, channels=2, bits=16):
    """Wrap raw PCM data in a WAV header."""
    data_size = len(data)
    byte_rate = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    header = struct.pack("<4sI4s4sIHHIIHH4sI",
                         b"RIFF", 36 + data_size, b"WAVE",
                         b"fmt ", 16, 1, channels,
                         sample_rate, byte_rate, block_align, bits,
                         b"data", data_size)
    return header + data


# ─── Command implementations ──────────────────────────────────────────────

def cmd_port(args):
    port = find_esp32_port()
    if port:
        print(json.dumps({"ok": True, "port": port}))
    else:
        print(json.dumps({"ok": False, "error": "No ESP32-S3 found"}))
        return 1
    return 0

def cmd_ping(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("ping")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_raw(args):
    # Send an arbitrary harness command (e.g. "hr_anchor 1") without needing a
    # dedicated subcommand. Useful for new/rare test-harness commands.
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command(args.command_string)
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_heap(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("heap")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_state(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("state")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_screenshot(args):
    bridge = Bridge(args.port, timeout=10.0)
    try:
        resp = bridge.send_command("screenshot")
        if not resp.get("ok"):
            print(json.dumps(resp))
            return 1

        size = resp.get("size", 0)
        width = resp.get("width", 320)
        height = resp.get("height", 240)
        stride = resp.get("stride", width * 2)
        data = bridge.read_binary(size)

        out_file = args.file or "screenshot.bmp"
        os.makedirs(os.path.dirname(out_file) or ".", exist_ok=True)
        bmp = rgb565_to_bmp(data, width, height, stride)
        with open(out_file, "wb") as f:
            f.write(bmp)

        print(json.dumps({
            "ok": True, "cmd": "screenshot",
            "file": out_file, "width": width, "height": height,
            "size_bytes": len(bmp)
        }))
        return 0
    finally:
        bridge.close()

def cmd_tap(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command(f"touch {args.x} {args.y} tap")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_touch(args):
    bridge = Bridge(args.port)
    try:
        action = args.action or "tap"
        resp = bridge.send_command(f"touch {args.x} {args.y} {action}")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_swipe(args):
    bridge = Bridge(args.port)
    try:
        steps = args.steps or 10
        resp = bridge.send_command(f"swipe {args.x1} {args.y1} {args.x2} {args.y2} {steps}")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_nav(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command(f"nav {args.div} {args.tab}")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_tree(args):
    bridge = Bridge(args.port, timeout=10.0)
    try:
        resp = bridge.send_command("tree")
        if not resp.get("ok"):
            print(json.dumps(resp))
            return 1
        widgets = bridge.read_stream_until_end("tree")
        print(json.dumps({"ok": True, "cmd": "tree", "widgets": widgets}))
        return 0
    finally:
        bridge.close()

def cmd_text(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("text")
        # The text command sends a single JSON blob with texts array
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_skip_boot(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("skip_boot")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_sensor_mock(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command(f"sensor_mock {args.zones}")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_sensor_real(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("sensor_real")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_audio(args):
    bridge = Bridge(args.port, timeout=10.0)
    try:
        frames = args.frames or 4096
        resp = bridge.send_command(f"audio_capture {frames}")
        if not resp.get("ok"):
            print(json.dumps(resp))
            return 1

        size = resp.get("size", 0)
        actual_frames = resp.get("frames", 0)
        if size == 0 or actual_frames == 0:
            print(json.dumps({"ok": True, "cmd": "audio_capture",
                              "frames": 0, "message": "no audio data available"}))
            return 0

        data = bridge.read_binary(size)

        out_file = args.file or "audio.wav"
        os.makedirs(os.path.dirname(out_file) or ".", exist_ok=True)
        wav = pcm_to_wav(data,
                         sample_rate=resp.get("sample_rate", 44100),
                         channels=resp.get("channels", 2),
                         bits=resp.get("bits", 16))
        with open(out_file, "wb") as f:
            f.write(wav)

        print(json.dumps({
            "ok": True, "cmd": "audio_capture",
            "file": out_file, "frames": actual_frames,
            "size_bytes": len(wav)
        }))
        return 0
    finally:
        bridge.close()

def cmd_fps(args):
    bridge = Bridge(args.port)
    try:
        subcmd = getattr(args, 'subcmd', None) or ""
        resp = bridge.send_command(f"fps {subcmd}".strip())
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_led_rate(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("led_rate")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_tool_state(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("tool_state")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_neopixel(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("neopixel_state")
        print(json.dumps(resp))
        return 0 if resp.get("ok") else 1
    finally:
        bridge.close()

def cmd_reboot(args):
    bridge = Bridge(args.port)
    try:
        resp = bridge.send_command("reboot")
        print(json.dumps(resp))
        return 0
    finally:
        bridge.close()

def cmd_multi(args):
    """Run multiple commands on a single serial connection (avoids port re-open)."""
    bridge = Bridge(args.port, timeout=5.0)
    try:
        results = []
        for cmd_str in args.commands:
            resp = bridge.send_command(cmd_str)
            results.append(resp)
            # If command returns binary data, skip it for multi mode
            if resp.get("size") and resp.get("cmd") in ("screenshot", "audio_capture"):
                size = resp["size"]
                bridge.read_binary(size)  # consume but discard
                resp["binary"] = "discarded"
        print(json.dumps({"ok": True, "results": results}))
        return 0
    except Exception as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        return 1
    finally:
        bridge.close()


def cmd_session(args):
    """Interactive session — keeps port open, reads commands from stdin."""
    # 10s covers most slow operations (theme_set screen rebuild, BLE scans);
    # `All` BT Spam still times out even at 20s+ and is tracked separately.
    # The channel-hopping ap_scan blocks the badge ~15-22s for a full 1-14
    # sweep, so callers that need it (badge_to_badge) raise the read deadline
    # via CLIPBOY_SESSION_TIMEOUT. The deadline only bounds the MAX wait — fast
    # commands still return immediately — so a larger value is harmless.
    sess_to = float(os.environ.get("CLIPBOY_SESSION_TIMEOUT", "10"))
    bridge = Bridge(args.port, timeout=sess_to)
    try:
        # Signal ready
        print(json.dumps({"session": "ready"}), flush=True)
        for line in sys.stdin:
            line = line.strip()
            if not line or line == "quit":
                break
            parts = line.split(None, 1)
            cmd = parts[0]
            cmd_args = parts[1] if len(parts) > 1 else ""
            full_cmd = f"{cmd} {cmd_args}".strip() if cmd_args else cmd

            try:
                resp = bridge.send_command(full_cmd)
                # Handle binary payloads
                if resp.get("ok") and resp.get("size") and resp.get("cmd") == "screenshot":
                    size = resp["size"]
                    width = resp.get("width", 320)
                    height = resp.get("height", 240)
                    stride = resp.get("stride", width * 2)
                    data = bridge.read_binary(size)
                    # Parity with CLI cmd_screenshot: convert RGB565 -> 24-bit
                    # BMP. (DC34-104: session mode used to dump the raw RGB565
                    # framebuffer to the caller's .bmp path, producing an
                    # un-openable file.) A .raw/.bin path still gets the raw dump.
                    fname = cmd_args.strip() or f"screenshot_{int(time.time())}.bmp"
                    out_dir = os.path.dirname(fname)
                    if out_dir:
                        os.makedirs(out_dir, exist_ok=True)
                    if fname.lower().endswith((".raw", ".bin")):
                        payload = data
                    else:
                        payload = rgb565_to_bmp(data, width, height, stride)
                    with open(fname, "wb") as f:
                        f.write(payload)
                    resp["file"] = fname
                    resp.pop("size", None)
                elif resp.get("ok") and resp.get("size") and resp.get("cmd") == "audio_capture":
                    size = resp["size"]
                    data = bridge.read_binary(size)
                    fname = f"audio_{int(time.time())}.bin"
                    if cmd_args.strip():
                        fname = cmd_args.strip()
                    with open(fname, "wb") as f:
                        f.write(data)
                    resp["file"] = fname
                    resp.pop("size", None)
                elif resp.get("ok") and resp.get("cmd") == "tree" and resp.get("start"):
                    widgets = bridge.read_stream_until_end("tree")
                    resp = {"ok": True, "cmd": "tree", "count": len(widgets)}

                print(json.dumps(resp), flush=True)
            except Exception as e:
                print(json.dumps({"ok": False, "error": str(e)}), flush=True)
    finally:
        bridge.close()
    return 0


def cmd_wait_boot(args):
    """Wait for the test harness to be ready after a reboot."""
    timeout = args.timeout or 15
    port = args.port
    deadline = time.time() + timeout

    while time.time() < deadline:
        if port is None:
            port = find_esp32_port()
        if port is None:
            time.sleep(0.5)
            port = None  # retry detection
            continue
        try:
            bridge = Bridge(port, timeout=min(3.0, deadline - time.time()))
            resp = bridge.send_command("ping")
            bridge.close()
            if resp.get("ok"):
                print(json.dumps(resp))
                return 0
        except Exception:
            time.sleep(0.5)
            port = None  # port may have changed after reboot
            continue

    print(json.dumps({"ok": False, "error": "Timeout waiting for device boot"}))
    return 1


# ─── CLI parser ────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Clip-Boy test bridge")
    parser.add_argument("--port", "-p", help="Serial port (auto-detect if omitted)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("port", help="Detect ESP32 port")

    sub.add_parser("ping", help="Ping test harness")
    sub.add_parser("heap", help="Query heap memory")
    sub.add_parser("state", help="Query UI state")

    p_ss = sub.add_parser("screenshot", help="Capture screen")
    p_ss.add_argument("--file", "-f", default="screenshot.bmp")

    p_tap = sub.add_parser("tap", help="Tap at coordinates")
    p_tap.add_argument("x", type=int)
    p_tap.add_argument("y", type=int)

    p_touch = sub.add_parser("touch", help="Touch press/release/tap")
    p_touch.add_argument("x", type=int)
    p_touch.add_argument("y", type=int)
    p_touch.add_argument("action", nargs="?", default="tap", choices=["press", "release", "tap"])

    p_swipe = sub.add_parser("swipe", help="Swipe gesture")
    p_swipe.add_argument("x1", type=int)
    p_swipe.add_argument("y1", type=int)
    p_swipe.add_argument("x2", type=int)
    p_swipe.add_argument("y2", type=int)
    p_swipe.add_argument("--steps", type=int, default=10)

    p_nav = sub.add_parser("nav", help="Navigate to division + tab")
    p_nav.add_argument("div", type=int, choices=[0, 1, 2])
    p_nav.add_argument("tab", type=int, choices=[0, 1, 2])

    sub.add_parser("tree", help="Dump widget tree")
    sub.add_parser("text", help="Get all visible text")
    sub.add_parser("skip_boot", help="Dismiss boot screen")

    p_sm = sub.add_parser("sensor_mock", help="Inject mock sensor data")
    p_sm.add_argument("zones", help="64 comma-separated distance values (mm)")

    sub.add_parser("sensor_real", help="Switch to real sensor")

    p_audio = sub.add_parser("audio", help="Capture audio buffer")
    p_audio.add_argument("--frames", type=int, default=4096)
    p_audio.add_argument("--file", "-f", default="audio.wav")

    sub.add_parser("neopixel_state", help="Read LED states")

    p_fps = sub.add_parser("fps", help="Query FPS (or 'fps reset')")
    p_fps.add_argument("subcmd", nargs="?", default="", help="'reset' to clear min/avg")

    sub.add_parser("led_rate", help="Query LED update rate (show() calls/sec)")
    sub.add_parser("tool_state", help="Query tool/ClipBoy operation state")

    sub.add_parser("reboot", help="Reboot device")

    p_multi = sub.add_parser("multi", help="Run multiple commands on one connection")
    p_multi.add_argument("commands", nargs="+", help="Commands to run (e.g. 'nav 1 2' 'state')")

    sub.add_parser("session", help="Interactive session (reads commands from stdin, keeps port open)")

    p_wb = sub.add_parser("wait_boot", help="Wait for device to boot")
    p_wb.add_argument("--timeout", type=int, default=15)

    p_raw = sub.add_parser("raw", help="Send a raw harness command, e.g. raw \"hr_anchor 1\"")
    p_raw.add_argument("command_string", help="Full command incl. args, e.g. 'hr_anchor 1'")

    args = parser.parse_args()

    cmd_map = {
        "port": cmd_port,
        "ping": cmd_ping,
        "heap": cmd_heap,
        "state": cmd_state,
        "screenshot": cmd_screenshot,
        "tap": cmd_tap,
        "touch": cmd_touch,
        "swipe": cmd_swipe,
        "nav": cmd_nav,
        "tree": cmd_tree,
        "text": cmd_text,
        "skip_boot": cmd_skip_boot,
        "sensor_mock": cmd_sensor_mock,
        "sensor_real": cmd_sensor_real,
        "audio": cmd_audio,
        "neopixel_state": cmd_neopixel,
        "fps": cmd_fps,
        "led_rate": cmd_led_rate,
        "tool_state": cmd_tool_state,
        "reboot": cmd_reboot,
        "multi": cmd_multi,
        "session": cmd_session,
        "wait_boot": cmd_wait_boot,
        "raw": cmd_raw,
    }

    try:
        rc = cmd_map[args.command](args)
        sys.exit(rc or 0)
    except TimeoutError as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        sys.exit(1)
    except serial.SerialException as e:
        print(json.dumps({"ok": False, "error": f"Serial error: {e}"}))
        sys.exit(1)
    except RuntimeError as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        sys.exit(1)


if __name__ == "__main__":
    main()
