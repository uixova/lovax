// Isolated baseline code-generator tests (RFC-026 Stage 2). Builds a synthetic
// bytecode region — the exact shape the compiler emits for a `while` loop over
// integer locals — compiles it, runs the machine code, and checks both the
// computed values AND the bytecode offset it hands back. Nothing here touches
// the VM: the code generator is proven before it is wired in.

#include <cstdio>
#include <cstring>
#include <vector>
#include "../../src/jit/disasm.hpp"
#include "../../src/jit/compile.hpp"

using namespace Lovax;
using namespace Lovax::Jit;

static int failures = 0, checks = 0;
#define CHECK(cond, name) do { ++checks; if (!(cond)) { \
    std::printf("  FAIL %s\n", name); ++failures; } } while (0)

// Builds:  while (i < n) { acc += (i*3+7) % 13; i += 1; }
// slots: 0 = n, 1 = acc, 2 = i
static Chunk buildLoopChunk() {
    Chunk c;
    auto op   = [&](Op o) { c.emitOp(o, 1); };
    auto u16  = [&](uint16_t v) { c.emitU16(v, 1); };
    c.addConst(Value::integer(1));           // const 0 = 1

    op(Op::GET_LOCAL); u16(2);               // 0  i
    op(Op::GET_LOCAL); u16(0);               // 3  n
    op(Op::LESS_JF);   u16(32);              // 6  -> 9+32 = 41 (exit) when !(i<n)
    op(Op::GET_LOCAL); u16(1);               // 9  acc
    op(Op::GET_LOCAL); u16(2);               // 12 i
    op(Op::MUL_I);     u16((uint16_t)3);     // 15
    op(Op::ADD_I);     u16((uint16_t)7);     // 18
    op(Op::MOD_I);     u16((uint16_t)13);    // 21
    op(Op::ADD_INPLACE);                     // 24
    op(Op::SET_LOCAL); u16(1);               // 25 acc =
    op(Op::GET_LOCAL); u16(2);               // 28 i
    op(Op::CONST);     u16(0);               // 31 1
    op(Op::ADD_INPLACE);                     // 34
    op(Op::SET_LOCAL); u16(2);               // 35 i =
    op(Op::LOOP);      u16(41);              // 38 -> 41-41 = 0 (back)
    return c;                                // region = [0, 41)
}

static Region compileRegion(const Chunk& c, size_t start, size_t end) {
    RegionCompiler rc(c, start, end);
    Region r;
    if (!rc.compile()) return r;
    void* p = mcodeAlloc(rc.a.size());
    if (!p) return r;
    std::memcpy(p, rc.a.data(), rc.a.size());
    if (!mcodeFinalize(p, rc.a.size())) return r;
    r.code = p; r.codeSize = rc.a.size();
    r.fn = reinterpret_cast<JitFn>(p);
    return r;
}

int main() {
    std::printf("== JIT baseline codegen (isolated) ==\n");
    Chunk c = buildLoopChunk();
    Region reg = compileRegion(c, 0, 41);
    CHECK(reg.fn != nullptr, "region compiles");
    if (!reg.fn) { std::printf("cannot continue\n"); return 1; }
    std::printf("  (%zu bytes of machine code for 41 bytes of bytecode)\n", reg.codeSize);

    std::vector<Value> stack(64);
    std::vector<unsigned char> defined(4, 1);
    std::vector<Value> globals(4);

    auto run = [&](long long n, Value accInit, long long& accOut,
                   long long& iOut, int64_t& resume) {
        Value slots[3];
        slots[0] = Value::integer(n);
        slots[1] = accInit;
        slots[2] = Value::integer(0);
        JitCtx ctx;
        ctx.slots = slots;
        ctx.sp = stack.data();
        ctx.globals = globals.data();
        ctx.consts = c.consts.data();
        ctx.globalDefined = defined.data();
        resume = reg.fn(&ctx);
        accOut = slots[1].isInt() ? slots[1].asInt() : -999999;
        iOut   = slots[2].isInt() ? slots[2].asInt() : -999999;
        return ctx.sp == stack.data();       // stack must be balanced on exit
    };

    // reference: what the language semantics say the loop computes
    auto expected = [](long long n) {
        long long acc = 0;
        for (long long i = 0; i < n; ++i) {
            long long m = (i * 3 + 7) % 13;
            if (m != 0 && ((m < 0) != (13 < 0))) m += 13;   // floor-mod
            acc += m;
        }
        return acc;
    };

    for (long long n : {0ll, 1ll, 2ll, 13ll, 100ll, 100000ll}) {
        long long acc = 0, i = 0; int64_t resume = -1;
        bool balanced = run(n, Value::integer(0), acc, i, resume);
        char name[96];
        std::snprintf(name, sizeof(name), "loop n=%lld result", n);
        CHECK(acc == expected(n), name);
        std::snprintf(name, sizeof(name), "loop n=%lld counter", n);
        CHECK(i == n, name);
        std::snprintf(name, sizeof(name), "loop n=%lld resume offset", n);
        CHECK(resume == 41, name);
        std::snprintf(name, sizeof(name), "loop n=%lld stack balanced", n);
        CHECK(balanced, name);
    }

    // Guard path: a non-integer accumulator must bail at the opcode that needs
    // an int, WITHOUT having mutated the stack — the interpreter resumes there.
    {
        Value slots[3];
        slots[0] = Value::integer(10);
        slots[1] = Value::real(1.5);          // acc is a double -> ADD_INPLACE bails
        slots[2] = Value::integer(0);
        JitCtx ctx;
        ctx.slots = slots; ctx.sp = stack.data();
        ctx.globals = globals.data(); ctx.consts = c.consts.data();
        ctx.globalDefined = defined.data();
        int64_t resume = reg.fn(&ctx);
        CHECK(resume == 24, "bails at ADD_INPLACE on a non-int operand");
        // sp must sit exactly where the interpreter expects for that opcode:
        // ADD_INPLACE consumes two values, so both are still pushed.
        CHECK(ctx.sp == stack.data() + 2, "stack left intact for the interpreter");
        CHECK(slots[1].isFloat() && slots[1].asFloat() == 1.5, "no partial mutation");
    }

    // A value that no longer fits the 47-bit inline range must bail too (the
    // interpreter heap-boxes it; compiled code never allocates).
    {
        Value slots[3];
        slots[0] = Value::integer(5);
        slots[1] = Value::integer((1ll << 46) - 2);   // near the inline ceiling
        slots[2] = Value::integer(0);
        JitCtx ctx;
        ctx.slots = slots; ctx.sp = stack.data();
        ctx.globals = globals.data(); ctx.consts = c.consts.data();
        ctx.globalDefined = defined.data();
        int64_t resume = reg.fn(&ctx);
        CHECK(resume >= 0 && resume <= 41, "overflow path returns a valid offset");
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    std::printf(failures == 0 ? "JIT CODEGEN GATE PASSED\n" : "JIT CODEGEN GATE FAILED\n");
    return failures == 0 ? 0 : 1;
}
