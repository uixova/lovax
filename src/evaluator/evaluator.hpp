#ifndef EVALUATOR_HPP
#define EVALUATOR_HPP

#include <memory>
#include <iostream>
#include <vector>
#include <cmath>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "../ast/ast.hpp"
#include "../object/object.hpp"
#include "../object/environment.hpp"
#include "../utils/colors.hpp"
#include "../parser/parser.hpp"
#include "builtins.hpp"
#include "stdlib.hpp"

namespace Lume {

class Evaluator {
public:
    // Lume call-depth limit: prevents the C++ stack from overflowing (segfault),
    // producing a clean runtime error instead.
    // The tree-walker uses several C++ frames per Lume call;
    // the limit is sized to stay safe even on 1 MB stacks (Windows).
    // The limit will rise once the bytecode VM manages its own call stack (see rfcs/).
    static constexpr int MAX_CALL_DEPTH = 500;

    static std::shared_ptr<Object> eval(const ASTNode* node, std::shared_ptr<Environment> env) {
        if (node == nullptr) return NULL_OBJ_;

        // Single switch instead of a dynamic_cast chain: node tag + static_cast
        // (RTTI maliyeti yok, dallanma tahmini dostu)
        switch (node->nodeType()) {

            case NodeType::PROGRAM: {
                const auto* program = static_cast<const Program*>(node);
                std::shared_ptr<Object> result = NULL_OBJ_;
                for (const auto& stmt : program->statements) {
                    result = eval(stmt.get(), env);
                    if (result == nullptr) { result = NULL_OBJ_; continue; }
                    switch (result->type()) {
                        case ObjectType::ERROR:
                            return result; // The first error stops execution
                        case ObjectType::RETURN_VALUE:
                            return makeError("'return' cannot be used outside a function", stmt->line());
                        case ObjectType::BREAK_SIGNAL:
                            return makeError("'break' cannot be used outside a loop", stmt->line());
                        case ObjectType::CONTINUE_SIGNAL:
                            return makeError("'continue' cannot be used outside a loop", stmt->line());
                        default: break;
                    }
                }
                return result;
            }

            case NodeType::EXPRESSION_STATEMENT: {
                const auto* stmt = static_cast<const ExpressionStatement*>(node);
                return eval(stmt->expression.get(), env);
            }

            case NodeType::BLOCK_STATEMENT:
                return evalBlockStatement(static_cast<const BlockStatement*>(node), env);

            case NodeType::SET_STATEMENT: {
                const auto* stmt = static_cast<const SetStatement*>(node);
                auto val = eval(stmt->value.get(), env);
                if (isError(val)) return val;
                env->define(stmt->name->value, val);
                return NULL_OBJ_;
            }

            case NodeType::ASSIGN_STATEMENT:
                return evalAssignStatement(static_cast<const AssignStatement*>(node), env);

            case NodeType::SAY_STATEMENT:
                return evalSayStatement(static_cast<const SayStatement*>(node), env);

            case NodeType::IF_STATEMENT:
                return evalIfStatement(static_cast<const IfStatement*>(node), env);

            case NodeType::MATCH_STATEMENT:
                return evalMatchStatement(static_cast<const MatchStatement*>(node), env);

            case NodeType::WHILE_STATEMENT:
                return evalWhileStatement(static_cast<const WhileStatement*>(node), env);

            case NodeType::FOR_STATEMENT:
                return evalForStatement(static_cast<const ForStatement*>(node), env);

            case NodeType::BREAK_STATEMENT:
                return std::make_shared<BreakSignalObject>(node->line());

            case NodeType::CONTINUE_STATEMENT:
                return std::make_shared<ContinueSignalObject>(node->line());

            case NodeType::RETURN_STATEMENT: {
                const auto* stmt = static_cast<const ReturnStatement*>(node);
                std::shared_ptr<Object> val = NULL_OBJ_;
                if (stmt->returnValue != nullptr) {
                    val = eval(stmt->returnValue.get(), env);
                    if (isError(val)) return val;
                }
                return std::make_shared<ReturnValueObject>(val);
            }

            case NodeType::IDENTIFIER: {
                const auto* ident = static_cast<const Identifier*>(node);
                auto val = env->get(ident->value);
                if (val == nullptr) {
                    return makeError("undefined variable '" + ident->value +
                                     "' (define it with: set " + ident->value + " = ...)",
                                     ident->token.line);
                }
                return val;
            }

            case NodeType::INTEGER_LITERAL:
                return std::make_shared<IntegerObject>(static_cast<const IntegerLiteral*>(node)->value);

            case NodeType::FLOAT_LITERAL:
                return std::make_shared<FloatObject>(static_cast<const FloatLiteral*>(node)->value);

            case NodeType::STRING_LITERAL:
                return std::make_shared<StringObject>(static_cast<const StringLiteral*>(node)->value);

            case NodeType::BOOLEAN_LITERAL:
                return boolObj(static_cast<const BooleanLiteral*>(node)->value);

            case NodeType::NULL_LITERAL:
                return NULL_OBJ_;

            case NodeType::INTERPOLATED_STRING: {
                const auto* interp = static_cast<const InterpolatedString*>(node);
                std::string out;
                for (const auto& part : interp->parts) {
                    auto val = eval(part.get(), env);
                    if (isError(val)) return val;
                    out += val->inspect();
                }
                return std::make_shared<StringObject>(out);
            }

            case NodeType::LIST_LITERAL: {
                const auto* lit = static_cast<const ListLiteral*>(node);
                auto list = std::make_shared<ListObject>();
                list->elements.reserve(lit->elements.size());
                for (const auto& el : lit->elements) {
                    auto val = eval(el.get(), env);
                    if (isError(val)) return val;
                    list->elements.push_back(val);
                }
                return list;
            }

            case NodeType::MAP_LITERAL: {
                const auto* lit = static_cast<const MapLiteral*>(node);
                auto map = std::make_shared<MapObject>();
                for (const auto& pair : lit->pairs) {
                    auto key = eval(pair.first.get(), env);
                    if (isError(key)) return key;
                    if (!isValidMapKey(key)) {
                        return makeError("map keys must be string, int or bool; got " +
                                         typeName(key->type()) + "", lit->token.line);
                    }
                    auto val = eval(pair.second.get(), env);
                    if (isError(val)) return val;
                    map->set(key, val);
                }
                return map;
            }

            case NodeType::INDEX_EXPRESSION: {
                const auto* expr = static_cast<const IndexExpression*>(node);
                auto obj = eval(expr->object.get(), env);
                if (isError(obj)) return obj;
                auto idx = eval(expr->index.get(), env);
                if (isError(idx)) return idx;
                return evalIndexAccess(obj, idx, expr->token.line);
            }

            case NodeType::MEMBER_EXPRESSION: {
                const auto* mem = static_cast<const MemberExpression*>(node);
                auto obj = eval(mem->object.get(), env);
                if (isError(obj)) return obj;
                return evalMemberAccess(obj, mem->property, mem->token.line);
            }

            case NodeType::USE_STATEMENT:
                return evalUseStatement(static_cast<const UseStatement*>(node), env);

            case NodeType::CONDITIONAL_EXPRESSION: {
                const auto* cond = static_cast<const ConditionalExpression*>(node);
                auto test = eval(cond->condition.get(), env);
                if (isError(test)) return test;
                return isTruthy(test) ? eval(cond->thenExpr.get(), env)
                                      : eval(cond->elseExpr.get(), env);
            }

            case NodeType::PREFIX_EXPRESSION: {
                const auto* expr = static_cast<const PrefixExpression*>(node);
                auto right = eval(expr->right.get(), env);
                if (isError(right)) return right;
                return evalPrefixExpression(expr->op, right, expr->token.line);
            }

            case NodeType::INFIX_EXPRESSION: {
                const auto* infix = static_cast<const InfixExpression*>(node);

                // and/or SHORT-CIRCUIT and return the operand:
                //   x or fallback   -> x if truthy, else fallback (Python/Lua idiom)
                //   cond and action   -> cond if falsy, otherwise action
                if (infix->op == "and" || infix->op == "or") {
                    auto left = eval(infix->left.get(), env);
                    if (isError(left)) return left;
                    bool leftTruth = isTruthy(left);
                    if (infix->op == "and" && !leftTruth) return left;
                    if (infix->op == "or"  &&  leftTruth) return left;
                    auto right = eval(infix->right.get(), env);
                    return right;
                }

                auto left = eval(infix->left.get(), env);
                if (isError(left)) return left;
                auto right = eval(infix->right.get(), env);
                if (isError(right)) return right;
                return evalInfixExpression(infix->op, left, right, infix->token.line);
            }

            case NodeType::FUNCTION_LITERAL: {
                const auto* fnLit = static_cast<const FunctionLiteral*>(node);
                std::vector<std::string> params;
                std::vector<const Expression*> defaults;
                params.reserve(fnLit->parameters.size());
                defaults.reserve(fnLit->defaults.size());
                for (const auto& p : fnLit->parameters) {
                    params.push_back(p->value);
                }
                for (const auto& d : fnLit->defaults) {
                    defaults.push_back(d.get());
                }
                // Build the function object, binding the current environment as its closure
                auto fnObj = std::make_shared<FunctionObject>(params, defaults, fnLit->body.get(), env,
                                                              fnLit->name ? fnLit->name->value : "");
                if (fnLit->name != nullptr) {
                    env->define(fnLit->name->value, fnObj);
                }
                return fnObj;
            }

            case NodeType::CALL_EXPRESSION:
                return evalCallExpression(static_cast<const CallExpression*>(node), env);
        }

        return NULL_OBJ_;
    }

    // Truthiness rules (Python model) — shared rule in object.hpp
    static bool isTruthy(const std::shared_ptr<Object>& obj) {
        return objectTruthy(obj);
    }

    // Calls a function OR a builtin — evalCallExpression and builtins' CallFn
    // (each/filter/emit) share the same engine.
    static std::shared_ptr<Object> callFunction(const std::shared_ptr<Object>& function,
                                                const std::vector<std::shared_ptr<Object>>& args,
                                                int line) {
        if (function->type() == ObjectType::BUILTIN) {
            auto* builtin = static_cast<BuiltinObject*>(function.get());
            return builtin->fn(args, line, builtinCallback());
        }

        if (function->type() != ObjectType::FUNCTION) {
            return makeError("not a function, cannot be called: " + typeName(function->type()), line);
        }

        auto* fnObj = static_cast<FunctionObject*>(function.get());

        // Arity check: required args must be given; the rest fall back to defaults
        size_t required = fnObj->requiredCount();
        size_t total = fnObj->parameters.size();
        if (args.size() < required || args.size() > total) {
            std::string fname = fnObj->name.empty() ? "fonksiyon" : "'" + fnObj->name + "'";
            std::string expected = (required == total)
                ? std::to_string(total)
                : std::to_string(required) + "-" + std::to_string(total);
            return makeError(fname + " expects " + expected + " parameter(s), got " +
                             std::to_string(args.size()) + "", line);
        }

        // Report a clean error before deep recursion overflows the C++ stack
        if (callDepth >= MAX_CALL_DEPTH) {
            return makeError("maximum call depth exceeded (" +
                             std::to_string(MAX_CALL_DEPTH) + ") — possible infinite recursion",
                             line);
        }

        // Create a fresh local environment for the call (closure-protected)
        auto localEnv = std::make_shared<Environment>(fnObj->env);
        for (size_t i = 0; i < args.size(); ++i) {
            localEnv->define(fnObj->parameters[i], args[i]);
        }
        // Missing arguments: default expressions are evaluated at call time, in order
        // (earlier parameters are visible: fn f(a, b = a + 1) works)
        for (size_t i = args.size(); i < total; ++i) {
            auto defVal = eval(fnObj->defaults[i], localEnv);
            if (isError(defVal)) return defVal;
            localEnv->define(fnObj->parameters[i], defVal);
        }

        callDepth++;
        auto result = eval(fnObj->body, localEnv);
        callDepth--;

        if (result == nullptr) return NULL_OBJ_;
        if (result->type() == ObjectType::RETURN_VALUE) {
            return static_cast<ReturnValueObject*>(result.get())->value;
        }
        if (result->type() == ObjectType::BREAK_SIGNAL) {
            return makeError("'break' cannot be used outside a loop",
                             static_cast<BreakSignalObject*>(result.get())->srcLine);
        }
        if (result->type() == ObjectType::CONTINUE_SIGNAL) {
            return makeError("'continue' cannot be used outside a loop",
                             static_cast<ContinueSignalObject*>(result.get())->srcLine);
        }
        return result; // propagates upward unchanged, including ERROR
    }

    // Callback handed to builtins: permission to call Lume functions
    static const BuiltinObject::CallFn& builtinCallback() {
        static BuiltinObject::CallFn cb =
            [](const std::shared_ptr<Object>& f,
               const std::vector<std::shared_ptr<Object>>& a,
               int l) { return callFunction(f, a, l); };
        return cb;
    }

private:
    static inline int callDepth = 0;

    static bool isValidMapKey(const std::shared_ptr<Object>& key) {
        return key->type() == ObjectType::STRING ||
               key->type() == ObjectType::INTEGER ||
               key->type() == ObjectType::BOOLEAN;
    }

    // ===== Statement evaluators =====

    static std::shared_ptr<Object> evalSayStatement(const SayStatement* stmt,
                                                    std::shared_ptr<Environment> env) {
        std::string out;
        std::shared_ptr<Object> firstVal = nullptr;

        for (size_t i = 0; i < stmt->values.size(); ++i) {
            auto val = eval(stmt->values[i].get(), env);
            if (isError(val)) return val;
            if (i == 0) firstVal = val;
            else out += " ";
            out += val->inspect();
        }

        // Color is chosen by the first value's type (no color codes off-terminal)
        std::string color;
        switch (firstVal->type()) {
            case ObjectType::INTEGER:
            case ObjectType::FLOAT:    color = Color::yellow(); break;
            case ObjectType::BOOLEAN:  color = Color::blue();   break;
            case ObjectType::NULL_OBJ: color = Color::red();    break;
            case ObjectType::STRING:   color = Color::green();  break;
            default:                   color = Color::cyan();   break;
        }
        std::cout << color << out << Color::reset() << "\n";
        return NULL_OBJ_;
    }

    static std::shared_ptr<Object> evalAssignStatement(const AssignStatement* stmt,
                                                       std::shared_ptr<Environment> env) {
        auto newVal = eval(stmt->value.get(), env);
        if (isError(newVal)) return newVal;

        // For compound assignment ("+=", "-=", ...) extract the base operator: "+=" -> "+"
        std::string binOp;
        if (stmt->op != "=") binOp = stmt->op.substr(0, 1);

        if (stmt->target->nodeType() == NodeType::IDENTIFIER) {
            const auto* ident = static_cast<const Identifier*>(stmt->target.get());

            if (!binOp.empty()) {
                auto oldVal = env->get(ident->value);
                if (oldVal == nullptr) {
                    return makeError("undefined variable '" + ident->value +
                                     "' (define it with: set " + ident->value + " = ...)",
                                     stmt->token.line);
                }
                newVal = evalInfixExpression(binOp, oldVal, newVal, stmt->token.line);
                if (isError(newVal)) return newVal;
            }

            // RFC-001: bare assignment updates an EXISTING variable; error otherwise.
            // (Prevents typos from silently creating new variables.)
            if (!env->assign(ident->value, newVal)) {
                return makeError("undefined variable '" + ident->value +
                                 "' (define it with: set " + ident->value + " = ...)",
                                 stmt->token.line);
            }
            return NULL_OBJ_;
        }

        // Target is a member: object.field = v (map/module)
        if (stmt->target->nodeType() == NodeType::MEMBER_EXPRESSION) {
            const auto* mem = static_cast<const MemberExpression*>(stmt->target.get());
            auto obj = eval(mem->object.get(), env);
            if (isError(obj)) return obj;
            if (obj->type() != ObjectType::MAP) {
                return makeError("member assignment (object.field = ...) only works on maps, got " +
                                 typeName(obj->type()) + "", stmt->token.line);
            }
            auto* map = static_cast<MapObject*>(obj.get());
            if (map->frozen) {
                return makeError("module '" + map->moduleName + "' cannot be modified (frozen)", stmt->token.line);
            }
            auto key = std::make_shared<StringObject>(mem->property);
            if (!binOp.empty()) {
                auto oldVal = map->get(key);
                if (oldVal == nullptr) {
                    return makeError("map'te olmayan anahtar: \"" + mem->property +
                                     "\" (assign a value first, or check with has())", stmt->token.line);
                }
                newVal = evalInfixExpression(binOp, oldVal, newVal, stmt->token.line);
                if (isError(newVal)) return newVal;
            }
            map->set(key, newVal);
            return NULL_OBJ_;
        }

        // Target is an index: list[i] = v  or  map["k"] = v
        const auto* idxExpr = static_cast<const IndexExpression*>(stmt->target.get());
        auto obj = eval(idxExpr->object.get(), env);
        if (isError(obj)) return obj;
        auto idx = eval(idxExpr->index.get(), env);
        if (isError(idx)) return idx;

        if (obj->type() == ObjectType::LIST) {
            auto* list = static_cast<ListObject*>(obj.get());
            if (idx->type() != ObjectType::INTEGER) {
                return makeError("list index must be an integer, got " +
                                 typeName(idx->type()) + "", stmt->token.line);
            }
            long long i = static_cast<IntegerObject*>(idx.get())->value;
            long long n = (long long)list->elements.size();
            if (i < 0) i += n; // negatif indeks: sondan say
            if (i < 0 || i >= n) {
                return makeError("list index out of range: " + idx->inspect() +
                                 " (length " + std::to_string(n) + ")", stmt->token.line);
            }
            if (!binOp.empty()) {
                newVal = evalInfixExpression(binOp, list->elements[i], newVal, stmt->token.line);
                if (isError(newVal)) return newVal;
            }
            list->elements[i] = newVal;
            return NULL_OBJ_;
        }

        if (obj->type() == ObjectType::MAP) {
            auto* map = static_cast<MapObject*>(obj.get());
            if (map->frozen) {
                return makeError("module '" + map->moduleName + "' cannot be modified (frozen)", stmt->token.line);
            }
            if (!isValidMapKey(idx)) {
                return makeError("map keys must be string, int or bool; got " +
                                 typeName(idx->type()) + "", stmt->token.line);
            }
            if (!binOp.empty()) {
                auto oldVal = map->get(idx);
                if (oldVal == nullptr) {
                    return makeError("key not in map: " + inspectQuoted(idx) +
                                     " (assign a value first, or check with has())", stmt->token.line);
                }
                newVal = evalInfixExpression(binOp, oldVal, newVal, stmt->token.line);
                if (isError(newVal)) return newVal;
            }
            map->set(idx, newVal);
            return NULL_OBJ_;
        }

        return makeError("indexed assignment only works on list and map, got " +
                         typeName(obj->type()) + "", stmt->token.line);
    }

    static std::shared_ptr<Object> evalIfStatement(const IfStatement* ifStmt,
                                                   std::shared_ptr<Environment> env) {
        auto condition = eval(ifStmt->condition.get(), env);
        if (isError(condition)) return condition;

        if (isTruthy(condition)) {
            return eval(ifStmt->consequence.get(), env);
        }
        if (ifStmt->alternative != nullptr) {
            // alternative may be a BlockStatement (else) or an IfStatement (else-if chain)
            return eval(ifStmt->alternative.get(), env);
        }
        return NULL_OBJ_;
    }

    static std::shared_ptr<Object> evalWhileStatement(const WhileStatement* whileStmt,
                                                      std::shared_ptr<Environment> env) {
        while (true) {
            auto cond = eval(whileStmt->condition.get(), env);
            if (isError(cond)) return cond;
            if (!isTruthy(cond)) break;

            auto result = eval(whileStmt->body.get(), env);
            if (result == nullptr) continue;
            if (result->type() == ObjectType::BREAK_SIGNAL) break;
            if (result->type() == ObjectType::CONTINUE_SIGNAL) continue;
            if (result->type() == ObjectType::RETURN_VALUE ||
                result->type() == ObjectType::ERROR) {
                return result;
            }
        }
        return NULL_OBJ_;
    }

    static std::shared_ptr<Object> evalForStatement(const ForStatement* forStmt,
                                                    std::shared_ptr<Environment> env) {
        auto iterable = eval(forStmt->iterable.get(), env);
        if (isError(iterable)) return iterable;

        const std::string& varName = forStmt->variable->value;

        // Runs the loop body for one element; signal handling is shared
        // Return: nullptr = continue, otherwise a result to propagate upward
        auto runBody = [&](const std::shared_ptr<Object>& item) -> std::shared_ptr<Object> {
            env->define(varName, item);
            auto result = eval(forStmt->body.get(), env);
            if (result == nullptr) return nullptr;
            if (result->type() == ObjectType::CONTINUE_SIGNAL) return nullptr;
            if (result->type() == ObjectType::BREAK_SIGNAL) return result; // the outer loop sees break and stops
            if (result->type() == ObjectType::RETURN_VALUE ||
                result->type() == ObjectType::ERROR) {
                return result;
            }
            return nullptr;
        };

        switch (iterable->type()) {
            case ObjectType::LIST: {
                auto* list = static_cast<ListObject*>(iterable.get());
                // Live iteration: size is re-checked each turn in case the body mutates the list
                for (size_t i = 0; i < list->elements.size(); ++i) {
                    auto r = runBody(list->elements[i]);
                    if (r != nullptr) {
                        if (r->type() == ObjectType::BREAK_SIGNAL) break;
                        return r;
                    }
                }
                return NULL_OBJ_;
            }
            case ObjectType::RANGE: {
                auto* range = static_cast<RangeObject*>(iterable.get());
                if (range->step > 0) {
                    for (long long v = range->start; v < range->end; v += range->step) {
                        auto r = runBody(std::make_shared<IntegerObject>(v));
                        if (r != nullptr) {
                            if (r->type() == ObjectType::BREAK_SIGNAL) break;
                            return r;
                        }
                    }
                } else {
                    for (long long v = range->start; v > range->end; v += range->step) {
                        auto r = runBody(std::make_shared<IntegerObject>(v));
                        if (r != nullptr) {
                            if (r->type() == ObjectType::BREAK_SIGNAL) break;
                            return r;
                        }
                    }
                }
                return NULL_OBJ_;
            }
            case ObjectType::STRING: {
                const std::string& s = static_cast<StringObject*>(iterable.get())->value;
                size_t i = 0;
                while (i < s.size()) {
                    int len = utf8CharLen(static_cast<unsigned char>(s[i]));
                    auto r = runBody(std::make_shared<StringObject>(s.substr(i, len)));
                    if (r != nullptr) {
                        if (r->type() == ObjectType::BREAK_SIGNAL) break;
                        return r;
                    }
                    i += len;
                }
                return NULL_OBJ_;
            }
            case ObjectType::MAP: {
                // Iterates over keys (Python model); takes a snapshot
                auto* map = static_cast<MapObject*>(iterable.get());
                std::vector<std::shared_ptr<Object>> keys;
                keys.reserve(map->entries.size());
                for (const auto& e : map->entries) keys.push_back(e.first);
                for (const auto& key : keys) {
                    auto r = runBody(key);
                    if (r != nullptr) {
                        if (r->type() == ObjectType::BREAK_SIGNAL) break;
                        return r;
                    }
                }
                return NULL_OBJ_;
            }
            default:
                return makeError("for loops iterate over list, range, string or map; got " +
                                 typeName(iterable->type()) + "", forStmt->token.line);
        }
    }

    static std::shared_ptr<Object> evalBlockStatement(const BlockStatement* block,
                                                      std::shared_ptr<Environment> env) {
        std::shared_ptr<Object> result = NULL_OBJ_;
        for (const auto& stmt : block->statements) {
            result = eval(stmt.get(), env);
            if (result == nullptr) { result = NULL_OBJ_; continue; }
            // return / break / continue / error: stop execution, propagate the signal
            ObjectType t = result->type();
            if (t == ObjectType::RETURN_VALUE || t == ObjectType::ERROR ||
                t == ObjectType::BREAK_SIGNAL || t == ObjectType::CONTINUE_SIGNAL) {
                return result;
            }
        }
        return result;
    }

    // ===== Expression evaluators =====

    static std::shared_ptr<Object> evalCallExpression(const CallExpression* call,
                                                      std::shared_ptr<Environment> env) {
        auto function = eval(call->function.get(), env);
        if (isError(function)) return function;

        // Evaluate arguments left to right
        std::vector<std::shared_ptr<Object>> args;
        args.reserve(call->arguments.size());
        for (const auto& arg : call->arguments) {
            auto val = eval(arg.get(), env);
            if (isError(val)) return val;
            args.push_back(val);
        }

        return callFunction(function, args, call->token.line);
    }

    // match: the subject is evaluated once; first matching branch runs, no fallthrough
    static std::shared_ptr<Object> evalMatchStatement(const MatchStatement* stmt,
                                                      std::shared_ptr<Environment> env) {
        auto subject = eval(stmt->subject.get(), env);
        if (isError(subject)) return subject;

        for (const auto& mcase : stmt->cases) {
            bool matched = mcase.isDefault;
            if (!matched) {
                for (const auto& pattern : mcase.patterns) {
                    auto pval = eval(pattern.get(), env);
                    if (isError(pval)) return pval;
                    if (objectEquals(subject, pval)) {
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) {
                // break/continue/return/errors propagate out of the body unchanged
                return eval(mcase.body.get(), env);
            }
        }
        return NULL_OBJ_;
    }

    // ===== Module system (RFC-006) =====

    // Directory of the entry script: relative module paths resolve from here
    static inline std::vector<std::string> baseDirs = {"."};
    static inline std::unordered_map<std::string, std::shared_ptr<Object>> moduleCache;
    static inline std::vector<std::string> loadingStack; // circular-use detection
    // Module ASTs must stay alive: module functions hold pointers into them
    static inline std::vector<std::unique_ptr<Program>> moduleAsts;

public:
    static void setBaseDir(const std::string& dir) {
        baseDirs.clear();
        baseDirs.push_back(dir.empty() ? "." : dir);
    }

private:
    // object.field read: module function or map field
    static std::shared_ptr<Object> evalMemberAccess(const std::shared_ptr<Object>& obj,
                                                    const std::string& prop, int line) {
        if (obj->type() != ObjectType::MAP) {
            return makeError("'.' access expects a map or module, got " +
                             typeName(obj->type()) + "", line);
        }
        auto* map = static_cast<MapObject*>(obj.get());
        auto val = map->get(std::make_shared<StringObject>(prop));
        if (val != nullptr) return val;

        if (!map->moduleName.empty()) {
            // Not in the module: list available names to aid discovery
            std::string avail;
            size_t shown = 0;
            for (const auto& e : map->entries) {
                if (shown >= 10) { avail += ", ..."; break; }
                if (shown > 0) avail += ", ";
                avail += e.first->inspect();
                shown++;
            }
            return makeError("'" + map->moduleName + "' module has no member '" + prop +
                             "' (available: " + avail + ")", line);
        }
        return makeError("map'te olmayan anahtar: \"" + prop +
                         "\" (check with has())", line);
    }

    static std::shared_ptr<Object> evalUseStatement(const UseStatement* stmt,
                                                    std::shared_ptr<Environment> env) {
        std::shared_ptr<Object> module;

        if (stmt->isFile) {
            module = loadFileModule(stmt->target, stmt->token.line);
        } else {
            module = StdLib::getBuiltinModule(stmt->target);
            if (module == nullptr) {
                // Not a built-in: try an installed package (lume_libs/<name>/, the
                // node_modules of Lume). Resolved from the project root (entry script dir).
                namespace fs = std::filesystem;
                fs::path root(baseDirs.front());
                fs::path candidates[2] = {
                    root / "lume_libs" / stmt->target / (stmt->target + ".lm"),
                    root / "lume_libs" / stmt->target / "main.lm"
                };
                for (const auto& cand : candidates) {
                    std::error_code ec;
                    if (fs::exists(cand, ec)) {
                        module = loadFileModule(std::filesystem::absolute(cand).string(), stmt->token.line);
                        break;
                    }
                }
                if (module == nullptr) {
                    return makeError("unknown module '" + stmt->target +
                                     "' (built-ins: " + StdLib::builtinModuleList() +
                                     "; no package at lume_libs/" + stmt->target + "/" +
                                     "; for a file module use quotes: use \"" + stmt->target + ".lm\")",
                                     stmt->token.line);
                }
            }
        }
        if (isError(module)) return module;

        auto* map = static_cast<MapObject*>(module.get());

        // Selective import: use math: lerp, clamp
        if (!stmt->names.empty()) {
            for (const auto& name : stmt->names) {
                auto val = map->get(std::make_shared<StringObject>(name));
                if (val == nullptr) {
                    return makeError("'" + map->moduleName + "' module has no member '" + name + "'",
                                     stmt->token.line);
                }
                env->define(name, val);
            }
            return NULL_OBJ_;
        }

        // Bind the module object: use math [as m]
        std::string bindName = stmt->alias;
        if (bindName.empty()) {
            if (stmt->isFile) {
                bindName = std::filesystem::path(stmt->target).stem().string();
            } else {
                bindName = stmt->target;
            }
        }
        if (bindName.empty()) {
            return makeError("could not derive a module name; give one with 'as': use \"...\" as name",
                             stmt->token.line);
        }
        env->define(bindName, module);
        return NULL_OBJ_;
    }

    // User file-module loader: cache + cycle detection + relative path resolution
    static std::shared_ptr<Object> loadFileModule(const std::string& rawPath, int line) {
        namespace fs = std::filesystem;

        fs::path p(rawPath);
        if (p.is_relative()) {
            p = fs::path(baseDirs.back()) / p;
        }
        std::error_code ec;
        fs::path resolved = fs::weakly_canonical(p, ec);
        if (ec) resolved = p.lexically_normal();
        std::string key = resolved.string();

        if (!fs::exists(resolved)) {
            return makeError("module file not found: " + rawPath +
                             " (resolved path: " + key + ")", line);
        }

        auto cacheIt = moduleCache.find(key);
        if (cacheIt != moduleCache.end()) return cacheIt->second;

        // Circular use detection: a.lm -> b.lm -> a.lm
        for (const auto& loading : loadingStack) {
            if (loading == key) {
                std::string chain;
                for (const auto& l : loadingStack) chain += fs::path(l).filename().string() + " -> ";
                chain += fs::path(key).filename().string();
                return makeError("circular use detected: " + chain, line);
            }
        }

        std::ifstream f(resolved, std::ios::binary);
        if (!f.is_open()) return makeError("cannot open module file: " + key, line);
        std::stringstream buf;
        buf << f.rdbuf();
        std::string source = buf.str();

        Lexer lexer(source);
        auto parser = std::make_unique<Parser>(lexer);
        auto program = parser->parseProgram();
        if (!parser->errors().empty()) {
            std::string msg = "syntax error in module [" + rawPath + "]: " +
                              parser->errors()[0].toString();
            if (parser->errors().size() > 1) {
                msg += " (+" + std::to_string(parser->errors().size() - 1) + " more)";
            }
            return makeError(msg, line);
        }

        // The module runs in its own clean scope (core builtins only; it uses stdlib itself)
        auto modEnv = std::make_shared<Environment>();
        Builtins::installBuiltins(modEnv);

        loadingStack.push_back(key);
        baseDirs.push_back(resolved.parent_path().string());
        auto result = eval(program.get(), modEnv);
        baseDirs.pop_back();
        loadingStack.pop_back();

        if (isError(result)) {
            auto* err = static_cast<ErrorObject*>(result.get());
            return makeError("error while loading module [" + rawPath + "]: " + err->message +
                             " (module line " + std::to_string(err->srcLine) + ")", line);
        }

        // Keep the AST alive: module functions hold pointers into it
        moduleAsts.push_back(std::move(program));

        // Exports: everything in the module's top scope, alphabetical (deterministic)
        auto mod = std::make_shared<MapObject>();
        std::vector<std::pair<std::string, std::shared_ptr<Object>>> exports(
            modEnv->entries().begin(), modEnv->entries().end());
        std::sort(exports.begin(), exports.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // Do NOT export core builtins (keep only the module's own definitions)
        auto probe = std::make_shared<Environment>();
        Builtins::installBuiltins(probe);
        for (const auto& [name, val] : exports) {
            auto coreIt = probe->entries().find(name);
            if (coreIt != probe->entries().end() && coreIt->second->type() == val->type() &&
                val->type() == ObjectType::BUILTIN &&
                static_cast<BuiltinObject*>(val.get())->name == name) {
                continue; // untouched core builtin
            }
            mod->set(std::make_shared<StringObject>(name), val);
        }
        mod->frozen = true;
        mod->moduleName = resolved.stem().string();

        moduleCache[key] = mod;
        return mod;
    }

    static std::shared_ptr<Object> evalIndexAccess(const std::shared_ptr<Object>& obj,
                                                   const std::shared_ptr<Object>& idx,
                                                   int line) {
        if (obj->type() == ObjectType::LIST) {
            if (idx->type() != ObjectType::INTEGER) {
                return makeError("list index must be an integer, got " + typeName(idx->type()) + "", line);
            }
            auto* list = static_cast<ListObject*>(obj.get());
            long long i = static_cast<IntegerObject*>(idx.get())->value;
            long long n = (long long)list->elements.size();
            if (i < 0) i += n; // negatif indeks: liste[-1] son eleman
            if (i < 0 || i >= n) {
                return makeError("list index out of range: " + idx->inspect() +
                                 " (length " + std::to_string(n) + ")", line);
            }
            return list->elements[i];
        }

        if (obj->type() == ObjectType::MAP) {
            if (!isValidMapKey(idx)) {
                return makeError("map keys must be string, int or bool; got " +
                                 typeName(idx->type()) + "", line);
            }
            auto val = static_cast<MapObject*>(obj.get())->get(idx);
            if (val == nullptr) {
                return makeError("key not in map: " + inspectQuoted(idx) +
                                 " (check with has())", line);
            }
            return val;
        }

        if (obj->type() == ObjectType::STRING) {
            if (idx->type() != ObjectType::INTEGER) {
                return makeError("string index must be an integer, got " + typeName(idx->type()) + "", line);
            }
            const std::string& s = static_cast<StringObject*>(obj.get())->value;
            long long i = static_cast<IntegerObject*>(idx.get())->value;
            long long n = utf8Length(s);
            if (i < 0) i += n;
            if (i < 0 || i >= n) {
                return makeError("string index out of range: " + idx->inspect() +
                                 " (length " + std::to_string(n) + ")", line);
            }
            return std::make_shared<StringObject>(utf8At(s, i));
        }

        return makeError("indexing only works on list, map and string; got " +
                         typeName(obj->type()) + "", line);
    }

    static std::shared_ptr<Object> evalPrefixExpression(const std::string& op,
                                                        const std::shared_ptr<Object>& right,
                                                        int line) {
        if (op == "not") {
            return boolObj(!isTruthy(right));
        }
        if (op == "~") {
            if (right->type() != ObjectType::INTEGER) {
                return makeError("'~' only works on integers, got " +
                                 typeName(right->type()) + "", line);
            }
            return std::make_shared<IntegerObject>(~static_cast<IntegerObject*>(right.get())->value);
        }
        if (op == "-") {
            if (right->type() == ObjectType::INTEGER) {
                return std::make_shared<IntegerObject>(-static_cast<IntegerObject*>(right.get())->value);
            }
            if (right->type() == ObjectType::FLOAT) {
                return std::make_shared<FloatObject>(-static_cast<FloatObject*>(right.get())->value);
            }
            return makeError("unary '-' only works on numbers, got " +
                             typeName(right->type()) + "", line);
        }
        return makeError("unknown unary operator: " + op, line);
    }

public:
    // Binary operator engine, also used by compound assignments
    static std::shared_ptr<Object> evalInfixExpression(const std::string& op,
                                                       const std::shared_ptr<Object>& left,
                                                       const std::shared_ptr<Object>& right,
                                                       int line) {
        // Equality is defined for every type pair
        if (op == "==") return boolObj(objectEquals(left, right));
        if (op == "!=") return boolObj(!objectEquals(left, right));

        // Membership: element in list / substring in string / key in map / number in range
        if (op == "in") {
            switch (right->type()) {
                case ObjectType::LIST: {
                    for (const auto& e : static_cast<ListObject*>(right.get())->elements) {
                        if (objectEquals(e, left)) return TRUE_OBJ;
                    }
                    return FALSE_OBJ;
                }
                case ObjectType::STRING: {
                    if (left->type() != ObjectType::STRING) {
                        return makeError("only a string can be searched inside a string: " +
                                         typeName(left->type()) + " in string", line);
                    }
                    const std::string& hay = static_cast<StringObject*>(right.get())->value;
                    const std::string& needle = static_cast<StringObject*>(left.get())->value;
                    return boolObj(hay.find(needle) != std::string::npos);
                }
                case ObjectType::MAP:
                    return boolObj(static_cast<MapObject*>(right.get())->get(left) != nullptr);
                case ObjectType::RANGE: {
                    if (left->type() != ObjectType::INTEGER) return FALSE_OBJ;
                    auto* r = static_cast<RangeObject*>(right.get());
                    long long v = static_cast<IntegerObject*>(left.get())->value;
                    if (r->step > 0) {
                        return boolObj(v >= r->start && v < r->end && (v - r->start) % r->step == 0);
                    }
                    return boolObj(v <= r->start && v > r->end && (r->start - v) % (-r->step) == 0);
                }
                default:
                    return makeError("'in' expects list/map/string/range on the right, got " +
                                     typeName(right->type()) + "", line);
            }
        }

        // String operations
        if (left->type() == ObjectType::STRING && right->type() == ObjectType::STRING) {
            const std::string& l = static_cast<StringObject*>(left.get())->value;
            const std::string& r = static_cast<StringObject*>(right.get())->value;
            if (op == "+")  return std::make_shared<StringObject>(l + r);
            if (op == "<")  return boolObj(l < r);
            if (op == ">")  return boolObj(l > r);
            if (op == "<=") return boolObj(l <= r);
            if (op == ">=") return boolObj(l >= r);
            return makeError("unsupported operator on strings: " + op, line);
        }

        // "abc" * 3 -> string repetition (Python model)
        if ((left->type() == ObjectType::STRING && right->type() == ObjectType::INTEGER) ||
            (left->type() == ObjectType::INTEGER && right->type() == ObjectType::STRING)) {
            if (op == "*") {
                const auto& strObj = (left->type() == ObjectType::STRING) ? left : right;
                const auto& intObj = (left->type() == ObjectType::INTEGER) ? left : right;
                long long count = static_cast<IntegerObject*>(intObj.get())->value;
                if (count < 0) count = 0;
                if (count > 1000000) {
                    return makeError("string repetition limit exceeded (1,000,000)", line);
                }
                const std::string& s = static_cast<StringObject*>(strObj.get())->value;
                std::string out;
                out.reserve(s.size() * (size_t)count);
                for (long long i = 0; i < count; ++i) out += s;
                return std::make_shared<StringObject>(out);
            }
        }

        // List concatenation: [1] + [2] -> [1, 2]
        if (left->type() == ObjectType::LIST && right->type() == ObjectType::LIST) {
            if (op == "+") {
                auto out = std::make_shared<ListObject>();
                auto* la = static_cast<ListObject*>(left.get());
                auto* lb = static_cast<ListObject*>(right.get());
                out->elements.reserve(la->elements.size() + lb->elements.size());
                out->elements.insert(out->elements.end(), la->elements.begin(), la->elements.end());
                out->elements.insert(out->elements.end(), lb->elements.begin(), lb->elements.end());
                return out;
            }
            return makeError("unsupported operator on lists: " + op, line);
        }

        // Integer operations
        if (left->type() == ObjectType::INTEGER && right->type() == ObjectType::INTEGER) {
            long long l = static_cast<IntegerObject*>(left.get())->value;
            long long r = static_cast<IntegerObject*>(right.get())->value;

            if (op == "+") return std::make_shared<IntegerObject>(l + r);
            if (op == "-") return std::make_shared<IntegerObject>(l - r);
            if (op == "*") return std::make_shared<IntegerObject>(l * r);
            if (op == "/") {
                if (r == 0) return makeError("division by zero", line);
                // Floor division: keeps the identity with floor-mod -> (a / b) * b + a % b == a
                long long q = l / r;
                if ((l % r != 0) && ((l < 0) != (r < 0))) q--;
                return std::make_shared<IntegerObject>(q);
            }
            if (op == "&") return std::make_shared<IntegerObject>(l & r);
            if (op == "|") return std::make_shared<IntegerObject>(l | r);
            if (op == "^") return std::make_shared<IntegerObject>(l ^ r);
            if (op == "<<" || op == ">>") {
                if (r < 0 || r > 63) {
                    return makeError("shift amount must be within 0-63: " + std::to_string(r), line);
                }
                return std::make_shared<IntegerObject>(op == "<<" ? (l << r) : (l >> r));
            }
            if (op == "%") {
                if (r == 0) return makeError("modulo by zero", line);
                // Floor mod (Python/Lua rule): the result carries the divisor's sign.
                // The right behavior for grid/angle wrapping in games: -5 % 3 -> 1
                long long m = l % r;
                if (m != 0 && ((m < 0) != (r < 0))) m += r;
                return std::make_shared<IntegerObject>(m);
            }
            if (op == "**") {
                double result = std::pow((double)l, (double)r);
                // Non-negative exponent with an integral result stays int (2 ** 10 -> 1024)
                if (r >= 0 && result == std::floor(result) && std::fabs(result) < 9.2e18) {
                    return std::make_shared<IntegerObject>((long long)result);
                }
                return std::make_shared<FloatObject>(result);
            }
            if (op == "<")  return boolObj(l < r);
            if (op == ">")  return boolObj(l > r);
            if (op == "<=") return boolObj(l <= r);
            if (op == ">=") return boolObj(l >= r);
        }

        // Float or mixed-number operations
        else if ((left->type() == ObjectType::FLOAT || left->type() == ObjectType::INTEGER) &&
                 (right->type() == ObjectType::FLOAT || right->type() == ObjectType::INTEGER)) {

            double l = (left->type() == ObjectType::FLOAT)
                       ? static_cast<FloatObject*>(left.get())->value
                       : (double)static_cast<IntegerObject*>(left.get())->value;
            double r = (right->type() == ObjectType::FLOAT)
                       ? static_cast<FloatObject*>(right.get())->value
                       : (double)static_cast<IntegerObject*>(right.get())->value;

            if (op == "+") return std::make_shared<FloatObject>(l + r);
            if (op == "-") return std::make_shared<FloatObject>(l - r);
            if (op == "*") return std::make_shared<FloatObject>(l * r);
            if (op == "/") {
                if (r == 0.0) return makeError("division by zero", line);
                return std::make_shared<FloatObject>(l / r);
            }
            if (op == "%") {
                if (r == 0.0) return makeError("modulo by zero", line);
                // Floor mod (Python/Lua rule), float version
                double m = std::fmod(l, r);
                if (m != 0.0 && ((m < 0.0) != (r < 0.0))) m += r;
                return std::make_shared<FloatObject>(m);
            }
            if (op == "**") {
                return std::make_shared<FloatObject>(std::pow(l, r));
            }
            if (op == "<")  return boolObj(l < r);
            if (op == ">")  return boolObj(l > r);
            if (op == "<=") return boolObj(l <= r);
            if (op == ">=") return boolObj(l >= r);
        }

        return makeError("unsupported operation: " + typeName(left->type()) + " " + op + " " +
                         typeName(right->type()) +
                         (op == "+" && (left->type() == ObjectType::STRING ||
                                        right->type() == ObjectType::STRING)
                          ? " (use text() or \"{...}\" interpolation to convert)"
                          : ""),
                         line);
    }
};

} // namespace Lume

#endif // EVALUATOR_HPP
