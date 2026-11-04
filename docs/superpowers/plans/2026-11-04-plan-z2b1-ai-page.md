# Plan Z2b-1 — the `/ai` page's frame, the Play tab, teach a word Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A child opens **AI** in the tank's ☰ menu and gets a page served by the tank. It shows the engine's camera with boxes in one view and depth in the other, the same moment in both. It says what the tank is thinking, has Find / Wander / Explore buttons and Why?, and lets an adult teach it a new word. STOP stays on the tank's own path the whole time.

**Architecture:**
- **The tank (bridge)** serves `GET /ai`. It learns the engine's address from the skill server's `/ws` socket and the `page` field of its hello. It tells each `ai-page` client where the feed is (`ai_at`). It turns `skill_learning` into `Caps::ai_learning`, which gives 7056.
- **The engine (`cq-drive serve`)** gains a `FeedServer` on `ws://0.0.0.0:8770/feed`:
  - one binary message per picture: camera, boxes and depth together;
  - `status` about 10 times a second; `why` and `things` when they change;
  - `teach` → `rep`.
  - A `Teacher` re-exports YOLO-World with the taught words, in a child process.
- **The page (`web/ai.html` + `web/ai.js`)** talks to the tank for STOP, the one-line programs and telemetry, and to the engine for everything AI.

**Tech Stack:**
- Bridge: C++17 on the ESP32-S3 (ESP-IDF httpd), PlatformIO with host tests on `native` (Unity).
- Logo generator: Python 3.12 via uv (`logogen`).
- AI engine: Python 3.12 via uv, websockets (already a dependency), numpy, opencv, pytest, ruff, pyright strict.
- Page: plain JS; deciding code tested with `node --test`; a Playwright browser check with faked WebSockets.

**Spec:**
- `docs/lld/esp32-bridge-lld.md` R27 (approved 2026-11-04).
- `docs/lld/ai-engine-lld.md` §22 (approved 2026-11-04).
- `docs/lld/tank-logo-lld.md` §24.4.
- `docs/superpowers/specs/2026-10-25-08-drive-while-thinking-design.md` §4.5.
- `docs/design-notes.md` §198-§200.
- The look: `docs/design/ai-page-mockup.dc.html`.
- The Room and Inside tabs are Plan Z2b-2, not here.

## Global Constraints

- **Safety:**
  - Nothing on the feed (port 8770) can move or stop the tank. STOP and every program go page → tank `/ws`.
  - Nothing on the Uno changes. The lease, the deadman (300 ms), STOP and the brake are untouched.
  - Never weaken a safety rule to make a test pass.
- **Never a person:** `person` cannot be taught (refused) and is never in the things list. `person` boxes are drawn on the page, but a person is never a thing to find.
- **Errors, verbatim:** 7056 "I'm learning a new word - try again in a minute."
- **Wire, bridge `/ws` (R27.2):**
  - the engine's hello gains `"page":{"port":8770,"proto":1}`; a port outside 1024-65535, or no `page`, means no URL;
  - the page's hello is `{"t":"hello","client":"ai-page"}`;
  - the bridge sends `{"t":"ai_at","url":"ws://<ip>:<port>/feed","proto":1}`, or `{"t":"ai_at","url":null}`;
  - the engine sends `{"t":"skill_learning","on":true|false}`, accepted only from the skill server;
  - the new `skill_failed` reason is `learning`.
- **Wire, feed (§22.2):**
  - text: `hello {proto:1}`, `status`, `why`, `things`, `teach`, `rep`;
  - **binary frame:**
    - a 4-byte **big-endian** header length, then the header JSON `{"seq","t_ms","boxes","cam","depth"}`, then the camera JPEG, then the depth JPEG (`depth` 0 = none in this frame);
    - `t_ms` is the bridge's clock (R17) of the picture;
    - boxes: at most 8, as `[word, score, x, y, w, h]` with x, y, w, h in 0..1000 of the picture; `ball:<name>` shows as `ball`; maybes (`sure: false`) are left out;
  - the camera is 320 × 240 at JPEG quality 70; the depth is 160 × 120, green, near bright.
- **Depth is not on every frame.** It comes with each picture the map used (`depth.every_n: 1`); a skipped picture has no depth. The page keeps the last depth until a new one comes, so the layer is at most one picture old. The camera, boxes and depth of one message are always one moment.
- **Bounds:** at most 4 pages (a fifth is refused). Each page has an outbox that keeps only the newest of each kind. A page stalled for 5 s is closed. Status goes out at about 10 Hz.
- **Teach:**
  - letters, spaces and `-` only; at most 24 bytes; at most 8 names; never `person`; refused while a skill runs; the export has a 200 s limit;
  - files go in `ai-engine/taught/` (git-ignored): `vocabulary.yaml`, `yoloworld-s-640.mlpackage`, `words.txt`;
  - the word reaches the child process only inside the YAML file, never in argv or a path.
- **Generated files** (`firmware/esp32/src/logo/generated/`, `firmware/esp32/test/test_logo_conformance/cases.h`, `firmware/esp32/test/test_logo_running/cases.h`, `firmware/esp32/web/generated/`) are never edited by hand. Change `logo/spec/*`, then run `cd logo && uv run logogen write`.
- **Checks before every commit:**
  - Bridge: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native` all pass, and `~/.platformio/penv/bin/pio run -e bridge` builds.
  - Logo: `cd logo && uv run pytest -q` and `uv run logogen check`.
  - AI engine: `cd ai-engine && uv run pytest -q`, `uv run ruff check .`, `uv run ruff format --check .` and `uv run pyright`, each run **unpiped**; each must exit 0.
  - Page: `node --test firmware/esp32/web/test/ai.test.js`.
  - Engine rule: `domain/` never imports `app/` or `adapters/`, reads no clock and prints nothing.
- **Branch:** `plan-z2b`, from `plan-z2a`. Fixes from the Z2a tank checks (Task 11 of Plan Z2a, owner present) are merged into `plan-z2b` before execution starts. Commits use Conventional Commits and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Hardware:**
  - The board machine is `mannu@yantra.lan`, with the ESP32-S3 on `/dev/ttyACM0`. The protocol version is unchanged, so the Uno is not reflashed.
  - Before every drive, say what moves and confirm stand or floor.
  - Pictures saved from the page stay on the phone.

## Review Focus

1. **The engine goes away or comes back.** Ctrl-C, the laptop sleeping, or the WiFi dropping. The page must keep STOP and the status strip, show "AI helper not running", and reconnect within about 2 s after `ai_at` gives a URL again. The bridge must send `ai_at null` whichever way the server's socket closed: a close, a ghost or the 3 s silent rule. Tests in Task 4 (link-loop compare) and Task 10 (camera states).
2. **A teach races a program.** A `FIND` checked just before `skill_learning on` reaches the bridge must end 7056, never hang. The engine answers that start with `skill_failed learning`. Tests in Task 2 (running case) and Task 9.
3. **Ctrl-C during an export.** The child process (uv → python) is killed with its group, the taught entry is removed and the old model is kept. Next start loads the measured model. Test in Task 9.
4. **A slow phone.** A page on weak WiFi must never slow the skill or grow memory. It gets only the newest frame, and after 5 s stalled it is closed. Test in Task 6, with a fake connection whose send blocks (a localhost socket buffers megabytes and would not stall).
5. **iOS Safari.** `speechSynthesis` speaks only after a user gesture, so the page unlocks it on the first tap. The binary frame arrives as an `ArrayBuffer` (`binaryType = "arraybuffer"`), and pictures are drawn with `createImageBitmap`. Checked in Task 11 and on the owner's phone in Task 12.

---

### Task 1: 7056 and the `learning` end in the Logo spec and generator

**Files:**
- Modify: `logo/spec/words.yaml` (error 7056), `logo/generator/logogen/*` (the conformance key `tank.learning`, and `learning` as a `skill_ends` value), `logo/spec/tests/errors.yaml`, `logo/spec/tests/running.yaml`
- Generated: `firmware/esp32/src/logo/generated/*`, both `cases.h`, `firmware/esp32/web/generated/words.json`
- Test: `logo/tests/test_logogen.py`

**Interfaces:**
- Produces:
  - error 7056 in the generated error table;
  - conformance cases may carry `tank: {ai: bool, learning: bool}`, emitted as a `learning` field in `cases.h`;
  - running cases may list `learning` in `world.skill_ends`.

- [ ] **Step 1: Write the failing tests.**
  - `test_logogen.py::test_error_7056_verbatim`.
  - `test_logogen.py::test_tank_learning_key`: a case with `tank.learning: true` emits `learning = true`.
  - `errors.yaml`:
    - `tank: {ai: true, learning: true}` gives 7056 for `FIND "ball` and for `SEE? "ball`;
    - `tank: {ai: false, learning: true}` gives 7020.
  - `running.yaml`: `FIND "toy` with `world: {skill_ends: [learning]}` ends 7056.
- [ ] **Step 2:** `cd logo && uv run pytest -q`. Expected: FAIL (no 7056, no key).
- [ ] **Step 3:** Add 7056 and the key to the generator; `learning` becomes the last `skill_ends` value.
- [ ] **Step 4:** `uv run pytest -q` passes; `uv run logogen write`; `uv run logogen check` exits 0. The new conformance and running cases fail natively until Task 2 (note it in the ledger).
- [ ] **Step 5: Commit** `feat(logo): error 7056 while the AI helper learns a word (Tank Logo LLD §24.4)`.

### Task 2: The checker and runner give 7056

**Files:**
- Modify: `firmware/esp32/src/logo/checker.h` / `.cpp`, `firmware/esp32/src/logo/run_controller.h` / `.cpp`, `firmware/esp32/src/core/ws_codec.cpp` (the reason table), `firmware/esp32/test/test_logo_conformance/` and `test_logo_running/` harnesses (read `learning`)
- Test: `test_logo_checker`, `test_logo_conformance`, `test_logo_running`, `test_ws_codec`

**Interfaces:**
- Consumes: Task 1's generated cases.
- Produces:
  - `Caps` gains `bool ai_learning = false;`;
  - `logo::SkillEnd` gains `kLearning`, as the last value;
  - `kBySkillEnd[kLearning] = 7056`;
  - the codec maps `skill_failed reason:"learning"` → `kLearning`.

- [ ] **Step 1: Write the failing tests.**
  - `test_logo_checker::test_learning_gives_7056_for_ai_words`: `ai = true, ai_learning = true` → 7056 at `FIND`; `FORWARD 10` is still OK.
  - `test_logo_checker::test_no_ai_beats_learning`: `ai = false, ai_learning = true` → 7020.
  - `test_ws_codec::test_skill_failed_learning`.
  - The Task 1 cases.
- [ ] **Step 2:** `pio test -e native -f test_logo_checker -f test_ws_codec -f test_logo_conformance -f test_logo_running`. Expected: FAIL.
- [ ] **Step 3:** In `Checker::needsAndLevel`:
  - `needs ai` and `!caps.ai` → 7020, as now;
  - otherwise, `needs ai` and `caps.ai_learning` → 7056;
  - then encoders, then level.
  - Add the enum value, the table row and the codec row. The `static_assert` on `kBySkillEnd` must hold.
- [ ] **Step 4:** All native tests pass.
- [ ] **Step 5: Commit** `feat(logo): 7056 for AI words while the helper learns; skill_failed learning ends with it`.

### Task 3: The bridge knows where the feed is — `SkillHub` page and learning, the codec, the peer address

**Files:**
- Modify:
  - `firmware/esp32/src/core/skill_hub.h` / `.cpp`;
  - `firmware/esp32/src/core/ws_codec.h` / `ws_codec.cpp` / `ws_codec_keys.cpp`: the hello's `page`, and `kSkillLearning` with `on`;
  - `firmware/esp32/src/ports/client_out.h` and `src/net/ports_esp32.cpp`: `clientPeerIp`;
  - `src/fakes/` (its fake);
  - `src/app/command_router.cpp` / `.h`: route `skill_learning`, pass the page to the hub;
  - `src/net/logo_worker.cpp`: `caps()` sets `ai_learning`.
- Test: `test_skill_hub`, `test_ws_codec`, `test_command_router`

**Interfaces:**
- Produces:
  - `WsCommand` gains `uint16_t page_port = 0; uint8_t page_proto = 0; bool on = false;` and kind `kSkillLearning`.
  - `ports::clientPeerIp(int fd, char* out, size_t cap) -> bool`: dotted IPv4 from `getpeername`, including a v4-mapped v6 address. False when not known.
  - `SkillHub::Decision onHello(int fd, const char* peer_ip, uint16_t page_port, uint8_t page_proto)`. It keeps the URL only for the server, and only when the port is in 1024-65535 and `peer_ip` is not empty.
  - `const char* SkillHub::pageUrl() const`: `"ws://<ip>:<port>/feed"`, or `""`. Backed by `char[40]`, cleared in `onClose` of the server.
  - `uint8_t SkillHub::pageProto() const`.
  - `void SkillHub::onLearning(int fd, bool on)`: ignored unless `fd == server()`.
  - `bool SkillHub::learning() const`: a `SharedFlag`, read on the worker task and cleared on the server's close.
- [ ] **Step 1: Write the failing tests.**
  - `test_skill_hub`:
    - `test_page_url_from_hello`: ip `192.168.1.23`, port 8770 → `ws://192.168.1.23:8770/feed`, proto 1;
    - `test_bad_page_port_no_url`: 0, 80 and 70000 (parsed as out of range) give `""`;
    - `test_no_peer_no_url`;
    - `test_close_clears_url_and_learning`;
    - `test_learning_only_from_server`.
  - `test_ws_codec`:
    - `test_hello_page`;
    - `test_skill_learning_on_off`;
    - `test_skill_learning_bad_on_refused` (`"on":"yes"` → bad field).
  - `test_command_router::test_skill_learning_routed_to_hub`.
- [ ] **Step 2:** `pio test -e native -f test_skill_hub -f test_ws_codec -f test_command_router`. Expected: FAIL.
- [ ] **Step 3:** Implement. Everything else on `/ws` stays as it is.
  - `CommandRouter::onHello` calls `ports::clientPeerIp` only for `ai-skills`.
  - The fake's peer IP can be set by tests (`fakes::setPeerIp(fd, "…")`).
- [ ] **Step 4:** All native tests pass; `pio run -e bridge` builds.
- [ ] **Step 5: Commit** `feat(bridge): the skill server's feed address and skill_learning (bridge LLD R27.1-R27.3)`.

### Task 4: `AiPages` — `ai_at` to each `/ai` page, and the page served

**Files:**
- Create: `firmware/esp32/src/core/ai_pages.h` / `.cpp`
- Modify:
  - `src/app/command_router.*`: an `ai-page` hello registers the page and replies `ai_at`;
  - `src/net/link_task.cpp`: once per loop, `pages.announceIfChanged(hub, out)`; `pages.onClose(fd)` on kWsClose;
  - `scripts/pack_web.py`: `/ai` = `web/ai.html` with `<!--CQ:AI_JS-->` replaced by `web/ai.js`, gzipped, with a **24 KB gzipped budget**.
- Create (stubs, so the build packs them; filled in Tasks 10-11): `firmware/esp32/web/ai.html`, `firmware/esp32/web/ai.js`
- Test: `firmware/esp32/test/test_ai_pages/`

**Interfaces:**
- Consumes: `SkillHub::pageUrl()` and `pageProto()` (Task 3).
- Produces:
  - `class AiPages { static constexpr uint8_t kMax = 4; bool onHello(int fd); void onClose(int fd); bool contains(int fd) const; static size_t text(char* buf, size_t cap, const char* url, uint8_t proto); bool changed(const char* url); }`
  - `text` gives exactly `{"t":"ai_at","url":"ws://…/feed","proto":1}`, or `{"t":"ai_at","url":null}` when `url` is `""`.
  - `changed` compares with the last URL announced and remembers it.
  - In `link_task`, on a change, the text goes to every page fd. On a page's hello, it goes to that fd only.
- [ ] **Step 1: Write the failing tests** (`test_ai_pages`):
  - `test_text_exact_with_url`;
  - `test_text_exact_null`;
  - `test_hello_registers_up_to_four`;
  - `test_close_forgets`;
  - `test_changed_only_on_a_new_url`;
  - `test_url_gone_is_a_change`. This covers every close path: the compare runs on the hub's state, not on a close event.
  - `test_command_router::test_ai_page_hello_gets_welcome_and_ai_at`;
  - `test_command_router::test_other_clients_get_no_ai_at`.
- [ ] **Step 2:** `pio test -e native -f test_ai_pages -f test_command_router`. Expected: FAIL.
- [ ] **Step 3:** Implement the class, the link-loop compare and the `pack_web.py` asset with its budget. The `ai.html` stub carries one line with the `<!--CQ:AI_JS-->` marker.
- [ ] **Step 4:** Native tests pass; the bridge builds; the build log lists `/ai`.
- [ ] **Step 5: Commit** `feat(bridge): ai_at tells each /ai page where the engine's feed is; GET /ai (bridge LLD R27)`.

### Task 5: The engine's words for the page — `Thinking`, `Pictures`, the frame format

**Files:**
- Create:
  - `ai-engine/src/conqueror_ai/domain/thinking.py`;
  - `domain/pictures.py` (cv2 is already used in `domain/balls.py`);
  - `domain/feed.py`.
- Test: `ai-engine/tests/test_thinking.py`, `test_pictures.py`, `test_feed_frame.py`

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) class Thought: state: str; mood: str; say: str`
  - `class Thinking`:
    - `update(self, word: str | None, phase: str | None, result: str | None, thing: str | None, *, battery_low: bool, learning: str | None) -> tuple[Thought, bool]`
    - `word` is `find`, `wander`, `explore` or `setxy`, or `None` between words.
    - `phase` is the roam state: `start`, `drive`, `check`, `recover`, `chase`, `done` or `failed`.
    - `result` is the skill's end reason, or `None`.
    - The bool is "changed": true only when the state or the thing changes.
  - `why_lines(word: str, thing: str | None, debug: dict[str, object], pose_xy: tuple[float, float] | None, elapsed_s: float) -> list[str]`. Exactly 3 lines (purpose, process, performance), built only from `debug()` keys: `state`, `dest`, `check`, `pending`, `ruled_out`.
  - `cam_jpeg(rgb: NDArray[np.uint8]) -> bytes`: 320 × 240, quality 70.
  - `depth_jpeg(inv: NDArray[np.floating]) -> bytes`: 160 × 120; the green channel = inverse depth scaled 0..255 between its 2nd and 98th percentiles (near bright); red and blue 0.
  - `page_boxes(dets: Sequence[Detection], w: int, h: int) -> list[list[object]]`: sure boxes only; `ball:*` → `ball`; at most 8, highest score first; `[word, round(score, 2), x, y, w, h]` as ints in 0..1000.
  - `encode_frame(header: dict[str, object], cam: bytes, depth: bytes | None) -> bytes` and `decode_frame(b: bytes) -> tuple[dict[str, object], bytes, bytes | None]`. The header gets `cam` and `depth` (the lengths) filled in.
- **The table, from §22.3, plus one new row:**

  | word / phase / result | state | mood | say |
  |---|---|---|---|
  | none | `ready` | `happy` | "Ready to play!" |
  | find / start | `looking` | `curious` | "I'm looking round for the {thing}." |
  | find / drive | `searching` | `curious` | "I'm searching for the {thing}." |
  | find / check | `checking` | `puzzled` | "Is that the {thing}? Let me check." |
  | find / chase | `chasing` | `excited` | "I see the {thing}! Going to it." |
  | any / recover | `unsticking` | `puzzled` | "Oops - I'm getting myself unstuck." |
  | wander or explore / any other phase | `exploring` | `curious` | "I'm exploring the room." |
  | setxy / any other phase | `going` | `busy` | "I'm going there." |
  | result `done` | `done` | `happy` | find: "I found the {thing}!"; others: "Done!" |
  | another result | `failed` | `sad` | the Logo error's own words: `not_found` 7050, `lost` 7051, `outside_area` 7054, `no_way` 7055, `blocked` "I'm blocked." |
  | `learning` set | `learning` | `thinking` | "I'm learning what a {learning} is." |

  `battery_low` keeps the state and the say, and sets the mood to `tired`.
- [ ] **Step 1: Write the failing tests.**
  - `test_thinking`:
    - one test per table row (parametrised);
    - `test_changed_only_on_new_state_or_thing`;
    - `test_battery_low_is_tired`;
    - `test_why_three_lines_for_check`: debug `state:"check"`, `check:(1.2, 0.0)`, pose `(0, 0)` → line 2 "There might be a toy 1.2 m away - I'm going to look closer.";
    - `test_why_counts_maybes`: `ruled_out` 2 and `pending` 1 → "I've checked 2 of 3 maybes in 14 s.".
  - `test_pictures`:
    - `test_cam_is_320x240` (decode the result with cv2);
    - `test_depth_near_is_bright_green` (a ramp: the large-inverse end has G > 200 and R = B = 0 within JPEG error 8).
  - `test_feed_frame`:
    - `test_round_trip_with_and_without_depth`;
    - `test_header_length_big_endian` (the first 4 bytes are `struct.pack(">I", n)`);
    - `test_page_boxes_scale_ball_word_maybes_left_out_top_8`.
- [ ] **Step 2:** `cd ai-engine && uv run pytest tests/test_thinking.py tests/test_pictures.py tests/test_feed_frame.py -q`. Expected: FAIL (no modules).
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** The tests and the full engine checks pass.
- [ ] **Step 5: Commit** `feat(ai): what the tank says, its pictures and the feed's frame format (AI engine LLD §22.2-§22.3)`.

### Task 6: `FeedServer` on port 8770

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/feed_server.py`
- Test: `ai-engine/tests/test_feed_server.py`

**Interfaces:**
- Produces:
  - `class PageConn(Protocol)`: `async send(self, data: str | bytes) -> None`; `async close(self) -> None`; `def __aiter__(self) -> AsyncIterator[str | bytes]`.
  - `class FeedServer`:
    - `__init__(self, host: str = "0.0.0.0", port: int = 8770, *, max_pages: int = 4, stall_s: float = 5.0, greeting: Callable[[], list[dict[str, object]]] = list)`
    - `async run(self, stop: asyncio.Event) -> None`: serves `/feed` with `websockets.asyncio.server.serve`; any other path is closed.
    - `async attach(self, conn: PageConn) -> None`: one page's life; `run` calls it; tests call it directly.
    - `pages: int` (property).
    - `frame(self, data: bytes) -> None` and `send(self, msg: dict[str, object]) -> None`: put into every page's outbox, newest wins. The slot is `"frame"` for frames and `msg["t"]` for text.
    - `async requests(self) -> AsyncIterator[tuple[int, dict[str, object]]]`: the page's number and its JSON message.
    - `reply(self, page: int, msg: dict[str, object]) -> None`.
  - On attach the page gets `{"t":"hello","proto":1}`, then each message of `greeting()`. A fifth page is closed with code 1013.
- [ ] **Step 1: Write the failing tests**, using a fake `PageConn` whose `send` can be blocked with an `asyncio.Event`:
  - `test_hello_then_greeting`;
  - `test_frame_newest_wins_while_blocked`: block, post frames 1-3, unblock → the page gets 1 (in flight) then 3;
  - `test_stalled_page_closed_after_stall_s` (`stall_s = 0.2`);
  - `test_fifth_page_refused`;
  - `test_requests_yield_page_and_message`;
  - `test_bad_json_dropped`;
  - `test_no_pages_frame_is_free` (`frame()` with no page keeps nothing);
  - `test_real_socket_round_trip`: port 0 and a websockets client, hello plus one binary frame.
- [ ] **Step 2:** `uv run pytest tests/test_feed_server.py -q`. Expected: FAIL.
- [ ] **Step 3:** Implement. Each page has one writer task: it waits for any filled slot, sends frames first and then text, and wraps each send in `asyncio.wait_for(…, stall_s)`.
- [ ] **Step 4:** The tests and the full engine checks pass.
- [ ] **Step 5: Commit** `feat(ai): the AI feed server for /ai pages, newest-wins per page (AI engine LLD §22.5)`.

### Task 7: `FeedPublisher` and the wiring in `cq-drive serve`

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/feed.py` (FeedPublisher, and `TickFan`, a `TickView` that calls several views)
- Modify:
  - `app/perception.py`: an optional `on_frame` hook, called after `world.on_picture`;
  - `adapters/skill_link.py`: the hello's `page`, and `learning(on)`;
  - `app/serve.py`: build the server and the publisher; `--page-port` (default 8770); the hook and the fan into the runner;
  - `Names.wire()` gains `measured`, with `Word.measured: bool = True` in `domain/vocabulary.py`.
- Test: `ai-engine/tests/test_feed_publisher.py`, `test_skill_link.py`, `test_perception.py`, `test_serve.py`

**Interfaces:**
- Consumes: Tasks 5 and 6.
- Produces:
  - `Perception(..., on_frame: Callable[[int, NDArray[np.uint8], list[Detection], NDArray[np.floating] | None], None] | None = None)`. The hook is called with `(cap_us, rgb, dets, inv)` for each decoded picture. `inv` is `None` when depth did not run on it.
  - `class FeedPublisher`:
    - `__init__(self, server: FeedServer, names: Callable[[], list[dict[str, object]]])`
    - `on_frame(self, cap_us: int, rgb, dets, inv) -> None`: does nothing when `server.pages == 0`; else encodes and posts one frame.
    - `on_tick(...)`: matches `TickView.tick`; keeps the word's phase and debug.
    - `word(self, word: str | None, thing: str | None) -> None`.
    - `ended(self, result: str) -> None`.
    - `learning(self, word: str | None) -> None`.
    - `async run(self, stop: asyncio.Event, telemetry: Callable[[], tuple[Pose | None, bool, str]]) -> None`: status every 0.1 s while a page is connected. The telemetry callable gives pose, battery low and health. `why` and `things` are sent only on change.
    - `greeting(self) -> list[dict[str, object]]`: status, why and things, for `FeedServer(greeting=…)`.
  - `SkillLink(url, things, silent_s=3.0, page_port: int | None = None)` puts `"page":{"port":p,"proto":1}` in the hello. `async learning(self, on: bool) -> None`.
  - `cq-drive serve --page-port N`: 0 turns the feed off.
- [ ] **Step 1: Write the failing tests.**
  - `test_feed_publisher`:
    - `test_no_page_no_encoding` (a spy on `cam_jpeg`);
    - `test_frame_has_boxes_cam_and_depth`;
    - `test_status_about_10hz` (a fake clock: 1 s gives 9-11);
    - `test_why_and_things_only_on_change`;
    - `test_greeting_has_status_why_things`.
  - `test_skill_link::test_hello_carries_page`.
  - `test_skill_link::test_learning_sent`.
  - `test_perception::test_on_frame_called_with_depth_or_none`.
  - `test_serve::test_page_port_zero_no_feed`.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement and wire.
  - `ServeApp` gets `feed: FeedPublisher | None` and `server: FeedServer | None`. `run` starts `server.run` and `feed.run` beside the pump.
  - The runner's view is `TickFan([rerun_view, feed])`, leaving out any that are `None`.
  - Health is `perception.state(now) or "ok"`.
- [ ] **Step 4:** The full engine checks pass.
- [ ] **Step 5: Commit** `feat(ai): cq-drive serve feeds /ai pages on port 8770 (--page-port) (AI engine LLD §22.1-§22.5)`.

### Task 8: `TaughtWords`, the model choice, and `export_world.py --extra --out`

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/taught_words.py`
- Modify:
  - `.gitignore`: add `ai-engine/taught/`;
  - `adapters/detector_config.py`: read `measured` and `taught`;
  - `app/objects.py`: `detectors()` takes a `Vocabulary`; one helper picks the model and vocabulary for every CLI;
  - `app/serve.py`, `app/drive.py`: use the helper;
  - `tools/measure/export_world.py`.
- Test: `ai-engine/tests/test_taught_words.py`

**Interfaces:**
- Produces:
  - `class TaughtWords`:
    - `__init__(self, folder: Path)`
    - `words(self) -> list[Word]`
    - `add(self, word: str, names: Sequence[str], today: str) -> None`: written as `{word, floor: true, names, measured: false, taught: today}`.
    - `remove(self, word: str) -> None`
    - `vocab_file: Path` (property).
  - `pick_words(config_vocab: Path, models_dir: Path, taught_dir: Path) -> tuple[Path, Vocabulary]`:
    - if `taught/words.txt` equals the merged vocabulary's `prompts` (one per line) and the taught model exists → `(taught model, merged)`;
    - otherwise → `(models/yoloworld-s-640.mlpackage, measured only)`.
    - The pair always agrees, so `check_model` never sees a mismatch.
  - `export_world.py`:
    - `--extra PATH` adds that file's words after the measured ones;
    - `--out DIR` writes `DIR/yoloworld-s-640.mlpackage` and `DIR/words.txt` instead of `ai-engine/models/`;
    - without `--out`, it behaves as today.
- [ ] **Step 1: Write the failing tests** (`test_taught_words`):
  - `test_add_remove_round_trip`;
  - `test_pick_taught_when_words_match`;
  - `test_pick_measured_when_words_txt_stale`;
  - `test_pick_measured_when_no_taught_model`;
  - `test_taught_words_not_measured`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement. `export_world.py` is not unit-tested (it downloads weights). Run it by hand once with `--extra` (a temp YAML holding `cup`) and `--out` a temp dir, and record the time and the output in the ledger.
- [ ] **Step 4:** The full engine checks pass; `cq-objects`, `cq-drive` and `cq-drive serve` start (`--help`).
- [ ] **Step 5: Commit** `feat(ai): taught words beside the measured ones, one model choice for every CLI, export_world --extra --out (AI engine LLD §22.4)`.

### Task 9: `Teacher` — teach a word from the page

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/teach.py`
- Modify:
  - `app/serve.py`: `_on_teach` answers the feed's `teach` requests; `_on_start` answers `skill_failed(id, "learning", thing)` while a teach runs; `apply_words` sets everything a word list touches;
  - `app/world.py`: `set_floor_classes`;
  - `app/perception.py`: `reload_detector`.
- Test: `ai-engine/tests/test_teach.py`, `test_serve.py`

**Interfaces:**
- Consumes: `TaughtWords` and `pick_words` (Task 8), `SkillLink.learning` (Task 7), `FeedServer.requests` / `reply` (Task 6).
- Produces:
  - `@dataclass(frozen=True) class Reply: ok: bool; msg: str`
  - `class Teacher`:
    - `__init__(self, taught: TaughtWords, export: Callable[[Path, Path], Awaitable[int]], *, limit_s: float = 200.0)`. `export(extra_yaml, out_dir)` returns the exit code; in production it is `run_export`.
    - `async teach(self, word: str, names: Sequence[str], known: Callable[[str], bool], busy: bool) -> Reply`
    - `learning: str | None`: the word being learnt.
  - `async run_export(extra: Path, out: Path) -> int`:
    - `asyncio.create_subprocess_exec("uv", "run", "--project", "tools/measure", "python", "tools/measure/export_world.py", "--extra", str(extra), "--out", str(out), cwd=<repo root>, start_new_session=True)`;
    - on cancel or timeout, it kills the process group (`os.killpg`) and waits.
  - `ServeApp.apply_words(self, vocab: Vocabulary, model: Path) -> None`:
    - builds the detector off the loop (`asyncio.to_thread`) and swaps it with `perception.reload_detector(det)` between two pictures;
    - rebuilds `self.names` and sets `world.set_floor_classes(...)`;
    - sets `link.things = names.wire()` (sent again on every reconnect) and awaits `link.set_things(...)`;
    - sends `things` to the pages.
- **Replies, verbatim:**
  - `ok` "I know {word} now." / "I already know {word}.";
  - not ok:
    - "Use letters, spaces and - only." / "That word is too long." / "At most 8 names." / "I never look for people.";
    - "I'm busy - try when I've stopped." / "I'm already learning a word.";
    - "Making the new model failed - I kept the old one." / "Making the new model took too long - I kept the old one."
- **Order inside `teach`:**
  1. Check the word.
  2. Known (a word or a name) → ok "I already know …", with no export.
  3. Busy → refused.
  4. `learning on` to the bridge, then `TaughtWords.add`.
  5. Export into a temp dir.
  6. On success: move into `taught/` (replacing the old model), `pick_words`, `apply_words`.
  7. `learning off`, then reply.
  - On any failure or cancel: `remove(word)`, the old model kept, `learning off`.
- [ ] **Step 1: Write the failing tests** (the export is a fake coroutine):
  - `test_bad_word_refused` (`"cup!"`, 25 bytes, 9 names, `person`);
  - `test_known_word_ok_no_export`;
  - `test_refused_while_busy`;
  - `test_success_swaps_and_learning_on_off` (the order of the link calls recorded);
  - `test_failed_export_removes_entry_keeps_model`;
  - `test_timeout_kept_old_model` (`limit_s = 0.1`);
  - `test_cancel_removes_entry` (cancel the task during export);
  - `test_serve::test_start_while_learning_fails_learning`;
  - `test_serve::test_apply_words_updates_names_floor_things_and_pages`;
  - `test_run_export_kills_group_on_cancel`: the command replaced by `sleep 30`, then the pid checked gone.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** The full engine checks pass.
- [ ] **Step 5: Commit** `feat(ai): teach the tank a word from the /ai page (AI engine LLD §22.4)`.

### Task 10: The page's deciding code — `web/ai.js`

**Files:**
- Modify: `firmware/esp32/web/ai.js` (from the Task 4 stub)
- Create: `firmware/esp32/web/test/ai.test.js`

**Interfaces:**
- Produces (CommonJS, like `logo.js`; the page calls the same functions):
  - `splitFrame(buf: ArrayBuffer) -> {header, cam: Uint8Array, depth: Uint8Array|null}`: reads the big-endian length; throws on a short buffer.
  - `findLine(thing, areaOn, area_m) -> string`: `AREA 2  FIND "toy` (two spaces), or `FIND "toy` with the area off. `wanderLine(s)` gives `WANDER 30`; `exploreLine()` gives `EXPLORE`.
  - `typed(text, things) -> {kind: "run", line} | {kind: "unknown", word}`: a word or name from the things list runs as `FIND`; anything else is unknown ("I don't know what a … is").
  - `teachCheck(word, names) -> string|null`: the same rules and messages as the engine.
  - `class Sayer { constructor(speak); say(text); mute(on); unlock() }`: each new sentence is spoken once; nothing is spoken before `unlock()` or while muted.
  - `cameraState(state, event) -> state`: the R27.4 diagram. Events are `ai_at_url`, `ai_at_null`, `feed_open`, `feed_closed` and `stream_ok`.
  - `protoOk(n) -> bool`: true for 1 only; otherwise the page shows "Please update the AI helper."
- [ ] **Step 1: Write the failing tests** (`ai.test.js`, one `test()` per item above), including:
  - a split of a frame built with the engine's layout (bytes written in the test);
  - `findLine("toy", false, 2) === 'FIND "toy'`;
  - `Sayer` silent before `unlock`;
  - `NoAI → TankStream → Engine` and `Engine → NoAI` on `feed_closed`.
- [ ] **Step 2:** `node --test firmware/esp32/web/test/ai.test.js`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(web): the /ai page's deciding code with tests (bridge LLD R27.4)`.

### Task 11: The `/ai` page — frame, Play tab, teach, and the menus

**Files:**
- Modify:
  - `firmware/esp32/web/ai.html` (from the stub);
  - `firmware/esp32/web/basic_drive.html`, `settings.html`, `logo.html`: the ☰ menu's **AI** item.
- Create: `firmware/esp32/web/test/ai_check.js` (Playwright `run_code`)
- Modify: `firmware/esp32/web/test/fake_tank.py` (serve `/ai`)

**Interfaces:**
- Consumes: `ai.js` (Task 10); the tank's `/ws` (`ai_at`, telemetry, `logo_run` results); the feed (Task 7).
- **Decisions:**
  - Look and frame from `docs/design/ai-page-mockup.dc.html`: the Phosphor Cockpit tokens, corner-bracket cards, STOP top right on every tab, the six status chips, a segmented tab bar. The Room and Inside tabs show "coming soon". Icons come only from `web/icons.json` markers.
  - The **AI** menu item always goes to `/ai`. The Settings "AI address" (`ai_url`) no longer drives the menu and is left in place, unused, until the owner rules (see design-notes §200).
  - The feed: `binaryType = "arraybuffer"`; on close it retries every 2 s while `ai_at` has a URL. A proto mismatch shows "Please update the AI helper." and stops retrying.
  - Each frame is decoded once (`createImageBitmap`) and drawn on two canvases:
    - view 1: the picture with boxes and words;
    - view 2: the picture with the depth at 60 % opacity over it.
  - Without the engine (state `TankStream`), view 1 shows `http://<tank>:81/stream` in an `<img>`, with no boxes.
  - The voice is unlocked on the first tap anywhere; mute is a toggle remembered in `localStorage`, inside try/catch.
  - Teach: the typed word gets "I don't know what a … is" and **teach it**. That opens word, names (comma-separated) and confirm, and is disabled while this page's program runs. The answer is shown. A taught word in the picker is marked "I'll try - I've never seen one".
  - Take a picture: view 1's canvas saved as `tank-<time>.jpg` on the phone.
- [ ] **Step 1: Write the browser check** (`ai_check.js`). Fake `WebSocket` by URL: `/ws` answers hello with `welcome` and `ai_at`; `:8770/feed` sends hello, things, status and one binary frame. It asserts:
  - both canvases are drawn;
  - the status line shows the `say`;
  - Find sends `logo_run` with `AREA 2  FIND "toy`;
  - STOP sends `stop` on `/ws`, never on the feed;
  - typing `cup` shows the teach button, and teach sends `{t:"teach",word:"cup"}`;
  - after the feed closes, "AI helper not running" shows and STOP still works;
  - at 390 px wide, no horizontal scroll.
- [ ] **Step 2:** Run it against `fake_tank.py`. Expected: FAIL (stub page).
- [ ] **Step 3:** Build the page and the menu items.
- [ ] **Step 4:** The check passes; `node --test` passes; `pio run -e bridge` builds and the `/ai` size is within its budget (the number goes in the ledger).
- [ ] **Step 5: Commit** `feat(web): the /ai page - camera with boxes and depth, Play, teach a word; AI in every menu (bridge LLD R27.4)`.

### Task 12: Stand checks (owner present)

Say what moves before each step. **Stand**: tracks off the ground.

- [ ] **Step 1:** Flash the bridge from yantra (`/dev/ttyACM0`; the Uno is unchanged). On the laptop, start `cd ai-engine && uv run cq-drive serve`. macOS asks once to allow incoming connections: the owner clicks Allow.
- [ ] **Step 2:** On the owner's phone, open ☰ → AI. Check that the page shows the engine's camera, both views move together, and the boxes sit on the things. Record the frame rate the page shows.
- [ ] **Step 3:** STOP from the page while the feed runs: the ack comes as today. Then press Find with the tank on the stand: the tracks move, and STOP from the page ends it at once.
- [ ] **Step 4:** Turn the laptop's WiFi off. The page keeps STOP and telemetry and shows "AI helper not running". Turn the WiFi back on: the feed comes back by itself.
- [ ] **Step 5:** Teach `cup` (names: mug). The page and the drive page say 7056 during the export. Afterwards "I know cup now." and `cup` is in the picker. Record the export time.
- [ ] **Step 6:** Clean up. Delete any saved pictures. Remove `ai-engine/taught/` if the owner does not want to keep `cup`. Write the results in design-notes.

### Task 13: As-built docs

- [ ] Mark R27 and §22 as built (Z2b-1) and update CLAUDE.md's plan list. Write the stand-check results and the measured numbers (frame rate, export time, the `/ai` gzipped size) into design-notes. Commit with `docs: Plan Z2b-1 as built`.
