from flask import Flask, request, jsonify, render_template
from datetime import datetime, timedelta, timezone
from threading import Lock, Thread
import sqlite3
import time

app = Flask(__name__)
state_lock = Lock()

DB_PATH = "pomodoro.db"

MODE_DURATIONS = {
    "work": 25 * 60,
    "long_work": 50 * 60,
    "break": 5 * 60,
    "auto_break_5": 5 * 60,
    "auto_break_10": 10 * 60,
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


# ---------------- Run ----------------

if __name__ == "__main__":
    init_db()
    Thread(target=auto_loop, daemon=True).start()
    app.run(port=3000, debug=False)