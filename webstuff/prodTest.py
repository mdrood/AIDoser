
import time
import json
import requests
from datetime import datetime
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

DB_URL = "https://aidoser-default-rtdb.firebaseio.com"
DEVICE_ID = "reefDoser6"
AUTH = ""  # set if you use auth token/secret

# ---------------- TEST PLAN ----------------
# Phase A: AI runs from /tests/latest WITHOUT selfTest (normal-mode AI trigger)
# Phase B: AI runs from /tests/latest WITH selfTest (bench mode, optional)
# Phase C: Schedule runs quickly (everyMin=1) and restores original schedule at end
#
# Nothing in firmware is modified by this script. It only touches RTDB paths:
# - /devices/{id}/settings (dosingMode, doseSchedule)
# - /devices/{id}/commands/selfTest
# - /devices/{id}/tests/latest
# It restores original settings at the end.

TEST_MODES = [1, 2, 3, 4, 5, 6]

POLL_SEC = 1.5
WAIT_MODE_APPLY_SEC = 30
WAIT_SELFTEST_START_SEC = 20
MODE_HARD_LIMIT_SEC = 90

INJECT_TEST_SAMPLES = True
SAMPLE_EVERY_SEC = 10

# A stable "normal reef" sample
FAKE_SAMPLE = {"tempF": 78.2, "ph": 8.10, "alk": 8.3, "ca": 440, "mg": 1350, "cond": 35.0}

# ---------------- HTTP HARDENING ----------------
sess = requests.Session()
retry = Retry(
    total=6,
    connect=6,
    read=6,
    backoff_factor=0.6,
    status_forcelist=(429, 500, 502, 503, 504),
    allowed_methods=frozenset(["GET", "PUT", "PATCH", "POST"]),
    raise_on_status=False,
)
adapter = HTTPAdapter(max_retries=retry, pool_connections=10, pool_maxsize=10)
sess.mount("https://", adapter)
sess.mount("http://", adapter)

def now_ms() -> int:
    return int(time.time() * 1000)

def log(msg: str):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)

def url(path: str) -> str:
    u = f"{DB_URL}{path}.json"
    if AUTH:
        u += f"?auth={AUTH}"
    return u

def _req(method: str, path: str, json_body=None):
    u = url(path)
    last = None
    for attempt in range(1, 6):
        try:
            r = sess.request(method, u, json=json_body, timeout=20)
            if r.status_code >= 400:
                raise RuntimeError(f"HTTP {method} {r.status_code} body={r.text[:300]}")
            return r.json()
        except (requests.exceptions.SSLError,
                requests.exceptions.ConnectionError,
                requests.exceptions.Timeout) as e:
            last = e
            wait = min(5.0, 0.5 * attempt)
            log(f"HTTP {method} transient ({type(e).__name__}): {e} | retry in {wait:.1f}s ({attempt}/5)")
            time.sleep(wait)
    raise RuntimeError(f"HTTP {method} failed after retries: {u} last={last}")

def get(path: str):
    return _req("GET", path)

def put(path: str, value):
    return _req("PUT", path, json_body=value)

def patch(path: str, obj: dict):
    return _req("PATCH", path, json_body=obj)

def read_state():
    return get(f"/devices/{DEVICE_ID}/state") or {}

def read_settings():
    return get(f"/devices/{DEVICE_ID}/settings") or {}

def read_settings_mode():
    return get(f"/devices/{DEVICE_ID}/settings/dosingMode")

def save_original_settings():
    s = read_settings()
    return {"dosingMode": s.get("dosingMode"), "doseSchedule": s.get("doseSchedule")}

def restore_original_settings(orig):
    try:
        patch(f"/devices/{DEVICE_ID}/settings", {
            "dosingMode": orig.get("dosingMode"),
            "doseSchedule": orig.get("doseSchedule"),
            "updatedAt": now_ms()
        })
        log("Restored original settings.")
    except Exception as e:
        log(f"WARNING: Failed to restore original settings: {e}")

def set_mode(mode: int):
    patch(f"/devices/{DEVICE_ID}/settings", {"dosingMode": mode, "updatedAt": now_ms()})
    log(f"Requested mode => {mode} (settings/dosingMode)")

def wait_for_mode_applied(mode: int, timeout_sec: int) -> bool:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            if read_state().get("dosingMode") == mode:
                return True
        except Exception:
            pass
        try:
            if read_settings_mode() == mode:
                return True
        except Exception:
            pass
        time.sleep(POLL_SEC)
    return False

def disable_schedule():
    try:
        patch(f"/devices/{DEVICE_ID}/settings/doseSchedule", {"enabled": False, "updatedAt": now_ms()})
        patch(f"/devices/{DEVICE_ID}/settings", {"updatedAt": now_ms()})
        log("Disabled schedule (doseSchedule.enabled=false)")
    except Exception as e:
        log(f"WARNING: couldn't disable schedule: {e}")

def enable_fast_schedule_every_minute():
    """
    Makes schedule run quickly for testing (everyMin=1) and a wide window.
    We restore original schedule at the end.
    """
    try:
        patch(f"/devices/{DEVICE_ID}/settings/doseSchedule", {
            "enabled": True,
            "startHour": 0,
            "endHour": 23,
            "everyMin": 1,
            "updatedAt": now_ms()
        })
        patch(f"/devices/{DEVICE_ID}/settings", {"updatedAt": now_ms()})
        log("Enabled FAST schedule (everyMin=1, 00:00-23:00).")
    except Exception as e:
        log(f"WARNING: couldn't enable fast schedule: {e}")

def write_tests_latest(mode: int, sample: dict, source: str):
    payload = dict(sample)
    payload["mode"] = mode
    payload["timestamp"] = now_ms()
    payload["source"] = source
    put(f"/devices/{DEVICE_ID}/tests/latest", payload)
    log(f"Wrote /tests/latest (mode {mode}) ts={payload['timestamp']} source={source}")

def set_selftest(flag: bool):
    put(f"/devices/{DEVICE_ID}/commands/selfTest", bool(flag))
    log(f"Set /commands/selfTest={flag}")

def trigger_selftest_edge():
    try:
        set_selftest(False)
    except Exception:
        pass
    time.sleep(0.8)
    set_selftest(True)
    log("Triggered /commands/selfTest (false->true)")

def wait_for_selftest_active(timeout_sec: int) -> bool:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            if read_state().get("selfTestActive") is True:
                return True
        except Exception:
            pass
        time.sleep(POLL_SEC)
    return False

def wait_for_ai_confirm(mode: int, min_epoch_ms: int, timeout_sec: int) -> bool:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            s = read_state()
            m = s.get("aiLastRunMode")
            t = s.get("aiLastRunEpochMs")
            if m == mode and isinstance(t, (int, float)) and t >= min_epoch_ms:
                log(f"AI CONFIRMED for mode {mode} (aiLastRunMode={m} aiLastRunEpochMs={t}).")
                return True
        except Exception:
            pass
        time.sleep(POLL_SEC)
    return False

def wait_for_pending_change(base_pending: dict, timeout_sec: int) -> bool:
    """
    Confirms that *something* about pendingMl changed (schedule or dosing tick).
    """
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            s = read_state()
            pending = s.get("pendingMl") or {}
            if pending != base_pending:
                log(f"Pending changed: {base_pending} -> {pending}")
                return True
        except Exception:
            pass
        time.sleep(POLL_SEC)
    return False

def run_ai_from_tests_without_selftest(mode: int):
    """
    Your MOST IMPORTANT test: AI runs in normal mode when /tests/latest updates.
    This does NOT require selfTest.
    """
    log("-" * 72)
    log(f"AI NORMAL TEST (no selfTest): mode {mode}")

    base_ai_epoch = read_state().get("aiLastRunEpochMs") or 0

    set_mode(mode)
    if wait_for_mode_applied(mode, WAIT_MODE_APPLY_SEC):
        log(f"Mode applied (mode={mode}).")
    else:
        log(f"WARNING: mode {mode} not reflected within {WAIT_MODE_APPLY_SEC}s.")

    # keep schedule off for this phase
    disable_schedule()

    # Write tests/latest twice to guarantee firmware sees it
    write_tests_latest(mode, FAKE_SAMPLE, source="pc_test_runner_normal")
    time.sleep(1.2)
    write_tests_latest(mode, FAKE_SAMPLE, source="pc_test_runner_normal")

    # Confirm AI ran
    if wait_for_ai_confirm(mode, min_epoch_ms=base_ai_epoch + 1, timeout_sec=30):
        log(f"PASS: AI ran from /tests/latest (mode {mode}) without selfTest.")
    else:
        log(f"FAIL: AI did NOT confirm within 30s for mode {mode} (no selfTest).")

def run_mode_with_selftest(mode: int):
    """
    Bench flow: selfTest triggers pump bursts + optional AI.
    """
    log("=" * 72)
    log(f"BENCH MODE {mode} (selfTest + /tests/latest)")

    base_ai_epoch = read_state().get("aiLastRunEpochMs") or 0

    set_mode(mode)
    if wait_for_mode_applied(mode, WAIT_MODE_APPLY_SEC):
        log(f"Mode applied (mode={mode}).")
    else:
        log(f"WARNING: mode {mode} not reflected within {WAIT_MODE_APPLY_SEC}s.")

    disable_schedule()

    trigger_selftest_edge()
    if wait_for_selftest_active(WAIT_SELFTEST_START_SEC):
        log("selfTestActive TRUE (started).")
    else:
        log("WARNING: selfTestActive didn't show TRUE (still writing tests/latest).")

    # feed samples during selfTest
    write_tests_latest(mode, FAKE_SAMPLE, source="pc_test_runner_bench")
    time.sleep(1.2)
    write_tests_latest(mode, FAKE_SAMPLE, source="pc_test_runner_bench")

    end = time.time() + MODE_HARD_LIMIT_SEC
    next_sample = time.time() + SAMPLE_EVERY_SEC

    while time.time() < end:
        if wait_for_ai_confirm(mode, min_epoch_ms=base_ai_epoch + 1, timeout_sec=0):
            break

        if INJECT_TEST_SAMPLES and time.time() >= next_sample:
            next_sample = time.time() + SAMPLE_EVERY_SEC
            write_tests_latest(mode, FAKE_SAMPLE, source="pc_test_runner_bench")

        try:
            if read_state().get("selfTestActive") is False:
                log("selfTestActive FALSE => ending mode.")
                break
        except Exception:
            pass

        time.sleep(POLL_SEC)

    try:
        set_selftest(False)
    except Exception as e:
        log(f"WARNING: clearing selfTest failed: {e}")

def run_fast_schedule_smoke(mode: int):
    """
    Proves the scheduler ticks in production code WITHOUT waiting 12 hours.
    Temporarily set everyMin=1 for ~2-3 minutes.
    """
    log("-" * 72)
    log(f"SCHEDULE SMOKE TEST (fast): mode {mode}")

    set_mode(mode)
    wait_for_mode_applied(mode, WAIT_MODE_APPLY_SEC)

    # Ensure selfTest is OFF (we want normal scheduler behavior)
    try:
        set_selftest(False)
    except Exception:
        pass

    base_state = read_state()
    base_pending = base_state.get("pendingMl") or {}

    enable_fast_schedule_every_minute()

    if wait_for_pending_change(base_pending, timeout_sec=150):
        log("PASS: schedule tick observed (pendingMl changed).")
    else:
        log("FAIL: no pendingMl change detected within 150s. (Scheduler may not change pendingMl in your build.)")

    disable_schedule()

def main():
    log("Starting AI Doser Production Test Runner v2")

    meta = get(f"/devices/{DEVICE_ID}/meta") or {}
    log("Meta:\n" + json.dumps(meta, indent=2))

    orig = save_original_settings()
    log("Saved original settings:\n" + json.dumps(orig, indent=2))

    try:
        # Phase A: AI must run from test input with NO selfTest (most important)
        for m in TEST_MODES:
            run_ai_from_tests_without_selftest(m)

        # Phase B: Optional bench (pump bursts + AI confirm)
        for m in TEST_MODES:
            run_mode_with_selftest(m)

        # Phase C: Schedule smoke (fast)
        mode_for_sched = orig.get("dosingMode") if isinstance(orig.get("dosingMode"), int) else 3
        run_fast_schedule_smoke(mode_for_sched)

    except KeyboardInterrupt:
        log("Ctrl+C pressed.")
    finally:
        restore_original_settings(orig)

    log("All tests complete.")

if __name__ == "__main__":
    main()
