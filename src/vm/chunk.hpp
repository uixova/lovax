#ifndef CHUNK_HPP
#define CHUNK_HPP

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include "value.hpp"

// Bytecode container and the runtime objects owned by the VM:
// function prototypes, closures, upvalues, and internal loop iterators.

namespace Lovax {

enum class Op : uint8_t {
    CONST,          // u16 const index -> push
    NIL, TRUE_, FALSE_,
    POP,
    DUP,

    GET_LOCAL,      // u16 slot
    SET_LOCAL,      // u16 slot (pops)
    GET_GLOBAL,     // u16 global slot (runtime-checked: may be undefined)
    DEFINE_GLOBAL,  // u16 global slot (pops; marks defined)
    SET_GLOBAL,     // u16 global slot (pops; error if not defined — RFC-001 bare assignment)
    GET_UPVALUE,    // u16 index
    SET_UPVALUE,    // u16 index (pops)

    EQUAL, NOT_EQUAL,          // deep equality, any types
    ADD, SUB, MUL, DIV, MOD, POW,
    LESS, GREATER, LESS_EQ, GREATER_EQ,
    BIT_AND, BIT_OR, BIT_XOR, SHL, SHR,
    IN,                         // membership
    NEGATE, NOT_, BIT_NOT,

    JUMP,           // u16 forward offset
    JUMP_IF_FALSE,  // u16 (pops condition)
    LOOP,           // u16 backward offset
    AND_KEEP,       // u16: if falsy jump keeping value, else pop (Python 'and' returns operand)
    OR_KEEP,        // u16: if truthy jump keeping value, else pop

    CALL,           // u8 argc (callee below args on stack)
    CALL_METHOD,    // u8 argc: [recv, method, args..]; passes recv as 'this' iff struct instance
    CLOSURE,        // u16 proto const index, then per-upvalue: u8 isLocal, u16 index
    ARG_DEFAULT,    // u16 param slot, u16 jump offset: skip default expr if arg was provided
    RETURN,         // pops result, closes upvalues, unwinds frame

    LIST,           // u16 element count -> builds a list
    MAP,            // u16 pair count -> builds a map (keys validated)
    INDEX_GET,      // obj idx -> value
    INDEX_GET_KEEP, // obj idx -> obj idx value (for compound index assignment)
    INDEX_SET,      // obj idx value -> (pops all three)
    MEMBER_GET,     // u16 name const: obj -> value
    MEMBER_GET_SAFE,// u16 name const: obj -> value (null if obj is null)
    MEMBER_GET_KEEP,// u16 name const: obj -> obj value
    MEMBER_SET,     // u16 name const: obj value -> (pops both)
    INTERP,         // u16 part count -> concatenated string
    SAY,            // u8 value count (pops, prints)

    FOR_SETUP,      // iterable -> iterator object (validates type)
    FOR_NEXT,       // u8 flags(2=pair), u16 var1, u16 var2, u16 exit offset
    CLOSE_UPVALUE,  // u16 slot: close open upvalues >= slot (per-iteration loop capture)

    USE,            // u16 use-spec index (module import, binds globals)
    RUNTIME_ERROR,  // u16 message const index ('break' outside loop etc., kept as runtime errors)

    TRY_PUSH,       // u16 forward offset to the catch target (RFC-008)
    TRY_POP,        // discards the innermost handler (normal try exit)
    THROW_,         // pops a value, raises it as a runtime error
    COALESCE,       // u16: if top is not null jump (keep it), else pop and eval rhs
    RANGE_NEW,      // pops end,start (ints) -> pushes range(start, end)
    IS_TYPE,        // pops type-name string and value -> bool
    UNPACK,         // u16 n: pops a list of exactly n items, pushes them in order
    SLICE,          // pops end,start,obj (nil = default) -> slice of list/string

    // Fused superinstructions (emitted by the compiler's peephole; RFC v0.8):
    ADD_I, SUB_I, MUL_I, MOD_I,   // i16 imm: top op= imm (int/float fast, generic fallback)
    BAND_I, BOR_I, BXOR_I,        // i16 imm: int-only bitwise with immediate
    LESS_JF, LESS_EQ_JF,          // u16 off: compare two stack values, jump if NOT true
    GREATER_JF, GREATER_EQ_JF,
    EQUAL_JF, NOT_EQUAL_JF,
    LGET2,                        // u16 a, u16 b: push two locals in one dispatch
    LGET_ADD_I, LGET_SUB_I,       // u16 slot, i16 imm: push local +/- imm (n-1 pattern)
    ADD_INPLACE,                  // like ADD, but emitted only for 't += e' / 't = t + e':
                                  // a uniquely-referenced string appends in place (no copy)
    LT_I_JF, LE_I_JF,             // i16 imm, u16 off: pop top, compare vs imm, jump if NOT true
    GT_I_JF, GE_I_JF,
    EQ_I_JF, NE_I_JF,
    YIELD_,                       // pop a value, suspend the coroutine, hand it to resume()

    // Compact structs (RFC-017):
    STRUCT_SHAPE,   // u16 name const, u16 nFields, nFields×u16 name consts,
                    // u16 nMethods, nMethods×u16 name consts; pops nMethods
                    // closures, pushes the built StructShapeObject
    STRUCT_BIND,    // pops factory closure and shape, attaches shape, pushes factory
    STRUCT_MAKE,    // u16 nFields: builds an instance from locals[0..n), pushes it

    HALT            // end of the top-level script
};

// Module import request compiled from a 'use' statement.
struct UseSpec {
    bool isFile = false;
    std::string target;
    std::string bindName;              // module binding (empty for selective import)
    uint16_t bindSlot = 0;             // global slot for the module object
    std::vector<std::string> names;    // selective names
    std::vector<uint16_t> nameSlots;   // their global slots
};

struct Chunk {
    std::vector<uint8_t> code;
    std::vector<int> lines;            // source line per byte (for error messages)
    std::vector<Value> consts;
    std::vector<UseSpec> useSpecs;
    // Inline-cache slots for member access sites (one per MEMBER_* instruction).
    // Mutated by the VM at run time; a hit is verified against the live entry,
    // so a stale index can only miss, never return a wrong field.
    mutable std::vector<uint32_t> icache;

    uint16_t addIC() {
        icache.push_back(0xFFFFFFFFu);
        return (uint16_t)(icache.size() - 1);
    }

    void emit(uint8_t byte, int line) {
        code.push_back(byte);
        lines.push_back(line);
    }
    void emitOp(Op op, int line) { emit((uint8_t)op, line); }
    void emitU16(uint16_t v, int line) {
        emit((uint8_t)(v >> 8), line);
        emit((uint8_t)(v & 0xFF), line);
    }
    int addConst(Value v) {
        consts.push_back(std::move(v));
        return (int)consts.size() - 1;
    }
};

// Compiled function prototype (shared, immutable after compilation).
struct Proto {
    std::string name;                  // for error messages ("" = anonymous)
    int paramCount = 0;
    int requiredCount = 0;             // params without defaults
    int localCount = 0;                // params + collected locals (stack reservation)
    int upvalueCount = 0;
    bool variadic = false;             // last parameter collects extra args as a list
    Chunk chunk;
};

// Wraps a Proto so it can live in a chunk's constant table (constants are Values).
class ProtoObject : public Object {
public:
    std::shared_ptr<Proto> proto;
    ProtoObject(std::shared_ptr<Proto> p)
        : Object(ObjectType::RETURN_VALUE), proto(std::move(p)) {}
    void gcMark() override { for (auto& c : proto->chunk.consts) gcMarkValue(c); }
    std::string inspect() const override { return "<proto>"; }
};

// An upvalue: points at a stack slot while open; owns the value once closed.
class UpvalueCell {
public:
    int stackSlot;                     // absolute stack index while open
    bool closed = false;
    Value value;                       // holds the value after closing
    UpvalueCell(int slot) : stackSlot(slot) {}
};

// Runtime closure: a proto plus captured upvalues. Callable like a builtin.
class ClosureObject : public Object {
public:
    std::shared_ptr<Proto> proto;
    std::vector<std::shared_ptr<UpvalueCell>> upvalues;
    // Non-null for functions that belong to an imported file module: their
    // GET_GLOBAL/SET_GLOBAL slots resolve against the MODULE's global table (kept
    // alive for the program), not the caller's — so a module function can read
    // its own module-level state no matter which VM invokes it. Borrowed pointer
    // (the module VM is kept alive), so no ownership cycle / leak.
    std::vector<Value>* moduleGlobals = nullptr;
    // Non-null only for struct factory closures: the shared shape that
    // STRUCT_MAKE attaches to every instance this factory builds (RFC-017).
    Ref<Object> structShape;

    ClosureObject(std::shared_ptr<Proto> p)
        : Object(ObjectType::FUNCTION), proto(std::move(p)) {}
    void gcMark() override {
        for (auto& c : proto->chunk.consts) gcMarkValue(c);
        // Closed upvalues own their Value; open ones live on the stack (a root),
        // and their cell->value is a harmless default until closed.
        for (auto& u : upvalues) if (u) gcMarkValue(u->value);
        gcMarkObject(structShape.get());
    }
    size_t gcBytes() const override {
        // shared_ptr control block + cell per captured upvalue, roughly
        return sizeof(*this) + upvalues.capacity() * 72;
    }
    std::string inspect() const override {
        return "fn " + (proto->name.empty() ? "?" : proto->name) + "(...)";
    }
};

// Internal iterator state for for-loops (never user-visible).
class IterObject : public Object {
public:
    IterObject() : Object(ObjectType::RETURN_VALUE) {}
    enum class Kind { LIST, RANGE, STRING, MAP_KEYS } kind;
    Ref<Object> source;                 // list/range/string source
    std::vector<Ref<Object>> snapshot;  // map keys snapshot
    long long index = 0;                            // element index / range value
    size_t bytePos = 0;                             // UTF-8 position for strings
    void gcMark() override {
        gcMarkObject(source.get());
        for (auto& s : snapshot) gcMarkObject(s.get());
    }
    std::string inspect() const override { return "<iterator>"; }
};

} // namespace Lovax

#endif // CHUNK_HPP
