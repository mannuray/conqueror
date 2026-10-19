# Tank Logo conformance tests

Every program that reads Tank Logo (the ESP32 reader in v1; anything later) must pass these cases. Grammar: `../grammar.md`, words: `../words.yaml`.

## Format
Each `*.yaml` file is a list of cases:

```yaml
- name: short description
  source: |
    the program text
  expect: ok                      # or an error, see below
  reads: (FD 50) (RT 90)          # how the program is read (ok cases)
  your_words: {SQUARE: {inputs: 1, gives_value: false}}   # optional: what pass 1 found
  handles_obstacles: false        # optional: the program uses BLOCKED? (design-notes §82 h)
  tail_calls: [COUNTDOWN]         # optional: the words whose calls in tail position reuse the frame (grammar.md §8)
  version: v1.1                   # optional: the case applies from that version (default v1)
  check: false                    # optional: only the reading is tested (the checker is not run)
```

Error cases:

```yaml
  expect: {error: 7004, line: 1, col: 7}   # the FIRST error; line and column are 1-based, the column of the token's first character
  message_has: "10 - 5"                    # optional: a piece of the friendly message
```

## The `reads` notation
A program is read as a list of statements; each is written in prefix form inside `( )`:
- a word and its inputs: `(FD 50)`, `(ARC RIGHT 90 50)` — preset inputs are written bare;
- operators: `(+ :n 5)`, `(= (RANDOM 2) 1)`, `(AND a b)`, `(NOT a)`, a negative number `-5`;
- a block: `[ (FD 50) (RT 90) ]`; data lists: `{1 2 3}` (so they cannot be mixed up with blocks); a quoted word: `"cat`;
- a definition: `(TO SQUARE (:size) [ … ])`;
- `WAIT UNTIL` is written `(WAIT-UNTIL …)`.

Whitespace in `reads` does not matter. Words are written in upper case and aliases as their full name (`FD` → `FORWARD`).

## Run tests (`running.yaml`)
What a program asks the tank to do, in order, and how it ends. Any runner must give the same list.

```yaml
- name: short description
  source: |
    the program text
  world: {distance: 20, cut_short: [1], stuck: [2]}   # optional, see below
  does: [FORWARD 50, RIGHT 90]                         # the actions, in order ([] for none)
  ends: finished                                       # or {error: 7024, line: 1}
  message_has: "in the way"                            # optional: a piece of the ending's message
  version: v1.1                                        # optional (default v1)
```

**The world** the program runs in:
- `distance` — what the ultrasonic sees ahead, in cm, all the time (default: nothing, so `DISTANCE` is 999 and `OBSTACLE?` is false; `OBSTACLE?` is true below 15 cm).
- `cut_short` — the motions (1 = the first move, turn, arc, pivot or `HOME`) that the obstacle brake cuts short.
- `stuck` — the motions that end with a stuck track.
- Every other motion ends exactly where it should, at once; turns change `HEADING` exactly; waits pass at once (the clock jumps); a `WAIT UNTIL` that never comes true gives up after 60 s and the program goes on. The tank starts with `SPEED 40` and `HEADING` 0. Run tests do not use `RANDOM` (its numbers are each runner's own).

**Actions** (Logo terms: cm, degrees, right = +):

| Action | Written |
|---|---|
| move | `FORWARD 50`, `BACK 3` (whole cm) |
| turn | `RIGHT 90`, `LEFT 45.5` |
| arc / pivot | `ARC RIGHT 90 50`, `PIVOT LEFT 45` |
| `HOME` | `HOME` |
| `SPEED` | `SPEED 60` |
| `WAIT` | `WAIT 1.5` |
| `LED` | `LED 0 255 0` (r g b) |
| `BEEP` / `SOUND` | `BEEP`, `SOUND HORN` |
| `PLAY` | `PLAY 262/250 330/500 0/250` (Hz/ms per note, 0 = rest) |
| `LOOK` | `LOOK 0 30` (pan tilt in degrees from the centre, right / up = +), `LOOK HOME` |
| `SAY` | `SAY well done` |
| `WAIT UNTIL` | `WAIT-UNTIL` (once, when it starts) |

**Endings:** `finished`, or the code and line of `words.yaml` `errors` (7024 obstacle, 7040 `STOP`, 7041 time limit, 7044 stuck, run-time errors …).
