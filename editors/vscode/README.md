# Lovax for VS Code

Syntax highlighting for the [Lovax](https://uixova.github.io/lovax/) programming
language — a simple, fast, zero-dependency language with its own JIT.

## Features

- Full syntax highlighting for `.lov` files: keywords, strings with `{}`
  interpolation, `#` comments, numbers (int / hex / binary / float), operators
  (including `//` floor division and `?? ?.` null-safety), builtins, and
  `struct` / `enum` / `fn` names.
- UTF-8 identifiers highlight correctly (`oyuncu_adı` is first-class in Lovax).
- Comment toggling, bracket matching, and indent-on-`:` for blocks.

## Install

From a packaged `.vsix`:

```
code --install-extension lovax-1.0.0.vsix
```

Or copy this folder into your VS Code extensions directory
(`~/.vscode/extensions/`) and reload.

## About

Lovax reads like Python and runs like native code. Docs: https://uixova.github.io/lovax/
