import sys, time
from bridge_echo import WsClient
ws = WsClient("192.168.31.167")
ws.send({"t": "hello", "client": "watch", "proto": 1})
t0 = time.monotonic(); end = t0 + float(sys.argv[1]); last = 0.0
while time.monotonic() < end:
    if time.monotonic() - last > 1.0:
        ws.send({"t": "ping", "t0": 1.0}); last = time.monotonic()
    m = ws.recv(0.1)
    if m and m.get("t") == "ev" and m.get("kind") not in ("control_changed",):
        print(f"{time.monotonic()-t0:5.1f} {m.get('kind')} {dict((k, v) for k, v in m.items() if k not in ('t', 'seq', 'tank_ms', 'kind'))}", flush=True)
