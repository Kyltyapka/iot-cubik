from flask import Flask, request, jsonify, render_template
from datetime import datetime, timedelta, timezone
from threading import Lock, Thread
import sqlite3
import time
import requests
import os
from dotenv import load_dotenv

app = Flask(__name__)
state_lock = Lock()

load_dotenv()
TELEGRAM_BOT_TOKEN = os.getenv("TELEGRAM_BOT_TOKEN")
TELEGRAM_CHAT_ID = os.getenv("TELEGRAM_CHAT_ID")
DB_PATH = "pomodoro.db"

MODE_DURATIONS = {
    "work": 25 * 60,
    "long_work": 50 * 60,
    "break": 5 * 60,
    "auto_break_5": 5 * 60,
    "auto_break_10": 10 * 60,
}

API_KEY = os.getenv("API_KEY")

NOTIFY_ACTIONS = {
    "work_started",
    "long_work_started",
    "break_started",
    "paused",
    "resume",
    "off",
    "auto_break_5_started",
    "auto_break_10_started",
    "timer_completed"
}


# ---------------- Time helpers ----------------

def now():
    return datetime.now(timezone.utc)

def to_iso(dt):
    if dt is None:
        return None
    return dt.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")

def parse_iso(value):
    if not value:
        return None
    return datetime.fromisoformat(value.replace("Z", "+00:00"))

from functools import wraps

def require_api_key(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if not API_KEY:
            return jsonify({"status": "error", "message": "API key is not configured"}), 500

        provided = request.headers.get("X-API-Key")
        if provided != API_KEY:
            return jsonify({"status": "error", "message": "Unauthorized"}), 401
        return f(*args, **kwargs)
    return decorated


# ---------------- DB ----------------

def db():
    return sqlite3.connect(DB_PATH, check_same_thread=False)

def init_db():
    conn = db()
    cur = conn.cursor()

    cur.execute("""
    CREATE TABLE IF NOT EXISTS timer_state (
        id INTEGER PRIMARY KEY,
        state TEXT,
        current_mode TEXT,
        started_at TEXT,
        ends_at TEXT,
        remaining_seconds INTEGER,
        paused_mode TEXT,
        paused_remaining_seconds INTEGER,
        last_action TEXT,
        notification TEXT
    )
    """)

    cur.execute("""
    CREATE TABLE IF NOT EXISTS sessions (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        mode TEXT,
        status TEXT,
        start_time TEXT,
        end_time TEXT,
        duration_seconds INTEGER
    )
    """)

    cur.execute("SELECT COUNT(*) FROM timer_state")
    if cur.fetchone()[0] == 0:
        cur.execute("""
        INSERT INTO timer_state (
            id, state, current_mode, started_at, ends_at, remaining_seconds,
            paused_mode, paused_remaining_seconds, last_action, notification
        )
        VALUES (
            1, 'idle', 'off', NULL, NULL, NULL,
            NULL, NULL, 'init', 'System initialized'
        )
        """)

    conn.commit()
    conn.close()

def load_state():
    conn = db()
    cur = conn.cursor()
    cur.execute("""
        SELECT
            state, current_mode, started_at, ends_at, remaining_seconds,
            paused_mode, paused_remaining_seconds, last_action, notification
        FROM timer_state
        WHERE id = 1
    """)
    row = cur.fetchone()
    conn.close()

    if not row:
        return None

    return {
        "state": row[0],
        "current_mode": row[1],
        "started_at": row[2],
        "ends_at": row[3],
        "remaining_seconds": row[4],
        "paused_mode": row[5],
        "paused_remaining_seconds": row[6],
        "last_action": row[7],
        "notification": row[8],
    }

def save_state(s):
    conn = db()
    cur = conn.cursor()

    cur.execute("""
    UPDATE timer_state SET
        state=?,
        current_mode=?,
        started_at=?,
        ends_at=?,
        remaining_seconds=?,
        paused_mode=?,
        paused_remaining_seconds=?,
        last_action=?,
        notification=?
    WHERE id=1
    """, (
        s["state"],
        s["current_mode"],
        s["started_at"],
        s["ends_at"],
        s["remaining_seconds"],
        s["paused_mode"],
        s["paused_remaining_seconds"],
        s["last_action"],
        s["notification"],
    ))

    conn.commit()
    conn.close()

def insert_session(mode, status, start_time, end_time, duration):
    conn = db()
    cur = conn.cursor()
    cur.execute("""
    INSERT INTO sessions (mode, status, start_time, end_time, duration_seconds)
    VALUES (?, ?, ?, ?, ?)
    """, (mode, status, start_time, end_time, duration))
    conn.commit()
    conn.close()

def format_seconds_human(seconds):
    if seconds is None:
        return "—"

    seconds = max(0, int(seconds))
    minutes = seconds // 60
    sec = seconds % 60
    return f"{minutes} min {sec} sec"

def build_notification_message(s):
    action = s.get("last_action")
    mode = s.get("current_mode")
    paused_mode = s.get("paused_mode")
    paused_remaining = s.get("paused_remaining_seconds")

    if action == "work_started":
        return "Work session started: 25 min."

    if action == "long_work_started":
        return "Long Work session started: 50 min."

    if action == "break_started":
        return "Break started: 5 min."

    if action == "paused":
        return f"Timer paused. Remaining: {format_seconds_human(paused_remaining)}."

    if action == "resume":
        return f"Session resumed: {mode}. Remaining: {format_seconds_human(s.get('remaining_seconds'))}."

    if action == "auto_break_5_started":
        return "Work completed. Auto break 5 min started."

    if action == "auto_break_10_started":
        return "Long Work completed. Auto break 10 min started."

    if action == "timer_completed":
        return "Timer completed. System is now Off / Idle."

    if action == "off":
        return "Timer switched to idle mode."

    return None

def send_telegram_message(text):
    if not TELEGRAM_BOT_TOKEN or not TELEGRAM_CHAT_ID:
        print("Telegram notification skipped: TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID is missing")
        return

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
    payload = {
        "chat_id": TELEGRAM_CHAT_ID,
        "text": text
    }

    print("Sending Telegram:", payload)

    try:
        response = requests.post(url, json=payload, timeout=5)
        print("Telegram response:", response.status_code)
        print("Telegram body:", response.text)
    except Exception as e:
        print("Telegram error:", repr(e))

last_telegram_message = {"text": None, "sent_at": None}

def should_send_telegram(text, cooldown_seconds=3):
    if not text:
        return False

    current_time = now()

    if (
        last_telegram_message["text"] == text
        and last_telegram_message["sent_at"] is not None
        and (current_time - last_telegram_message["sent_at"]).total_seconds() < cooldown_seconds
    ):
        return False

    last_telegram_message["text"] = text
    last_telegram_message["sent_at"] = current_time
    return True


# ---------------- Logic ----------------

def get_remaining_seconds(s):
    if s["state"] != "running" or not s["ends_at"]:
        return s["remaining_seconds"]

    diff = int((parse_iso(s["ends_at"]) - now()).total_seconds())
    return max(0, diff)

def start_mode(mode):
    duration = MODE_DURATIONS[mode]
    start = now()
    end = start + timedelta(seconds=duration)

    insert_session(mode, "started", to_iso(start), None, duration)

    return {
        "state": "running",
        "current_mode": mode,
        "started_at": to_iso(start),
        "ends_at": to_iso(end),
        "remaining_seconds": duration,
        "paused_mode": None,
        "paused_remaining_seconds": None,
        "last_action": f"{mode}_started",
        "notification": f"{mode} started"
    }

def set_idle(action="off", notification="System switched to Off / Idle"):
    return {
        "state": "idle",
        "current_mode": "off",
        "started_at": None,
        "ends_at": None,
        "remaining_seconds": None,
        "paused_mode": None,
        "paused_remaining_seconds": None,
        "last_action": action,
        "notification": notification
    }

def pause_state(s):
    if s["state"] != "running":
        s["last_action"] = "pause_ignored"
        s["notification"] = "Pause ignored: no active timer"
        return s

    remaining = get_remaining_seconds(s)

    insert_session(
        s["current_mode"],
        "paused",
        s["started_at"],
        to_iso(now()),
        remaining
    )

    return {
        "state": "paused",
        "current_mode": "pause",
        "started_at": None,
        "ends_at": None,
        "remaining_seconds": None,
        "paused_mode": s["current_mode"],
        "paused_remaining_seconds": remaining,
        "last_action": "paused",
        "notification": "Timer paused"
    }

def resume_state(s):
    if s["state"] != "paused" or not s["paused_mode"] or not s["paused_remaining_seconds"]:
        s["last_action"] = "resume_ignored"
        s["notification"] = "Resume ignored: no paused session"
        return s

    start = now()
    end = start + timedelta(seconds=s["paused_remaining_seconds"])

    insert_session(
        s["paused_mode"],
        "resumed",
        to_iso(start),
        None,
        s["paused_remaining_seconds"]
    )

    return {
        "state": "running",
        "current_mode": s["paused_mode"],
        "started_at": to_iso(start),
        "ends_at": to_iso(end),
        "remaining_seconds": s["paused_remaining_seconds"],
        "paused_mode": None,
        "paused_remaining_seconds": None,
        "last_action": "resume",
        "notification": f"Session resumed: {s['paused_mode']}"
    }

def cancel_current(s):
    if s["state"] == "running":
        insert_session(
            s["current_mode"],
            "cancelled",
            s["started_at"],
            to_iso(now()),
            get_remaining_seconds(s)
        )


# ---------------- API ----------------

@app.route("/api/mode", methods=["POST"])
@require_api_key
def api_mode():
    data = request.get_json(silent=True) or {}
    mode = data.get("mode")

    if not mode:
        return jsonify({"status": "error", "message": "Field 'mode' is required"}), 400

    with state_lock:
        s = load_state()

        if mode in ["work", "long_work", "break"]:
            cancel_current(s)
            s = start_mode(mode)

        elif mode == "pause":
            s = pause_state(s)

        elif mode == "resume":
            s = resume_state(s)

        elif mode == "off":
            cancel_current(s)
            s = set_idle()

        else:
            s["last_action"] = "unknown_mode"
            s["notification"] = f"Unknown mode: {mode}"

        save_state(s)

        message = None
        if s["last_action"] in NOTIFY_ACTIONS:
            message = build_notification_message(s)
        if should_send_telegram(message):
            send_telegram_message(message)

        return jsonify({"status": "ok", "data": s})


@app.route("/api/state", methods=["GET"])
def api_state():
    with state_lock:
        s = load_state()
        if s["state"] == "running":
            s["remaining_seconds"] = get_remaining_seconds(s)
    return jsonify({"status": "ok", "data": s})


@app.route("/api/sessions", methods=["GET"])
def api_sessions():
    conn = db()
    cur = conn.cursor()
    cur.execute("SELECT * FROM sessions ORDER BY id DESC LIMIT 20")
    rows = cur.fetchall()
    conn.close()

    result = []
    for r in rows:
        result.append({
            "id": r[0],
            "mode": r[1],
            "status": r[2],
            "start": r[3],
            "end": r[4],
            "duration": r[5]
        })

    return jsonify({"data": result})


def duration_from_times(start_value, end_value):
    if not start_value or not end_value:
        return 0

    try:
        start = parse_iso(start_value)
        end = parse_iso(end_value)
        return max(0, int((end - start).total_seconds()))
    except Exception:
        return 0


@app.route("/api/statistics", methods=["GET"])
def api_statistics():
    conn = db()
    cur = conn.cursor()
    cur.execute("""
        SELECT mode, status, start_time, end_time, duration_seconds
        FROM sessions
        ORDER BY id ASC
    """)
    rows = cur.fetchall()
    conn.close()

    focus_modes = {"work", "long_work"}
    break_modes = {"break", "auto_break_5", "auto_break_10"}

    summary = {
        "total_events": 0,
        "focus_started": 0,
        "focus_completed": 0,
        "break_started": 0,
        "break_completed": 0,
        "paused": 0,
        "cancelled": 0,
        "focus_seconds": 0,
        "break_seconds": 0,
        "today_focus_seconds": 0,
        "average_focus_seconds": 0,
        "last_session_at": None,
    }
    by_mode = {}
    today = now().date()

    for mode, status, start_time, end_time, duration_seconds in rows:
        mode_key = mode or "unknown"
        status_key = status or "unknown"
        actual_seconds = duration_from_times(start_time, end_time)

        if mode_key not in by_mode:
            by_mode[mode_key] = {
                "mode": mode_key,
                "events": 0,
                "started": 0,
                "completed": 0,
                "seconds": 0,
            }

        by_mode[mode_key]["events"] += 1
        summary["total_events"] += 1

        if status_key == "started":
            by_mode[mode_key]["started"] += 1
        elif status_key == "completed":
            by_mode[mode_key]["completed"] += 1
        elif status_key == "paused":
            summary["paused"] += 1
        elif status_key == "cancelled":
            summary["cancelled"] += 1

        if mode_key in focus_modes:
            if status_key == "started":
                summary["focus_started"] += 1
            if status_key == "completed":
                summary["focus_completed"] += 1
            summary["focus_seconds"] += actual_seconds

            try:
                if start_time and parse_iso(start_time).date() == today:
                    summary["today_focus_seconds"] += actual_seconds
            except Exception:
                pass

        if mode_key in break_modes:
            if status_key == "started":
                summary["break_started"] += 1
            if status_key == "completed":
                summary["break_completed"] += 1
            summary["break_seconds"] += actual_seconds

        by_mode[mode_key]["seconds"] += actual_seconds

        candidate_last = end_time or start_time
        if candidate_last and (
            summary["last_session_at"] is None
            or candidate_last > summary["last_session_at"]
        ):
            summary["last_session_at"] = candidate_last

    if summary["focus_completed"] > 0:
        summary["average_focus_seconds"] = summary["focus_seconds"] // summary["focus_completed"]

    return jsonify({
        "status": "ok",
        "data": {
            "summary": summary,
            "by_mode": list(by_mode.values()),
        }
    })


@app.route("/api/system", methods=["GET"])
def api_system():
    conn = db()
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) FROM sessions")
    session_count = cur.fetchone()[0]
    cur.execute("""
        SELECT mode, status, start_time, end_time
        FROM sessions
        ORDER BY id DESC
        LIMIT 1
    """)
    last_session = cur.fetchone()
    conn.close()

    with state_lock:
        current_state = load_state()

    return jsonify({
        "status": "ok",
        "data": {
            "backend_time": to_iso(now()),
            "database": "ok",
            "session_count": session_count,
            "current_state": current_state["state"] if current_state else None,
            "current_mode": current_state["current_mode"] if current_state else None,
            "last_action": current_state["last_action"] if current_state else None,
            "last_session": {
                "mode": last_session[0],
                "status": last_session[1],
                "start": last_session[2],
                "end": last_session[3],
            } if last_session else None,
            "telegram_configured": bool(TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID),
            "api_key_configured": bool(API_KEY),
        }
    })


@app.route("/api/health", methods=["GET"])
def api_health():
    return jsonify({"status": "ok", "message": "Backend is running"})


@app.route("/", methods=["GET"])
def dashboard():
    return render_template("dashboard.html")




# ---------------- Auto transitions ----------------

def auto_loop():
    while True:
        time.sleep(1)

        with state_lock:
            s = load_state()

            if s["state"] != "running" or not s["ends_at"]:
                continue

            if parse_iso(s["ends_at"]) > now():
                continue

            mode = s["current_mode"]

            insert_session(mode, "completed", s["started_at"], to_iso(now()), 0)

            if mode == "work":
                s = start_mode("auto_break_5")
                s["last_action"] = "auto_break_5_started"
                s["notification"] = "Work completed. Auto break 5 min started"

            elif mode == "long_work":
                s = start_mode("auto_break_10")
                s["last_action"] = "auto_break_10_started"
                s["notification"] = "Long Work completed. Auto break 10 min started"

            elif mode in ["break", "auto_break_5", "auto_break_10"]:
                s = set_idle("timer_completed", f"{mode} completed. System is now Off / Idle")

            save_state(s)

            if s["last_action"] in NOTIFY_ACTIONS:
                message = build_notification_message(s)
                if should_send_telegram(message):
                    send_telegram_message(message)


# ---------------- Run ----------------

if __name__ == "__main__":
    init_db()
    Thread(target=auto_loop, daemon=True).start()
    app.run(port=3000, debug=False)
