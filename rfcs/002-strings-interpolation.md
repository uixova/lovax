# RFC-002: String Conversion and Interpolation

**Status:** ACCEPTED (implemented)

## Motivation
`"hp: " + 5` is "hp: 5" in JavaScript, a TypeError in Python, an error in Lua.
Implicit conversion produces surprises ("5" + 5 = ?) — but building text must stay painless.

## Decision
1. **No implicit conversion**: `"a" + 5` is an error whose message suggests the fix
   (use `text()` or interpolation).
2. **`text(x)`** converts any value to a string (Lovax-flavored name).
3. **Interpolation**: `"hp: {hp}, double: {hp * 2}"` — full expressions inside braces
   (inner strings, indexing, calls included: `"{m["k"]}"`). Literal braces: `\{` and `\}`.
4. Escapes: `\n \t \r \\ \" \{ \}` — unknown escapes are syntax errors.
5. `"ab" * 3` is string repetition (the only implicit number-string operation; unambiguous).
6. Multiline strings use `"""..."""` and support the same escapes/interpolation.

## Implementation note
The lexer captures strings RAW (it tracks `{...}` regions so inner quotes don't terminate);
escapes + interpolation are processed by the parser in one pass. Inner expressions are parsed
with a fresh mini lexer+parser.
