#ifndef LOVAX_JIT_DISASM_HPP
#define LOVAX_JIT_DISASM_HPP

// Bytecode disassembler (RFC-026 tooling). A code generator cannot be written
// blind: this prints exactly what the compiler emitted, with operands, so the
// JIT's opcode coverage can be checked against real programs.
//
// Operand widths are taken from the VM handlers (the authoritative reader).
// walkChunk() self-checks: a wrong width desynchronises the stream and lands on
// an invalid opcode, which is reported instead of silently mis-decoding.

#include <cstdio>
#include <string>
#include "../vm/chunk.hpp"

namespace Lovax {
namespace Jit {

inline const char* opName(Op op) {
    switch (op) {
        case Op::CONST: return "CONST"; case Op::NIL: return "NIL";
        case Op::TRUE_: return "TRUE_"; case Op::FALSE_: return "FALSE_";
        case Op::POP: return "POP"; case Op::DUP: return "DUP";
        case Op::GET_LOCAL: return "GET_LOCAL"; case Op::SET_LOCAL: return "SET_LOCAL";
        case Op::GET_GLOBAL: return "GET_GLOBAL"; case Op::DEFINE_GLOBAL: return "DEFINE_GLOBAL";
        case Op::SET_GLOBAL: return "SET_GLOBAL"; case Op::GET_UPVALUE: return "GET_UPVALUE";
        case Op::SET_UPVALUE: return "SET_UPVALUE";
        case Op::EQUAL: return "EQUAL"; case Op::NOT_EQUAL: return "NOT_EQUAL";
        case Op::ADD: return "ADD"; case Op::SUB: return "SUB"; case Op::MUL: return "MUL";
        case Op::DIV: return "DIV"; case Op::MOD: return "MOD"; case Op::POW: return "POW";
        case Op::LESS: return "LESS"; case Op::GREATER: return "GREATER";
        case Op::LESS_EQ: return "LESS_EQ"; case Op::GREATER_EQ: return "GREATER_EQ";
        case Op::BIT_AND: return "BIT_AND"; case Op::BIT_OR: return "BIT_OR";
        case Op::BIT_XOR: return "BIT_XOR"; case Op::SHL: return "SHL"; case Op::SHR: return "SHR";
        case Op::IN: return "IN"; case Op::NEGATE: return "NEGATE"; case Op::NOT_: return "NOT_";
        case Op::BIT_NOT: return "BIT_NOT";
        case Op::JUMP: return "JUMP"; case Op::JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case Op::LOOP: return "LOOP"; case Op::AND_KEEP: return "AND_KEEP";
        case Op::OR_KEEP: return "OR_KEEP";
        case Op::CALL: return "CALL"; case Op::CALL_METHOD: return "CALL_METHOD";
        case Op::CLOSURE: return "CLOSURE"; case Op::ARG_DEFAULT: return "ARG_DEFAULT";
        case Op::RETURN: return "RETURN";
        case Op::LIST: return "LIST"; case Op::MAP: return "MAP";
        case Op::INDEX_GET: return "INDEX_GET"; case Op::INDEX_GET_KEEP: return "INDEX_GET_KEEP";
        case Op::INDEX_SET: return "INDEX_SET";
        case Op::MEMBER_GET: return "MEMBER_GET"; case Op::MEMBER_GET_SAFE: return "MEMBER_GET_SAFE";
        case Op::MEMBER_GET_KEEP: return "MEMBER_GET_KEEP"; case Op::MEMBER_SET: return "MEMBER_SET";
        case Op::INTERP: return "INTERP"; case Op::SAY: return "SAY";
        case Op::FOR_SETUP: return "FOR_SETUP"; case Op::FOR_NEXT: return "FOR_NEXT";
        case Op::CLOSE_UPVALUE: return "CLOSE_UPVALUE";
        case Op::USE: return "USE"; case Op::RUNTIME_ERROR: return "RUNTIME_ERROR";
        case Op::TRY_PUSH: return "TRY_PUSH"; case Op::TRY_POP: return "TRY_POP";
        case Op::THROW_: return "THROW_"; case Op::COALESCE: return "COALESCE";
        case Op::RANGE_NEW: return "RANGE_NEW"; case Op::IS_TYPE: return "IS_TYPE";
        case Op::UNPACK: return "UNPACK"; case Op::SLICE: return "SLICE";
        case Op::ADD_I: return "ADD_I"; case Op::SUB_I: return "SUB_I";
        case Op::MUL_I: return "MUL_I"; case Op::MOD_I: return "MOD_I";
        case Op::BAND_I: return "BAND_I"; case Op::BOR_I: return "BOR_I";
        case Op::BXOR_I: return "BXOR_I";
        case Op::LESS_JF: return "LESS_JF"; case Op::LESS_EQ_JF: return "LESS_EQ_JF";
        case Op::GREATER_JF: return "GREATER_JF"; case Op::GREATER_EQ_JF: return "GREATER_EQ_JF";
        case Op::EQUAL_JF: return "EQUAL_JF"; case Op::NOT_EQUAL_JF: return "NOT_EQUAL_JF";
        case Op::LGET2: return "LGET2";
        case Op::LGET_ADD_I: return "LGET_ADD_I"; case Op::LGET_SUB_I: return "LGET_SUB_I";
        case Op::ADD_INPLACE: return "ADD_INPLACE";
        case Op::LT_I_JF: return "LT_I_JF"; case Op::LE_I_JF: return "LE_I_JF";
        case Op::GT_I_JF: return "GT_I_JF"; case Op::GE_I_JF: return "GE_I_JF";
        case Op::EQ_I_JF: return "EQ_I_JF"; case Op::NE_I_JF: return "NE_I_JF";
        case Op::YIELD_: return "YIELD_";
        case Op::STRUCT_SHAPE: return "STRUCT_SHAPE"; case Op::STRUCT_BIND: return "STRUCT_BIND";
        case Op::STRUCT_MAKE: return "STRUCT_MAKE"; case Op::TUPLE: return "TUPLE";
        case Op::HALT: return "HALT";
    }
    return "?";
}

// Operand layout: how many u16 fields follow, and whether a leading u8 does.
// (CLOSURE and STRUCT_SHAPE are variable-length and handled separately.)
struct OpInfo { int u8s; int u16s; };
inline OpInfo opOperands(Op op) {
    switch (op) {
        // no operands
        case Op::NIL: case Op::TRUE_: case Op::FALSE_: case Op::POP: case Op::DUP:
        case Op::EQUAL: case Op::NOT_EQUAL: case Op::ADD: case Op::SUB: case Op::MUL:
        case Op::DIV: case Op::MOD: case Op::POW: case Op::LESS: case Op::GREATER:
        case Op::LESS_EQ: case Op::GREATER_EQ: case Op::BIT_AND: case Op::BIT_OR:
        case Op::BIT_XOR: case Op::SHL: case Op::SHR: case Op::IN: case Op::NEGATE:
        case Op::NOT_: case Op::BIT_NOT: case Op::RETURN: case Op::INDEX_GET:
        case Op::INDEX_GET_KEEP: case Op::INDEX_SET: case Op::FOR_SETUP:
        case Op::TRY_POP: case Op::THROW_: case Op::RANGE_NEW: case Op::IS_TYPE:
        case Op::SLICE: case Op::ADD_INPLACE: case Op::YIELD_: case Op::STRUCT_BIND:
        case Op::HALT:
            return {0, 0};
        // one u8
        case Op::CALL: case Op::CALL_METHOD: case Op::SAY:
            return {1, 0};
        // one u16
        case Op::CONST: case Op::GET_LOCAL: case Op::SET_LOCAL: case Op::GET_GLOBAL:
        case Op::DEFINE_GLOBAL: case Op::SET_GLOBAL: case Op::GET_UPVALUE:
        case Op::SET_UPVALUE: case Op::JUMP: case Op::JUMP_IF_FALSE: case Op::LOOP:
        case Op::AND_KEEP: case Op::OR_KEEP: case Op::LIST: case Op::MAP:
        case Op::TUPLE: case Op::INTERP: case Op::CLOSE_UPVALUE: case Op::USE:
        case Op::RUNTIME_ERROR: case Op::TRY_PUSH: case Op::COALESCE: case Op::UNPACK:
        case Op::STRUCT_MAKE:
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF:
            return {0, 1};
        // two u16
        case Op::ARG_DEFAULT: case Op::MEMBER_GET: case Op::MEMBER_GET_SAFE:
        case Op::MEMBER_GET_KEEP: case Op::MEMBER_SET: case Op::LGET2:
        case Op::LGET_ADD_I: case Op::LGET_SUB_I:
        case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF: case Op::GE_I_JF:
        case Op::EQ_I_JF: case Op::NE_I_JF:
            return {0, 2};
        // u8 flags + 3 u16 (var1, var2, exit)
        case Op::FOR_NEXT:
            return {1, 3};
        // variable-length: caller handles
        case Op::CLOSURE: case Op::STRUCT_SHAPE:
            return {-1, -1};
    }
    return {-1, -1};
}

// Returns the byte length of the instruction at `off`, or 0 if unknown/variable
// in a way the caller must resolve. `upvalCount` is needed for CLOSURE.
inline int instrLength(const Chunk& c, size_t off, int upvalCountForClosure = 0) {
    Op op = (Op)c.code[off];
    if (op == Op::CLOSURE) return 1 + 2 + upvalCountForClosure * 3;
    if (op == Op::STRUCT_SHAPE) {
        // u16 name, u16 nFields, nFields*u16, u16 nMethods, nMethods*u16
        size_t p = off + 1;
        auto rd = [&](size_t at) { return (int)((c.code[at] << 8) | c.code[at + 1]); };
        p += 2;
        int nf = rd(p); p += 2 + nf * 2;
        int nm = rd(p); p += 2 + nm * 2;
        return (int)(p - off);
    }
    OpInfo in = opOperands(op);
    if (in.u8s < 0) return 0;
    return 1 + in.u8s + in.u16s * 2;
}

inline void disassemble(const Chunk& c, const std::string& name, FILE* out = stdout) {
    std::fprintf(out, "== %s (%zu bytes) ==\n", name.c_str(), c.code.size());
    size_t off = 0;
    while (off < c.code.size()) {
        Op op = (Op)c.code[off];
        int len = instrLength(c, off);
        std::fprintf(out, "%5zu  %-16s", off, opName(op));
        if (len == 0) { std::fprintf(out, "  <variable/unknown>\n"); break; }
        OpInfo in = opOperands(op);
        size_t p = off + 1;
        if (in.u8s == 1) { std::fprintf(out, " u8=%d", c.code[p]); p += 1; }
        for (int i = 0; i < in.u16s; ++i) {
            int v = (c.code[p] << 8) | c.code[p + 1];
            std::fprintf(out, " %d", v);
            p += 2;
        }
        // annotate branch targets
        if (op == Op::JUMP || op == Op::JUMP_IF_FALSE || op == Op::AND_KEEP ||
            op == Op::OR_KEEP || op == Op::COALESCE || op == Op::TRY_PUSH) {
            int d = (c.code[off + 1] << 8) | c.code[off + 2];
            std::fprintf(out, "   -> %zu", off + len + d);
        } else if (op == Op::LOOP) {
            int d = (c.code[off + 1] << 8) | c.code[off + 2];
            std::fprintf(out, "   -> %zu (back)", off + len - d);
        }
        std::fprintf(out, "\n");
        off += len;
    }
    if (off != c.code.size())
        std::fprintf(out, "  !! stream desynchronised at %zu (expected %zu)\n",
                     off, c.code.size());
}

} // namespace Jit
} // namespace Lovax

#endif // LOVAX_JIT_DISASM_HPP
