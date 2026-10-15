# Hub UX — Flows and Screens (living document)

Status: CONTENT APPROVED (visual design next) · Date: 2026-10-10 · Visual design tool: Google Stitch (after content is agreed)

## Principles
- **Full screen** = a place you stay and work in. **Dialog** = a short task with an end. **Panel/tab** = information you want next to the main thing.
- Safety controls are never more than one tap away in any tank view.

## Users
| User | Can do |
|---|---|
| Admin | everything |
| Guest (optional) | watch video + telemetry, press STOP |

## Structure (DECIDED: 6 screens + 4 dialogs)
```
Login
 └─ Fleet (home) ──[Add tank dialog]
     ├─ Tank: Cockpit | Timeline | Settings      (three tabs at tank level)
     └─ Hub settings                             (from the top bar)
```

| # | Screen | Contains |
|---|---|---|
| 1 | Login | sign in; first-run "create admin password" state |
| 2 | Fleet | tank cards, hub-health strip, Add tank |
| 3 | **Cockpit** | main view Camera ⇄ Map (other as picture-in-picture) + side panel tabs AI · Map/Missions · Status |
| 4 | Timeline | history charts + events + recordings + in-place player |
| 5 | Tank settings | Safety · Calibration · Surface profiles · Camera (+ benchmark) · Sounds & LED · Keys · Tank info/firmware |
| 6 | Hub settings | Health & metrics · Logs · AI engine · Account & guest · Storage |

Dialogs: **Add tank** · **Calibration stepper** (single item or full setup wizard) · **Child-play checklist** · **Media viewer** (+ small confirms).

Always present in tank views: floating **STOP**, status bar (connection, battery, owner, surface profile, limits badge), alert banners, toasts, control banner ("IR has control" / "AI is driving — Take control").

Phone: Cockpit landscape full-screen with overlaid joysticks; side panel → bottom sheet; other screens stack; settings side menu → list.

## Key flows
1. First time: Login (create password) → Fleet (empty) → Add tank → pair → Calibration stepper (setup wizard) → Cockpit.
2. Everyday: Login → Fleet → tank → Cockpit.
3. AI command: Cockpit AI panel (or quick bar) → text/voice → thinking → actions + log → result → cancel anytime.
4. Baby game: AI panel → game → Child-play checklist → limits badge → run → stop clears.
5. Mission: Cockpit main view = Map → set home → draw waypoints → save → run → live progress → result.
6. Calibration: Tank settings → Calibration → stepper → result saved to the active surface profile.
7. Surface change: toast → one tap.
8. Investigate: alert/event → Timeline → range → charts/events → play recording at that moment.
9. Trouble states: tank offline, body offline (camera only), firmware mismatch.

## Screen contents

### Screen 1 — Login (APPROVED)
**State A — first run (no admin yet):**
- Title "Set up Conqueror Hub"
- Create password + confirm (strength hint)
- Optional: enable guest access (view + STOP only) with its own PIN
- "Create & continue" → Fleet

**State B — normal:**
- Hub name/logo
- Password → "Sign in"
- "Continue as guest" (only if guest access is enabled)
- Footer: hub version, AI engine link status dot

No sign-up and no "forgot password": recovery is a documented command-line reset on the hub machine (local hobby server).

### Screen 2 — Fleet (APPROVED)
**Top bar (shared by all screens):** hub name · Fleet · Hub settings · AI engine status dot · user (admin/guest) · logout.

**Hub-health strip** (one line; loud only when something is wrong): hub OK · AI engine online/offline + tier · storage used % · alert count.

**Tank card** (grid, one per tank), priority order:
1. Name + live camera thumbnail (snapshot every few seconds; full stream only inside the tank)
2. State badge: Online / Offline (last seen …) / Body offline / Firmware mismatch / Needs calibration
3. Battery % + bar (red when low), WiFi signal
4. Who is in control: none / you / AI / IR remote
5. Flag icons: blocked, lifted, tilt, child-play limits, recording
6. AI activity line ("AI: keep-away from baby" / idle)
7. Actions: **Open** (→ Cockpit) · quick **STOP** · menu (Timeline, Settings, Rename, Unpair)

**Empty state:** illustration + "Add your first tank" + hint (tank on the same WiFi).
**"+ Add tank" card** at the end of the grid.

**Add tank dialog:**
1. Discovered tanks (live mDNS): name, ID, firmware, signal; "Not seeing it?" help (same WiFi, tank hotspot mode, manual IP)
2. Select → enter tank admin password → Pair
3. Success: "Paired. Run the setup wizard now? (~10 min, needs floor space)" → Start wizard / Later

**Guest:** same cards without menus or Add tank; only Open (view-only) and STOP.

### Screen 3 — Cockpit (per tank) (APPROVED)

**A. Tank header (shared by the three tank tabs):** tank name + switcher ▾ · tabs Cockpit | Timeline | Settings.

**B. Status bar (one compact row):**
| Item | Shows |
|---|---|
| Connection | online + link delay (ms); red when degraded |
| Battery | % + estimated minutes left; amber low, red critical |
| WiFi | signal bars |
| Control chip | You / AI / IR remote / None + **Take control** or **Release to AI** |
| Surface profile | "Tile ▾" (tap to switch) |
| Badges | child-play limits · obstacle brake OFF (warning) · recording ● · muted |

**C. Main view — Camera ⇄ Map** (other in a picture-in-picture corner; tap to swap)
- Camera overlays (toggleable): heading line + distance-ahead bar with time-to-collision (red inside the brake zone) · AI detections (boxes + labels + distance) · depth heat-map (opacity slider) · tilt horizon + pan/tilt mini-indicator · glass-to-glass delay readout.
- Map: occupancy grid · tank pose + heading + trail · home marker · AI object pins (last-seen time) · tap "Go here" · mission waypoints · zoom/fit/centre-on-tank.

**C2. VITALS instrument cluster (added; always visible, directly under the main view):**
SPEED (cm/s with encoders, % without; L/R track bars) · HEADING (compass dial, bearing ° + cardinal, gyro) · TURN RATE (°/s needle) ·
TILT (artificial horizon, pitch/roll °; amber near limit, red beyond) · SONAR (distance cm + TTC; red inside brake zone) · TEMP (°C, MPU6050) ·
BATTERY (V + sag) · SLIP (%) · LINK (ms). Full detail remains in the Status tab; the thin on-video HUD (heading tape, distance bar) stays.

**D. Controls zone:** left joystick = drive (smooth arcs, car-style reverse) · right joystick = camera (follows stick, springs home) + lock + home · max-speed slider · phone tilt-steering toggle.

**E. Quick actions:** **STOP** (big, red, floating, always visible) · Record · Snapshot (stream / full 3 MP) · Sounds ▾ · LED ▾ · Obstacle brake on/off · Moves ▾ (turn 90° L/R, turn 180°, forward 50 cm, return home) · Child-play ▶.

**F. Side panel (collapsible, tabs):**
1. **AI** (default): command box (text + push-to-talk) with suggestion chips (What do you see? · Find the ball · Follow me · Go home) · active skill card (name, what it does, progress, Stop skill) · fun modes (Baby game, Follow me, Hide & seek, Patrol, Photographer, Guard) · live decision log (thinking → tool calls → results with thumbnails; spoken replies with play button).
2. **Map & Missions:** saved missions → edit on map (add/move/delete points, loop, speed) → Save / Run / Cancel · live progress (waypoint 3/7) · set home here · return home · reset origin.
3. **Status:** gauges (track speed L/R commanded vs actual, heading compass, pitch/roll, distance + TTC, battery V/sag/minutes, temperature, WiFi, link delay, Uno load, slip %, roughness) · active flags · calibration status summary (link to Settings).

**G. Keyboard (desktop):** W A S D drive · arrows camera · **Space = STOP** · H camera home · L lock · M swap camera/map · R record · P snapshot · Enter → AI command box.

**H. Banners/toasts:** alert banners (lifted, tilt, low/critical battery, link lost, body offline, firmware mismatch) · control banner ("IR remote has control" / "AI is driving — Take control") · toasts (surface suggestion, AI finished, mission done/failed + reason).

**I. Phone (landscape):** full-screen main view; joysticks at bottom corners; STOP top-right; compact status chips top-left; side panel → swipe-up bottom sheet; PiP tap to swap.

**J. Guest:** video + status bar + STOP only.

### Screen 4 — Timeline (per tank) (APPROVED)
Answers "what happened?" — history, events and recordings on one time axis.

**A. Time range bar:** presets Live · 15 min · 1 h · Today · Yesterday · 7 days · custom; drag-to-zoom on charts with "back"; telemetry resolution auto-switches (raw when zoomed in, 1 s / 10 s / 1 min when zoomed out).

**B. Event strip:** one track with colour-coded markers — safety (bump, lifted, tilt, blocked, stall) · control (owner changes, IR override) · AI (skill start/end, decisions) · missions · battery · connection · calibration; **recording blocks** on the same strip; marker → card → **Jump here** (moves cursor + player).

**C. Charts** (stacked, shared time axis + synced vertical cursor; "Charts ▾" picker):
defaults speed L/R (commanded vs actual) · heading + turn rate · distance ahead (brake-zone band) · battery V + % · pitch/roll;
optional PWM, slip %, roughness, temperature, Uno load, WiFi, link delay, encoder counts.

**D. Player** (right on desktop, top on phone): plays the recording at the cursor ("No recording at this time" + nearest snapshot otherwise) · telemetry overlay toggle · play/pause, ±5 s, 0.5×–4×, frame step · snapshot from video · open in Media viewer · player ⇄ chart cursor stay synced.

**E. Lists (tabs):** Events (filter by category/severity/text; click → jump) · Recordings (start, duration, size, trigger manual/AI/mission/alert; rename, download MP4, download telemetry CSV, delete) · Snapshots (grid; view, download, delete) · AI decisions (command → plan → actions → result; click → jump).

**F. Extras:**
- **Auto-record on alerts — DECIDED, default ON:** the hub keeps a rolling 30 s video buffer per connected tank and saves 30 s before + 30 s after every safety event (clips marked "alert").
- Export: time range → ZIP (video + telemetry CSV + events JSON).
- Storage indicator (link to Hub settings).

**G. Phone:** range chips; player on top; charts stacked and swipeable; lists below.
**H. Guest:** read-only (watch, scrub, view events; no delete/export/rename).

### Screen 5 — Tank settings (per tank) (APPROVED)
Layout: left section menu (list on phone) + content. Every section uses a **Save / Discard** bar; concurrent edits → "changed elsewhere — reload?" (ETag); each setting shows its default and a short "why it matters" hint. Guest: no access.

1. **Overview** — rename; ID; firmware (Uno, ESP32), protocol, camera sensor (OV3660), hardware config (encoders); **calibration health** (X of 12 OK, missing/stale → Fix); **Run full setup wizard**.
2. **Driving** (added) — default max speed, acceleration limit, turn-rate max, turn-rate-vs-speed curve (2-point editor), reverse steering **Car**/Tank, pivot fallback, joystick deadzone + expo (app-side), tilt sensitivity, encoders enabled + counts/rev.
3. **Safety** — obstacle stop/slow distances (diagram), tilt limit, bump sensitivity, deadman timeout, battery low/critical, handback timers (IR 3 s, human 5 s), child-play preset values (speed cap, accel cap; brake always locked), parking beeps.
4. **Calibration** — all items with status (missing/OK/stale), last done, surface profile, **Run** → Calibration stepper; full setup wizard.
5. **Surface profiles** — list (≤3): name, active, last calibrated, roughness signature; activate / create (copy) / rename / delete; auto-switch toggle; suggestion history.
6. **Camera** — resolution, quality, flip/mirror, brightness/contrast/saturation (live preview); pan/tilt limits, trim, speed, return-home delay, idle power-off; **camera + depth benchmark** (run, history, side-by-side compare).
7. **Sounds & LED** — audio mode normal/quiet/mute; preview every sound and LED pattern; optional cue toggles (forward chirp, reverse beeps, parking beeps); LED brightness.
8. **Keys & access** — tank keys (name, role, created, last used); create (shown once + QR for the phone app) / revoke; hub pairing status.
9. **Network & firmware** — current WiFi, signal, saved networks (link to the tank setup page), hotspot name; firmware versions + compatibility; ESP32 OTA upload with progress; Uno USB update instructions.
10. **Danger zone** (typed confirmation) — **Reset settings to defaults keeps calibration data** (DECIDED; resets preferences and limits only) · Clear calibration (separate action) · Unpair tank.

### Screen 6 — Hub settings (APPROVED, admin only)
Layout as Tank settings (section menu + Save/Discard bar).

1. **Overview** — hub name, version, uptime, host (Docker/native, CPU/arch); health checks (DB, tank manager, storage, AI engine); paired tanks online/offline.
2. **Health & metrics** — live cards (HTTP rate/errors, WS clients browsers/AI, per-tank telemetry rate, command latency send → ack, video relay fps + dropped frames, event-loop lag, memory, CPU); small charts (last hour/day); links to `/metrics` and OpenTelemetry export status.
3. **Logs** — live tail + search (level, text, tank, time range, **request ID** = everything for one command); expand to JSON, copy, download range; runtime log level (info/debug).
4. **AI engine** — connection, detected hardware tier (+ override), preset; **capabilities table** (model, runtime, licence, status/reason, fps/latency, swap ▾); **LLM profiles** Fast/Smart/Vision (provider, model, time budget, fallback chain, "Test" shows real round-trip); cloud API keys (masked, write-only); optional plugins (3D mapping, face identity **off**, alternative depth) with licence warnings; voice STT/TTS + voice preview; **model registry** (sizes, licences, delete).
5. **Account & access** — change admin password; guest access on/off + PIN + visibility; active sessions (sign out others); regenerate AI engine service token.
6. **Storage & retention** — usage by kind (telemetry DB, recordings, snapshots, logs, models); caps/retention (telemetry 7 d raw / 90 d downsampled, recordings GB cap, alert clips, logs 7 d / 200 MB); Clean up now.
7. **Backup** (DECIDED: keep) — download backup (settings, tanks, missions, tank keys encrypted, AI memory; recordings optional) · restore from file.
8. **About** — versions (hub, web, AI engine, protocol), bundled licences, GitHub link.
9. **Appearance** (added) — colour theme (Phosphor / Amber / Ice), glow/scanline intensity, per browser.

## Dialogs (APPROVED)

### Add tank
See Screen 2 (Fleet).

### Calibration stepper
One component: **single item** (Settings → Calibration) or **full setup wizard** (chained, after pairing). Full-screen sheet on phone.
- Frame: header ("Setup wizard: step 4 of 12 — Motor deadband", target surface profile) · progress rail (wizard: ✓ done / ● current / ○ upcoming / ↷ skipped) · footer Back · Skip (wizard) · Cancel · Next/Start.
- Every step: **Prepare** (instructions + illustration + safety checkboxes) → **Run** (live progress, live readings, camera where useful, STOP always visible) → **Input** (guided items: tape measurement, hand-turn count) → **Result** (values, comparison with previous, **Accept & save** / Retry).
- Failure: reason + suggestion → Retry / Skip / Cancel; nothing saved unless accepted.
- Wizard end: summary (calibrated / skipped) → Go to Cockpit.

### Child-play checklist
From the AI panel (baby game) or Quick actions.
- Explanation of what changes.
- Required checklist: adult present and watching · track guards fitted · flat area, no stairs or drops · STOP within reach (phone / IR remote).
- Limits shown read-only (speed cap 20 %, gentle acceleration, obstacle brake locked).
- Game options: duration 5 / 10 / 15 min; optional play area = current map area.
- **Start** enabled only when all boxes are ticked / Cancel.
- While active: limits badge; End child-play in Quick actions; the AI cannot remove the limits; ending the game asks "Remove child-play limits?".

### Media viewer
Full-size snapshot/clip; zoom/pan (3 MP stills); info (time, tank, resolution, trigger, detections overlay toggle); download, delete, **Jump to Timeline**; previous/next.

### Confirmations (one consistent style)
- Typed confirmation (type the tank name): unpair, clear calibration, reset settings, delete surface profile.
- Simple confirm: delete recording/snapshot/mission, revoke key, sign out other sessions.
- **Never confirm safety actions:** STOP, ending child-play.

## Visual design (Google Stitch)
- Stitch project: "Conqueror Hub" (`projects/9914699709114202518`), design system **Phosphor Cockpit** (`assets/10357344405480861504`).
- **Look (DECIDED):** retro green-phosphor CRT terminal, dark only. Phosphor green #33FF66 = UI colour on near-black; **status colours are not green** (amber #FFB000 warning, red #FF3B30 danger/STOP, white-green #E8FFE8 active), always with icon + text. JetBrains Mono for labels/numbers, IBM Plex Sans for prose. Glow/scanlines **only on chrome, never over video or map**. 4 px corners, bracketed section titles `[ STATUS ]`.
- **Switchable colour themes (DECIDED):** UI built on design tokens; a theme swaps colours only, never layout.
  | Theme | UI colour | Warning | Danger/STOP | Active |
  |---|---|---|---|---|
  | **Phosphor** (default) | green #33FF66 | amber #FFB000 | red #FF3B30 | white-green #E8FFE8 |
  | **Amber** | amber #FFB000 | yellow-white #FFF2B3 | red #FF3B30 | warm white #FFF8E8 |
  | **Ice** | cyan #4DE1FF | amber #FFB000 | red #FF3B30 | white #EAFBFF |
  - Switch in **Hub settings → Appearance** (new section) or a quick top-bar toggle; stored per browser.
  - Appearance also has a glow/scanline intensity slider (incl. off).
  - Invariants in every theme: STOP is solid red; status always icon + text.
- Screens: `docs/design/stitch/` (vN = iteration).
  - `03-cockpit-v1.png` — first Cockpit draft (review notes below).
  - `03-cockpit-v2.png` — Cockpit with VITALS cluster, accuracy fixes, theme toggle. Polish later: remove "ARM_CORTEX_M7" under tank name, "US/IR 10Hz" map label, "2S LIPO" → "2S Li-ion".
  - `01-login-v1.png` — Login + first-run variant. Polish: pre-login boot log must not reveal fleet info (tank counts) before authentication; remove invented "port 8888".
  - `02-fleet-v1.png` — Fleet with 3 tank states + Add tank card. Polish: WiFi is **2.4 GHz only** (ESP32) — also fix "5.8 GHz" in Cockpit; replace "recommend dock return" (no dock) with "return home"; remove "port 8888 // RTOS v3.8+".
  - `04-timeline-v1.png` — Timeline (range bar, event strip with jump card, 5 synced charts, replay player, event table, auto-record footer). Polish: FIT0458 encoders are **Hall** not "optical"; no physical "front bumper" (bump comes from the accelerometer → "BUMP (front, accel)"); remove "DVR 1080s", "SD_CARD"; retention 7 d raw / 90 d downsampled (not "14 days").
  - `05-tank-settings-v1.png` — Tank settings, Safety section (sonar cone diagram, sliders with "why it matters", battery bar, handback, child-play locks, sticky save bar). Polish: top nav must not highlight HUB SETTINGS on a tank page; no "return-to-dock"; buzzer pin **A0** (not "PWM pin 9"); ultrasonic pins **12/13**; encoders **Hall**; battery capacity unknown (remove "2500 mAh"); "2S Li-ion" not "LIPO".
  - `06-hub-settings-v1.png` — Hub settings, AI engine section (tier card, capabilities table with licence chips, LLM profiles). Polish: Apple Silicon has unified memory (no separate "VRAM"); remove "port 8888".
- **Polish pass (done):** Cockpit v3 (`03-cockpit-v3.png`), Login v2 (`01-login-v2.png`, no pre-auth fleet info), Tank settings v2 (`05-tank-settings-v2.png`), plus in-place fixes to Fleet, Timeline, Hub settings in Stitch (2.4 GHz, Hall encoders, no bumper/dock/port 8888, unified memory).
  - `07-cockpit-phone-v1.png` — phone landscape Cockpit: full-screen video, overlaid joysticks, STOP top-right, compact chips, heading tape, distance bar, PiP map, quick-action pills, one-line vitals, AI·MAP·STATUS bottom-sheet handle, tilt-steer toggle. Note: max-speed slider lives in the bottom sheet on phone.
  - `08-calibration-stepper-v1.png` — wizard step 4/12 motor deadband: progress rail (done/skip/run/queued/optional), phase pills, live PWM-vs-yaw chart with detected start, live tiles, STOP/abort, result preview with Accept & save / Retry. Polish: remove "RTOS_BUS 100Hz", "current draw" (no current sensor), "surface friction μ"; profile name must match the active one ("TILE", not "SMOOTH_TILE").
  - `09-child-play-checklist-v1.png` — 4 required checks (3/4, start disabled + amber hint), locked limits, game options. Wording: limits are "enforced by tank firmware" (not "hardware interlock").
  - `10-theme-amber.png`, `11-theme-ice.png` — theme switch demo; layout identical, video untinted. Polish (Amber): warnings must use **yellow-white** (BRAKE ON, RELEASE TO AI currently amber = same as UI colour, violates the distinct-status rule). Remove duplicated "2.4 GHz" in the WiFi chip (all themes).
- **Visual design status: COMPLETE (v1 set).** Remaining small polish items above are applied during implementation from these notes.


### Screen 7b — Cockpit-lite: the ESP32 phone page (owner, 2026-10-15)
The bridge's own page (`/` on `conqueror-bridge.local`, no hub, no login) follows the phone Cockpit (`stitch/07-cockpit-phone-v1.png`) in look and layout, minus everything that needs the hub or the AI engine. Same Phosphor tokens (already used by the page). No new mock-up (owner): the Stitch phone Cockpit is the reference.

Layout (landscape, full-screen video; portrait shows "rotate the phone"):
| Area | Content |
|---|---|
| Top-left chips | link latency (ping round trip, ms) · WiFi signal (bars from `/info` RSSI, refreshed every 10 s) · **CTRL**: YOU / phone-N / IR / bench / none · **speaker chip** (owner, 2026-10-15): 🔊 normal, crossed speaker in amber = muted, `QUIET` = quiet; tap toggles mute ↔ normal (`audio_mode`; quiet stays with the IR remote / hub); a sound pressed while muted shows the toast "Tank is muted — tap 🔊" |
| Top centre | **heading pill** as the phone Cockpit: heading −15° · ▼ heading · compass letter · heading +15° |
| Left edge | **battery gauge** (owner, 2026-10-15: mirrors the range gauge): vertical bar filling from the bottom = battery % (2S curve from `batt_v`) over a colour scale like the range bar (owner, 2026-10-15): red at the bottom up to critical (6.6 V), amber at low (7.2 V ≈ 20 %), green above; the % text takes the tank's battery level colour (green OK, amber `batt_low`, red `batt_critical`); box beside it **BATT nn%** over **x.xx V**; on USB (< 5.5 V) the bar is grey and the box says **USB** |
| Top-right | **STOP** (solid red, always visible) |
| Right edge | **distance bar** (0–2 m, green → amber < 50 cm → red < obstacle stop), **RANGE nn cm** box beside it |
| Bottom-left | **drive joystick** · TILT STEER toggle (as today) |
| Bottom-right | **camera joystick** with **LOCK** and **HOME** buttons beside it. **Camera position in the ring** (owner, 2026-10-15): a ghost dot inside the ring shows where the camera points (ring edge = 90° from home; home = pan 90° / tilt 90° = centre; stick right = smaller pan angle, stick up = smaller tilt angle = camera looks up, design-notes §45), title `GIMBAL // CAM ←20° ↑5°`. States: you dragging → knob bright, dot faint; you not touching → knob dim, dot normal (stays with LOCK, glides home without); someone else steers (IR, another phone, later AI/hub) → knob hidden, ring not touchable, dot bright, title `GIMBAL // IR` / `// PHONE-2` / `// AI` |
| Bottom centre | pills: **SNAP** (save a photo) · **SOUNDS** (sheet with the melody names) · **MOVES** (only when the tank reports TURN/MOVE; hidden on the Uno Lite) · speed slider in a sheet behind the **⚡ MAX nn%** pill (owner, 2026-10-15: the pill shows the speed limit; was ⋯) |
| Bottom line | vitals: **SPEED nn% · TURN ±nn°/s** (owner, 2026-10-15: heading only in the top pill; battery volts in the battery gauge). Top pill heading is gyro heading from power-on (0° = where the tank pointed at start; no compass) |
| Banners | alarms from `ts.flags` / events (blocked, tilted, lifted, battery low/critical, link lost, self-test locked) as today, status colour + icon + text |
Not on Cockpit-lite (hub only): REC, AI panel, map / PiP map, missions, detection boxes, surface chip, child-play toggle (needs login/roles; the bridge has no authentication).
Capability rule: buttons for features the tank lacks are **hidden**, decided from `welcome.tank.caps` (HELLO CapBits): MOVES needs `turn`/`move`; SOUNDS needs `audio_led`.
