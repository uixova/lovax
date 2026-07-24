// Isolated x86-64 assembler tests (RFC-026 Stage 1). Proves the ENCODER before
// any JIT logic depends on it: every case builds real machine code, flips it to
// executable (W^X), calls it, and checks the result. A wrong REX/ModRM byte
// shows up as a wrong answer or a crash here, not as a mystery miscompile later.
//
// SysV AMD64: int args RDI, RSI, RDX, RCX, R8, R9; return RAX;
// callee-saved RBX, RBP, R12-R15.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include "../../src/jit/emit_x64.hpp"
#include "../../src/jit/mcode.hpp"

using namespace Lovax::Jit;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    ++checks; \
    if (!(cond)) { std::printf("  FAIL %s\n", name); ++failures; } \
} while (0)

// Finalize an assembler buffer into callable machine code.
template <class Fn>
static Fn build(Asm& a) {
    void* p = mcodeAlloc(a.size());
    if (!p) return nullptr;
    std::memcpy(p, a.data(), a.size());
    if (!mcodeFinalize(p, a.size())) return nullptr;
    return reinterpret_cast<Fn>(p);
}

static long long helper_triple(long long x) { return x * 3; }

// --dump <file>: emit a canonical instruction sequence as raw bytes so the
// shell gate can disassemble it with objdump and compare mnemonics — catches a
// wrong encoding that happens to produce the right answer by luck.
static int dumpCanonical(const char* path) {
    Asm a;
    a.movRR(RAX, RDI);          // mov    %rdi,%rax
    a.addRR(RAX, RSI);          // add    %rsi,%rax
    a.subRR(RAX, RDX);          // sub    %rdx,%rax
    a.imulRR(RAX, RCX);         // imul   %rcx,%rax
    a.andRR(R8, R9);            // and    %r9,%r8
    a.xorRR(R10, R11);          // xor    %r11,%r10
    a.cmpRI(RAX, 1234);         // cmp    $0x4d2,%rax
    a.shlRI(RAX, 17);           // shl    $0x11,%rax
    a.sarRI(RAX, 17);           // sar    $0x11,%rax
    a.movRM(RBX, RSP, 0);       // mov    (%rsp),%rbx
    a.movMR(RBP, -8, RBX);      // mov    %rbx,-0x8(%rbp)
    a.movAbs(RDX, 0x1122334455667788ull); // movabs $0x1122334455667788,%rdx
    a.negR(RAX);                // neg    %rax
    a.notR(RAX);                // not    %rax
    a.cqo();                    // cqto
    a.idivR(RCX);               // idiv   %rcx
    a.pushR(R12); a.popR(R12);  // push %r12 / pop %r12
    a.ret();                    // ret
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(a.data(), 1, a.size(), f);
    std::fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0) return dumpCanonical(argv[2]);
    std::printf("== x64 assembler (isolated) ==\n");

    // 1. constant return: movabs rax, imm64; ret
    {
        Asm a;
        a.movAbs(RAX, 0x123456789ABCull);
        a.ret();
        auto f = build<long long(*)()>(a);
        CHECK(f && f() == 0x123456789ABCll, "movabs + ret");
    }

    // 2. add two args: mov rax, rdi; add rax, rsi; ret
    {
        Asm a;
        a.movRR(RAX, RDI);
        a.addRR(RAX, RSI);
        a.ret();
        auto f = build<long long(*)(long long, long long)>(a);
        CHECK(f && f(30, 12) == 42, "movRR + addRR");
        CHECK(f && f(-5, 5) == 0, "addRR negative");
    }

    // 3. full ALU set through extended registers (exercises REX.R/REX.B)
    {
        Asm a;
        a.movRR(R8, RDI);
        a.movRR(R9, RSI);
        a.imulRR(R8, R9);        // r8 = a*b
        a.movRR(RAX, R8);
        a.subRI(RAX, 1);
        a.andRI(RAX, 0xFFFF);
        a.ret();
        auto f = build<long long(*)(long long, long long)>(a);
        CHECK(f && f(7, 6) == ((7 * 6 - 1) & 0xFFFF), "imul/sub/and with R8-R15");
    }

    // 4. memory round-trip through RSP-relative and RBP-relative addressing —
    //    the two ModRM quirks (SIB for RSP/R12, no mod=00 for RBP/R13).
    {
        Asm a;
        a.pushR(RBP);
        a.movRR(RBP, RSP);
        a.subRI(RSP, 64);
        a.movMR(RSP, 0,  RDI);     // [rsp+0]  = a   (needs SIB)
        a.movMR(RBP, -8, RSI);     // [rbp-8]  = b
        a.movRM(RAX, RSP, 0);
        a.movRM(RCX, RBP, -8);
        a.addRR(RAX, RCX);
        a.movRR(RSP, RBP);
        a.popR(RBP);
        a.ret();
        auto f = build<long long(*)(long long, long long)>(a);
        CHECK(f && f(100, 23) == 123, "mem [rsp+0] / [rbp-8] round-trip");
    }

    // 5. backward branch loop: sum 1..n  (labels, jcc, cmp)
    {
        Asm a;
        Label top, done;
        a.movAbs(RAX, 0);          // acc
        a.movRR(RCX, RDI);         // i = n
        a.bind(top);
        a.cmpRI(RCX, 0);
        a.jcc(LE, done);
        a.addRR(RAX, RCX);
        a.subRI(RCX, 1);
        a.jmp(top);
        a.bind(done);
        a.ret();
        auto f = build<long long(*)(long long)>(a);
        CHECK(f && f(10) == 55, "loop: sum 1..10");
        CHECK(f && f(100) == 5050, "loop: sum 1..100");
        CHECK(f && f(0) == 0, "loop: zero trip");
    }

    // 6. forward branch + setcc (comparison materialised as 0/1)
    {
        Asm a;
        a.cmpRR(RDI, RSI);
        a.setcc(L, RAX);
        a.movzxR8(RAX, RAX);
        a.ret();
        auto f = build<long long(*)(long long, long long)>(a);
        CHECK(f && f(1, 2) == 1, "setcc L true");
        CHECK(f && f(2, 1) == 0, "setcc L false");
        CHECK(f && f(2, 2) == 0, "setcc L equal");
    }

    // 7. shifts + the NaN-box int unbox/box shape the JIT will emit
    //    (sign-extend from bit 46: shl 17, sar 17)
    {
        Asm a;
        a.movRR(RAX, RDI);
        a.shlRI(RAX, 17);
        a.sarRI(RAX, 17);
        a.ret();
        auto f = build<long long(*)(long long)>(a);
        CHECK(f && f(12345) == 12345, "shl/sar sign-extend positive");
        // a 47-bit negative pattern must sign-extend to a negative int64
        uint64_t neg47 = ((uint64_t)(-3)) & 0x00007FFFFFFFFFFFull;
        CHECK(f && f((long long)neg47) == -3, "shl/sar sign-extend negative");
    }

    // 8. signed division (cqo + idiv)
    {
        Asm a;
        a.movRR(RAX, RDI);
        a.cqo();
        a.idivR(RSI);
        a.ret();                    // quotient in RAX
        auto f = build<long long(*)(long long, long long)>(a);
        CHECK(f && f(100, 7) == 14, "idiv quotient");
        CHECK(f && f(-100, 7) == -14, "idiv negative (truncating)");
    }

    // 9. calling a C function from generated code (movabs addr + call reg)
    {
        Asm a;
        a.pushR(RBX);                       // keep 16-byte alignment + callee-saved
        a.movAbs(RAX, (uint64_t)(uintptr_t)&helper_triple);
        a.callR(RAX);                       // arg already in RDI
        a.popR(RBX);
        a.ret();
        auto f = build<long long(*)(long long)>(a);
        CHECK(f && f(14) == 42, "call C helper");
    }

    // 10. SSE2 doubles: load, add, store, and int->double conversion
    {
        Asm a;
        a.movsdXM(XMM0, RDI, 0);
        a.movsdXM(XMM1, RDI, 8);
        a.addsd(XMM0, XMM1);
        a.movsdMX(RDI, 16, XMM0);
        a.ret();
        auto f = build<void(*)(double*)>(a);
        double v[3] = {1.5, 2.25, 0.0};
        if (f) f(v);
        CHECK(f && v[2] == 3.75, "movsd + addsd");
    }
    {
        Asm a;
        a.cvtsi2sd(XMM0, RDI);
        a.movqRX(RAX, XMM0);
        a.ret();
        auto f = build<uint64_t(*)(long long)>(a);
        double expect = 7.0;
        uint64_t bits; std::memcpy(&bits, &expect, 8);
        CHECK(f && f(7) == bits, "cvtsi2sd + movq");
    }

    // 11. the actual NaN-box tag guard the baseline JIT emits:
    //     and rcx, TAG_MASK ; cmp rcx, mk(T_INT) ; sete
    {
        const uint64_t TAGS = 0xFFF8000000000000ull;
        const uint64_t TAG_MASK = TAGS | (0xFull << 47);
        const uint64_t T_INT_TAG = TAGS | ((uint64_t)4 << 47);
        Asm a;
        a.movRR(RCX, RDI);
        a.movAbs(RDX, TAG_MASK);
        a.andRR(RCX, RDX);
        a.movAbs(RDX, T_INT_TAG);
        a.cmpRR(RCX, RDX);
        a.setcc(E, RAX);
        a.movzxR8(RAX, RAX);
        a.ret();
        auto f = build<long long(*)(uint64_t)>(a);
        uint64_t anInt = T_INT_TAG | (99ull & 0x00007FFFFFFFFFFFull);
        uint64_t aNil  = TAGS | ((uint64_t)1 << 47);
        double d = 3.5; uint64_t aDouble; std::memcpy(&aDouble, &d, 8);
        CHECK(f && f(anInt) == 1, "tag guard accepts T_INT");
        CHECK(f && f(aNil) == 0, "tag guard rejects T_NIL");
        CHECK(f && f(aDouble) == 0, "tag guard rejects double");
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    if (failures == 0) std::printf("JIT ASM GATE PASSED\n");
    else std::printf("JIT ASM GATE FAILED\n");
    return failures == 0 ? 0 : 1;
}
