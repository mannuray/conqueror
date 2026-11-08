# Spec 10 — the people games: the baby game first, the intelligence in Logo (approved 2026-11-08)

Owner decisions: design-notes §223 (2026-11-08). Earlier: §194 (the people games stored), §214 (the baby game in the
owner's words: "first always face the baby except for escape; if the baby crawls or walks towards you, escape; if the
baby goes away from you, follow"; "let's find the small one"), spec 4 §5 and design-notes §27 (child play, keep away).

## 1. Intent

A crawling baby plays chase with the tank: the tank keeps facing the baby; when the baby crawls at it, it scoots
away; when the baby crawls off, it comes after them - never close. It is the project's first game with a person in it,
and the first built on the owner's principle that **the intelligence lives in Logo**: the game is a ready-made Tank
Logo program a parent can read, change and save; the engine gives it senses and safe moves as words; safety sits
underneath every word, where no program can reach it.

Why now: the drive-while-thinking engine (specs 8-9) can see, map, route and move safely; Tank Logo can call it (the
AI words, Plan Z2). What is missing is (a) seeing a person on purpose, (b) moves that keep a distance from a person,
(c) the child-play limits switched on end to end, and (d) a way to write behaviour in Logo instead of in the engine.

Success: an adult crawling on the floor (the stand-in, owner) plays the game for 5 minutes in the owner's room
inside a play area: the tank faces them, escapes when they come, follows when they go, never comes within 30 cm, stops
and beeps when caught, and the whole game is a Logo program on `/logo` that the owner can change (distances, length)
and run again.

## 2. Goals, non-goals, anti-goals

**Goals**
- G1. The engine can see and range a person on the floor (crawling) at 0.3-2 m from the tank's camera - measured
  first, with an adult crawling (§5 phase 0). If it cannot, the game is not built on it.
- G2. Game words (level 3, `needs: [ai]`): keep a person in view, escape from them, follow at a distance, how far,
  caught? - each one move done well and safely, so the game's decisions can be Logo.
- G3. Safety underneath (§4): a 30 cm keep-out around the person, caught = all motors stop + a beep, the child-play
  speed limits on for the whole game - in the engine / bridge / Uno, not in the program.
- G4. The baby game as a ready-made, configurable Logo program (a lesson / prebuilt program on `/logo`, startable
  from `/ai`).
- G5. The words layer is shaped so FIND, WANDER and EXPLORE can later become prebuilt Logo programs too (§6, a later
  spec; not built here).

**Non-goals (this spec)**
- Rebuilding FIND / WANDER / EXPLORE in Logo (next, own design - §6).
- "Follow me, friend" for a bigger child (later, its own small spec; reuses FOLLOW).
- Recognising *which* person, faces, or names; talking; pets.
- Nudging or touching anything.

**Anti-goals (a failure even if it "works")**
- The tank ever driving **at** a person, or closer than 30 cm, for any program, setting or bug in a program.
- A Logo program being able to switch off any safety: the 30 cm keep-out, caught, STOP, the obstacle stop, the
  child-play limits, the deadman / link-loss stops.
- The person exception leaking: FIND, GO, SETXY, a route goal, the thing memory or the ROOM map ever targeting or
  remembering a person.
- Backing blindly into the unknown.
- A game that frightens: fast starts, sudden spins, loud sounds.

## 3. What the owner decided (verbatim where possible)

- **Who:** a crawling baby. ("for crawling baby, for a bigger baby it will be good as follow me friend")
- **The loop:** always face the baby; the baby comes towards the tank -> escape; the baby goes away -> follow.
- **Escaping:** "our route plan can come into the picture where it knows the room, and it should be able to drive
  backward by trust; if untrusted, turn to check." The room map and the route planner pick where; back away while
  facing the baby when the floor behind is trusted, otherwise turn and look first.
- **30 cm:** the closest the tank may ever be to the baby.
- **Caught:** "stop all motors and play a beep sound."
- **Configurable:** "game should be configurable."
- **In Logo:** "yes, in fact all our intelligence should be done by logo"; FIND, WANDER and EXPLORE "should also be
  logo programs, and come as prebuilt programs."
- **Order:** the words layer and the baby game together first; FIND / WANDER / EXPLORE after.
- **Testing:** with an adult crawling (not the baby) until the owner decides otherwise.

## 4. Safety underneath (fixed - no setting, no program can change it)

- **S1. Child-play limits on for the whole game.** The Uno's envelope (speed cap, gentle acceleration, `ENVELOPE`
  command, Uno Lite LLD) is **set when a game word first moves the tank and cleared when the program ends or STOP**.
  Today nothing sends `ENVELOPE` (bridge LLD R2: "child-play mode arrives with the phase-4 gateway") - this spec wires
  it. **Cap (owner, 2026-11-08): 25 cm/s (~75 % of the measured ~33.5 cm/s top) with a gentle 10 cm/s² ramp.**
  Spec 4 §5's "≤ 20 %" (~7 cm/s) cannot move this tank (its slowest smooth speed is ~18 cm/s, design-notes §69 /
  `Limits.v_min_cms`) - it is replaced for this tank. A crawler (~20-40 cm/s) is about as fast as the tank: escapes
  work by starting early, and a determined crawler sometimes catches it - a safe stop and a beep (S3), part of the
  game.
- **S2. 30 cm keep-out.** No game word ever drives so that its predicted path brings the tank's nearest edge within
  30 cm of the person's last estimate (with its uncertainty). Any word's motion is cut when the person is estimated
  inside 30 cm. Inside ~50 cm the camera and the sonar are unreliable (the sonar's blind zone is ~25 cm; a person
  filling the picture is hard for the detector), so there the last good estimate, the sonar and "towards me" are
  trusted pessimistically: unsure = closer.
- **S3. Caught.** The person within 30 cm, **or** the tank grabbed (the Uno's stall / lift / tilt detection, already
  built), **or** pushing but not moving (bridge R21) -> **all motors stop** (drive and camera) and **a beep**. The
  program is told (a `CAUGHT?` reporter / an event), and nothing moves until the person is out beyond the escape
  distance again and the program moves on (a configurable pause, §5 G4).
- **S4. Never towards.** Game words never route to a person's spot; FOLLOW's goal is a point the follow distance
  short of them, on the tank's side, and never inside 30 cm whatever the setting.
- **S5. The person exception is narrow.** People are seen by a separate path used only by the game words. The
  existing rules stay as they are for everything else: the detector boxes over people are dropped before the thing
  tracker (`person_guard`), the thing memory never keeps a person (`memory.NEVER`), names refuse person words
  (`names.NEVER`, `teach.PERSON_WORDS`), `FIND "person` is an error.
- **S6. Everything that already stops the tank still does:** STOP from any page, the obstacle stop (sonar 10 cm),
  the deadman (300 ms), link loss, battery critical, the drive lease (engine killed -> 7052).
- **S7. Never backs into the unknown.** Backing only over floor that is trusted (§5 G2: seen free recently, near,
  no marks since); otherwise the word turns to look first.

- **S8. Every move during a game, not only the game words (builder check, answered by the anti-goals).** From the
  first person word of a program until the program ends or STOP, the program is in **game mode**: every motion it
  makes - `FORWARD`, `GO`, `SETXY`, `LAUNCH` parts, the game words - passes the keep-out (S2) and the child-play
  limits (S1). The phone's stick does not drive during a program (the lease, today); STOP always works.
- **S9. After a catch (my call).** The tank stays still until the person is beyond a **fixed release distance (50 cm)**
  for 2 s - not a program input; then the program's own pause runs. A person sitting right beside it keeps it still
  (safe); the game's time or STOP ends it. `ESCAPE` cannot move it out of a catch.
- **S10. Grabbed (my call).** The Uno's stall (a firmly held track, ~3 s, already built), the bridge's pushing but not
  moving (R21, ~2 s), and the engine's check that a commanded move gives no motion on the odometry and the gyro.
  The Uno is not changed (no flash left); its lift / tilt stop is a bonus while still.
- **S11. Cornered (my call).** No trusted floor behind and no open spot inside the AREA: the tank stays still and
  faces the person (`ESCAPE` reports "no way"; it may be caught - safe). It never turns its back to look while the
  person is inside the escape distance.
- **S12. The engine gone mid-game (my call).** The keep-out lives in the engine, so when the engine is lost (7052)
  during game mode the bridge **holds all motion** (like a catch) until a person presses STOP / starts again - ending
  the program alone is not enough.

## 5. How it is built (phases)

**Phase 0 - can it see a crawling person? (measure first, go / no-go).** An adult crawls at 0.3, 0.5, 1, 1.5 and 2 m
in front of the tank, ahead and 30° aside, still and moving, at the driving tilt and level. Measure: detected (how
often), the distance estimate against tape, the bearing, from YOLO's `person` and from YOLO-World asked
"baby" / "crawling person". Recording kept local (the room, a person). If ranging inside 1 m is not good enough for
S2, the design changes before anything moves (e.g. the camera tilted down for the game, the sonar as the near
sense) - back to the owner. **The bar (owner, 2026-11-08):** seen in ≥ 8 of 10 pictures at 0.5-2 m (still and
crawling); the distance within ±10 cm inside 1 m and ±20 % beyond; the direction within 5°; inside 50 cm the sonar
sees them ahead. Below it: back to the owner before anything moves.

**Phase 1 - the person path and the words (engine + Tank Logo).**
- A `PersonWatch` beside the thing tracker: the person boxes the guard drops for everything else go here only;
  per picture a person estimate (position in the room, distance from the tank's front edge, bearing, moving towards /
  away, how sure, age). Never on the map, never in memory.
- Game words (names are proposals - the builder may refine; levels and `needs` as the AI words):
  - `FACE "baby` - keep the person in view for a moment: the camera follows; beyond the camera's range the tank turns
    on the spot. One call = one short look (≤ ~1 s), so a program loops it.
  - `ESCAPE "baby cm` - move about `cm` away from the person to open, trusted floor picked with the room map and the
    route planner: back away facing them if the way is trusted (S7), else turn to look first; ends facing them.
  - `FOLLOW "baby seconds` (reserved already, level 3) - come after the person, stopping at the follow distance
    (a setting, never inside 30 cm), for up to `seconds`.
  - `HOWFAR "baby` / `SEE? "baby` - as today's words, answering for the person from `PersonWatch` (today they refuse
    persons; the exception is only for the game's person words).
  - `CAUGHT?` - true while caught (S3).
  - Which person words count as "the game's person" (`"baby`, `"person`, …) - the builder decides with the owner's
    loose-names rule (Plan Z2b).
- **"Trusted floor"** for backing (owner's word): seen free from near within the last few seconds, no marks since
  - the builder sets the numbers from the map's existing settings and documents them.

**Phase 2 - safety underneath (S1-S4)** in the engine (the keep-out, never towards), the bridge (the envelope set /
cleared with the lease, caught -> stop + beep, the program told) and the Uno (unchanged; its stall / lift / tilt and
envelope are reused). Tested with the stand-in on a play area, the tank on the floor.

**Phase 3 - the game as a Logo program.** A ready-made `BABYGAME` (a lesson / prebuilt program on `/logo`, a button
on `/ai`'s Play tab), its settings as the procedure's inputs (Tank Logo has no globals): escape at (default ~70 cm),
follow stops at (~1 m), escape length (30-60 cm), game length (minutes), the pause after caught, sounds on / off.
The play area as today (`AREA`). Losing the person: the program stops and looks round (FACE), never drives about
searching. Example shape (the builder writes the real one):

```
TO BABYGAME :escape_at :follow_at :minutes
  REPEAT :minutes * 60 [
    IFELSE CAUGHT? [ WAIT 3 ] [
      IFELSE HOWFAR "baby < :escape_at [ ESCAPE "baby 40 ] [
        IF HOWFAR "baby > :follow_at [ FOLLOW "baby 2 ] ]
      FACE "baby ] ]
END
```

## 6. Later: FIND, WANDER and EXPLORE as prebuilt Logo programs

Not built here; this spec only keeps the words layer ready for it. Today they are whole strategies in the engine
(`RoamSkill`: look round, search, candidates, chase, recovery). Later they become prebuilt Logo programs over smaller
engine words (e.g. look round, the next spot to search, go to a spot, check a maybe) - their own design, and they
replace the engine skills **only once they match them on the floor and in `--replay`**.

## 7. Constraints

- Every hard rule of CLAUDE.md: design before code (this spec -> UX -> LLD with class / sequence diagrams -> plan),
  generated files only from `logo/spec` / `protocol/defs`, Python strict, never weaken safety.
- The Logo words go through the skill server and drive lease (Plan Z2a); a question to the engine answers within
  ~300 ms - fine for a crawler (~0.2-0.4 m/s); the fast safety (S2, S3) runs per picture in the engine and on the
  bridge, not in the program's loop.
- The camera and sonar face forward only; the tank is blind behind (S7).
- Flash: the Uno has 654 B left - nothing new on the Uno (it already has the envelope, stall, lift and tilt).
- Pictures with people stay on the owner's machine; nothing is committed.

## 8. Left to the builder

The exact word names and inputs, `PersonWatch`'s numbers, how "towards / away" is judged, the trusted-floor numbers,
how the bridge carries the envelope and the caught event, the beep, the LLD sections and their diagrams, the lesson's
text - each documented as the builder's call, with the owner shown the safety numbers before anything moves.

## 9. Open (guesses - the owner to confirm in the UX / LLD)

Escape at ~70 cm; follow stops ~1 m; escape 30-60 cm; inside ~50 cm pessimistic; "trusted" = seen free within a few
seconds. (Decided since: the speed cap and the phase 0 bar - owner; the release distance, grabbed, cornered and the
engine lost - S9-S12, my calls for the owner to check.)
