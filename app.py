import os
import time
import json
import base64
import threading
import subprocess
import requests

import serial
from flask import Flask, jsonify, request, render_template, send_file

# ---------- config ----------
SERIAL_PORT   = "/dev/ttyUSB0"
BAUD          = 9600
IMAGE_PATH    = "item.jpg"
MODEL         = "claude-sonnet-4-6"
API_KEY       = "sk-ant-YOUR-REAL-KEY-HERE"
ROUTING_FILE  = "routing.json"

DEFAULT_ROUTING = {"PET": 1, "HDPE": 2, "PP": 0, "OTHER": 0}

app = Flask(__name__)
ser = serial.Serial(SERIAL_PORT, BAUD, timeout=1)
ser.reset_input_buffer()

write_lock   = threading.Lock()
state_lock   = threading.Lock()
routing_lock = threading.Lock()

state = {
    "belt":           "starting",
    "mode":           "auto",
    "scanning":       False,
    "last_type":      None,    # what the camera saw
    "last_station":   0,       # station number actually sent to Arduino
    "last_sorted":    None,    # type of the last item pushed into a bin
    "last_sorted_ts": 0,
    "last_image_ts":  0,
    "containers":     {"1": False, "2": False},
    "counts":         {"1": 0, "2": 0},
}

# ---------- routing ----------
def load_routing():
    try:
        with open(ROUTING_FILE) as f:
            data = json.load(f)
            # make sure all keys exist
            for k, v in DEFAULT_ROUTING.items():
                if k not in data:
                    data[k] = v
            return data
    except Exception:
        return DEFAULT_ROUTING.copy()

def save_routing(r):
    try:
        with open(ROUTING_FILE, "w") as f:
            json.dump(r, f, indent=2)
    except Exception as e:
        print(f"[ERROR] could not save routing: {e}")

routing = load_routing()
print(f"[ROUTING] loaded: {routing}")

# ---------- helpers ----------
def set_state(**kw):
    with state_lock:
        state.update(kw)

def take_photo():
    subprocess.run(["rpicam-still", "-o", IMAGE_PATH, "--nopreview", "-t", "1000"])

def classify_plastic():
    with open(IMAGE_PATH, "rb") as f:
        img = base64.standard_b64encode(f.read()).decode("utf-8")

    response = requests.post(
        "https://api.anthropic.com/v1/messages",
        headers={
            "x-api-key":          API_KEY,
            "anthropic-version":  "2023-06-01",
            "content-type":       "application/json"
        },
        json={
            "model":      MODEL,
            "max_tokens": 50,
            "messages": [{
                "role": "user",
                "content": [
                    {
                        "type":   "image",
                        "source": {
                            "type":       "base64",
                            "media_type": "image/jpeg",
                            "data":       img
                        }
                    },
                    {
                        "type": "text",
                        "text": (
                            "Look at this plastic item. Identify the plastic type. "
                            "Reply with ONLY one word: PET, HDPE, PP, or OTHER. "
                            "No explanation."
                        )
                    }
                ]
            }]
        },
        timeout=30
    )

    if response.status_code == 200:
        result = response.json()["content"][0]["text"].strip()
        print(f"[API] classified as: {result}")
        return result
    else:
        print(f"[API ERROR] status={response.status_code} body={response.text}")
        return "OTHER"

def resolve_station(ptype):
    """
    Look up the user's routing config, then redirect to reject (0)
    if the target container is already full.
    Returns the final station number as an int.
    """
    with routing_lock:
        station = routing.get(ptype, 0)

    if station > 0:
        with state_lock:
            full = state["containers"].get(str(station), False)
        if full:
            print(f"[ROUTING] Container {station} is FULL — {ptype} redirected to reject")
            station = 0

    return station

def send_serial(text):
    with write_lock:
        ser.write((text + "\n").encode())

# ---------- serial line handler ----------
def handle_line(line):
    up = line.upper()

    # ----- scan request from the Arduino -----
    if line == "SCAN":
        ptype   = "OTHER"
        station = 0
        set_state(scanning=True)
        try:
            take_photo()
            set_state(last_image_ts=time.time())
            ptype   = classify_plastic()
            station = resolve_station(ptype)
            set_state(last_type=ptype, last_station=station)
            print(f"[ROUTING] {ptype} → station {station}")
        except Exception as e:
            print(f"[ERROR] classify/route crashed: {e}")
        finally:
            send_serial(str(station))   # send just the number to Arduino
            set_state(scanning=False)
        return

    # ----- status / events from the Arduino -----
    if "CLEANING" in up:
        set_state(belt="cleaning")
    elif "BELT CLEAR" in up:
        set_state(belt="working")
    elif "STATUS:WORKING" in up:
        set_state(belt="working", mode="auto")
    elif "BELT MANUAL ON" in up:
        set_state(belt="manual_on", mode="manual")
    elif "BELT MANUAL OFF" in up:
        set_state(belt="manual_off", mode="manual")
    elif up.startswith("BIN"):                      # "BIN1 FULL" / "BIN2 OK"
        parts = line.split()
        num = parts[0][3:]
        with state_lock:
            state["containers"][num] = ("FULL" in up)
    elif up.startswith("SORTED:"):                  # item pushed by machine
        num = line.split(":", 1)[1].strip()
        with state_lock:
            state["counts"][num] = state["counts"].get(num, 0) + 1
            state["last_sorted"]    = state["last_type"]
            state["last_sorted_ts"] = time.time()
    elif up.startswith("MANUAL_DROP:"):             # item dropped by hand
        num = line.split(":", 1)[1].strip()
        with state_lock:
            state["counts"][num] = state["counts"].get(num, 0) + 1

def serial_loop():
    while True:
        try:
            raw = ser.readline()
        except Exception as e:
            print(f"[ERROR] serial read: {e}")
            time.sleep(0.5)
            continue
        if not raw:
            continue
        line = raw.decode("utf-8", errors="ignore").strip()
        if line:
            print("[ARDUINO]", line)
            try:
                handle_line(line)
            except Exception as e:
                print(f"[ERROR] handle_line: {e}")
                set_state(scanning=False)

# ---------- web routes ----------
@app.route("/")
def index():
    return render_template("dashboard.html")

@app.route("/status")
def status():
    with state_lock:
        s = dict(state)
    with routing_lock:
        s["routing"] = dict(routing)
    return jsonify(s)

@app.route("/image")
def image():
    if os.path.exists(IMAGE_PATH):
        return send_file(IMAGE_PATH, mimetype="image/jpeg")
    return ("no image yet", 404)

@app.route("/routing", methods=["GET"])
def get_routing():
    with routing_lock:
        return jsonify(dict(routing))

@app.route("/routing", methods=["POST"])
def set_routing():
    data        = request.get_json(silent=True) or {}
    valid_types = ["PET", "HDPE", "PP", "OTHER"]
    new_routing = {}
    for k, v in data.items():
        if k in valid_types:
            try:
                val = int(v)
                if val in [0, 1, 2]:
                    new_routing[k] = val
            except Exception:
                pass
    if not new_routing:
        return jsonify(ok=False, error="no valid routing data"), 400
    with routing_lock:
        routing.update(new_routing)
        r_copy = dict(routing)
    save_routing(r_copy)
    print(f"[ROUTING] updated: {r_copy}")
    return jsonify(ok=True, routing=r_copy)

@app.route("/command", methods=["POST"])
def command():
    cmd = (request.get_json(silent=True) or {}).get("cmd", "")
    allowed = {
        "BELT_ON":  "CMD:BELT_ON",
        "BELT_OFF": "CMD:BELT_OFF",
        "AUTO":     "CMD:AUTO"
    }
    if cmd not in allowed:
        return jsonify(ok=False, error="unknown command"), 400
    with state_lock:
        busy = state["scanning"]
    if busy:
        return jsonify(ok=False, error="busy scanning, try again"), 409
    send_serial(allowed[cmd])
    return jsonify(ok=True)

if __name__ == "__main__":
    threading.Thread(target=serial_loop, daemon=True).start()
    app.run(host="0.0.0.0", port=5000, threaded=True)