# RFC-015 — Networking and the capability sandbox

## Two features, one goal: make Lume safe to run other people's code

v0.10 opens Lume up beyond a single trusted script — packages, servers, tools.
That raises the question every general-purpose language must answer: **how do you
run code you didn't write without it stealing your files or phoning home?**

## Networking (`net`)

A `net` module of blocking TCP/UDP primitives, built on OS syscalls only (zero
third-party deps, like `file`/`os`):

```lume
use net
set server = net.tcp_listen(8080)
net.set_timeout(server, 5.0)          # blocking calls can't hang forever
set client = net.tcp_accept(server)
say net.recv(client, 1024)
net.send(client, "pong")
net.close(client)
```

- TCP: `tcp_listen` / `tcp_accept` / `tcp_connect` / `send` / `recv` / `close`.
- UDP: `udp_socket` / `udp_bind` / `udp_send` / `udp_recv`.
- `set_timeout(sock, seconds)` so a server loop is never wedged forever.
- A socket is an int handle; every function validates its args and returns a
  catchable error on failure — a misused socket never crashes the VM.

Enough to run a service on a VPS, do LAN multiplayer messaging, or write a small
client. (A full async server framework waits for the coroutine scheduler in the
engine, v1.0 — `net` is the primitive layer.)

## The malicious-code problem, and how others (don't) solve it

| Ecosystem | Defense | Reality |
|-----------|---------|---------|
| npm / pip | none (trust) | post-install scripts + transitive deps are a live supply-chain attack surface |
| Deno | capability permissions | code can't touch net/fs/env without an explicit `--allow-*` flag |
| Lua | host-defined sandbox | depends entirely on the embedder |

Lume takes **two layers**:

### Layer 1 — version pinning (RFC-007 phase 2)
`lume install user/repo@v1.2.0` clones exactly that tag and locks the resolved
commit SHA in `lume.lock`. A dependency can't silently change under you — the
thing npm/pip never gave you.

### Layer 2 — capability sandbox (this RFC)
Dangerous operations are gated behind permissions:

```
lume --sandbox --allow-net app.lm     # network yes, filesystem/env no
lume --allow-read report.lm           # mentioning a permission opts into the sandbox
lume app.lm                           # your own script: everything allowed
```

- `--sandbox` denies everything; `--allow-net/read/write/env/run` grant back
  exactly what's needed; `--allow-all` opens it up.
- **Mentioning any permission flag opts into the sandbox** (deny-all baseline,
  then grant) — Deno's ergonomics. With no flag at all, everything is allowed,
  because a script you wrote and ran yourself is code you already trust.
- Gated today: `use net` (allow-net), file reads (allow-read), file writes /
  delete / mkdir / rename (allow-write), `os.env` / `os.set_env` (allow-env).
  Denials are ordinary catchable errors — a program can ask forgiveness.

The sandbox is process-wide (matching Deno). Per-package permissions — "this
dependency may use net but not fs" — need the permission set threaded through
module boundaries and are a future refinement.

## Testing

`tests/sandbox.sh` asserts **both directions**: every capability is denied under
`--sandbox` and allowed once granted. A permission that only "usually" blocks is
worthless, so this runs in CI alongside the fuzz gate.
