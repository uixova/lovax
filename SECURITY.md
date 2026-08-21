# Security Policy

## Supported Versions

Lovax is under active development; security fixes land on the latest release
(currently the `v1.x` line) and `main`.

| Version | Supported |
|---------|-----------|
| latest (v1.x) | ✅ |
| older         | ❌ |

## Reporting a Vulnerability

**Please do not open a public issue for security problems.**

Report a vulnerability privately through GitHub's
[private vulnerability reporting](https://github.com/uixova/lovax/security/advisories/new)
on this repository. Include:

- a description of the issue and its impact,
- the smallest input or steps that reproduce it,
- the affected version or commit.

Because the interpreter parses and runs untrusted source and exposes a capability
sandbox, reports about sandbox escapes, memory-safety issues in the VM/JIT, or
crashes on adversarial input are especially welcome. You can expect an initial
response within a few days.
