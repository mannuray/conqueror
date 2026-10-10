# Hub UX — Flows and Screens (living document)

Status: IN PROGRESS · Date: 2026-10-10 · Visual design tool: Google Stitch (after content is agreed)

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
