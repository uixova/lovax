#ifndef COMPILER_HPP
#define COMPILER_HPP

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include "../ast/ast.hpp"
#include "chunk.hpp"

// AST -> bytecode compiler.
// Locals are function-scoped (Lume blocks share their function's scope) and are
// pre-allocated in a collection pass, so mid-loop 'set' never unbalances the stack.
// Closures use the classic open/closed upvalue model.

namespace Lume {

// Per-compilation-unit global registry (the script and each module get their own).
struct GlobalTable {
    std::unordered_map<std::string, uint16_t> index;
    std::vector<std::string> names;

    uint16_t slot(const std::string& name) {
        auto it = index.find(name);
        if (it != index.end()) return it->second;
        uint16_t s = (uint16_t)names.size();
        index[name] = s;
        names.push_back(name);
        return s;
    }
};

class Compiler {
public:
    Compiler(GlobalTable& globals) : globals_(globals) {}

    // Compiles a whole program into the top-level "<script>" proto.
    std::shared_ptr<Proto> compileProgram(const Program* program) {
        FnCtx script;
        script.proto = std::make_shared<Proto>();
        script.proto->name = "<script>";
        script.enclosing = nullptr;
        script.isScript = true;
        ctx_ = &script;

        for (const auto& stmt : program->statements) {
            compileStatement(stmt.get());
        }
        emitOp(Op::HALT, lastLine_);
        return script.proto;
    }

private:
    struct Local { std::string name; };
    struct UpvalDesc { bool isLocal; uint16_t index; };
    struct LoopCtx {
        int continueTarget;            // byte offset to LOOP back to
        std::vector<int> breakJumps;   // JUMP operand offsets to patch at loop end
        int stackPayload;              // values the loop keeps on the stack (for: iterator)
    };
    struct FnCtx {
        std::shared_ptr<Proto> proto;
        std::vector<Local> locals;
        std::vector<UpvalDesc> upvals;
        std::vector<LoopCtx> loops;
        FnCtx* enclosing = nullptr;
        bool isScript = false;
    };

    // Constant names per compilation unit (script + each function share this set;
    // const is a compile-time contract, so a global name marked const stays const).
    std::unordered_set<std::string> constNames_;
    int hiddenCounter_ = 0;

    GlobalTable& globals_;
    FnCtx* ctx_ = nullptr;
    int lastLine_ = 0;

    Chunk& chunk() { return ctx_->proto->chunk; }
    void emitOp(Op op, int line) { chunk().emitOp(op, line); lastLine_ = line; }
    void emitU16(uint16_t v, int line) { chunk().emitU16(v, line); }
    void emitU8(uint8_t v, int line) { chunk().emit(v, line); }

    uint16_t addConst(Value v) { return (uint16_t)chunk().addConst(std::move(v)); }
    uint16_t strConst(const std::string& s) {
        return addConst(Value::object(std::make_shared<StringObject>(s)));
    }

    void emitConst(Value v, int line) {
        emitOp(Op::CONST, line);
        emitU16(addConst(std::move(v)), line);
    }

    // Emits a forward jump; returns the operand offset to patch later.
    int emitJump(Op op, int line) {
        emitOp(op, line);
        emitU16(0xFFFF, line);
        return (int)chunk().code.size() - 2;
    }
    void patchJump(int operandAt) {
        int distance = (int)chunk().code.size() - (operandAt + 2);
        chunk().code[operandAt]     = (uint8_t)(distance >> 8);
        chunk().code[operandAt + 1] = (uint8_t)(distance & 0xFF);
    }
    void emitLoop(int targetOffset, int line) {
        emitOp(Op::LOOP, line);
        int distance = (int)chunk().code.size() + 2 - targetOffset;
        emitU16((uint16_t)distance, line);
    }

    void emitRuntimeError(const std::string& msg, int line) {
        emitOp(Op::RUNTIME_ERROR, line);
        emitU16(strConst(msg), line);
    }

    // ===== Name resolution =====

    int resolveLocal(FnCtx* ctx, const std::string& name) {
        for (int i = (int)ctx->locals.size() - 1; i >= 0; --i) {
            if (ctx->locals[i].name == name) return i;
        }
        return -1;
    }

    int addUpvalue(FnCtx* ctx, bool isLocal, uint16_t index) {
        for (size_t i = 0; i < ctx->upvals.size(); ++i) {
            if (ctx->upvals[i].isLocal == isLocal && ctx->upvals[i].index == index) {
                return (int)i;
            }
        }
        ctx->upvals.push_back({isLocal, index});
        ctx->proto->upvalueCount = (int)ctx->upvals.size();
        return (int)ctx->upvals.size() - 1;
    }

    int resolveUpvalue(FnCtx* ctx, const std::string& name) {
        if (ctx->enclosing == nullptr || ctx->enclosing->isScript) return -1;
        int local = resolveLocal(ctx->enclosing, name);
        if (local != -1) return addUpvalue(ctx, true, (uint16_t)local);
        int up = resolveUpvalue(ctx->enclosing, name);
        if (up != -1) return addUpvalue(ctx, false, (uint16_t)up);
        return -1;
    }

    // ===== Local collection pass =====
    // Gathers every function-scoped name (params, set, for variables, use bindings)
    // so slots exist before any code runs. Does not descend into nested functions.

    void collectLocals(const Statement* stmt, std::vector<std::string>& out) {
        auto add = [&](const std::string& n) {
            for (const auto& e : out) if (e == n) return;
            out.push_back(n);
        };
        switch (stmt->nodeType()) {
            case NodeType::SET_STATEMENT: {
                const auto* ss = static_cast<const SetStatement*>(stmt);
                add(ss->name->value);
                for (const auto& e : ss->extraNames) add(e->value);
                break;
            }
            case NodeType::EXPRESSION_STATEMENT: {
                // A named nested 'fn' binds its own name in this scope.
                const auto* e = static_cast<const ExpressionStatement*>(stmt);
                if (e->expression->nodeType() == NodeType::FUNCTION_LITERAL) {
                    const auto* f = static_cast<const FunctionLiteral*>(e->expression.get());
                    if (f->name) add(f->name->value);
                }
                break;
            }
            case NodeType::FOR_STATEMENT: {
                const auto* f = static_cast<const ForStatement*>(stmt);
                add(f->variable->value);
                if (f->variable2) add(f->variable2->value);
                for (const auto& s : f->body->statements) collectLocals(s.get(), out);
                break;
            }
            case NodeType::REPEAT_STATEMENT: {
                const auto* r = static_cast<const RepeatStatement*>(stmt);
                for (const auto& s : r->body->statements) collectLocals(s.get(), out);
                break;
            }
            case NodeType::TRY_STATEMENT: {
                const auto* t = static_cast<const TryStatement*>(stmt);
                for (const auto& s : t->tryBlock->statements) collectLocals(s.get(), out);
                add(t->catchName->value);
                for (const auto& s : t->catchBlock->statements) collectLocals(s.get(), out);
                break;
            }
            case NodeType::WHILE_STATEMENT: {
                const auto* w = static_cast<const WhileStatement*>(stmt);
                for (const auto& s : w->body->statements) collectLocals(s.get(), out);
                break;
            }
            case NodeType::IF_STATEMENT: {
                const auto* i = static_cast<const IfStatement*>(stmt);
                for (const auto& s : i->consequence->statements) collectLocals(s.get(), out);
                if (i->alternative) collectLocals(i->alternative.get(), out);
                break;
            }
            case NodeType::MATCH_STATEMENT: {
                const auto* m = static_cast<const MatchStatement*>(stmt);
                for (const auto& c : m->cases) {
                    for (const auto& s : c.body->statements) collectLocals(s.get(), out);
                }
                break;
            }
            case NodeType::BLOCK_STATEMENT: {
                const auto* b = static_cast<const BlockStatement*>(stmt);
                for (const auto& s : b->statements) collectLocals(s.get(), out);
                break;
            }
            // 'use' binds globals even inside functions, so it allocates no locals.
            default:
                break;
        }
    }

    // ===== Statements =====

    void compileStatement(const Statement* stmt) {
        switch (stmt->nodeType()) {
            case NodeType::SET_STATEMENT:
                compileSet(static_cast<const SetStatement*>(stmt));
                break;
            case NodeType::PASS_STATEMENT:
                break; // no-op
            case NodeType::THROW_STATEMENT: {
                const auto* t = static_cast<const ThrowStatement*>(stmt);
                compileExpression(t->value.get());
                emitOp(Op::THROW_, t->token.line);
                break;
            }
            case NodeType::TRY_STATEMENT:
                compileTry(static_cast<const TryStatement*>(stmt));
                break;
            case NodeType::REPEAT_STATEMENT:
                compileRepeat(static_cast<const RepeatStatement*>(stmt));
                break;
            case NodeType::ENUM_STATEMENT:
                compileEnum(static_cast<const EnumStatement*>(stmt));
                break;
            case NodeType::STRUCT_STATEMENT:
                compileStruct(static_cast<const StructStatement*>(stmt));
                break;
            case NodeType::ASSIGN_STATEMENT:
                compileAssign(static_cast<const AssignStatement*>(stmt));
                break;
            case NodeType::SAY_STATEMENT: {
                const auto* s = static_cast<const SayStatement*>(stmt);
                for (const auto& v : s->values) compileExpression(v.get());
                emitOp(Op::SAY, s->token.line);
                emitU8((uint8_t)s->values.size(), s->token.line);
                break;
            }
            case NodeType::IF_STATEMENT: {
                const auto* s = static_cast<const IfStatement*>(stmt);
                compileExpression(s->condition.get());
                int elseJump = emitJump(Op::JUMP_IF_FALSE, s->token.line);
                compileBlock(s->consequence.get());
                if (s->alternative) {
                    int endJump = emitJump(Op::JUMP, s->token.line);
                    patchJump(elseJump);
                    compileStatement(s->alternative.get()); // block or chained if
                    patchJump(endJump);
                } else {
                    patchJump(elseJump);
                }
                break;
            }
            case NodeType::BLOCK_STATEMENT:
                compileBlock(static_cast<const BlockStatement*>(stmt));
                break;
            case NodeType::MATCH_STATEMENT:
                compileMatch(static_cast<const MatchStatement*>(stmt));
                break;
            case NodeType::WHILE_STATEMENT: {
                const auto* s = static_cast<const WhileStatement*>(stmt);
                int loopStart = (int)chunk().code.size();
                ctx_->loops.push_back({loopStart, {}, 0});
                compileExpression(s->condition.get());
                if (s->untilForm) emitOp(Op::NOT_, s->token.line); // until = while not
                int exitJump = emitJump(Op::JUMP_IF_FALSE, s->token.line);
                compileBlock(s->body.get());
                emitLoop(loopStart, s->token.line);
                patchJump(exitJump);
                for (int bj : ctx_->loops.back().breakJumps) patchJump(bj);
                ctx_->loops.pop_back();
                break;
            }
            case NodeType::FOR_STATEMENT:
                compileFor(static_cast<const ForStatement*>(stmt));
                break;
            case NodeType::BREAK_STATEMENT: {
                int line = stmt->line();
                if (ctx_->loops.empty()) {
                    emitRuntimeError("'break' cannot be used outside a loop", line);
                    break;
                }
                for (int i = 0; i < ctx_->loops.back().stackPayload; ++i) emitOp(Op::POP, line);
                ctx_->loops.back().breakJumps.push_back(emitJump(Op::JUMP, line));
                break;
            }
            case NodeType::CONTINUE_STATEMENT: {
                int line = stmt->line();
                if (ctx_->loops.empty()) {
                    emitRuntimeError("'continue' cannot be used outside a loop", line);
                    break;
                }
                emitLoop(ctx_->loops.back().continueTarget, line);
                break;
            }
            case NodeType::RETURN_STATEMENT: {
                const auto* s = static_cast<const ReturnStatement*>(stmt);
                if (ctx_->isScript) {
                    emitRuntimeError("'return' cannot be used outside a function", s->token.line);
                    break;
                }
                if (s->returnValue) compileExpression(s->returnValue.get());
                else emitOp(Op::NIL, s->token.line);
                emitOp(Op::RETURN, s->token.line);
                break;
            }
            case NodeType::USE_STATEMENT:
                compileUse(static_cast<const UseStatement*>(stmt));
                break;
            case NodeType::EXPRESSION_STATEMENT: {
                const auto* s = static_cast<const ExpressionStatement*>(stmt);
                compileExpression(s->expression.get());
                emitOp(Op::POP, s->token.line);
                break;
            }
            default:
                break;
        }
    }

    void compileBlock(const BlockStatement* block) {
        for (const auto& s : block->statements) compileStatement(s.get());
    }

    // 'set' semantics: define a global at top level, a (pre-allocated) local in functions.
    void defineName(const std::string& name, int line) {
        if (ctx_->isScript) {
            emitOp(Op::DEFINE_GLOBAL, line);
            emitU16(globals_.slot(name), line);
        } else {
            int slot = resolveLocal(ctx_, name);
            // The collection pass guarantees the slot exists.
            emitOp(Op::SET_LOCAL, line);
            emitU16((uint16_t)slot, line);
        }
    }

    void markConst(const std::string& name) { constNames_.insert(name); }

    // Hidden compiler-generated temporaries (repeat counter): reuse a bank of
    // reserved local slots that no user name can collide with.
    int declareHidden(int line) {
        (void)line;
        std::string hn = "\x01hidden" + std::to_string(hiddenCounter_++);
        if (ctx_->isScript) return (int)globals_.slot(hn);
        int slot = resolveLocal(ctx_, hn);
        if (slot == -1) { ctx_->locals.push_back({hn}); slot = (int)ctx_->locals.size() - 1; }
        return slot;
    }
    void emitStoreHidden(int slot, int line) {
        if (ctx_->isScript) { emitOp(Op::DEFINE_GLOBAL, line); emitU16((uint16_t)slot, line); }
        else { emitOp(Op::SET_LOCAL, line); emitU16((uint16_t)slot, line); }
    }
    void emitLoadHidden(int slot, int line) {
        if (ctx_->isScript) { emitOp(Op::GET_GLOBAL, line); emitU16((uint16_t)slot, line); }
        else { emitOp(Op::GET_LOCAL, line); emitU16((uint16_t)slot, line); }
    }

    void compileAssign(const AssignStatement* s) {
        int line = s->token.line;
        // const contract: reassigning a const name is a compile-time detected error.
        if (s->target->nodeType() == NodeType::IDENTIFIER) {
            const auto* id = static_cast<const Identifier*>(s->target.get());
            if (constNames_.count(id->value)) {
                emitRuntimeError("cannot assign to constant '" + id->value + "'", line);
                return;
            }
        }
        std::string binOp;
        if (s->op != "=") binOp = s->op.substr(0, s->op.size() - 1); // "+=" -> "+", "**=" safe too

        if (s->target->nodeType() == NodeType::IDENTIFIER) {
            const auto* ident = static_cast<const Identifier*>(s->target.get());
            if (binOp == "??") { // ??= : assign only when currently null
                emitNameGet(ident->value, line);
                int j = emitJump(Op::COALESCE, line);
                compileExpression(s->value.get());
                patchJump(j);
                emitNameSet(ident->value, line);
                return;
            }
            if (!binOp.empty()) {
                emitNameGet(ident->value, line);
                compileExpression(s->value.get());
                emitCompound(binOp, line);
            } else {
                compileExpression(s->value.get());
            }
            emitNameSet(ident->value, line);
            return;
        }

        if (s->target->nodeType() == NodeType::INDEX_EXPRESSION) {
            const auto* idx = static_cast<const IndexExpression*>(s->target.get());
            compileExpression(idx->object.get());
            compileExpression(idx->index.get());
            if (!binOp.empty()) {
                emitOp(Op::INDEX_GET_KEEP, line);       // obj idx -> obj idx old
                compileExpression(s->value.get());
                emitCompound(binOp, line);              // obj idx new
            } else {
                compileExpression(s->value.get());
            }
            emitOp(Op::INDEX_SET, line);
            return;
        }

        // Member target: object.field = value
        const auto* mem = static_cast<const MemberExpression*>(s->target.get());
        uint16_t nameC = strConst(mem->property);
        compileExpression(mem->object.get());
        if (!binOp.empty()) {
            emitOp(Op::MEMBER_GET_KEEP, line);           // obj -> obj old
            emitU16(nameC, line);
            compileExpression(s->value.get());
            emitCompound(binOp, line);                  // obj new
        } else {
            compileExpression(s->value.get());
        }
        emitOp(Op::MEMBER_SET, line);
        emitU16(nameC, line);
    }

    void emitNameGet(const std::string& name, int line) {
        if (!ctx_->isScript) {
            int slot = resolveLocal(ctx_, name);
            if (slot != -1) {
                emitOp(Op::GET_LOCAL, line);
                emitU16((uint16_t)slot, line);
                return;
            }
            int up = resolveUpvalue(ctx_, name);
            if (up != -1) {
                emitOp(Op::GET_UPVALUE, line);
                emitU16((uint16_t)up, line);
                return;
            }
        }
        emitOp(Op::GET_GLOBAL, line);
        emitU16(globals_.slot(name), line);
    }

    void emitNameSet(const std::string& name, int line) {
        if (!ctx_->isScript) {
            int slot = resolveLocal(ctx_, name);
            if (slot != -1) {
                emitOp(Op::SET_LOCAL, line);
                emitU16((uint16_t)slot, line);
                return;
            }
            int up = resolveUpvalue(ctx_, name);
            if (up != -1) {
                emitOp(Op::SET_UPVALUE, line);
                emitU16((uint16_t)up, line);
                return;
            }
        }
        emitOp(Op::SET_GLOBAL, line);
        emitU16(globals_.slot(name), line);
    }

    void compileMatch(const MatchStatement* m) {
        int line = m->token.line;
        compileExpression(m->subject.get());   // subject stays on the stack

        std::vector<int> endJumps;
        for (const auto& c : m->cases) {
            if (c.isDefault) {
                emitOp(Op::POP, line);         // discard subject
                compileBlock(c.body.get());
                endJumps.push_back(emitJump(Op::JUMP, line));
                // Subsequent branches are unreachable but must still balance the
                // stack for the shared exit; compile them behind an always-jump.
                continue;
            }
            std::vector<int> bodyJumps;
            std::vector<int> nextPattern;
            for (size_t pi = 0; pi < c.patterns.size(); ++pi) {
                emitOp(Op::DUP, line);
                compileExpression(c.patterns[pi].get());
                emitOp(Op::EQUAL, line);
                if (pi + 1 < c.patterns.size()) {
                    // On match, short-circuit into the body.
                    int j = emitJump(Op::JUMP_IF_FALSE, line);
                    bodyJumps.push_back(emitJump(Op::JUMP, line));
                    patchJump(j);
                } else {
                    nextPattern.push_back(emitJump(Op::JUMP_IF_FALSE, line));
                }
            }
            for (int j : bodyJumps) patchJump(j);
            emitOp(Op::POP, line);             // discard subject before the body
            compileBlock(c.body.get());
            endJumps.push_back(emitJump(Op::JUMP, line));
            for (int j : nextPattern) patchJump(j);
        }
        emitOp(Op::POP, line);                 // no branch matched: discard subject
        for (int j : endJumps) patchJump(j);
    }

    // set/const with optional parallel targets and list unpacking
    void compileSet(const SetStatement* s) {
        int line = s->token.line;
        size_t targets = 1 + s->extraNames.size();

        if (targets == 1) {
            compileExpression(s->value.get());
            defineName(s->name->value, line);
            if (s->isConst) markConst(s->name->value);
            return;
        }

        if (s->extraValues.empty()) {
            // List unpacking: set a, b = some_list
            compileExpression(s->value.get());
            emitOp(Op::UNPACK, line);
            emitU16((uint16_t)targets, line);
        } else {
            // Parallel: set a, b = 1, 2 — push all values, then bind in reverse
            compileExpression(s->value.get());
            for (const auto& v : s->extraValues) compileExpression(v.get());
        }
        // Stack now holds the N values in order; bind last-to-first.
        std::vector<const Identifier*> names;
        names.push_back(s->name.get());
        for (const auto& e : s->extraNames) names.push_back(e.get());
        for (int i = (int)names.size() - 1; i >= 0; --i) {
            defineName(names[i]->value, line);
            if (s->isConst) markConst(names[i]->value);
        }
    }

    void compileTry(const TryStatement* t) {
        int line = t->token.line;
        int handlerJump = emitJump(Op::TRY_PUSH, line); // operand = catch target
        compileBlock(t->tryBlock.get());
        emitOp(Op::TRY_POP, line);
        int endJump = emitJump(Op::JUMP, line);
        patchJump(handlerJump);
        // Catch entry: the VM pushed the error message string; bind it.
        defineName(t->catchName->value, line);
        compileBlock(t->catchBlock.get());
        patchJump(endJump);
        // 'finally' runs on both the normal and the caught path (RFC-008).
        if (t->finallyBlock) compileBlock(t->finallyBlock.get());
    }

    // enum State: IDLE, WALK, ATTACK -> a map { "IDLE": 0, "WALK": 1, ... }
    void compileEnum(const EnumStatement* e) {
        int line = e->token.line;
        for (size_t i = 0; i < e->members.size(); ++i) {
            emitConst(Value::object(std::make_shared<StringObject>(e->members[i])), line);
            emitConst(Value::integer((long long)i), line);
        }
        emitOp(Op::MAP, line);
        emitU16((uint16_t)e->members.size(), line);
        defineName(e->name, line);
        markConst(e->name);
    }

    // struct Player: -> a factory closure that builds a tagged map instance.
    // Fields become factory parameters (defaults preserved); methods are stored
    // as closures whose implicit first parameter is 'this' (see CALL_METHOD).
    void compileStruct(const StructStatement* st) {
        int line = st->token.line;

        FnCtx fnCtx;
        fnCtx.proto = std::make_shared<Proto>();
        fnCtx.proto->name = st->name;
        fnCtx.proto->paramCount = (int)st->fields.size();
        fnCtx.enclosing = ctx_;
        fnCtx.isScript = false;

        int required = 0;
        for (const auto& d : st->fieldDefaults) if (d == nullptr) required++;
        fnCtx.proto->requiredCount = required;

        for (const auto& f : st->fields) fnCtx.locals.push_back({f});

        FnCtx* saved = ctx_;
        ctx_ = &fnCtx;

        // Field defaults (evaluated when the argument is missing).
        for (size_t i = 0; i < st->fieldDefaults.size(); ++i) {
            if (st->fieldDefaults[i] == nullptr) continue;
            emitOp(Op::ARG_DEFAULT, line);
            emitU16((uint16_t)i, line);
            int skip = (int)chunk().code.size();
            emitU16(0xFFFF, line);
            compileExpression(st->fieldDefaults[i].get());
            emitOp(Op::SET_LOCAL, line);
            emitU16((uint16_t)i, line);
            patchJump(skip);
        }

        // Build the instance map: __type__, then fields, then methods.
        int pairCount = 1 + (int)st->fields.size() + (int)st->methods.size();
        emitConst(Value::object(std::make_shared<StringObject>("__type__")), line);
        emitConst(Value::object(std::make_shared<StringObject>(st->name)), line);
        for (size_t i = 0; i < st->fields.size(); ++i) {
            emitConst(Value::object(std::make_shared<StringObject>(st->fields[i])), line);
            emitOp(Op::GET_LOCAL, line);
            emitU16((uint16_t)i, line);
        }
        for (const auto& m : st->methods) {
            emitConst(Value::object(std::make_shared<StringObject>(m->name->value)), line);
            compileFunction(m.get(), /*asMethod=*/true);
        }
        emitOp(Op::MAP, line);
        emitU16((uint16_t)pairCount, line);
        emitOp(Op::RETURN, line);

        std::vector<UpvalDesc> upvals = fnCtx.upvals;
        std::shared_ptr<Proto> proto = fnCtx.proto;
        proto->localCount = (int)fnCtx.locals.size();

        ctx_ = saved;

        emitOp(Op::CLOSURE, line);
        emitU16(addConst(Value::object(std::make_shared<ProtoObject>(proto))), line);
        emitU16((uint16_t)upvals.size(), line);
        for (const auto& u : upvals) {
            emitU8(u.isLocal ? 1 : 0, line);
            emitU16(u.index, line);
        }
        defineName(st->name, line);
        markConst(st->name);
    }

    void compileRepeat(const RepeatStatement* r) {
        int line = r->token.line;
        // Desugar to: set _n = count; while _n > 0: body; _n -= 1
        compileExpression(r->count.get());
        int counterSlot = declareHidden(line);
        emitStoreHidden(counterSlot, line);

        int loopStart = (int)chunk().code.size();
        ctx_->loops.push_back({loopStart, {}, 0});
        emitLoadHidden(counterSlot, line);
        emitConst(Value::integer(0), line);
        emitOp(Op::GREATER, line);
        int exitJump = emitJump(Op::JUMP_IF_FALSE, line);
        compileBlock(r->body.get());
        emitLoadHidden(counterSlot, line);
        emitConst(Value::integer(1), line);
        emitOp(Op::SUB, line);
        emitStoreHidden(counterSlot, line);
        emitLoop(loopStart, line);
        patchJump(exitJump);
        for (int bj : ctx_->loops.back().breakJumps) patchJump(bj);
        ctx_->loops.pop_back();
    }

    void compileFor(const ForStatement* f) {
        int line = f->token.line;
        compileExpression(f->iterable.get());
        emitOp(Op::FOR_SETUP, line);           // iterable -> iterator (stays on stack)

        auto varOperand = [&](const Identifier* id) -> uint16_t {
            return ctx_->isScript ? globals_.slot(id->value) : (uint16_t)resolveLocal(ctx_, id->value);
        };
        uint16_t v1 = varOperand(f->variable.get());
        uint16_t v2 = f->variable2 ? varOperand(f->variable2.get()) : 0;

        int loopStart = (int)chunk().code.size();
        ctx_->loops.push_back({loopStart, {}, 1});

        uint8_t flags = 0;
        if (ctx_->isScript) flags |= 1;        // write into globals
        if (f->variable2)   flags |= 2;        // pair form
        emitOp(Op::FOR_NEXT, line);
        emitU8(flags, line);
        emitU16(v1, line);
        emitU16(v2, line);
        int exitJump = (int)chunk().code.size();
        emitU16(0xFFFF, line);                 // patched below

        compileBlock(f->body.get());
        emitLoop(loopStart, line);
        patchJump(exitJump);
        emitOp(Op::POP, line);                 // discard the iterator
        for (int bj : ctx_->loops.back().breakJumps) patchJump(bj);
        ctx_->loops.pop_back();
    }

    void compileUse(const UseStatement* u) {
        UseSpec spec;
        spec.isFile = u->isFile;
        spec.target = u->target;
        if (!u->names.empty()) {
            spec.names = u->names;
            for (const auto& n : u->names) spec.nameSlots.push_back(globals_.slot(n));
        } else {
            std::string bind = u->alias;
            if (bind.empty()) {
                bind = u->isFile
                     ? std::filesystem::path(u->target).stem().string()
                     : u->target;
            }
            spec.bindName = bind;
            spec.bindSlot = globals_.slot(bind);
        }
        chunk().useSpecs.push_back(std::move(spec));
        emitOp(Op::USE, u->token.line);
        emitU16((uint16_t)(chunk().useSpecs.size() - 1), u->token.line);
    }

    // ===== Expressions =====

    void compileExpression(const Expression* expr) {
        switch (expr->nodeType()) {
            case NodeType::INTEGER_LITERAL:
                emitConst(Value::integer(static_cast<const IntegerLiteral*>(expr)->value),
                          expr->line());
                break;
            case NodeType::FLOAT_LITERAL:
                emitConst(Value::real(static_cast<const FloatLiteral*>(expr)->value),
                          expr->line());
                break;
            case NodeType::STRING_LITERAL:
                emitConst(Value::object(std::make_shared<StringObject>(
                              static_cast<const StringLiteral*>(expr)->value)),
                          expr->line());
                break;
            case NodeType::BOOLEAN_LITERAL:
                emitOp(static_cast<const BooleanLiteral*>(expr)->value ? Op::TRUE_ : Op::FALSE_,
                       expr->line());
                break;
            case NodeType::NULL_LITERAL:
                emitOp(Op::NIL, expr->line());
                break;
            case NodeType::IDENTIFIER:
                emitNameGet(static_cast<const Identifier*>(expr)->value, expr->line());
                break;
            case NodeType::INTERPOLATED_STRING: {
                const auto* s = static_cast<const InterpolatedString*>(expr);
                for (const auto& part : s->parts) compileExpression(part.get());
                emitOp(Op::INTERP, s->token.line);
                emitU16((uint16_t)s->parts.size(), s->token.line);
                break;
            }
            case NodeType::LIST_LITERAL: {
                const auto* l = static_cast<const ListLiteral*>(expr);
                for (const auto& e : l->elements) compileExpression(e.get());
                emitOp(Op::LIST, l->token.line);
                emitU16((uint16_t)l->elements.size(), l->token.line);
                break;
            }
            case NodeType::MAP_LITERAL: {
                const auto* m = static_cast<const MapLiteral*>(expr);
                for (const auto& p : m->pairs) {
                    compileExpression(p.first.get());
                    compileExpression(p.second.get());
                }
                emitOp(Op::MAP, m->token.line);
                emitU16((uint16_t)m->pairs.size(), m->token.line);
                break;
            }
            case NodeType::INDEX_EXPRESSION: {
                const auto* i = static_cast<const IndexExpression*>(expr);
                compileExpression(i->object.get());
                if (i->isSlice) {
                    if (i->index) compileExpression(i->index.get());
                    else emitOp(Op::NIL, i->token.line);
                    if (i->indexEnd) compileExpression(i->indexEnd.get());
                    else emitOp(Op::NIL, i->token.line);
                    emitOp(Op::SLICE, i->token.line);
                } else {
                    compileExpression(i->index.get());
                    emitOp(Op::INDEX_GET, i->token.line);
                }
                break;
            }
            case NodeType::MEMBER_EXPRESSION: {
                const auto* m = static_cast<const MemberExpression*>(expr);
                compileExpression(m->object.get());
                emitOp(m->safe ? Op::MEMBER_GET_SAFE : Op::MEMBER_GET, m->token.line);
                emitU16(strConst(m->property), m->token.line);
                break;
            }
            case NodeType::CONDITIONAL_EXPRESSION: {
                const auto* c = static_cast<const ConditionalExpression*>(expr);
                compileExpression(c->condition.get());
                int elseJump = emitJump(Op::JUMP_IF_FALSE, c->token.line);
                compileExpression(c->thenExpr.get());
                int endJump = emitJump(Op::JUMP, c->token.line);
                patchJump(elseJump);
                compileExpression(c->elseExpr.get());
                patchJump(endJump);
                break;
            }
            case NodeType::PREFIX_EXPRESSION: {
                const auto* p = static_cast<const PrefixExpression*>(expr);
                compileExpression(p->right.get());
                if (p->op == "-")        emitOp(Op::NEGATE, p->token.line);
                else if (p->op == "not") emitOp(Op::NOT_, p->token.line);
                else                     emitOp(Op::BIT_NOT, p->token.line);
                break;
            }
            case NodeType::INFIX_EXPRESSION: {
                const auto* b = static_cast<const InfixExpression*>(expr);
                if (b->op == "and" || b->op == "or") {
                    compileExpression(b->left.get());
                    int j = emitJump(b->op == "and" ? Op::AND_KEEP : Op::OR_KEEP,
                                     b->token.line);
                    compileExpression(b->right.get());
                    patchJump(j);
                    break;
                }
                if (b->op == "??") {
                    compileExpression(b->left.get());
                    int j = emitJump(Op::COALESCE, b->token.line);
                    compileExpression(b->right.get());
                    patchJump(j);
                    break;
                }
                compileExpression(b->left.get());
                compileExpression(b->right.get());
                if (b->op == "..")      emitOp(Op::RANGE_NEW, b->token.line);
                else if (b->op == "is") emitOp(Op::IS_TYPE, b->token.line);
                else                    emitBinary(b->op, b->token.line);
                break;
            }
            case NodeType::FUNCTION_LITERAL:
                compileFunction(static_cast<const FunctionLiteral*>(expr));
                break;
            case NodeType::CALL_EXPRESSION: {
                const auto* c = static_cast<const CallExpression*>(expr);
                // Method-call sugar: obj.name(args). The receiver is kept so the VM
                // can pass it as 'this' when obj is a struct instance (see CALL_METHOD).
                if (c->function->nodeType() == NodeType::MEMBER_EXPRESSION &&
                    !static_cast<const MemberExpression*>(c->function.get())->safe) {
                    const auto* m = static_cast<const MemberExpression*>(c->function.get());
                    compileExpression(m->object.get());
                    emitOp(Op::MEMBER_GET_KEEP, m->token.line);
                    emitU16(strConst(m->property), m->token.line);
                    for (const auto& a : c->arguments) compileExpression(a.get());
                    emitOp(Op::CALL_METHOD, c->token.line);
                    emitU8((uint8_t)c->arguments.size(), c->token.line);
                    break;
                }
                compileExpression(c->function.get());
                for (const auto& a : c->arguments) compileExpression(a.get());
                emitOp(Op::CALL, c->token.line);
                emitU8((uint8_t)c->arguments.size(), c->token.line);
                break;
            }
            default:
                break;
        }
    }

    // Maps a compound-assignment base op (e.g. "&", "<<") to its binary opcode.
    void emitCompound(const std::string& op, int line) {
        if      (op == "&")  emitOp(Op::BIT_AND, line);
        else if (op == "|")  emitOp(Op::BIT_OR, line);
        else if (op == "^")  emitOp(Op::BIT_XOR, line);
        else if (op == "<<") emitOp(Op::SHL, line);
        else if (op == ">>") emitOp(Op::SHR, line);
        else                 emitBinary(op, line);
    }

    void emitBinary(const std::string& op, int line) {
        if      (op == "+")  emitOp(Op::ADD, line);
        else if (op == "-")  emitOp(Op::SUB, line);
        else if (op == "*")  emitOp(Op::MUL, line);
        else if (op == "/")  emitOp(Op::DIV, line);
        else if (op == "%")  emitOp(Op::MOD, line);
        else if (op == "**") emitOp(Op::POW, line);
        else if (op == "==") emitOp(Op::EQUAL, line);
        else if (op == "!=") emitOp(Op::NOT_EQUAL, line);
        else if (op == "<")  emitOp(Op::LESS, line);
        else if (op == ">")  emitOp(Op::GREATER, line);
        else if (op == "<=") emitOp(Op::LESS_EQ, line);
        else if (op == ">=") emitOp(Op::GREATER_EQ, line);
        else if (op == "&")  emitOp(Op::BIT_AND, line);
        else if (op == "|")  emitOp(Op::BIT_OR, line);
        else if (op == "^")  emitOp(Op::BIT_XOR, line);
        else if (op == "<<") emitOp(Op::SHL, line);
        else if (op == ">>") emitOp(Op::SHR, line);
        else if (op == "in") emitOp(Op::IN, line);
    }

    void compileFunction(const FunctionLiteral* fn, bool asMethod = false) {
        int line = fn->token.line;
        int base = asMethod ? 1 : 0; // slot 0 is the implicit 'this' for methods

        FnCtx fnCtx;
        fnCtx.proto = std::make_shared<Proto>();
        fnCtx.proto->name = fn->name ? fn->name->value : "";
        fnCtx.proto->paramCount = (int)fn->parameters.size() + base;
        fnCtx.enclosing = ctx_;
        fnCtx.isScript = false;

        int required = base;
        for (const auto& d : fn->defaults) {
            if (d == nullptr) required++;
        }
        fnCtx.proto->requiredCount = required;

        // Slot layout: [0..paramCount) = parameters, then the collected locals.
        if (asMethod) fnCtx.locals.push_back({"this"});
        for (const auto& p : fn->parameters) fnCtx.locals.push_back({p->value});
        std::vector<std::string> names;
        for (const auto& s : fn->body->statements) collectLocals(s.get(), names);
        for (const auto& n : names) {
            if (resolveLocal(&fnCtx, n) == -1) fnCtx.locals.push_back({n});
        }
        fnCtx.proto->chunk.consts.reserve(8);

        FnCtx* saved = ctx_;
        ctx_ = &fnCtx;

        // Default parameters: evaluated at call time when the argument is missing.
        for (size_t i = 0; i < fn->defaults.size(); ++i) {
            if (fn->defaults[i] == nullptr) continue;
            uint16_t slot = (uint16_t)(i + base);
            emitOp(Op::ARG_DEFAULT, line);
            emitU16(slot, line);
            int skip = (int)chunk().code.size();
            emitU16(0xFFFF, line);
            compileExpression(fn->defaults[i].get());
            emitOp(Op::SET_LOCAL, line);
            emitU16(slot, line);
            patchJump(skip);
        }

        compileBlock(fn->body.get());
        emitOp(Op::NIL, lastLine_);      // implicit 'return null'
        emitOp(Op::RETURN, lastLine_);

        std::vector<UpvalDesc> upvals = fnCtx.upvals;
        std::shared_ptr<Proto> proto = fnCtx.proto;
        proto->localCount = (int)fnCtx.locals.size();
        proto->variadic = fn->variadic;

        ctx_ = saved;

        emitOp(Op::CLOSURE, line);
        emitU16(addConst(Value::object(std::make_shared<ProtoObject>(proto))), line);
        emitU16((uint16_t)upvals.size(), line);
        for (const auto& u : upvals) {
            emitU8(u.isLocal ? 1 : 0, line);
            emitU16(u.index, line);
        }

        // Named function: also bind the name in the current scope, keep the
        // value on the stack (the expression statement above pops it).
        // Methods are stored into the instance map by compileStruct, not bound
        // as names, so they must NOT run the name-binding path.
        if (fn->name && !asMethod) {
            emitOp(Op::DUP, line);
            defineName(fn->name->value, line);
        }
    }

};

} // namespace Lume

#endif // COMPILER_HPP
