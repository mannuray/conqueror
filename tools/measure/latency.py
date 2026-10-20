"""Phase 0: how old a camera picture is when it reaches the laptop, and pictures per second per size.

Point the tank's camera at this laptop's screen (about 30 cm away, the screen filling the picture). The
script turns a full-screen window white for 0.3 s once a second and watches the stream for the picture
getting bright: the delay from "white" to "bright picture arrived" is the picture's age on arrival.
Nothing moves. Runs each picture size in turn (it changes the size in the bridge's camera settings and
puts the first one back at the end).

    uv run python latency.py [--host 192.168.31.167] [--sizes QVGA,VGA] [--seconds 20]
"""

from __future__ import annotations

import argparse
import statistics
import threading
import time

import cv2
import numpy as np

from bridge import DEFAULT_HOST, Bridge, frames
from measure_lib import match_delays, rising_edges


def watch(host: str, seconds: float, out: list[tuple[float, float]]) -> None:
    """(receive time, centre brightness) of every picture for `seconds`."""
    end = time.monotonic() + seconds
    for t, jpeg in frames(host):
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_GRAYSCALE)
        if img is not None:  # pyright: ignore[reportUnnecessaryComparison] - None for a broken picture
            h, w = img.shape
            out.append((t, float(img[h // 3 : 2 * h // 3, w // 3 : 2 * w // 3].mean())))
        if t > end:
            return


def run_size(host: str, size: str, seconds: float) -> None:
    samples: list[tuple[float, float]] = []
    reader = threading.Thread(target=watch, args=(host, seconds + 1, samples), daemon=True)
    reader.start()
    flashes: list[float] = []
    cv2.namedWindow("flash", cv2.WND_PROP_FULLSCREEN)
    cv2.setWindowProperty("flash", cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
    black, white = np.zeros((90, 160), np.uint8), np.full((90, 160), 255, np.uint8)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        cv2.imshow("flash", white)
        cv2.waitKey(1)
        flashes.append(time.monotonic())
        cv2.waitKey(300)
        cv2.imshow("flash", black)
        cv2.waitKey(700)
    cv2.destroyWindow("flash")
    reader.join(timeout=5)
    if len(samples) < 10:
        print(f"{size}: too few pictures ({len(samples)}) - is the stream reachable?")
        return
    times = [t for t, _ in samples]
    values = [v for _, v in samples]
    threshold = (min(values) + max(values)) / 2
    delays = match_delays(flashes, rising_edges(times, values, threshold))
    fps = (len(times) - 1) / (times[-1] - times[0])
    if not delays:
        print(f"{size}: {fps:.1f} pictures/s; no flash seen - point the camera at the screen")
        return
    print(
        f"{size}: {fps:.1f} pictures/s; age on arrival median {statistics.median(delays) * 1000:.0f} ms, "
        f"min {min(delays) * 1000:.0f}, max {max(delays) * 1000:.0f} ({len(delays)} flashes)"
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--sizes", default="QVGA,VGA")
    ap.add_argument("--seconds", type=float, default=20)
    a = ap.parse_args()
    bridge = Bridge(a.host)
    sizes = a.sizes.split(",")
    try:
        for size in sizes:
            bridge.camera_size(size)
            time.sleep(2)  # the camera restarts with the new size
            run_size(a.host, size, a.seconds)
        bridge.camera_size(sizes[0])
    finally:
        bridge.close()


if __name__ == "__main__":
    main()
