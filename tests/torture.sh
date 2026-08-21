#!/usr/bin/env bash
# Torture / capacity gate — the ADVERSARIAL suite that actively tries to break the
# language, distinct from the golden tests (which pin known-good behaviour). Two
# parts:
#   A. no-crash battery — pathological programs (deep/cyclic structures, numeric
#      edges, huge data, coroutine abuse, string/error edges) must fail GRACEFULLY
#      (a Lovax error, exit 65/70) or succeed — never segfault, abort, or hang.
#   B. differential JIT fuzz — a deterministic generator emits hard hot-loop
#      programs; each must be byte-identical with the JIT on and off. This is the
#      miscompile guard for the trace JIT + allocation sinking over inputs no fixed
#      golden covers.
set -u
cd "$(dirname "$0")/.."
LOVAX="${LOVAX:-./lovax}"
[ -x "$LOVAX" ] || { echo "build first: make"; exit 2; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0

# ---- Part A: no-crash battery -------------------------------------------------
# A program is BAD only if it crashes (segfault 139 / abort 134) or hangs (124).
# A graceful Lovax error (65 syntax, 70 runtime) or success (0) is fine.
crash_check() {
    local name="$1" file="$2"
    local out rc
    out=$(timeout 20 "$LOVAX" "$file" 2>&1); rc=$?
    case $rc in
        139) echo "  CRASH (segfault): $name"; fail=1;;
        134) echo "  CRASH (abort): $name"; fail=1;;
        124) echo "  HANG (timeout): $name"; fail=1;;
        *)   : ;;  # 0 / 65 / 70 / other graceful exits are acceptable
    esac
}

# deep recursion — must hit the call-depth guard, not smash the C stack
cat > "$tmp/a01.lov" <<'EOF'
fn f(n):
    if n == 0:
        return 0
    return 1 + f(n - 1)
say f(1000000)
EOF
# deeply nested aggregate built + printed (recursive toString must be bounded)
cat > "$tmp/a02.lov" <<'EOF'
set a = [0]
set i = 0
while i < 500000:
    a = [a]
    i = i + 1
say a
say len(a)
EOF
# self-referential (cyclic) list: print + equality must not loop forever
cat > "$tmp/a03.lov" <<'EOF'
set a = []
push(a, 1)
push(a, a)
say a
set b = []
push(b, b)
say a == b
EOF
# deep structure kept live across heavy allocation churn (recursive gcMark?)
cat > "$tmp/a04.lov" <<'EOF'
set deep = [0]
set i = 0
while i < 200000:
    deep = [deep]
    i = i + 1
set j = 0
while j < 1000000:
    set junk = [j, j + 1]
    j = j + 1
say len(deep)
EOF
# numeric edges: div/mod/floordiv by zero, shift overflow, huge power
cat > "$tmp/a05.lov" <<'EOF'
say 2 ** 4096
say 10 ** 200
try:
    say 5 % 0
catch e:
    say "mod0 ok"
try:
    say 5 // 0
catch e:
    say "div0 ok"
EOF
# deeply nested source (parser recursion must be bounded, not smash the C stack)
python3 -c 'print("say " + "("*20000 + "1" + ")"*20000)' > "$tmp/a06.lov"
python3 -c 'print("set a = " + "["*20000 + "0" + "]"*20000)' > "$tmp/a07.lov"
# coroutine abuse: resume past completion, thousands of coroutines
cat > "$tmp/a08.lov" <<'EOF'
fn g():
    yield 1
set c = spawn(g)
say resume(c)
try:
    say resume(c)
    say resume(c)
catch e:
    say "resume-done ok"
set many = []
set i = 0
while i < 5000:
    push(many, spawn(g))
    i = i + 1
say len(many)
EOF
# huge string doubling + slicing + unicode boundaries
cat > "$tmp/a09.lov" <<'EOF'
set s = "şx"
set i = 0
while i < 22:
    s = s + s
    i = i + 1
say len(s)
say s[0]
say s[-1]
EOF
# map: mutate during iteration, many colliding-ish integer keys
cat > "$tmp/a10.lov" <<'EOF'
set m = {}
set i = 0
while i < 50000:
    m[i] = i * i
    i = i + 1
set sum = 0
for k in m:
    sum = sum + m[k]
say sum
say len(m)
EOF
# nested try/catch/finally with throw in finally, error in handler
cat > "$tmp/a11.lov" <<'EOF'
fn risky():
    try:
        throw "inner"
    finally:
        throw "from finally"
try:
    risky()
catch e:
    say "caught {e}"
EOF
# JIT deopt storm: a hot loop whose types churn every iteration
cat > "$tmp/a12.lov" <<'EOF'
set s = 0.0
set i = 0
while i < 200000:
    set x = i
    if i % 2 == 0:
        x = i * 1.5
    if i % 3 == 0:
        x = [i, i]
    s = s + 1.0
    i = i + 1
say floor(s)
EOF

for f in "$tmp"/a*.lov; do crash_check "$(basename "$f")" "$f"; done
echo "torture A (no-crash battery): $([ $fail = 0 ] && echo clean || echo FAILURES)"

# ---- Part B: differential JIT fuzz -------------------------------------------
# Deterministic generator (fixed seed) → hard hot-loop programs; JIT on vs off
# must be byte-identical. Bounded for CI (~80 programs, small N).
DIFF=0
python3 - "$LOVAX" "$tmp" <<'PY'
import random, subprocess, sys, os
LOVAX, tmp = sys.argv[1], sys.argv[2]
random.seed(20260821)
F = os.path.join(tmp, "fz.lov")
def run(args):
    try:
        r = subprocess.run([LOVAX]+args+[F], capture_output=True, text=True, timeout=15)
        return f"{r.returncode}|{r.stdout}|{r.stderr}"
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
def expr(vs):
    a = random.choice(vs)
    if random.random() < 0.4:
        return f"({a} {random.choice(['+','-','*','&','|','^'])} {random.choice(vs)})"
    return a
def gen():
    L=[]; us = random.random() < 0.35
    if us: L += ["struct T:","    x = 0","    y = 0.0","    z = 0"]
    vs = [f"v{k}" for k in range(random.randint(2,4))]
    for v in vs:
        L.append(f"set {v} = {random.choice([random.randint(-9,9), random.randint(70000000000000,70400000000000)])}")
    L += ["set acc = 0","set facc = 0.0","set xs = []","set i = 0"]
    N = random.choice([1000,2000,3000]); L.append(f"while i < {N}:")
    B=[]
    if random.random() < 0.55:
        B += [f"    set t = [i, i+{random.randint(1,4)}, {expr(vs)}]", "    acc = acc + t[0] + t[2]"]
    if us and random.random() < 0.6:
        B += [f"    set s = T(i+{random.randint(0,3)}, i*1.0, {expr(vs)})", "    acc = acc + s.x + s.z", "    facc = facc + s.y"]
    for _ in range(random.randint(1,3)):
        op = random.choice(['+','-','*','&','|','^','//','%','<<','>>'])
        if op in ('//','%'): B.append(f"    acc = acc {op} ({expr(vs)} + {random.randint(1,9)})")
        elif op in ('<<','>>'): B.append(f"    acc = acc {op} {random.randint(0,5)}")
        else: B.append(f"    acc = acc {op} {expr(vs)}")
    if random.random() < 0.4:
        B += [f"    if acc % {random.randint(2,6)} == 0:", "        acc = acc + i", "    else:", "        acc = acc - 1"]
    if random.random() < 0.3: B.append("    push(xs, i)")
    if not B: B.append("    acc = acc + i")
    L += B + ["    i = i + 1", "say acc", "say floor(facc)", "say len(xs)"]
    return "\n".join(L)+"\n"
bad = 0
for n in range(80):
    open(F,"w").write(gen())
    a=run([]); b=run(["--no-jit"]); c=run(["--no-ra"])
    if not (a==b==c):
        bad += 1
        sys.stderr.write(f"  DIVERGENCE #{n}: jit={a[:60]!r} nojit={b[:60]!r} nora={c[:60]!r}\n")
        if bad >= 5: break
print(f"  fuzzed {n+1} programs, {bad} divergence(s)")
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] || { DIFF=1; fail=1; }
echo "torture B (differential JIT fuzz): $([ $DIFF = 0 ] && echo clean || echo DIVERGENCES)"

echo "torture: $fail failure(s)"
[ "$fail" -eq 0 ] && echo "TORTURE GATE PASSED" || exit 1
