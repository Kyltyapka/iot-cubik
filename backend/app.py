from flask import Flask, request, jsonify

app = Flask(__name__)

@app.route('/api/timer/start', methods=['POST'])
def start_timer():
    data = request.json
    print("START:", data)
    return jsonify({"status": "started", "data": data})

@app.route('/api/timer/pause', methods=['POST'])
def pause_timer():
    print("PAUSE")
    return jsonify({"status": "paused"})

@app.route('/api/timer/resume', methods=['POST'])
def resume_timer():
    print("RESUME")
    return jsonify({"status": "resumed"})

app.run(port=3000)