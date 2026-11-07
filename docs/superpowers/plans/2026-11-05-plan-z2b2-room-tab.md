# Plan Z2b-2 - the ROOM tab on /ai Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The `/ai` page gets a ROOM tab - the engine's room map with the tank, trail, home, play area, remembered things (FIND), names the owner gives blocks (GO THERE), layers - fed by the engine's AI feed; the INSIDE tab goes.

**Architecture:** Engine: pure domain pieces (`room_picture.py`: cell kinds, crop, PNG, Trail; `places.py`: names, the GO THERE spot, where words) used by an app-level `RoomFeed` that runs in `FeedPublisher.run`'s 10 Hz loop and sends only to pages that said `room on`; names are saved with the home (version 2). Page: `web/room.js` (RoomModel, RoomView, RoomWords - pure, node-tested) packed into `/ai` at `<!--CQ:ROOM_JS-->`; `ai.html` is the controller: it feeds the model from the feed and runs FIND / GO THERE as one-line programs through the tank, as the Play tab does.

**Tech Stack:** Python 3.12 (uv, ruff, pyright strict, pytest, numpy, opencv for PNG, websockets); plain ES5 page packed by `pack_web.py`, node `--test`, Playwright MCP checks on `web/test/fake_tank.py`.

**Spec:** `docs/lld/ai-engine-lld.md` §23, `docs/lld/esp32-bridge-lld.md` R32, `docs/design-notes.md` §211 (Stitch: `docs/design/stitch/22-ai-room-desktop-v1.jpg`, `23-ai-room-mobile-v1.jpg`).

## Global Constraints
- **Nothing on the feed drives the tank.** FIND and GO THERE are one-line programs (`FIND "ball`, `SETXY <right cm> <ahead cm>`, with `AREA n  ` in front when the Play tab's area chip is on) checked by `/logo/check` and sent as `logo_run`, through the Play tab's `runLine`. STOP, the lease, R29 / R31 unchanged.
- A person is never a pin or a name (`NEVER`, the page's `PERSON_WORDS`).
- Map frame: kinds 0 unknown, 1 seen before, 2 floor, 3 low, 4 high; 5 cm cells; crop = known cells + 0.5 m; header `{"kind":"map","seq","cell_m","x0","y0","w","h","png"}`; at most 1 / s and only when changed; `room` at most 2 / s and only when changed; both only to pages with room on.
- Coordinates: metres, x ahead at the start / home, y left, heading counter-clockwise from +x; the page draws up = +x, right = −y.
- Names: letters, spaces, `-`, 1-24 bytes, trimmed, lower case, not a person word, ≤ 50; same name twice allowed; a tap names the block within 15 cm; GO THERE spot 0.30-0.45 m from its block, ≥ 0.30 m from other blocks, floor seen this run, nearest the tank.
- Where words: nearest name or thing within 1.0 m → "near the bed"; else "1.2 m from home" / "1.2 m from the start".
- Trail: a point each 5 cm, at most 2000, since serve started, fed whether or not a word runs.
- Things: the Logo word (`ball:<name>` → `ball`), words not in the things list left out, never a person, never gone, newest first, ≤ 20.
- `/ai` budget **30 KB** gzipped (owner). Page ES5; colours from CSS variables (`--room-*`, cockpit on the page, Sunny in `theme.css`'s `/ai` block); layers remembered in `cq_room_layers` (try / catch).
- Python: type hints, ruff + pyright strict clean, no blocking in the event loop, no new dependency.
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A map frame or `room` arriving while ROOM is hidden, or before the first `room` - kept, drawn only when ROOM shows, no error (Task 8 check).
2. GO THERE pressed after the tank moved since the card opened - the SETXY line is worked out at the press from the newest pose, not at card open (Task 6 test, Task 8 check).
3. The engine restarted (feed reconnect) while ROOM is shown - the page sends `room on` again and the names come back from the home (Task 5 test, Task 8 check).
4. Two pages, one on ROOM and one on PLAY, one teaching while the other names - the PLAY page gets no map; neither `rep` is lost (Task 4 test).
5. The look flipped while ROOM shows - the map redraws in the new colours at once (Task 8 check).

---

### Task 1: The room as a picture, and the trail (engine, domain)

**Files:** Create `ai-engine/src/conqueror_ai/domain/room_picture.py`, `ai-engine/tests/test_room_picture.py`.

**Interfaces:**
- Produces: `KINDS = (UNKNOWN, BEFORE, FLOOR, LOW, HIGH) = 0..4`; `kinds(snap: MapSnapshot, seen_before: NDArray[np.bool_] | None) -> NDArray[np.uint8]` (indexed `[ix, iy]` as the map); `@dataclass(frozen=True) Crop(x0: float, y0: float, w: int, h: int, cells: NDArray[np.uint8])` (`cells[i, j]` = cell at x0 + i·cell, y0 + j·cell; `w` along x, `h` along y); `crop(k: NDArray[np.uint8], cell_m: float, size_m: float, margin_m: float = 0.5) -> Crop`; `png(c: Crop) -> bytes` (grey PNG, row = x index, column = y index, via `cv2.imencode`); `class Trail(step_m=0.05, cap=2000)` with `add(x: float, y: float) -> bool` and `points() -> list[tuple[float, float]]` (rounded to 0.01 m).

- [ ] **Step 1: Failing tests** - `test_kinds_values` (unknown 0, `seen_before & ~seen` 1, seen 2, low-only blocked 3, high 4 even when also low); `test_crop_margin` (one seen cell at (1.0, −0.5) → `x0 ≈ 0.5`, `y0 ≈ −1.0`, `w == h == 21`); `test_crop_empty` (all unknown → `w == h == 0`); `test_png_round_trip` (`cv2.imdecode(png(c), IMREAD_UNCHANGED)` equals `c.cells`); `test_trail_step` (points 3 cm apart → one kept; 6 cm → two); `test_trail_cap` (2001 points 10 cm apart → 2000, the first gone).
- [ ] **Step 2:** `cd ai-engine && uv run pytest tests/test_room_picture.py -q` - Expected: FAIL (module missing).
- [ ] **Step 3:** Write `room_picture.py` (pure, numpy + cv2; module docstring citing LLD §23.2).
- [ ] **Step 4:** Same command - PASS; `uv run ruff check . && uv run pyright` clean.
- [ ] **Step 5:** Commit `feat(ai): the room as a picture of cell kinds, and the trail (AI engine LLD §23.2)`.

### Task 2: Names on the map, the GO THERE spot, where words (engine, domain)

**Files:** Create `ai-engine/src/conqueror_ai/domain/places.py`, `ai-engine/tests/test_places.py`.

**Interfaces:**
- Consumes: `MapSnapshot` (`blocked`, `seen`, `cell_m`), `Pose`, `Remembered`.
- Produces: `@dataclass(frozen=True) Place(id: int, name: str, x_m: float, y_m: float)`; `@dataclass(frozen=True) Refusal(msg: str)`; `name_check(name: str) -> str | Refusal` (the cleaned name); `class Places(places: Sequence[Place] = ())` with `name_at(snap, x, y, name) -> Place | Refusal`, `rename(id, name) -> Place | Refusal`, `unname(id) -> bool`, `all() -> list[Place]`, `go_spot(snap, place, pose: Pose | None) -> tuple[float, float] | None`, `changed: bool` (set by any change, cleared by the caller); `where_words(x, y, places, things: Sequence[tuple[str, float, float]], home: bool) -> str`. Constants `TAP_M = 0.15`, `MAX_BLOCK = 4000`, `MAX_PLACES = 50`, `GO_NEAR_M = 0.30`, `GO_FAR_M = 0.45`, `GO_CLEAR_M = 0.30`, `NEAR_M = 1.0`.
- Refusal words: no block → "Tap a red or amber block to name it."; bad name → "Names are letters, spaces and - (up to 24)."; a person word → "I don't name people."; full → "That's 50 names - take one off first."; unknown id → "I can't find that name."

- [ ] **Step 1: Failing tests** - `test_name_at_near_block` (block cells around (1, 0); tap at (1.0, 0.25) where the block's edge is 0.10 m away → Place at the block's middle); `test_name_at_too_far` (0.20 m → Refusal "Tap a red or amber block…"); `test_name_l_shape` (an L block whose mean falls outside → the name on one of its cells); `test_name_rules` (`""`, 25 bytes, `"bed2"`, `"person"`, `"kids"` refused; `"  Toy Box "` → `"toy box"`); `test_rename_unname`; `test_fifty_one`; `test_same_name_twice`; `test_go_spot_open_floor` (a 40 × 40 cm block, floor seen round it, tank 1.5 m away on −x → spot on the −x side, distance to the block 0.30-0.45 m); `test_go_spot_unseen_side` (the tank's side unseen → the seen side); `test_go_spot_none` (no seen floor round it → None; the block faded → None); `test_go_spot_clear_of_others` (another block 0.35 m away on that side → no spot within 0.30 m of it); `test_where_near_name`, `test_where_near_thing`, `test_where_nearest_of_two`, `test_where_far_home` ("1.2 m from home"), `test_where_far_start` ("… from the start").
- [ ] **Step 2:** `uv run pytest tests/test_places.py -q` - FAIL.
- [ ] **Step 3:** Write `places.py`. The block: an 8-neighbour flood fill over `snap.blocked` from the nearest blocked cell, stopping at `MAX_BLOCK`. `go_spot`: distances to the block's cells and to other blocked cells from the floor cells within 0.5 m of the block (numpy, vectorised; no scipy). Person words: reuse `domain/person_guard.py`'s list.
- [ ] **Step 4:** PASS; ruff + pyright clean.
- [ ] **Step 5:** Commit `feat(ai): names on the map, the GO THERE spot and where words (AI engine LLD §23.3)`.

### Task 3: Names kept with the home (engine)

**Files:** Modify `domain/home.py`, `adapters/home_store.py`, `app/drive.py` (`end_home`), `tests/test_home.py`, `tests/test_home_store.py`.

**Interfaces:**
- Produces: `Home.places: tuple[Place, ...] = ()`; `Home.from_run(room, memory, earlier, run_id, places: Sequence[Place] | None = None)` (None → `earlier.places` if any, else `()`); `HOME_VERSION = 2`, `about["places"] = [asdict(p)…]`; a version-1 file loads with `places == ()`; `end_home(…, places: Sequence[Place] | None = None)`.

- [ ] **Step 1: Failing tests** - `test_from_run_keeps_live_places`, `test_from_run_carries_earlier_places_when_none` (a plain `cq-drive --home` run), `test_store_round_trip_places`, `test_store_loads_version_1` (a v1 file written by the old code path → no places).
- [ ] **Step 2:** `uv run pytest tests/test_home.py tests/test_home_store.py -q` - FAIL.
- [ ] **Step 3:** Implement; `drive.py`'s `end_home` call passes nothing (carries through).
- [ ] **Step 4:** PASS, and the whole engine suite `uv run pytest -q` - PASS; ruff + pyright clean.
- [ ] **Step 5:** Commit `feat(ai): names saved with the home, version 2 (AI engine LLD §23.3)`.

### Task 4: The feed knows who shows ROOM (engine, adapter)

**Files:** Modify `adapters/feed_server.py`, `tests/test_feed_server.py`, `domain/feed.py` (+ `tests/test_feed_frame.py`).

**Interfaces:**
- Produces: `FeedServer.rooms() -> set[int]` (pages with room on; a page starts off; gone on disconnect); `{"t":"room","on":bool}` handled inside the server (not put on `requests()`); `FeedServer.room_frame(data: bytes)` (slot `"map"`, only to `rooms()`); `FeedServer.send_rooms(msg)` (slot `msg["t"]`, only to `rooms()`); `FeedServer.room_wanted() -> bool` (any room page); `reply(page, msg)` uses slot `f"rep:{msg['id']}"` when `msg["t"] == "rep"`; `encode_map(header: dict, png: bytes) -> bytes` in `domain/feed.py` (the §22.2 layout, header with `"kind":"map"`), `decode_frame` unchanged for camera frames.

- [ ] **Step 1: Failing tests** - `test_room_on_off_per_page` (two FakePages; one sends room on → `rooms() == {1}`; it closes → `set()`); `test_room_message_not_a_request`; `test_room_frame_only_to_room_pages` (the PLAY page gets no map); `test_two_reps_both_sent` (teach rep id 4 and name rep id 5 put while the writer is blocked → both arrive); `test_encode_map_round_trip` (`decode_frame(encode_map(h, png))` → header with `kind == "map"`, the PNG bytes).
- [ ] **Step 2:** `uv run pytest tests/test_feed_server.py tests/test_feed_frame.py -q` - FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** PASS; ruff + pyright clean.
- [ ] **Step 5:** Commit `feat(ai): the feed knows which pages show ROOM; replies kept apart by id (AI engine LLD §23.2)`.

### Task 5: RoomFeed in the feed's loop, names from the pages (engine, app)

**Files:** Create `app/room_feed.py`, `tests/test_room_feed.py`. Modify `app/feed.py` (`FeedPublisher.run` calls `RoomFeed.tick`; `Telemetry` returns `(pose, battery_low, health, pan_deg)`; `status` gains `"pan"`; last area kept), `app/serve.py` (telemetry with pan; `name` / `unname` requests → `RoomFeed.on_name`; `Places` from the home at start, passed to `end_home`; the things the room sends), `tests/test_feed_publisher.py`, `tests/test_serve.py`.

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: `class RoomFeed(server, world: Callable[[], RoomInputs], places: Places, home: bool)`; `RoomInputs` = `(snap: MapSnapshot | None, seen_before, pose: Pose | None, things: list[Remembered], known_words: set[str], area: tuple[float, float, float] | None, notes: dict[str, object])`; `tick(now_s: float) -> None` (does nothing when `not server.room_wanted()`; adds the pose to its `Trail` always; map frame ≤ 1 / s when the kinds changed or a page just turned room on; `room` ≤ 2 / s when its dict changed; `go` recomputed when a name or the map changed or the tank moved 0.2 m); `on_name(page: int, msg: dict) -> dict` (the `rep`); `word_of(cls: str) -> str` (`ball:<x>` → `ball`).
- `room` dict exactly as AI engine LLD §23.2 (`trail`, `home`, `area`, `things` with `where` and `ago_s`, `names` with `go`, `why` from the running skill's notes `route` / `pending` / `check` / `looked`, `seen_m2`).

- [ ] **Step 1: Failing tests** (`test_room_feed.py`, a fake server recording what it sent): `test_nothing_without_room_page` (no map / room sent, but the trail still grows); `test_map_at_once_on_room_on`; `test_map_once_a_second_only_on_change`; `test_room_twice_a_second_only_on_change`; `test_things_logo_words` (`ball:red` → `ball`; a word not known left out; `person` never); `test_name_then_room_carries_it` (with `go`); `test_name_refused_reply`; `test_unname`; `test_area_kept_after_word`; `test_why_empty_between_words`. `test_feed_publisher.py`: `test_status_has_pan`. `test_serve.py`: `test_names_from_home_and_saved` (start with a home with a name → it is in `room`; a new name → saved at the next save point); `test_room_on_again_after_reconnect` (a new page connection starts off; after its `room on` it gets the map at once).
- [ ] **Step 2:** `uv run pytest tests/test_room_feed.py tests/test_feed_publisher.py tests/test_serve.py -q` - FAIL.
- [ ] **Step 3:** Implement. `FeedPublisher.run` calls telemetry every loop (not only with pages) so the trail grows; it keeps the last `SkillCall`'s area after the word ends.
- [ ] **Step 4:** The whole engine suite PASS; ruff + pyright clean.
- [ ] **Step 5:** Commit `feat(ai): the ROOM tab's map and room on the AI feed, names from the pages (AI engine LLD §23.4)`. Update §23 if anything was ruled; note it in the ledger.

### Task 6: RoomModel and RoomWords (page, pure)

**Files:** Create `firmware/esp32/web/room.js`, `firmware/esp32/web/test/room.test.js`.

**Interfaces:**
- Produces: `window.cqRoom = { RoomModel, RoomView, RoomWords }` (`module.exports` the same, as `bar.js`). `new RoomModel()`: `state = {cells: Uint8Array|null, crop: {x0,y0,w,h,cell}|null, trail: [], home: null, area: null, things: [], names: [], why: {route:[],pending:[],check:null,looked:[]}, seenM2: 0, pose: null, pan: 0, layers: {things:true, trail:true, names:true, why:false}, feed: "none"|"open"|"lost"}`; `applyMap(header, cells)`, `applyRoom(msg)`, `applyStatus(msg)`, `feedOpen()`, `feedDown()`, `setLayer(key, on)`, `loadLayers(json)` / `layersJson()`, `on(fn)`. `RoomWords.ago(s)` ("just now" < 60 s, "N min ago" < 1 h, "today" < 24 h, else "earlier"); `RoomWords.facts(state)` ("home room2 · play area 3 m · floor seen 4.2 m²"; "no home"; "anywhere"); `RoomWords.nameCheck(name)` (null or the refusal text, same words as Task 2); `RoomWords.setxyLine(go, pose)` → `"SETXY <right> <ahead>"` or null.

- [ ] **Step 1: Failing tests** - `applyMap` keeps the crop and cells; `applyRoom` drops a `person` thing and person-word names; `feedDown` → `feed == "lost"` and the map kept; layers default, `loadLayers` of a bad string → defaults; `ago` 30 / 120 / 7200 / 90000 s; `facts` the three forms; `nameCheck` (empty, 25 bytes, `bed2`, `kids` refused; `toy box` ok); `setxyLine({go:[1,0]}, pose 0,0,0)` → `SETXY 0 100`; `([0,1], pose 0,0,0)` → `SETXY -100 0`; `([0,1], pose 0,0,h 90)` → `SETXY 0 100`; `([20,0], pose 0,0,0)` → null; no pose → null; and the Review Focus 2 case: the line uses the pose given at the call (`setxyLine` with the newer pose).
- [ ] **Step 2:** `node --test firmware/esp32/web/test/room.test.js` - FAIL.
- [ ] **Step 3:** Write the model and words (ES5, no DOM).
- [ ] **Step 4:** PASS, and `node --test firmware/esp32/web/test/` all PASS.
- [ ] **Step 5:** Commit `feat(web): the ROOM tab's model and words (bridge LLD R32.2)`.

### Task 7: RoomView, and room.js packed into /ai

**Files:** Modify `web/room.js`, `web/test/room.test.js`, `scripts/pack_web.py` (`<!--CQ:ROOM_JS-->`, required in `ai.html`; `AI_BUDGET = 30 * 1024`), create `scripts/test_ai_pack.py`; modify `web/ai.html` (only the marker in the script block for now).

**Interfaces:**
- Produces: `RoomView.fit(state, w, h) -> {scale, ox, oy}` (the crop, trail and tank inside, at least 2 × 2 m, up = +x, right = −y); `toScreen(t, x, y) -> [px, py]`; `toRoom(t, px, py) -> [x, y]`; `hit(state, t, px, py) -> {kind: "pin"|"name"|"block"|"none", ref}` (pin within 22 px, then a name label's box, then a cell of kind 3 / 4 within 15 cm, else none; hidden layers not hit); `draw(ctx, state, t, colours)`; `colours` read by the page from `--room-floor --room-before --room-low --room-high --room-trail --room-route --room-area --room-pin --room-cone`.

- [ ] **Step 1: Failing tests** - `fit` with a crop 2 × 1 m in a 400 × 400 canvas: +x up, −y right, everything inside, at least 2 m shown; `toRoom(toScreen(p)) ≈ p`; `hit` order (a pin over a block → pin; a name over a block → name; a block 10 cm away → block; floor → none; things layer off → the pin not hit); `draw` on a fake 2D context records `fillRect` / `arc` calls without throwing on an empty state. `test_ai_pack.py`: the marker replaced, `window.cqRoom =` once, no `data-tab="inside"` after Task 8 (skip until then with a clear reason), `/ai` under 30 KB, missing marker → `SystemExit` naming it.
- [ ] **Step 2:** `node --test firmware/esp32/web/test/room.test.js && python3 -m unittest discover -s firmware/esp32/scripts -p 'test_*.py'` - FAIL.
- [ ] **Step 3:** Implement the view; the packer step beside `<!--CQ:AI_JS-->`.
- [ ] **Step 4:** Both PASS; `python3 firmware/esp32/scripts/pack_web.py` prints the `/ai` size.
- [ ] **Step 5:** Commit `feat(web): the ROOM map's view, room.js packed into /ai, budget 30 KB (bridge LLD R32)`.

### Task 8: The ROOM tab on the page

**Files:** Modify `web/ai.html`, `web/theme.css` (`/ai` block: `--room-*` Sunny), `web/test/ai_check.js` (no INSIDE), create `web/test/room_check.js`; `scripts/test_ai_pack.py` (un-skip the INSIDE check).

**Interfaces:**
- Consumes: Tasks 6-7; the Play tab's `runLine(line)`, its area chip state, `AiCore.splitFrame`.
- The page: tabs PLAY | ROOM (INSIDE and `#tInside` removed); `#tRoom` = the map card (`<canvas id="roomMap">`, the 1 m scale, the AI-off / feed-lost line) + the panel (`#roomLayers`, `#roomList`, `#roomFacts`); laptop ≥ 900 px side by side, phone stacked with layer chips; the pin card (`#roomCard`: word, where · when, FIND), the name card (GO THERE, RENAME, TAKE OFF), the new-name card ("What's this?", input, NAME); a reply shown in the card. `room on` when ROOM shows and the feed is open (again on each feed open), `room off` on PLAY and when hidden. Binary feed messages: `kind === "map"` → decode the PNG (`createImageBitmap`, off-screen canvas, red channel) → `applyMap`; else the camera frame as now. Overlays redraw on `status` only while ROOM shows; on `cqtheme` at once. Words exactly as R32.1 ("Start the AI helper on the laptop to see the room.", "Lost the AI helper - reconnecting…", "Nothing yet - things it sees are kept here.", "I don't know a way there yet.", "Where am I? Start the AI helper.").

- [ ] **Step 1: Failing check** - `room_check.js` (the in-page WebSocket stand-in as `ai_check.js`; the fake feed builds the map PNG with a canvas in the page): at 390 × 844 and 1440 × 900, both looks: tabs PLAY / ROOM only; layout (laptop map left, panel right; phone stacked; no sideways scroll); ROOM sends `room on`, PLAY `room off`; a map frame and `room` sent while PLAY shows are drawn when ROOM opens (Review Focus 1); a pin tap opens its card and FIND sends `logo_run` for `FIND "ball` and nothing else; a name tap → GO THERE sends `SETXY` from the **newest** status pose (move the pose between the card and the press; Review Focus 2); `go: null` greys GO THERE with its words; a block tap → card → NAME sends `name` with the tapped room point; a refused `rep` shows its words; feed closed and reopened → `room on` sent again (Review Focus 3); AI off and feed lost words; 🎨 flip redraws (the canvas pixels change; Review Focus 5); the layers choice survives a reload.
- [ ] **Step 2:** Run it on `fake_tank.py` (restart it after packing) - FAIL.
- [ ] **Step 3:** Build the tab in `ai.html` + `theme.css`.
- [ ] **Step 4:** `room_check`, `ai_check`, `look_check`, `bar_check`, `strip_check`, `link_check`, `theme_check`; node tests; Python unittests - PASS. `/ai` ≤ 30 KB.
- [ ] **Step 5:** Commit `feat(web): the ROOM tab - map, layers, I remember with FIND, names with GO THERE (bridge LLD R32)`.

### Task 9: Together, on the tank

**Files:** Modify `docs/design-notes.md` (§211 as built), `docs/lld/ai-engine-lld.md` (§23 as built), `docs/lld/esp32-bridge-lld.md` (R32 as built), `CLAUDE.md`.
- [ ] **Step 1:** Engine suite, ruff, pyright; all node tests; all Python unittests; all page checks - PASS. Screenshots of ROOM, phone and laptop, both looks.
- [ ] **Step 2:** OTA flash; `curl --compressed` `/ai` shows `window.cqRoom =` and no INSIDE.
- [ ] **Step 3 (owner present; stand first, nothing moves):** `cq-drive serve --home room2`; ROOM on the laptop and the iPhone - the map, the tank, layers, I remember, a name on a block saved across a serve restart. **Then floor (owner confirms; STOP in hand; camera checked):** FIND from a pin; GO THERE on a named block ends ~30-45 cm in front of it; STOP during each.
- [ ] **Step 4:** Docs as built; commit `docs: Plan Z2b-2 as built (design-notes §211, AI engine LLD §23, bridge LLD R32)`; push.
