#ifndef LOVAX_JIT_EMIT_X64_HPP
#define LOVAX_JIT_EMIT_X64_HPP

// Our own x86-64 instruction encoder (RFC-026). Zero dependencies: no DynASM,
// no asmjit, no LLVM — the instruction set is exactly what the code generator
// needs and grows on demand. Method studied from sljit's emit API and LuaJIT's
// lj_emit_x86.h; the encoding tables are written here from the Intel manual.
//
// Encoding shape:  [REX] opcode [ModRM] [SIB] [disp] [imm]
//   REX  = 0100 W R X B   (W=1 -> 64-bit operand, R/X/B = high bit of reg fields)
//   ModRM= mod(2) reg(3) rm(3)
// Two rm quirks the helpers below handle so callers never have to:
//   rm==RSP/R12 (low 3 bits 100) always needs a SIB byte
//   rm==RBP/R13 (low 3 bits 101) with mod=00 means RIP-relative, so a zero
//   displacement must be encoded as mod=01 disp8=0 instead.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace Lovax {
namespace Jit {

enum Reg : int {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8  = 8, R9  = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15
};
enum Xmm : int {
    XMM0 = 0, XMM1 = 1, XMM2 = 2, XMM3 = 3, XMM4 = 4, XMM5 = 5, XMM6 = 6, XMM7 = 7
};
// Condition codes (the low nibble of the jcc/setcc opcode).
enum Cond : int {
    O = 0x0, NO = 0x1, B = 0x2, AE = 0x3, E = 0x4, NE = 0x5, BE = 0x6, A = 0x7,
    S = 0x8, NS = 0x9, L = 0xC, GE = 0xD, LE = 0xE, G = 0xF
};

// A forward/backward branch target. Bind it once; every jump to it is patched.
struct Label {
    int  bound = -1;                 // byte offset once bound
    std::vector<size_t> patches;     // offsets of rel32 fields awaiting the target
};

class Asm {
public:
    std::vector<uint8_t> buf;

    size_t size() const { return buf.size(); }
    const uint8_t* data() const { return buf.data(); }

    // ---- raw emit ----
    void u8(uint8_t v)  { buf.push_back(v); }
    void u32(uint32_t v){ for (int i = 0; i < 4; ++i) u8((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v){ for (int i = 0; i < 8; ++i) u8((uint8_t)(v >> (8 * i))); }

    // ---- prefixes / operands ----
    // Emits REX when 64-bit operands or extended registers are involved.
    void rex(bool w, int reg, int rm, int index = 0) {
        uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >> 3) << 2) |
                              ((index >> 3) << 1) | (rm >> 3));
        if (r != 0x40) u8(r);
    }
    void modrmReg(int reg, int rm) {            // register-direct: mod = 11
        u8((uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    }
    // [base + disp] with the RSP/R12 (SIB) and RBP/R13 (no mod=00) quirks.
    void modrmMem(int reg, int base, int32_t disp) {
        int b = base & 7;
        int mod;
        if (disp == 0 && b != 5)      mod = 0;   // RBP/R13 cannot use mod=00
        else if (disp >= -128 && disp <= 127) mod = 1;
        else                          mod = 2;
        u8((uint8_t)((mod << 6) | ((reg & 7) << 3) | b));
        if (b == 4) u8(0x24);                    // SIB: base=RSP/R12, no index
        if (mod == 1) u8((uint8_t)(int8_t)disp);
        else if (mod == 2) u32((uint32_t)disp);
    }

    // ---- moves ----
    void movRR(Reg d, Reg s)   { rex(true, s, d); u8(0x89); modrmReg(s, d); }
    void movRM(Reg d, Reg base, int32_t disp) {         // d = [base+disp]
        rex(true, d, base); u8(0x8B); modrmMem(d, base, disp);
    }
    void movMR(Reg base, int32_t disp, Reg s) {         // [base+disp] = s
        rex(true, s, base); u8(0x89); modrmMem(s, base, disp);
    }
    void movAbs(Reg d, uint64_t imm) {                  // movabs d, imm64
        rex(true, 0, d); u8((uint8_t)(0xB8 + (d & 7))); u64(imm);
    }
    void movRI32(Reg d, int32_t imm) {                  // sign-extended imm32
        rex(true, 0, d); u8(0xC7); modrmReg(0, d); u32((uint32_t)imm);
    }

    // ---- ALU (reg, reg) ----
    void aluRR(uint8_t op, Reg d, Reg s) { rex(true, s, d); u8(op); modrmReg(s, d); }
    void addRR(Reg d, Reg s)  { aluRR(0x01, d, s); }
    void subRR(Reg d, Reg s)  { aluRR(0x29, d, s); }
    void andRR(Reg d, Reg s)  { aluRR(0x21, d, s); }
    void orRR (Reg d, Reg s)  { aluRR(0x09, d, s); }
    void xorRR(Reg d, Reg s)  { aluRR(0x31, d, s); }
    void cmpRR(Reg d, Reg s)  { aluRR(0x39, d, s); }
    void testRR(Reg d, Reg s) { aluRR(0x85, d, s); }
    void imulRR(Reg d, Reg s) { rex(true, d, s); u8(0x0F); u8(0xAF); modrmReg(d, s); }

    // ---- ALU (reg, imm32) — /digit selects the operation ----
    void aluRI(int digit, Reg d, int32_t imm) {
        rex(true, 0, d); u8(0x81); modrmReg(digit, d); u32((uint32_t)imm);
    }
    void addRI(Reg d, int32_t i) { aluRI(0, d, i); }
    void orRI (Reg d, int32_t i) { aluRI(1, d, i); }
    void andRI(Reg d, int32_t i) { aluRI(4, d, i); }
    void subRI(Reg d, int32_t i) { aluRI(5, d, i); }
    void xorRI(Reg d, int32_t i) { aluRI(6, d, i); }
    void cmpRI(Reg d, int32_t i) { aluRI(7, d, i); }

    // ---- shifts (imm8) ----
    void shiftRI(int digit, Reg d, uint8_t n) {
        rex(true, 0, d); u8(0xC1); modrmReg(digit, d); u8(n);
    }
    void shlRI(Reg d, uint8_t n) { shiftRI(4, d, n); }
    void shrRI(Reg d, uint8_t n) { shiftRI(5, d, n); }
    void sarRI(Reg d, uint8_t n) { shiftRI(7, d, n); }
    // variable shifts use CL
    void shlCL(Reg d) { rex(true, 0, d); u8(0xD3); modrmReg(4, d); }
    void shrCL(Reg d) { rex(true, 0, d); u8(0xD3); modrmReg(5, d); }
    void sarCL(Reg d) { rex(true, 0, d); u8(0xD3); modrmReg(7, d); }

    void negR(Reg d) { rex(true, 0, d); u8(0xF7); modrmReg(3, d); }
    void notR(Reg d) { rex(true, 0, d); u8(0xF7); modrmReg(2, d); }
    void cqo()       { u8(0x48); u8(0x99); }                       // sign-extend RAX -> RDX:RAX
    void idivR(Reg d){ rex(true, 0, d); u8(0xF7); modrmReg(7, d); } // RDX:RAX / d

    // ---- stack / calls ----
    void pushR(Reg r) { if (r >= R8) u8(0x41); u8((uint8_t)(0x50 + (r & 7))); }
    void popR (Reg r) { if (r >= R8) u8(0x41); u8((uint8_t)(0x58 + (r & 7))); }
    void callR(Reg r) { rex(false, 0, r); u8(0xFF); modrmReg(2, r); }
    void ret()        { u8(0xC3); }

    // setcc writes a byte; REX is required for SPL/BPL/SIL/DIL to mean the
    // low byte rather than AH/CH/DH/BH.
    void setcc(Cond c, Reg d) {
        if (d >= RSP) u8((uint8_t)(0x40 | (d >> 3)));
        u8(0x0F); u8((uint8_t)(0x90 + c)); modrmReg(0, d);
    }
    void movzxR8(Reg d, Reg s) {                 // zero-extend byte -> 64-bit
        rex(true, d, s); u8(0x0F); u8(0xB6); modrmReg(d, s);
    }
    // movzx r64, BYTE PTR [base+disp] — reads exactly one byte, so it is safe
    // on a bool / last element of a byte array (a qword load would over-read).
    void movzxRM8(Reg d, Reg base, int32_t disp) {
        rex(true, d, base); u8(0x0F); u8(0xB6); modrmMem(d, base, disp);
    }

    // ---- branches ----
    void jmp(Label& l)          { u8(0xE9); ref(l); }
    void jcc(Cond c, Label& l)  { u8(0x0F); u8((uint8_t)(0x80 + c)); ref(l); }
    void bind(Label& l) {
        l.bound = (int)buf.size();
        for (size_t at : l.patches) patchRel32(at, l.bound);
        l.patches.clear();
    }

    // ---- SSE2 (doubles) ----
    void sse(uint8_t pfx, uint8_t op, int reg, int rm, bool w = false) {
        if (pfx) u8(pfx);
        rex(w, reg, rm);
        u8(0x0F); u8(op);
    }
    void movsdXM(Xmm d, Reg base, int32_t disp) {          // d = [base+disp]
        u8(0xF2); rex(false, d, base); u8(0x0F); u8(0x10); modrmMem(d, base, disp);
    }
    void movsdMX(Reg base, int32_t disp, Xmm s) {          // [base+disp] = s
        u8(0xF2); rex(false, s, base); u8(0x0F); u8(0x11); modrmMem(s, base, disp);
    }
    void sseRR(uint8_t op, Xmm d, Xmm s) {
        u8(0xF2); rex(false, d, s); u8(0x0F); u8(op); modrmReg(d, s);
    }
    void addsd(Xmm d, Xmm s) { sseRR(0x58, d, s); }
    void subsd(Xmm d, Xmm s) { sseRR(0x5C, d, s); }
    void mulsd(Xmm d, Xmm s) { sseRR(0x59, d, s); }
    void divsd(Xmm d, Xmm s) { sseRR(0x5E, d, s); }
    void ucomisd(Xmm d, Xmm s) {                            // 66 0F 2E /r
        u8(0x66); rex(false, d, s); u8(0x0F); u8(0x2E); modrmReg(d, s);
    }
    void cvtsi2sd(Xmm d, Reg s) {                           // F2 REX.W 0F 2A /r
        u8(0xF2); rex(true, d, s); u8(0x0F); u8(0x2A); modrmReg(d, s);
    }
    void cvttsd2si(Reg d, Xmm s) {                          // F2 REX.W 0F 2C /r
        u8(0xF2); rex(true, d, s); u8(0x0F); u8(0x2C); modrmReg(d, s);
    }
    void movqXR(Xmm d, Reg s) {                             // 66 REX.W 0F 6E /r
        u8(0x66); rex(true, d, s); u8(0x0F); u8(0x6E); modrmReg(d, s);
    }
    void movqRX(Reg d, Xmm s) {                             // 66 REX.W 0F 7E /r
        u8(0x66); rex(true, s, d); u8(0x0F); u8(0x7E); modrmReg(s, d);
    }

private:
    // Emits a rel32 placeholder (or the final displacement for an already-bound
    // label) relative to the END of the instruction.
    void ref(Label& l) {
        size_t at = buf.size();
        u32(0);
        if (l.bound >= 0) patchRel32(at, l.bound);
        else l.patches.push_back(at);
    }
    void patchRel32(size_t at, int target) {
        int32_t rel = (int32_t)(target - (int)(at + 4));
        for (int i = 0; i < 4; ++i) buf[at + i] = (uint8_t)((uint32_t)rel >> (8 * i));
    }
};

} // namespace Jit
} // namespace Lovax

#endif // LOVAX_JIT_EMIT_X64_HPP
