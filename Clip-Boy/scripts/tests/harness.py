"""
harness.py — Shared test helper for Clip-Boy badge tests.

Manages a persistent serial session to avoid DTR resets.
All test scripts should use this instead of calling test_bridge.py directly.

Usage:
    from harness import Harness

    h = Harness()          # Opens session, waits for boot, skips boot screen
    h.nav(1, 2)            # Navigate to ITEMS > SAOs
    state = h.state()      # Query UI state
    h.screenshot("out.bmp")
    fps = h.fps()          # Query FPS
    h.close()              # Close session
"""

import subprocess
import json
import sys
import os
import time
import signal

BRIDGE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "test_bridge.py")

class HarnessTimeout(RuntimeError):
    """The badge did not answer a query within the retry budget.

    Deliberately an EXCEPTION rather than a falsy return: "the badge said no" and "the badge
    said nothing" must never be the same value. A dropped native-CDC response silently
    becoming an empty result is what made SB1 look like an intermittent firmware bug, and it
    survived two refuted hypotheses and a shipped fix before the truth showed up in a diagnostic.
    """


class Harness:
    """Persistent serial session to the Clip-Boy test harness."""

    # Post-tap settle: a tap that rebuilds a screen (nav/modal open/close) isn't done
    # when `touch` returns -- the next command can read a half-built UI and log a FALSE
    # error. Sleep after each tap so the UI quiesces first. Tunable via CLIPBOY_TAP_SETTLE
    # (seconds) -- set it to 0 for a fast local run where flakiness isn't a concern.
    TAP_SETTLE_S = float(os.environ.get("CLIPBOY_TAP_SETTLE", "1.0"))

    def __init__(self, port=None, skip_boot=True):
        # Port precedence: explicit arg > CLIPBOY_PORT env > bridge auto-detect.
        # The env hook lets run_all.py / any auto-detecting module be aimed at a
        # SPECIFIC badge when several ESP32-S3s are enumerated (otherwise the bridge
        # grabs whichever COM port Windows lists first — non-deterministic with two
        # badges attached). Scripts that pass an explicit port still win.
        if port is None:
            port = os.environ.get("CLIPBOY_PORT") or None
        # REMEMBER the resolved port. reboot_and_wait() re-runs __init__, and it used to do so
        # WITHOUT the port -- so after the first reboot the session fell through to the bridge's
        # auto-detect and grabbed "whichever COM port Windows lists first". With two badges
        # attached that silently retargets the rest of the run at the OTHER badge. See the note
        # on reboot_and_wait for what that cost.
        self.port = port
        cmd = [sys.executable, BRIDGE_PATH]
        if port:
            cmd += ["--port", port]
        cmd.append("session")

        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)

        # Wait for session ready
        ready = self._read_response()
        if not ready.get("session") == "ready":
            raise RuntimeError(f"Session failed to start: {ready}")

        if skip_boot:
            self.cmd("skip_boot")

    # Idempotent read/query commands: safe to AUTO-RETRY when the native USB-CDC
    # drops/truncates a single response under load. A lost response otherwise times out
    # and false-fails a test in a random spot each run (sd_exists, cat_pos/tool_list,
    # state...). Mutations / stateful commands (tool_start, touch, reboot, sd_write,
    # cfg_set, kb_*) are NOT auto-retried -- those are handled case-by-case (e.g.
    # tool_start via confirm-by-state in the tool tests).
    _RETRYABLE = frozenset({
        "ping", "state", "heap", "fps", "led_rate", "neopixel_state", "tool_state",
        "tool_list", "pkt_counters", "detect_counts", "text", "tree",
        "sd_exists", "sd_list", "sd_read", "cfg_get",
        # `find` was MISSING here, and it is the query every touch-driven test runs through
        # (tap_text -> find/find_exact). A dropped response therefore returned no hits, which
        # is indistinguishable from "the widget isn't there" -- so a CDC drop under load read
        # as a firmware failure. That is what made SB1 look like an intermittent keyboard bug
        # for hours: the modal was on screen every time, but the answer never came back.
        "find",
        # Other pure queries that were likewise un-retried.
        "channel_activity", "hr_debug",
    })

    def cmd(self, command, _max_reads=64):
        """Send a command and return the JSON response. Auto-retries idempotent
        read/query commands (see _RETRYABLE) when the response is LOST/timed out to
        the native-CDC drop-under-load, so a single dropped response doesn't
        false-fail a test. Real error responses (not timeouts) are returned as-is."""
        first = command.split()[0] if command else ""
        attempts = 3 if first in self._RETRYABLE else 1
        last = None
        for i in range(attempts):
            last = self._cmd_once(command, _max_reads)
            if not isinstance(last, dict) or last.get("ok"):
                return last
            if "No response" not in str(last.get("error", "")):
                return last              # a genuine error response -> don't retry
            if i + 1 < attempts:
                time.sleep(0.25)
        return last

    def _cmd_once(self, command, _max_reads=64):
        """One send/read round-trip (the un-retried primitive).

        The verbose --test build can leak an async log line into the session
        stream during long idles (e.g. while a companion device generates RF),
        which arrives as {"raw": <non-JSON>}. Skip those and return the first
        properly-framed JSON response so a stimulus wait can't desync the reader.
        """
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        last = None
        for _ in range(_max_reads):
            last = self._read_response()
            if isinstance(last, dict) and "raw" not in last:
                return last
        return last

    def _read_response(self):
        """Read one JSON line from the session stdout."""
        line = self.proc.stdout.readline().strip()
        if not line:
            raise RuntimeError("Session closed unexpectedly")
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            return {"raw": line}

    def close(self):
        """Close the session gracefully."""
        try:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()

    # ─── Convenience methods ───────────────────────────────────────────

    def nav(self, div, tab):
        return self.cmd(f"nav {div} {tab}")

    def state(self):
        return self.cmd("state")

    def touch(self, x, y, action="tap"):
        resp = self.cmd(f"touch {x} {y} {action}")
        # Let the tap's effect (nav rebuild, modal open/close) settle before the caller
        # reads state, so a not-yet-built UI isn't mistaken for a failure.
        if action in ("tap", "release") and self.TAP_SETTLE_S > 0:
            time.sleep(self.TAP_SETTLE_S)
        return resp

    def swipe(self, x1, y1, x2, y2, steps=10):
        return self.cmd(f"swipe {x1} {y1} {x2} {y2} {steps}")

    def screenshot(self, filepath, verify=True):
        """Capture the live framebuffer to `filepath`.

        The bridge converts the RGB565 frame to a 24-bit BMP (unless the path
        ends in .raw/.bin, which gets the raw dump). With verify=True (default)
        a .bmp output is sanity-checked for the "BM" magic so a broken capture
        path fails the test loudly instead of leaving an un-openable file
        (DC34-104).
        """
        resp = self.cmd(f"screenshot {filepath}")
        if verify and resp.get("ok") and filepath.lower().endswith(".bmp"):
            out = resp.get("file", filepath)
            if not os.path.isfile(out):
                raise AssertionError(f"screenshot reported ok but {out} is missing")
            with open(out, "rb") as f:
                magic = f.read(2)
            if magic != b"BM":
                raise AssertionError(
                    f"screenshot {out} is not a valid BMP (magic={magic!r})")
        return resp

    def fps(self):
        return self.cmd("fps")

    def fps_reset(self):
        return self.cmd("fps reset")

    def led_rate(self):
        return self.cmd("led_rate")

    def heap(self):
        return self.cmd("heap")

    def tool_state(self):
        return self.cmd("tool_state")

    def neopixel_state(self):
        return self.cmd("neopixel_state")

    def text(self):
        return self.cmd("text")

    def find(self, needle):
        """Locate widgets whose label contains `needle`; returns the hits list.

        Each hit has the label centre (x,y) plus hit_x/hit_y = the centre of the nearest
        CLICKABLE ancestor, which is what actually receives the tap (a button keeps its
        label in a child). Use tap_text() unless you need the geometry.

        RAISES HarnessTimeout if the response never came back, rather than returning [].
        An empty list means "the badge answered, and the widget is not there"; a timeout means
        "we don't know". Collapsing those two into [] is what turned a dropped serial response
        into a phantom firmware bug (SB1), and it cost two wrong hypotheses and a wasted fix
        before the diagnostic showed the widget had been on screen all along.
        """
        return self._find_raw(f"find {needle}", needle)

    def find_exact(self, label):
        """Exact-label match. Prefer this for short labels -- find("L") matches "FL"."""
        return self._find_raw(f"find ={label}", label)

    def _find_raw(self, command, needle):
        resp = self.cmd(command) or {}
        if not resp.get("ok") and "No response" in str(resp.get("error", "")):
            raise HarnessTimeout(
                f"find {needle!r}: no response after retries -- the badge did not answer, so "
                f"whether the widget exists is UNKNOWN (do not read this as 'not found')")
        return resp.get("hits") or []

    def tap_text(self, needle, index=0, settle_ms=None):
        """Tap the widget whose label contains `needle`. Returns True if something was hit.

        This is what makes touch-driven bugs testable at all: `tree` reports only a node
        count and `text` has no geometry, so repros behind a real button used to need a
        human finger.
        """
        hits = self.find_exact(needle[1:]) if needle.startswith("=") else self.find(needle)
        if len(hits) <= index:
            return False
        hp = hits[index]
        # Refuse to tap a target that is not actually visible. lv_obj_get_coords() reports
        # laid-out coordinates even for a widget scrolled past the content pane, and tapping
        # those pixels hits whatever IS there -- for an overflowing list that is the division
        # bar, so the tap silently NAVIGATES and the test fails somewhere else entirely.
        if hp.get("onscreen") is False:
            self.last_tap_error = (f"target {needle!r} is off-screen at "
                                   f"({hp['hit_x']},{hp['hit_y']}) -- scroll it into view first")
            return False
        self.last_tap_error = None
        self.touch(hp["hit_x"], hp["hit_y"], "tap")
        if settle_ms:
            self.wait(settle_ms)
        return True

    # The badge's content pane, in screen coords: status bar 16px + tab bar 26px above,
    # division bar 26px below (320x240 landscape). A widget laid out outside this band is
    # scrolled out of view, and tapping its coordinates hits whatever IS there.
    CONTENT_TOP = 42
    CONTENT_BOT = 214

    def scroll_into_view(self, needle, tries=6, step_px=80):
        """Swipe the containing list until `needle` is actually visible; return its hit or None.

        `tap_text` correctly REFUSES an off-screen target (tapping its laid-out coordinates hits
        the division bar and silently navigates), but nothing scrolled -- so a target below the
        fold was simply untestable. That is how the loss-of-signal battery's RSSI case, the most
        valuable one in it, reported cannot-test with the AP it needed sitting one swipe away.

        Scrolls in the direction the laid-out y implies, re-checking after each swipe rather than
        assuming a fixed number of swipes lands it. Returns None if the label does not exist at
        all, which the caller must distinguish from "exists but unreachable".
        """
        exact = needle.startswith("=")
        for _ in range(tries + 1):
            hits = self.find_exact(needle[1:]) if exact else self.find(needle)
            if not hits:
                return None
            hp = hits[0]
            if hp.get("onscreen") is not False:
                return hp
            x = max(8, min(312, int(hp.get("hit_x", 160))))
            y = int(hp.get("hit_y", 0))
            if y >= self.CONTENT_BOT:          # below the fold -> drag content upward
                self.swipe(x, self.CONTENT_BOT - 20, x, self.CONTENT_BOT - 20 - step_px, 10)
            elif y <= self.CONTENT_TOP:        # above the fold -> drag content downward
                self.swipe(x, self.CONTENT_TOP + 20, x, self.CONTENT_TOP + 20 + step_px, 10)
            else:
                return hp                      # inside the band but flagged: let the caller try
            time.sleep(0.4)
        return None

    def tap_text_scrolled(self, needle, settle_ms=None, tries=6):
        """scroll_into_view + tap. Use for any list that can overflow the content pane."""
        hp = self.scroll_into_view(needle, tries=tries)
        if not hp:
            self.last_tap_error = f"{needle!r} not found, or could not be scrolled into view"
            return False
        self.last_tap_error = None
        self.touch(hp["hit_x"], hp["hit_y"], "tap")
        if settle_ms:
            self.wait(settle_ms)
        return True

    def tree(self):
        return self.cmd("tree")

    def sensor_mock(self, zones_csv):
        return self.cmd(f"sensor_mock {zones_csv}")

    def sensor_real(self):
        return self.cmd("sensor_real")

    def geiger_start(self):
        """Start geiger counter (deauth sniffer + audio)."""
        return self.cmd("geiger_start")

    def geiger_stop(self):
        """Stop geiger counter."""
        return self.cmd("geiger_stop")

    def tool_start(self, cat, item):
        """Start a tool by category and item index."""
        return self.cmd(f"tool_start {cat} {item}")

    def cat_pos(self, name):
        """Return the array position of a tool category by name (robust to
        reorg/SKU), or None if absent. The Sn34k-Boy build compiles out the
        ACTIVE RESEARCH cats, so e.g. cat_pos("Deauth") is None there — callers
        must guard for None and skip those tools on Sn34k.

        Retries when the tool_list response comes back EMPTY *or SHORT*: under load the
        native USB-CDC can drop or TRUNCATE a single response.

        ⚠ The earlier version only retried on an EMPTY list and took "populated but the
        name isn't in it" as a definitive SKU-gated absence. That is wrong, and it produced
        a VACUOUS PASS on hardware (2026-07-26): on a res34rch build, `tool_list` carries 12
        categories instead of Sn34k's 6, and `Deauth` sits at index 6 -- so a truncated read
        returned the first few categories, parsed fine, and lacked `Deauth`. cat_pos reported
        None, R1 concluded "skipped: Sn34k build has no TAT_STA tools", and the suite scored
        that skip as a PASS. The badge was demonstrably res34rch (12 cats, Deauth at 6) when
        queried seconds later.

        The response reports `count` = NUM_TOOL_CATS, so completeness is checkable rather
        than assumed: len(cats) < count means the read was truncated, NOT that the build
        lacks the category. Only a COMPLETE list may answer "absent"."""
        for attempt in range(4):
            resp = self.cmd("tool_list") or {}
            cats = resp.get("cats", [])
            count = resp.get("count")
            complete = bool(cats) and (count is None or len(cats) == count)
            if complete:
                for i, c in enumerate(cats):
                    if c["name"] == name:
                        return i
                return None                      # complete list, genuinely absent
            self.last_cat_pos_error = (
                f"tool_list read {len(cats)}/{count} categories on attempt {attempt + 1}")
            time.sleep(0.3)
        # Never silently downgrade an unreadable list to "absent" -- that is the vacuous-pass
        # path. Raise so the caller reports cannot-test instead of skipped.
        raise HarnessTimeout(
            f"tool_list never returned a complete category list "
            f"({getattr(self, 'last_cat_pos_error', 'no reads')}) -- cannot tell whether "
            f"{name!r} is SKU-gated or the response was truncated")

    def tool_stop(self):
        """Stop any running tool/geiger."""
        return self.cmd("tool_stop")

    def kb_type(self, text):
        """Type text into the active keyboard modal textarea."""
        return self.cmd(f"kb_type {text}")

    def kb_ok(self):
        """Press OK on the active keyboard modal."""
        return self.cmd("kb_ok")

    def kb_cancel(self):
        """Press Cancel on the active keyboard modal."""
        return self.cmd("kb_cancel")

    def ap_scan(self, target_ssid=None):
        """Scan for APs, optionally select one by SSID. Blocking (~5-10s)."""
        cmd = f"ap_scan {target_ssid}" if target_ssid else "ap_scan"
        return self.cmd(cmd)

    def ap_list(self, ssid_or_bssid=None):
        """READ-ONLY dump of the current access_points list (no scan, no state
        change): per-entry i/essid/bssid/ch/rssi/pkts/sel. Optional essid-or-bssid
        substring filter. This is the JSON reader for the AP list -- the stock
        `list -a` CLI omits BSSID AND its plain-text output is dropped by
        test_bridge (non-STX), so it is unreadable from automation. Capped at 12
        entries to stay under the CDC TX buffer. Use to answer 'one entry or a
        wrong-channel duplicate for this BSSID?' -- e.g. classify a station-directed
        miss as a dedup bug vs. a station->AP mis-attribution."""
        return self.cmd(f"ap_list {ssid_or_bssid}" if ssid_or_bssid else "ap_list")

    def mac_track(self, mac_filter=None):
        """READ-ONLY dump of Monitor>MAC Tracker's top-talker table (top-10 by
        frame count): [{mac, frames, rssi}]. Mirrors the shipping poller
        (getMACTrackerTop10 over a static POD array) -- race/UAF-safe. Optional MAC
        substring filter. Use to prove MAC Tracker RECEIVES: an injected emitter's
        src MAC appears with frames>0 vs a fabricated MAC that never does."""
        return self.cmd(f"mac_track {mac_filter}" if mac_filter else "mac_track")

    def sd_list(self, path="/"):
        """List files on SD card."""
        return self.cmd(f"sd_list {path}")

    def sd_read(self, path):
        """Read a file from SD card (text, max 64KB)."""
        return self.cmd(f"sd_read {path}")

    def sd_write(self, path, content):
        """Write text content to a file on SD card."""
        return self.cmd(f"sd_write {path} {content}")

    def sd_exists(self, path):
        """Check if a file exists on SD card."""
        return self.cmd(f"sd_exists {path}")

    def sd_rm(self, path):
        """Remove a file from SD card. Response has 'removed' bool."""
        return self.cmd(f"sd_rm {path}")

    def cfg_get(self, key):
        """Read a persistent config value. Returns response with 'value' field."""
        return self.cmd(f"cfg_get {key}")

    def cfg_set(self, key, value):
        """Write a persistent config value (saves to NVS immediately)."""
        if isinstance(value, bool):
            value = "true" if value else "false"
        return self.cmd(f"cfg_set {key} {value}")

    def theme_set(self, idx):
        """Switch to theme live: restyle + persist + rebuild screen. idx 0-2."""
        return self.cmd(f"theme_set {idx}")

    def led_set(self, idx, r, g, b, brightness=255, anim=0, speed=5):
        """Set per-LED config. anim: 0=None, 1=Breathe, 2=Chase."""
        return self.cmd(f"led_set {idx} {r} {g} {b} {brightness} {anim} {speed}")

    def led_preset(self, idx):
        """Apply preset. 0=Mojave, 1=RibbitCity, 2=Flashbang, 3=Rainbow, 4=Off."""
        return self.cmd(f"led_preset {idx}")

    def hr_scan_start(self):
        """Start HR code scanner programmatically."""
        return self.cmd("hr_scan_start")

    def hr_scan_stop(self):
        """Stop HR code scanner."""
        return self.cmd("hr_scan_stop")

    def theremin_start(self):
        """Enable the theremin (claims VL53L5CX + audio synth)."""
        return self.cmd("theremin_start")

    def theremin_stop(self):
        """Disable the theremin (releases sensor + audio)."""
        return self.cmd("theremin_stop")

    def hr_feed(self, zones_csv):
        return self.cmd(f"hr_feed {zones_csv}")

    def pkt_counters(self):
        """WiFi frame counters (mgmt/data/beacon/deauth/eapol/req/resp/numProbe)."""
        return self.cmd("pkt_counters")

    def detect_counts(self):
        """Detection/list counts (ap/sta/ssid/bt/airtag/flipper/flock/pwn/probe/esp/multissid)."""
        return self.cmd("detect_counts")

    def cli(self, command):
        """Run a serial CLI command -- harness-side (`coll add 1`, `heap`) or, for anything else,
        the REAL MARAUDER parser via cli_obj.runCommand().

        The badge's Marauder CLI only ever sees plain-text lines, while harness commands are
        STX-framed and intercepted first, so the CLI was unreachable from automation until the
        firmware side of this was made to actually forward (it previously acknowledged commands it
        never ran). Two surfaces are reachable ONLY this way: the CLI-only deref sites, and whether
        the CLI honours Airplane mode -- its gate lives in the UI layer, not in the CLI.
        The response carries "via": "coll" | "harness" | "marauder" -- check it rather than assuming
        the command was executed. The CLI's own output arrives as non-JSON lines and is skipped.
        """
        return self.cmd(f"cli {command}")

    # Where auto-captured failure evidence lands.
    FAIL_SHOT_DIR = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "shots", "failures"))

    def snap_on_failure(self, label):
        """Best-effort screenshot for a FAILING or CANNOT-TEST case. Returns a path or None.

        WHY: every other observation a touch test makes -- find(), text(), tool_state() -- goes
        through the same object-tree walk, so when that layer misleads, every assertion built on it
        inherits the error silently. The framebuffer is an INDEPENDENT channel: it is what the user
        actually gets. A failure without pixels is a puzzle; with them it is usually obvious.
        Earned: `find` returns the nearest CLICKABLE ANCESTOR when the labelled widget is not
        clickable, so a tap "succeeded" onto a 172px container instead of a 42px button -- and one
        screenshot would have shown that button plainly dimmed while I spent three probe runs
        arguing with the tree.

        ⚠ LIMITS, so these files are not over-read: a screenshot shows WHAT IS DISPLAYED and
        nothing else. It cannot establish that a control is disabled (appearance and capability are
        independent in every UI toolkit), it shows state rather than causation, and the capture path
        can fail on its own. A lead, not a verdict.

        NEVER RAISES. A diagnostic must not become the failure it documents: if the badge is wedged,
        capture is precisely what will not work, and that must not mask the real result.
        """
        try:
            os.makedirs(self.FAIL_SHOT_DIR, exist_ok=True)
            safe = "".join(c if (c.isalnum() or c in "-_.") else "_" for c in str(label))[:48]
            base = os.path.join(self.FAIL_SHOT_DIR, f"{safe}_{int(time.time())}")
            bmp = base + ".bmp"
            self.screenshot(bmp, verify=False)
            if not os.path.isfile(bmp):
                return None
            # Also emit a PNG when the converter is importable: a PNG opens anywhere and embeds in
            # the HTML report. The BMP is kept as the unmodified capture.
            try:
                import base64
                import gen_test_report as _g
                b64 = _g.bmp_to_png_b64(bmp)
                if b64:
                    with open(base + ".png", "wb") as f:
                        f.write(base64.b64decode(b64))
            except Exception:
                pass
            return bmp
        except Exception:
            return None

    def ping(self):
        return self.cmd("ping")

    def skip_boot(self):
        return self.cmd("skip_boot")

    def reboot_and_wait(self, timeout=25):
        """Reboot device and reconnect. 25s default covers the ~20s post-boot
        window where harness commands that touch WiFi or NimBLE race with
        still-settling init tasks and time out.

        ⚠ RECONNECT TO THE SAME BADGE. This used to call `self.__init__(skip_boot=True)` with
        no port, so the reconnect fell through to CLIPBOY_PORT (usually unset) and then to the
        bridge's auto-detect, which grabs whichever COM port Windows lists FIRST. With two
        badges attached that silently retargeted every test after the first inter-test reboot
        at the OTHER badge. Found 2026-07-26 and it explains two separate confusing results in
        one session: a res34rch suite whose R1 reported "skipped: Sn34k build has no TAT_STA
        tools" (it had drifted onto the Sn34k badge, whose tool_list really does lack Deauth),
        and a concurrent run that died with PermissionError(13) because BOTH sessions had ended
        up on the same port. A test that quietly changes which device it is measuring reports
        confident results about the wrong hardware -- and any prior two-badge run is suspect."""
        self.cmd("reboot")
        self.close()
        time.sleep(timeout)
        self.__init__(port=self.port, skip_boot=True)

    # ─── Assertions ────────────────────────────────────────────────────

    def assert_state(self, div=None, tab=None, div_name=None, tab_name=None):
        """Query state and assert expected values. Returns state dict."""
        s = self.state()
        assert s.get("ok"), f"State query failed: {s}"
        if div is not None:
            assert s["div"] == div, f"Expected div={div}, got {s['div']}"
        if tab is not None:
            assert s["tab"] == tab, f"Expected tab={tab}, got {s['tab']}"
        if div_name is not None:
            assert s["div_name"] == div_name, f"Expected div_name='{div_name}', got '{s['div_name']}'"
        if tab_name is not None:
            assert s["tab_name"] == tab_name, f"Expected tab_name='{tab_name}', got '{s['tab_name']}'"
        return s

    def assert_fps_above(self, threshold=15):
        """Query FPS and assert it's above threshold. Returns fps dict."""
        f = self.fps()
        assert f.get("ok"), f"FPS query failed: {f}"
        current = f.get("fps", 0)
        assert current >= threshold, f"FPS {current} below threshold {threshold}"
        return f

    def assert_tool_running(self, expected=True):
        """Assert tool running state. Returns tool_state dict."""
        w = self.tool_state()
        assert w.get("ok"), f"Tool state query failed: {w}"
        assert w["running"] == expected, \
            f"Expected tool running={expected}, got {w['running']} (name='{w.get('name', '')}')"
        return w

    def wait(self, ms):
        """Wait specified milliseconds."""
        time.sleep(ms / 1000.0)
