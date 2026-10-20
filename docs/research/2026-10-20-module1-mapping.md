# Module 1 (sensor fusion + space map) — how to do it better (research, 2026-10-20)

Second research pass for the AI engine's module 1 (design-notes §107). Labels: **proven** (widely used), **promising** (works in papers / demos, unmeasured for us), **hype** (not practical now), **reasoning / guess** (our own inference). Background: `2026-10-20-ai-landscape.md`.

## Findings
1. **Metric indoor depth** — DA V2 Metric-Hypersim-Small exists (Apache-2.0, no accuracy figures, no CoreML build) https://huggingface.co/depth-anything/Depth-Anything-V2-Metric-Hypersim-Small ; Apple's CoreML DA V2 Small: 32.8 ms on the M1 Max Neural Engine https://huggingface.co/apple/coreml-depth-anything-v2-small ; DA3's metric model is Large only; Depth Pro too slow (offline teacher only); UniDepth / Metric3D unmeasured on M1. A metric model does **not** remove the need for a scale anchor: our camera sits ~15 cm above the floor, unlike the training views (guess); per-frame scale + shift fits on known geometry are best practice (GVDepth https://arxiv.org/html/2412.06080 , ground-plane scale https://arxiv.org/pdf/2101.05995).
2. **Free floor** — "expected floor depth" test (pixel nearer than the flat floor should be) recommended as the core (reasoning, ground-geometry cues); floor-colour models (Ulrich & Nourbakhsh https://ri.cmu.edu/?p=16753) proven but brittle (rug edges, shadows) — a cheap second vote; learned floor segmentation (SegFormer ADE20K, Isaac free-space DNN) promising, low viewpoint out of distribution. Robot vacuums use a camera **plus structured light / line lasers** because cameras miss thin low items (cables, socks) https://www.dreametech.com/blogs/blog/best-robot-vacuum-with-obstacle-avoidance .
3. **Depth → grid** — pseudo-scan (nearest point per image column inside a height band, ray-cast), Nav2 costmap habits: mark ~1.5-2 m, clear ~3 m https://docs.nav2.org/configuration/packages/costmap-plugins/obstacle.html ; sonar inverse model (Moravec & Elfes 1985) — **never clear on "no echo"** (fabric absorbs sound); decay only cells in view (blind zone keeps memory). Code to copy: PythonRobotics mapping https://github.com/AtsushiSakai/PythonRobotics/tree/master/Mapping .
4. **Pose** — encoder speed + gyro rate (robot_localization pattern) as a small EKF with a **gyro-bias state**, bias updated when both encoders and the motor command are zero; encoder difference only a weak heading check (slip). Visual odometry not worth it now (DPVO needs CUDA); KLT flow on the floor for stuck / slip detection; AprilTags later for drift resets.
5. **Time sync** — esp32-camera stamps each frame at VSYNC with `esp_timer_get_time()` https://raw.githubusercontent.com/espressif/esp32-camera/master/driver/cam_hal.c ; if the bridge also stamps each Uno telemetry packet on arrival with the same clock, frames and poses share one clock (no WiFi clock sync for pairing). Look up the pose at VSYNC + half the readout; skip / down-weight frames while turning fast (gyro rate gate).
6. **Objects** — image tracker (ByteTrack) only for IDs; a Kalman filter per object in **floor coordinates**; objects expire after T seconds or when the camera looks at the spot and does not see them.
7. **Learned end-to-end** (ViNT / NoMaD, bird's-eye occupancy nets) — collisions / need own training data: hype for us now.
8. **Testing** — record raw MJPEG + stamped telemetry and replay deterministically (Rerun https://github.com/rerun-io/rerun); metrics in safety order: false-free rate within 1 m per obstacle type, distance error vs tape (0.3 / 0.6 / 1 / 2 m), smallest detectable obstacle, capture-to-grid delay, pose drift per 5 m loop and per 360°.

## Recommended changes (to the approach of §107)
1. Bridge stamps Uno telemetry with `esp_timer` on UART receive (same clock as the frame stamp).
2. Floor plane = main depth scale anchor; ultrasonic = cross-check and "blocked at d".
3. Obstacles by the expected-floor-depth test.
4. Ultrasonic never clears on "no echo"; clears only a narrow inner cone.
5. Mark to ~1.5-2 m, clear to ~3 m; decay only cells in view.
6. 4-state pose filter (x, y, heading, gyro bias) with the standstill bias update.
7. Gate frames by gyro rate; pose at VSYNC + half the readout.
8. Objects tracked in floor coordinates; expire on "looked and not seen".
9. Try YOLOE (named small things: cable, sock, shoe) — experiment.
10. Convert DA V2 Metric-Hypersim-Small to CoreML and compare on recorded data — experiment.
11. Record and replay (Rerun) from day one, with the metrics above.
12. Thin / low obstacles need a range sensor near the floor (8×8 ToF or a line laser) — hardware, later.

## To measure on the tank
OV3660 readout time per frame size; the Neural Engine budget with YOLO + depth together on the base M1; depth error vs tape (relative + floor fit vs metric model) on wood and rug; false-free rate per obstacle type (chair leg, cable, sock, sofa); ultrasonic no-echo rate on fabric and angled walls; MPU6050 bias drift with and without the standstill update; camera height / tilt and blind-zone length; gyro rate where frames become unusable; UART receive-to-stamp delay.
