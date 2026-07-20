#ifndef LOVAX_MODULE_REGEX_HPP
#define LOVAX_MODULE_REGEX_HPP

#include "common.hpp"

// regex — our own small engine (RFC-021). Zero dependencies; std::regex was
// rejected (slow, stack-overflows on adversarial patterns, behavior varies
// across standard libraries — it would break the fuzz gate).
//
// Design: a backtracking bytecode VM (the classic virtual-machine approach to
// regex matching — studied from the literature, written from scratch).
//  - Compile: pattern -> instructions {CHAR, ANY, CLASS, SPLIT, JMP, SAVE, ...}
//  - Match: recursive backtracking with a GLOBAL STEP BUDGET — a pathological
//    pattern (ReDoS) hits the budget and returns a clean error, never a hang.
//  - Program size is capped, so {1000}{1000} expansion bombs fail at compile.
//  - UTF-8: '.' consumes one code point; literals match byte sequences, so
//    Turkish text works; classes cover ASCII ranges.
//
// Supported: literals, escapes (\d \D \w \W \s \S \n \t \r and \<punct>),
// '.', classes [abc] [a-z0-9] [^...], groups (..) and (?:..), alternation |,
// quantifiers * + ? {m} {m,} {m,n} with lazy '?' variants, anchors ^ $.
// Not yet (v2): lookahead/lookbehind, named groups, backreferences, flags.

namespace Lovax {
namespace StdLib {
namespace Rx {

enum class Op : uint8_t { CHAR, ANY, CLASS, SPLIT, JMP, SAVE, BOL, EOL, MATCH };

struct Inst {
    Op op;
    unsigned char ch = 0;        // CHAR
    int x = 0, y = 0;            // SPLIT/JMP targets, SAVE slot
    int classIdx = -1;           // CLASS
};

struct CharClass {
    bool negate = false;
    std::vector<std::pair<unsigned char, unsigned char>> ranges;
    bool matches(unsigned char c) const {
        bool in = false;
        for (auto& r : ranges) {
            if (c >= r.first && c <= r.second) { in = true; break; }
        }
        return negate ? !in : in;
    }
};

constexpr int MAX_PROG = 10000;      // caps {m,n} expansion bombs
constexpr int MAX_GROUPS = 10;       // $0 (whole match) + $1..$9
constexpr long long MAX_STEPS = 1000000; // ReDoS budget per top-level call
// Recursion-depth budget: the matcher recurses per SPLIT/SAVE, so a zero-width
// loop like (a*)*  recurses without bound and overflows the C++ stack BEFORE the
// step budget bites (a SIGSEGV, not a clean error). This cap turns any such
// pattern into the same clean "step budget exceeded" error. Sized well under an
// 8 MB stack; a legitimately deep single-token match (e.g. \w+ over a very long
// run) may hit it and is reported, never crashed.
constexpr int MAX_DEPTH = 8000;

struct Prog {
    std::vector<Inst> code;
    std::vector<CharClass> classes;
    int groupCount = 1;              // group 0 = whole match
    std::string error;               // non-empty = compile failed
};

// ---- Compiler: recursive descent over the pattern ----
struct Compiler {
    const std::string& pat;
    size_t i = 0;
    Prog prog;

    explicit Compiler(const std::string& p) : pat(p) {}

    bool fail(const std::string& msg) {
        if (prog.error.empty()) prog.error = msg;
        return false;
    }
    int emit(const Inst& in) {
        prog.code.push_back(in);
        return (int)prog.code.size() - 1;
    }
    bool overflow() {
        if (prog.code.size() > MAX_PROG) return !fail("pattern too large");
        return false;
    }

    // fragment = [start, end) instruction range appended at the tail
    struct Frag { int start, end; };

    bool compile() {
        emit({Op::SAVE, 0, 0});
        Frag f;
        if (!parseAlt(f)) return false;
        if (i < pat.size()) return fail("unexpected ')' in pattern");
        emit({Op::SAVE, 0, 1});
        emit({Op::MATCH});
        return true;
    }

    bool parseAlt(Frag& out) {
        int start = (int)prog.code.size();
        Frag left;
        if (!parseSeq(left)) return false;
        while (i < pat.size() && pat[i] == '|') {
            i++;
            // left | right => SPLIT L1, L2; L1: left JMP end; L2: right
            std::vector<Inst> leftCode(prog.code.begin() + left.start, prog.code.end());
            prog.code.resize(left.start);
            int split = emit({Op::SPLIT});
            int l1 = (int)prog.code.size();
            // The left fragment is re-emitted shifted forward by the inserted
            // SPLIT; its internal jump targets (SPLIT x/y, JMP x — but NOT SAVE's
            // capture-slot x) are absolute and must move with it, or a 3+-way
            // alternation's inner branches point at the wrong ops (an infinite
            // loop that only the step budget stops). Only 2-way worked before.
            int shift = l1 - left.start;
            for (auto& in : leftCode) {
                if (in.op == Op::SPLIT) { in.x += shift; in.y += shift; }
                else if (in.op == Op::JMP) { in.x += shift; }
                prog.code.push_back(in);
            }
            if (overflow()) return false;
            int jmp = emit({Op::JMP});
            int l2 = (int)prog.code.size();
            Frag right;
            if (!parseSeq(right)) return false;
            prog.code[split].x = l1;
            prog.code[split].y = l2;
            prog.code[jmp].x = (int)prog.code.size();
            left = {start, (int)prog.code.size()};
        }
        out = {start, (int)prog.code.size()};
        return true;
    }

    bool parseSeq(Frag& out) {
        int start = (int)prog.code.size();
        while (i < pat.size() && pat[i] != '|' && pat[i] != ')') {
            Frag piece;
            if (!parsePiece(piece)) return false;
        }
        out = {start, (int)prog.code.size()};
        return true;
    }

    bool parsePiece(Frag& out) {
        Frag atom;
        if (!parseAtom(atom)) return false;
        // quantifier?
        if (i < pat.size()) {
            char q = pat[i];
            if (q == '*' || q == '+' || q == '?') {
                i++;
                bool lazy = (i < pat.size() && pat[i] == '?');
                if (lazy) i++;
                return applyRepeat(atom, q == '+' ? 1 : 0, q == '?' ? 1 : -1, lazy, out);
            }
            if (q == '{') {
                size_t save = i;
                i++;
                long long lo = -1, hi = -1;
                if (!readIntBound(lo)) { i = save; out = atom; return true; }
                if (i < pat.size() && pat[i] == ',') {
                    i++;
                    if (i < pat.size() && pat[i] == '}') hi = -1;   // {m,}
                    else if (!readIntBound(hi)) { i = save; out = atom; return true; }
                } else hi = lo;                                      // {m}
                if (i >= pat.size() || pat[i] != '}') { i = save; out = atom; return true; }
                i++;
                if (lo > 1000 || (hi != -1 && (hi > 1000 || hi < lo))) {
                    return fail("repetition count out of range (limit 1000)");
                }
                bool lazy = (i < pat.size() && pat[i] == '?');
                if (lazy) i++;
                return applyRepeat(atom, (int)lo, (int)hi, lazy, out);
            }
        }
        out = atom;
        return true;
    }

    bool readIntBound(long long& v) {
        if (i >= pat.size() || pat[i] < '0' || pat[i] > '9') return false;
        v = 0;
        while (i < pat.size() && pat[i] >= '0' && pat[i] <= '9') {
            v = v * 10 + (pat[i] - '0');
            if (v > 100000) return false;
            i++;
        }
        return true;
    }

    // Repeat the atom fragment lo..hi times (hi == -1 -> unbounded).
    bool applyRepeat(const Frag& atom, int lo, int hi, bool lazy, Frag& out) {
        std::vector<Inst> body(prog.code.begin() + atom.start, prog.code.end());
        prog.code.resize(atom.start);
        int start = (int)prog.code.size();

        auto append = [&](int shift) {
            int base = (int)prog.code.size() - shift;
            for (auto in : body) {
                if (in.op == Op::SPLIT) { in.x += base - atom.start + shift; in.y += base - atom.start + shift; }
                else if (in.op == Op::JMP) { in.x += base - atom.start + shift; }
                prog.code.push_back(in);
            }
        };
        // relocate helper: body was compiled at atom.start; append at current tail
        auto appendBody = [&]() {
            int delta = (int)prog.code.size() - atom.start;
            for (auto in : body) {
                if (in.op == Op::SPLIT) { in.x += delta; in.y += delta; }
                else if (in.op == Op::JMP) in.x += delta;
                prog.code.push_back(in);
            }
        };
        (void)append;

        for (int k = 0; k < lo; ++k) {
            appendBody();
            if (overflow()) return false;
        }
        if (hi == -1) {
            // (body)* tail: L: SPLIT body,end ; body ; JMP L
            int split = emit({Op::SPLIT});
            int bodyStart = (int)prog.code.size();
            appendBody();
            if (overflow()) return false;
            int jmp = emit({Op::JMP});
            prog.code[jmp].x = split;
            int end = (int)prog.code.size();
            if (lazy) { prog.code[split].x = end; prog.code[split].y = bodyStart; }
            else      { prog.code[split].x = bodyStart; prog.code[split].y = end; }
        } else {
            // up to (hi-lo) optional copies: SPLIT body,end each
            std::vector<int> splits;
            for (int k = lo; k < hi; ++k) {
                int split = emit({Op::SPLIT});
                splits.push_back(split);
                appendBody();
                if (overflow()) return false;
            }
            int end = (int)prog.code.size();
            for (int s : splits) {
                int bodyStart = s + 1;
                if (lazy) { prog.code[s].x = end; prog.code[s].y = bodyStart; }
                else      { prog.code[s].x = bodyStart; prog.code[s].y = end; }
            }
        }
        out = {start, (int)prog.code.size()};
        return true;
    }

    void emitClass(bool negate,
                   std::initializer_list<std::pair<unsigned char, unsigned char>> rs) {
        CharClass cc;
        cc.negate = negate;
        for (auto& r : rs) cc.ranges.push_back(r);
        prog.classes.push_back(cc);
        emit({Op::CLASS, 0, 0, 0, (int)prog.classes.size() - 1});
    }

    bool parseAtom(Frag& out) {
        int start = (int)prog.code.size();
        if (i >= pat.size()) return fail("pattern ended unexpectedly");
        char c = pat[i];

        if (c == '(') {
            i++;
            bool capturing = true;
            if (i + 1 < pat.size() && pat[i] == '?' && pat[i + 1] == ':') {
                capturing = false;
                i += 2;
            }
            int slot = -1;
            if (capturing) {
                if (prog.groupCount >= MAX_GROUPS) return fail("too many capture groups (limit 9)");
                slot = prog.groupCount++;
                emit({Op::SAVE, 0, slot * 2});
            }
            Frag inner;
            if (!parseAlt(inner)) return false;
            if (i >= pat.size() || pat[i] != ')') return fail("missing ')'");
            i++;
            if (capturing) emit({Op::SAVE, 0, slot * 2 + 1});
            out = {start, (int)prog.code.size()};
            return true;
        }
        if (c == '^') { i++; emit({Op::BOL}); out = {start, (int)prog.code.size()}; return true; }
        if (c == '$') { i++; emit({Op::EOL}); out = {start, (int)prog.code.size()}; return true; }
        if (c == '.') { i++; emit({Op::ANY}); out = {start, (int)prog.code.size()}; return true; }
        if (c == '[') {
            i++;
            CharClass cc;
            if (i < pat.size() && pat[i] == '^') { cc.negate = true; i++; }
            bool first = true;
            while (i < pat.size() && (pat[i] != ']' || first)) {
                unsigned char lo;
                if (pat[i] == '\\' && i + 1 < pat.size()) {
                    i++;
                    char e = pat[i];
                    // class shorthands inside [...]
                    if (e == 'd') { cc.ranges.push_back({'0','9'}); i++; first = false; continue; }
                    if (e == 'w') { cc.ranges.push_back({'a','z'}); cc.ranges.push_back({'A','Z'});
                                    cc.ranges.push_back({'0','9'}); cc.ranges.push_back({'_','_'});
                                    i++; first = false; continue; }
                    if (e == 's') { cc.ranges.push_back({' ',' '}); cc.ranges.push_back({'\t','\t'});
                                    cc.ranges.push_back({'\n','\n'}); cc.ranges.push_back({'\r','\r'});
                                    i++; first = false; continue; }
                    if (e == 'n') lo = '\n';
                    else if (e == 't') lo = '\t';
                    else if (e == 'r') lo = '\r';
                    else lo = (unsigned char)e;
                    i++;
                } else {
                    lo = (unsigned char)pat[i];
                    i++;
                }
                unsigned char hi = lo;
                if (i + 1 < pat.size() && pat[i] == '-' && pat[i + 1] != ']') {
                    i++;
                    if (pat[i] == '\\' && i + 1 < pat.size()) i++;
                    hi = (unsigned char)pat[i];
                    i++;
                    if (hi < lo) return fail("invalid class range");
                }
                cc.ranges.push_back({lo, hi});
                first = false;
            }
            if (i >= pat.size()) return fail("missing ']'");
            i++;
            prog.classes.push_back(cc);
            emit({Op::CLASS, 0, 0, 0, (int)prog.classes.size() - 1});
            out = {start, (int)prog.code.size()};
            return true;
        }
        if (c == '\\') {
            if (i + 1 >= pat.size()) return fail("dangling '\\'");
            i++;
            char e = pat[i];
            i++;
            switch (e) {
                case 'd': emitClass(false, {{'0','9'}}); break;
                case 'D': emitClass(true,  {{'0','9'}}); break;
                case 'w': emitClass(false, {{'a','z'},{'A','Z'},{'0','9'},{'_','_'}}); break;
                case 'W': emitClass(true,  {{'a','z'},{'A','Z'},{'0','9'},{'_','_'}}); break;
                case 's': emitClass(false, {{' ',' '},{'\t','\t'},{'\n','\n'},{'\r','\r'}}); break;
                case 'S': emitClass(true,  {{' ',' '},{'\t','\t'},{'\n','\n'},{'\r','\r'}}); break;
                case 'n': emit({Op::CHAR, '\n'}); break;
                case 't': emit({Op::CHAR, '\t'}); break;
                case 'r': emit({Op::CHAR, '\r'}); break;
                default:  emit({Op::CHAR, (unsigned char)e}); break;
            }
            out = {start, (int)prog.code.size()};
            return true;
        }
        if (c == '*' || c == '+' || c == '?' || c == ')') {
            return fail(std::string("misplaced '") + c + "' in pattern");
        }
        i++;
        emit({Op::CHAR, (unsigned char)c});
        out = {start, (int)prog.code.size()};
        return true;
    }
};

// ---- Matcher: recursive backtracking with a step budget ----
struct Matcher {
    const Prog& prog;
    const std::string& text;
    long long steps = 0;
    bool budgetHit = false;
    size_t caps[MAX_GROUPS * 2];

    Matcher(const Prog& p, const std::string& t) : prog(p), text(t) {
        for (auto& c : caps) c = std::string::npos;
    }

    bool run(int pc, size_t sp, int depth = 0) {
        if (++steps > MAX_STEPS) { budgetHit = true; return false; }
        if (depth > MAX_DEPTH) { budgetHit = true; return false; }  // stack guard
        for (;;) {
            const Inst& in = prog.code[pc];
            switch (in.op) {
                case Op::CHAR:
                    if (sp >= text.size() || (unsigned char)text[sp] != in.ch) return false;
                    sp++; pc++;
                    break;
                case Op::ANY: {
                    if (sp >= text.size() || text[sp] == '\n') return false;
                    int len = utf8CharLen((unsigned char)text[sp]);
                    sp += (size_t)len; pc++;
                    if (sp > text.size()) return false;
                    break;
                }
                case Op::CLASS:
                    if (sp >= text.size() ||
                        !prog.classes[in.classIdx].matches((unsigned char)text[sp])) return false;
                    sp++; pc++;
                    break;
                case Op::SPLIT:
                    if (run(in.x, sp, depth + 1)) return true;
                    if (budgetHit) return false;
                    if (++steps > MAX_STEPS) { budgetHit = true; return false; }
                    pc = in.y;
                    break;
                case Op::JMP:
                    pc = in.x;
                    break;
                case Op::SAVE: {
                    size_t old = caps[in.x];   // slot index rides in .x
                    caps[in.x] = sp;
                    if (run(pc + 1, sp, depth + 1)) return true;
                    caps[in.x] = old;
                    return false;
                }
                case Op::BOL:
                    if (sp != 0) return false;
                    pc++;
                    break;
                case Op::EOL:
                    if (sp != text.size()) return false;
                    pc++;
                    break;
                case Op::MATCH:
                    return true;
            }
        }
    }

    // Tries to match starting exactly at 'from'.
    bool matchAt(size_t from) {
        for (auto& c : caps) c = std::string::npos;
        return run(0, from);
    }
};

} // namespace Rx

// Compile with caching (scripts reuse the same few patterns in loops).
inline const Rx::Prog* rxCompile(const std::string& pattern, std::string& err) {
    static std::unordered_map<std::string, Rx::Prog> cache;
    auto it = cache.find(pattern);
    if (it == cache.end()) {
        if (cache.size() > 256) cache.clear();   // bound the cache
        Rx::Compiler c(pattern);
        c.compile();
        it = cache.emplace(pattern, std::move(c.prog)).first;
    }
    if (!it->second.error.empty()) { err = it->second.error; return nullptr; }
    return &it->second;
}

inline ObjPtr makeRegexModule() {
    static ObjPtr cached = nullptr;
    if (cached) return cached;

    auto mod = makeObj<MapObject>();
    auto def = [&](const std::string& name, BuiltinObject::BuiltinFn fn) {
        mod->set(strKey(name), makeObj<BuiltinObject>(name, std::move(fn)));
    };

    auto twoStrings = [](const Args& args, const char* fname, int line,
                         const std::string*& text, const std::string*& pat) -> ObjPtr {
        if (args.size() != 2 || args[0]->type() != ObjectType::STRING ||
            args[1]->type() != ObjectType::STRING) {
            return makeError(std::string("regex.") + fname +
                             "(text, pattern) expects two strings", line);
        }
        text = &static_cast<StringObject*>(args[0].get())->value;
        pat = &static_cast<StringObject*>(args[1].get())->value;
        return nullptr;
    };
    auto stepError = [](int line) {
        return makeError("regex step limit exceeded (pattern too complex for this input)", line);
    };

    // match(text, pattern): match anchored at the START; matched text or null
    def("match", [twoStrings, stepError](const Args& args, int line, const CallFn&) -> ObjPtr {
        const std::string *t, *p;
        if (auto e = twoStrings(args, "match", line, t, p)) return e;
        std::string err;
        const Rx::Prog* prog = rxCompile(*p, err);
        if (!prog) return makeError("regex: " + err, line);
        Rx::Matcher m(*prog, *t);
        if (m.matchAt(0)) return makeObj<StringObject>(t->substr(m.caps[0], m.caps[1] - m.caps[0]));
        if (m.budgetHit) return stepError(line);
        return NULL_OBJ_;
    });

    // search(text, pattern): first match anywhere; matched text or null
    def("search", [twoStrings, stepError](const Args& args, int line, const CallFn&) -> ObjPtr {
        const std::string *t, *p;
        if (auto e = twoStrings(args, "search", line, t, p)) return e;
        std::string err;
        const Rx::Prog* prog = rxCompile(*p, err);
        if (!prog) return makeError("regex: " + err, line);
        Rx::Matcher m(*prog, *t);
        for (size_t from = 0; from <= t->size(); ++from) {
            if (m.matchAt(from)) {
                return makeObj<StringObject>(t->substr(m.caps[0], m.caps[1] - m.caps[0]));
            }
            if (m.budgetHit) return stepError(line);
        }
        return NULL_OBJ_;
    });

    // groups(text, pattern): [whole, g1, g2, ...] of the first match, or null
    def("groups", [twoStrings, stepError](const Args& args, int line, const CallFn&) -> ObjPtr {
        const std::string *t, *p;
        if (auto e = twoStrings(args, "groups", line, t, p)) return e;
        std::string err;
        const Rx::Prog* prog = rxCompile(*p, err);
        if (!prog) return makeError("regex: " + err, line);
        Rx::Matcher m(*prog, *t);
        for (size_t from = 0; from <= t->size(); ++from) {
            if (m.matchAt(from)) {
                auto out = makeObj<ListObject>();
                GcRoot _gr(out.get());
                for (int g = 0; g < prog->groupCount; ++g) {
                    size_t a = m.caps[g * 2], b = m.caps[g * 2 + 1];
                    if (a == std::string::npos || b == std::string::npos) {
                        out->elements.push_back(NULL_OBJ_);
                    } else {
                        out->elements.push_back(makeObj<StringObject>(t->substr(a, b - a)));
                    }
                }
                return out;
            }
            if (m.budgetHit) return stepError(line);
        }
        return NULL_OBJ_;
    });

    // find_all(text, pattern): every non-overlapping match, in order
    def("find_all", [twoStrings, stepError](const Args& args, int line, const CallFn&) -> ObjPtr {
        const std::string *t, *p;
        if (auto e = twoStrings(args, "find_all", line, t, p)) return e;
        std::string err;
        const Rx::Prog* prog = rxCompile(*p, err);
        if (!prog) return makeError("regex: " + err, line);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        Rx::Matcher m(*prog, *t);
        size_t from = 0;
        while (from <= t->size()) {
            if (m.matchAt(from)) {
                size_t a = m.caps[0], b = m.caps[1];
                out->elements.push_back(makeObj<StringObject>(t->substr(a, b - a)));
                from = (b > a) ? b : b + 1;   // advance past empty matches
            } else {
                if (m.budgetHit) return stepError(line);
                from++;
            }
        }
        return out;
    });

    // replace(text, pattern, replacement): all matches; $0..$9 insert groups
    def("replace", [stepError](const Args& args, int line, const CallFn&) -> ObjPtr {
        if (args.size() != 3 || args[0]->type() != ObjectType::STRING ||
            args[1]->type() != ObjectType::STRING || args[2]->type() != ObjectType::STRING) {
            return makeError("regex.replace(text, pattern, replacement) expects three strings", line);
        }
        const std::string& t = static_cast<StringObject*>(args[0].get())->value;
        const std::string& p = static_cast<StringObject*>(args[1].get())->value;
        const std::string& rep = static_cast<StringObject*>(args[2].get())->value;
        std::string err;
        const Rx::Prog* prog = rxCompile(p, err);
        if (!prog) return makeError("regex: " + err, line);
        std::string out;
        Rx::Matcher m(*prog, t);
        size_t from = 0;
        while (from <= t.size()) {
            if (m.matchAt(from)) {
                size_t a = m.caps[0], b = m.caps[1];
                // substitute $0..$9 ($$ = literal $)
                for (size_t k = 0; k < rep.size(); ++k) {
                    if (rep[k] == '$' && k + 1 < rep.size()) {
                        char n = rep[k + 1];
                        if (n == '$') { out += '$'; k++; continue; }
                        if (n >= '0' && n <= '9') {
                            int g = n - '0';
                            if (g < prog->groupCount &&
                                m.caps[g * 2] != std::string::npos) {
                                out += t.substr(m.caps[g * 2],
                                                m.caps[g * 2 + 1] - m.caps[g * 2]);
                            }
                            k++;
                            continue;
                        }
                    }
                    out += rep[k];
                }
                if (b > a) from = b;
                else { if (b < t.size()) out += t[b]; from = b + 1; }
            } else {
                if (m.budgetHit) return stepError(line);
                if (from < t.size()) out += t[from];
                from++;
            }
        }
        return makeObj<StringObject>(out);
    });

    // split(text, pattern): pieces between matches
    def("split", [twoStrings, stepError](const Args& args, int line, const CallFn&) -> ObjPtr {
        const std::string *t, *p;
        if (auto e = twoStrings(args, "split", line, t, p)) return e;
        std::string err;
        const Rx::Prog* prog = rxCompile(*p, err);
        if (!prog) return makeError("regex: " + err, line);
        auto out = makeObj<ListObject>();
        GcRoot _gr(out.get());
        Rx::Matcher m(*prog, *t);
        size_t from = 0, pieceStart = 0;
        while (from <= t->size()) {
            if (m.matchAt(from)) {
                size_t a = m.caps[0], b = m.caps[1];
                if (b > a) {
                    out->elements.push_back(makeObj<StringObject>(t->substr(pieceStart, a - pieceStart)));
                    pieceStart = b;
                    from = b;
                } else {
                    from++;   // empty match: no split, move on
                }
            } else {
                if (m.budgetHit) return stepError(line);
                from++;
            }
        }
        out->elements.push_back(makeObj<StringObject>(t->substr(pieceStart)));
        return out;
    });

    mod->frozen = true;
    mod->moduleName = "regex";
    gcPermanentRoot(mod.get());
    cached = mod;
    return mod;
}

} // namespace StdLib
} // namespace Lovax

#endif // LOVAX_MODULE_REGEX_HPP
