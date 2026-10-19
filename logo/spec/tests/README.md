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
