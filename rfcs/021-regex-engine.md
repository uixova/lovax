# RFC-021 — regex: our own engine

`use regex` → `match` (anchored at start), `search`, `find_all`, `replace`
(with `$0`-`$9` group substitution), `split`, `groups`.

## Why not std::regex

Slow, recursion-based (stack overflow on adversarial patterns — would fail our
fuzz gate as a crash), and behavior/performance vary per standard library.
Rejected by decision before implementation.

## Engine

The classic backtracking bytecode VM (~450 lines, studied from the regex-VM
literature, written from scratch): patterns compile to
`CHAR / ANY / CLASS / SPLIT / JMP / SAVE / BOL / EOL / MATCH` instructions; a
recursive matcher executes them; greedy vs lazy is just SPLIT arm order.
Compiled patterns are cached (256 entries).

Supported: literals, `\d \D \w \W \s \S` (+ escapes), `.` (one UTF-8 code
point), `[a-z0-9]` classes with `^` negation, capturing and `(?:)` groups,
`|`, greedy/lazy `* + ? {m,n}`, `^ $`. v2: lookarounds, named groups,
backreferences, flags.

## Safety (the design center)

- **Step budget** (1M): catastrophic backtracking (`(a+)+$` on `aaaa…X`)
  returns a clean runtime error — never a hang. Golden-tested.
- **Program cap** (10k instructions): `{m,n}` expansion bombs die at compile.
- **Repetition bound** (1000) per quantifier.

## Language notes

- Keywords became valid member names after `.` (`regex.match` used to parse as
  the match statement).
- Unknown string escapes now pass through literally, so `"\d+"` reads
  naturally. Braces still belong to interpolation: write `\{2,5\}` (raw
  strings are a v2 candidate).
