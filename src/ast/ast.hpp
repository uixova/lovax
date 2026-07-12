#ifndef AST_HPP
#define AST_HPP

#include <string>
#include <vector>
#include <memory> // Smart memory management (unique_ptr)
#include "../lexer/token.hpp"

namespace Lume {

// Node type tag: lets the evaluator dispatch
// with a single switch (performance: no RTTI, branch-predictor friendly).
enum class NodeType {
    PROGRAM,

    // Statements
    SET_STATEMENT,
    ASSIGN_STATEMENT,
    SAY_STATEMENT,
    IF_STATEMENT,
    MATCH_STATEMENT,
    WHILE_STATEMENT,
    FOR_STATEMENT,
    BREAK_STATEMENT,
    CONTINUE_STATEMENT,
    RETURN_STATEMENT,
    USE_STATEMENT,
    TRY_STATEMENT,
    THROW_STATEMENT,
    PASS_STATEMENT,
    REPEAT_STATEMENT,
    ENUM_STATEMENT,
    STRUCT_STATEMENT,
    BLOCK_STATEMENT,
    EXPRESSION_STATEMENT,

    // Expressions
    IDENTIFIER,
    INTEGER_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,
    INTERPOLATED_STRING,
    BOOLEAN_LITERAL,
    NULL_LITERAL,
    LIST_LITERAL,
    MAP_LITERAL,
    INDEX_EXPRESSION,
    MEMBER_EXPRESSION,
    CONDITIONAL_EXPRESSION,
    PREFIX_EXPRESSION,
    INFIX_EXPRESSION,
    FUNCTION_LITERAL,
    CALL_EXPRESSION
};

// 1. Base node class (everything derives from this)
class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual NodeType nodeType() const = 0;        // Tag for the evaluator switch
    virtual std::string tokenLiteral() const = 0; // Returns the token's raw text for debugging
    virtual int line() const = 0;                 // Line number for error messages
};

// 2. Base class for statements
// Note: lines that perform an action without producing a value (e.g. set x = 5)
class Statement : public ASTNode {
public:
    virtual void statementNode() = 0;
};

// 3. Base class for expressions
// Note: constructs that produce a value (e.g. 5 + 5, speed, "merhaba")
class Expression : public ASTNode {
public:
    virtual void expressionNode() = 0;
};

// 4. Root node: represents the whole Lume program
class Program : public ASTNode {
public:
    std::vector<std::unique_ptr<Statement>> statements;

    NodeType nodeType() const override { return NodeType::PROGRAM; }
    int line() const override { return statements.empty() ? 0 : statements[0]->line(); }
    std::string tokenLiteral() const override {
        if (!statements.empty()) {
            return statements[0]->tokenLiteral();
        }
        return "";
    }
};

// Expression class representing variable names in the tree
class Identifier : public Expression {
public:
    Token token;        // The TokenType::IDENTIFIER token
    std::string value;  // The variable name (e.g. "speed")

    NodeType nodeType() const override { return NodeType::IDENTIFIER; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Variable definition statement -> "set <identifier> = <expression>"
// Also covers: const bindings, parallel assignment (set a, b = 1, 2) and
// list unpacking (set a, b = some_list).
class SetStatement : public Statement {
public:
    Token token; // 'set' or 'const' token
    bool isConst = false;
    std::unique_ptr<Identifier> name;
    std::vector<std::unique_ptr<Identifier>> extraNames;   // parallel targets after the first
    std::unique_ptr<Expression> value;
    std::vector<std::unique_ptr<Expression>> extraValues;  // parallel values after the first

    NodeType nodeType() const override { return NodeType::SET_STATEMENT; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void statementNode() override {}
};

// Reassignment and compound assignment -> "x = 5", "x += 1", "list[0] = 9"
// RFC-001: 'set' DEFINES; bare assignment updates an EXISTING variable (error otherwise).
class AssignStatement : public Statement {
public:
    Token token;                        // '=' / '+=' / '-=' ... token
    std::string op;                     // "=", "+=", "-=", "*=", "/="
    std::unique_ptr<Expression> target; // Identifier or IndexExpression
    std::unique_ptr<Expression> value;

    NodeType nodeType() const override { return NodeType::ASSIGN_STATEMENT; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void statementNode() override {}
};

// Integer literal node (e.g. 42)
class IntegerLiteral : public Expression {
public:
    Token token;
    long long value;

    NodeType nodeType() const override { return NodeType::INTEGER_LITERAL; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Float literal node (e.g. 3.14)
class FloatLiteral : public Expression {
public:
    Token token;
    double value;

    NodeType nodeType() const override { return NodeType::FLOAT_LITERAL; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

class StringLiteral : public Expression {
public:
    Token token;       // STRING token
    std::string value; // Final text with escapes processed

    NodeType nodeType() const override { return NodeType::STRING_LITERAL; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Interpolated string -> "hp: {hp}" (RFC-002)
// Parts are evaluated in order and joined as text.
class InterpolatedString : public Expression {
public:
    Token token;
    std::vector<std::unique_ptr<Expression>> parts; // StringLiteral chunks mixed with embedded expressions

    NodeType nodeType() const override { return NodeType::INTERPOLATED_STRING; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Boolean literal node -> "true" or "false"
class BooleanLiteral : public Expression {
public:
    Token token;
    bool value;

    NodeType nodeType() const override { return NodeType::BOOLEAN_LITERAL; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Null value -> "null"
class NullLiteral : public Expression {
public:
    Token token;

    NodeType nodeType() const override { return NodeType::NULL_LITERAL; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// List literal -> [1, 2, "three"]
class ListLiteral : public Expression {
public:
    Token token; // '[' token
    std::vector<std::unique_ptr<Expression>> elements;

    NodeType nodeType() const override { return NodeType::LIST_LITERAL; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Map literal -> {"name": "Kai", "hp": 100}
class MapLiteral : public Expression {
public:
    Token token; // '{' token
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> pairs;

    NodeType nodeType() const override { return NodeType::MAP_LITERAL; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Index access -> list[0], map["key"], text[2]
class IndexExpression : public Expression {
public:
    Token token; // '[' token
    std::unique_ptr<Expression> object;
    std::unique_ptr<Expression> index;    // slice: start (null = from 0)
    bool isSlice = false;
    std::unique_ptr<Expression> indexEnd; // slice: end (null = to length)

    NodeType nodeType() const override { return NodeType::INDEX_EXPRESSION; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Member access -> math.lerp, player.hp (RFC-006)
class MemberExpression : public Expression {
public:
    Token token; // '.' or '?.' token
    std::unique_ptr<Expression> object;
    std::string property; // name after the dot
    bool safe = false;    // '?.' — yields null instead of erroring when object is null

    NodeType nodeType() const override { return NodeType::MEMBER_EXPRESSION; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Module import -> use math / use "file.lm" as x / use math: lerp, clamp (RFC-006)
class UseStatement : public Statement {
public:
    Token token;                     // 'use' token
    bool isFile = false;             // true: a "file.lm" path, false: a built-in module name
    std::string target;              // module name or file path
    std::string alias;               // 'as' alias (empty if none)
    std::vector<std::string> names;  // selective import names (if empty, the module object is bound)

    NodeType nodeType() const override { return NodeType::USE_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// Conditional expression (ternary) -> "low" if x < 5 else "high"
class ConditionalExpression : public Expression {
public:
    Token token; // 'if' jetonu
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> thenExpr;
    std::unique_ptr<Expression> elseExpr;

    NodeType nodeType() const override { return NodeType::CONDITIONAL_EXPRESSION; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Unary (prefix) node -> -5, not active
class PrefixExpression : public Expression {
public:
    Token token;    // '-' or 'not' token
    std::string op; // "-" or "not"
    std::unique_ptr<Expression> right;

    NodeType nodeType() const override { return NodeType::PREFIX_EXPRESSION; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Binary operation node (e.g. Left + Right)
class InfixExpression : public Expression {
public:
    Token token;             // Operator token (+, -, *, /, and, or ...)
    std::string op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;

    NodeType nodeType() const override { return NodeType::INFIX_EXPRESSION; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void expressionNode() override {}
};

// Print statement node -> "say <expr>, <expr>, ..."
class SayStatement : public Statement {
public:
    Token token; // 'say' token
    std::vector<std::unique_ptr<Expression>> values; // Comma-separated values

    NodeType nodeType() const override { return NodeType::SAY_STATEMENT; }
    int line() const override { return token.line; }
    std::string tokenLiteral() const override { return token.literal; }
    void statementNode() override {}
};

class BlockStatement : public Statement {
public:
    Token token;
    std::vector<std::unique_ptr<Statement>> statements;

    NodeType nodeType() const override { return NodeType::BLOCK_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// try / catch error handling (RFC-008) -> try: ... catch err: ...
class TryStatement : public Statement {
public:
    Token token; // 'try' token
    std::unique_ptr<BlockStatement> tryBlock;
    std::unique_ptr<Identifier> catchName; // the caught error message binding
    std::unique_ptr<BlockStatement> catchBlock;
    std::unique_ptr<BlockStatement> finallyBlock; // null unless a finally clause is present

    NodeType nodeType() const override { return NodeType::TRY_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// throw <expr> -> raises a runtime error with the value's text
class ThrowStatement : public Statement {
public:
    Token token;
    std::unique_ptr<Expression> value;

    NodeType nodeType() const override { return NodeType::THROW_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// pass -> empty-block no-op
class PassStatement : public Statement {
public:
    Token token;

    NodeType nodeType() const override { return NodeType::PASS_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// repeat <count>: body -> runs the body count times
class RepeatStatement : public Statement {
public:
    Token token;
    std::unique_ptr<Expression> count;
    std::unique_ptr<BlockStatement> body;

    NodeType nodeType() const override { return NodeType::REPEAT_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// enum State: IDLE, WALK, ATTACK -> a frozen module-like map of int constants
class EnumStatement : public Statement {
public:
    Token token;
    std::string name;
    std::vector<std::string> members; // members[i] gets value i

    NodeType nodeType() const override { return NodeType::ENUM_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class IfStatement : public Statement {
public:
    Token token; // 'if' token (also used by chained else-if)
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStatement> consequence;
    // 'else' block (BlockStatement) OR else-if chain (IfStatement) — may stay null
    std::unique_ptr<Statement> alternative;

    NodeType nodeType() const override { return NodeType::IF_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// One match branch: patterns (comma-separated) + body. isDefault = the '_' wildcard branch
struct MatchCase {
    std::vector<std::unique_ptr<Expression>> patterns;
    std::unique_ptr<BlockStatement> body;
    bool isDefault = false;
};

// match <expr>: + indented pattern branches (RFC-004)
class MatchStatement : public Statement {
public:
    Token token; // 'match' jetonu
    std::unique_ptr<Expression> subject;
    std::vector<MatchCase> cases;

    NodeType nodeType() const override { return NodeType::MATCH_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class WhileStatement : public Statement {
public:
    Token token; // 'while' or 'until' token
    bool untilForm = false; // until cond: == while not cond:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStatement> body;

    NodeType nodeType() const override { return NodeType::WHILE_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// for <name> in <expr>: body
// Pair form: for i, x in list / for k, v in map (index+element / key+value)
class ForStatement : public Statement {
public:
    Token token; // 'for' token
    std::unique_ptr<Identifier> variable;
    std::unique_ptr<Identifier> variable2; // null unless the pair form is used
    std::unique_ptr<Expression> iterable;
    std::unique_ptr<BlockStatement> body;

    NodeType nodeType() const override { return NodeType::FOR_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class BreakStatement : public Statement {
public:
    Token token;

    NodeType nodeType() const override { return NodeType::BREAK_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class ContinueStatement : public Statement {
public:
    Token token;

    NodeType nodeType() const override { return NodeType::CONTINUE_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class ReturnStatement : public Statement {
public:
    Token token; // 'return' token
    std::unique_ptr<Expression> returnValue;

    NodeType nodeType() const override { return NodeType::RETURN_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class FunctionLiteral : public Expression {
public:
    Token token; // 'fn' token
    std::unique_ptr<Identifier> name; // stays null for anonymous functions
    std::vector<std::unique_ptr<Identifier>> parameters;
    bool variadic = false; // last parameter declared as rest... collects extra args
    // Defaults: aligned with parameters; null for parameters without one
    std::vector<std::unique_ptr<Expression>> defaults;
    std::unique_ptr<BlockStatement> body;

    NodeType nodeType() const override { return NodeType::FUNCTION_LITERAL; }
    int line() const override { return token.line; }
    void expressionNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

// struct Player: field defaults + methods (RFC-003, composition — no inheritance)
class StructStatement : public Statement {
public:
    Token token;
    std::string name;
    std::vector<std::string> fields;                          // field names
    std::vector<std::unique_ptr<Expression>> fieldDefaults;   // aligned; null = no default
    std::vector<std::unique_ptr<FunctionLiteral>> methods;    // methods (first param is implicit 'this')

    NodeType nodeType() const override { return NodeType::STRUCT_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class CallExpression : public Expression {
public:
    Token token; // Left paren '(' token
    std::unique_ptr<Expression> function;
    std::vector<std::unique_ptr<Expression>> arguments;

    NodeType nodeType() const override { return NodeType::CALL_EXPRESSION; }
    int line() const override { return token.line; }
    void expressionNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

class ExpressionStatement : public Statement {
public:
    Token token; // First token of the expression
    std::unique_ptr<Expression> expression;

    NodeType nodeType() const override { return NodeType::EXPRESSION_STATEMENT; }
    int line() const override { return token.line; }
    void statementNode() override {}
    std::string tokenLiteral() const override { return token.literal; }
};

} // namespace Lume

#endif // AST_HPP
