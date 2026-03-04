import time
import json
import requests
from datetime import datetime
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

DB_URL = "https://aidoser-default-rtdb.firebaseio.com"
DEVICE_ID = "reefDoser6"
AUTH = ""  # set if you use auth token/secret

TEST_MODES = [1, 2, 3, 4, 5, 6]

POLL_SEC = 1.5
WAIT_MODE_APPLY_SEC = 30
WAIT_SELFTEST_START_SEC = 20
MODE_HARD_LIMIT_SEC = 90

INJECT_TEST_SAMPLES = True
SAMPLE_EVERY_SEC = 10

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
            # Firebase can return "null"
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

def read_settings_mode():
    return get(f"/devices/{DEVICE_ID}/settings/dosingMode")

def save_original_settings():
    s = get(f"/devices/{DEVICE_ID}/settings") or {}
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

def write_tests_latest(mode: int):
    payload = dict(FAKE_SAMPLE)
    payload["mode"] = mode
    payload["timestamp"] = now_ms()
    payload["source"] = "pc_test_runner"
    put(f"/devices/{DEVICE_ID}/tests/latest", payload)
    log(f"Wrote /tests/latest (mode {mode}) ts={payload['timestamp']}")

def set_selftest(flag: bool):
    put(f"/devices/{DEVICE_ID}/commands/selfTest", bool(flag))
    log(f"Set /commands/selfTest={flag}")

def trigger_selftest_edge():
    # Edge-trigger style: false -> true
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

def run_mode(mode: int):
    log("=" * 72)
    log(f"RUN MODE {mode}")

    # baseline for "AI moved forward"
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

    # two immediate writes to guarantee firmware sees it
    write_tests_latest(mode)
    time.sleep(1.2)
    write_tests_latest(mode)

    end = time.time() + MODE_HARD_LIMIT_SEC
    next_sample = time.time() + SAMPLE_EVERY_SEC

    # confirm AI (or keep feeding)
    while time.time() < end:
        if wait_for_ai_confirm(mode, min_epoch_ms=base_ai_epoch + 1, timeout_sec=0):
            break

        if INJECT_TEST_SAMPLES and time.time() >= next_sample:
            next_sample = time.time() + SAMPLE_EVERY_SEC
            write_tests_latest(mode)

        # end early if firmware ended selftest
        try:
            if read_state().get("selfTestActive") is False:
                log("selfTestActive FALSE => ending mode.")
                break
        except Exception:
            pass

        time.sleep(POLL_SEC)

    # cleanup
    try:
        set_selftest(False)
    except Exception as e:
        log(f"WARNING: clearing selfTest failed: {e}")

def main():
    log("Starting PC Test Runner (AI confirm + /tests/latest)")

    meta = get(f"/devices/{DEVICE_ID}/meta") or {}
    log("Meta:\n" + json.dumps(meta, indent=2))

    orig = save_original_settings()
    log("Saved original settings:\n" + json.dumps(orig, indent=2))

    try:
        for m in TEST_MODES:
            run_mode(m)
    except KeyboardInterrupt:
        log("Ctrl+C pressed.")
    finally:
        restore_original_settings(orig)

    log("All modes complete.")

if __name__ == "__main__":
    main()