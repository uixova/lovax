# RFC-019 — datetime: calendar time

A datetime is a **plain map** `{year, month, day, hour, minute, second,
weekday, timestamp}` — dot-readable (`dt.year`), printable, JSON-saveable.
No new object type needed.

API (`use datetime`): `now`, `now_local`, `from_timestamp`, `make(y,m,d[,h,mi,s])`
(validated, leap years), `add(dt, days, hours, minutes, seconds)`, `diff(a, b)`
→ seconds, `format(dt, fmt)` / `parse(text, fmt)` with `%Y %m %d %H %M %S %w`,
`weekday` (Monday = 0).

**Determinism:** all arithmetic is the standard civil-calendar algorithm
(days-from-civil / civil-from-days) over the UTC timestamp — no libc timezone
tables, identical output on every platform. Local time appears only in
`now_local()`. Timezone database support is explicitly out of scope (UTC-only
policy, decided v0.11-era).
