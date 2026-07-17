#ifndef PARSER_HPP
#define PARSER_HPP

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include "../lexer/lexer.hpp"
#include "../ast/ast.hpp"

namespace Lovax {

enum Precedence {
    LOWEST = 1,
    TERNARY,       // a if condition else b
    P_COALESCE,    // a ?? b (null-coalescing)
    P_OR,          // or (lowest-precedence logical operator)
    P_AND,         // and
    EQUALS,        // == and !=
    LESSGREATER,   // < > <= >= in is
    P_RANGE,       // a..b (end-exclusive range)
    P_BITOR,       // |
    P_BITXOR,      // ^
    P_BITAND,      // &
    P_SHIFT,       // << >>
    SUM,           // + -
    PRODUCT,       // * / %
    PREFIX,        // -x, not x
    P_POWER,       // ** (right-associative; -x**2 = -(x**2))
    CALL,          // function call f(x)
    INDEX          // liste[0], nesne.alan
};

// Collected syntax errors: the parser recovers and continues instead of stopping at the first
struct ParserError {
    std::string message;
    int line;
    int column;

    std::string toString() const {
        return "[Syntax Error] line " + std::to_string(line) +
               ", column " + std::to_string(column) + ": " + message;
    }
};

class Parser {
private:
    Lexer& lexer;
    Token curToken;
    Token peekToken;
    std::vector<ParserError> errorList;
    int exprDepth = 0; // Protection against pathological nesting (fuzz safety)

    static constexpr int MAX_ERRORS = 20;
    static constexpr int MAX_EXPR_DEPTH = 200;

    std::unordered_map<TokenType, Precedence> precedences = {
        {TokenType::OR, Precedence::P_OR},
        {TokenType::AND, Precedence::P_AND},
        {TokenType::EQUAL, Precedence::EQUALS},
        {TokenType::NOT_EQUAL, Precedence::EQUALS},
        {TokenType::LESS_THAN, Precedence::LESSGREATER},
        {TokenType::GREATER_THAN, Precedence::LESSGREATER},
        {TokenType::LESS_EQUAL, Precedence::LESSGREATER},
        {TokenType::GREATER_EQUAL, Precedence::LESSGREATER},
        {TokenType::IN, Precedence::LESSGREATER},
        {TokenType::IS, Precedence::LESSGREATER},
        {TokenType::DOTDOT, Precedence::P_RANGE},
        {TokenType::QQ, Precedence::P_COALESCE},
        {TokenType::PIPE, Precedence::P_BITOR},
        {TokenType::CARET, Precedence::P_BITXOR},
        {TokenType::AMPERSAND, Precedence::P_BITAND},
        {TokenType::SHIFT_LEFT, Precedence::P_SHIFT},
        {TokenType::SHIFT_RIGHT, Precedence::P_SHIFT},
        {TokenType::IF, Precedence::TERNARY},
        {TokenType::POWER, Precedence::P_POWER},
        {TokenType::PLUS, Precedence::SUM},
        {TokenType::MINUS, Precedence::SUM},
        {TokenType::ASTERISK, Precedence::PRODUCT},
        {TokenType::SLASH, Precedence::PRODUCT},
        {TokenType::PERCENT, Precedence::PRODUCT},
        {TokenType::LPAREN, Precedence::CALL},
        {TokenType::LBRACKET, Precedence::INDEX},
        {TokenType::DOT, Precedence::INDEX},
        {TokenType::QDOT, Precedence::INDEX}
    };

    // RAII guard for parseExpression depth protection
    struct DepthGuard {
        int& depth;
        DepthGuard(int& d) : depth(d) { depth++; }
        ~DepthGuard() { depth--; }
    };

public:
    Parser(Lexer& l) : lexer(l) {
        nextParserToken();
        nextParserToken();
    }

    const std::vector<ParserError>& errors() const { return errorList; }

    void nextParserToken() {
        curToken = peekToken;
        peekToken = lexer.nextToken();
    }

    std::unique_ptr<Program> parseProgram() {
        auto program = std::make_unique<Program>();

        while (curToken.type != TokenType::END_OF_FILE) {
            if (errorList.size() >= MAX_ERRORS) {
                addError("too many errors, parsing stopped", curToken);
                break;
            }
            // Skip blank lines in the main loop
            if (curToken.type == TokenType::NEWLINE) {
                nextParserToken();
                continue;
            }
            size_t errsBefore = errorList.size();
            auto stmt = parseStatement();
            if (stmt != nullptr) {
                program->statements.push_back(std::move(stmt));
            } else if (errorList.size() > errsBefore) {
                // On error, panic-mode: skip to end of line and try the next statement
                synchronize();
            }
            nextParserToken();
        }

        return program;
    }

private:
    void addError(const std::string& msg, const Token& tok) {
        errorList.push_back(ParserError{msg, tok.line, tok.column});
    }

    // Panic-mode recovery: skips to the next newline (or end of file).
    // This lets a single run report multiple errors in one file.
    void synchronize() {
        while (curToken.type != TokenType::NEWLINE &&
               curToken.type != TokenType::END_OF_FILE) {
            nextParserToken();
        }
    }

    Precedence peekPrecedence() {
        auto it = precedences.find(peekToken.type);
        if (it != precedences.end()) return it->second;
        return Precedence::LOWEST;
    }

    Precedence curPrecedence() {
        auto it = precedences.find(curToken.type);
        if (it != precedences.end()) return it->second;
        return Precedence::LOWEST;
    }

    bool expectPeek(TokenType t) {
        if (peekToken.type == t) {
            nextParserToken();
            return true;
        }
        if (peekToken.type == TokenType::ILLEGAL) {
            addError(peekToken.literal, peekToken);
        } else {
            addError("expected " + tokenTypeName(t) + " but got " +
                     tokenTypeName(peekToken.type) + "", peekToken);
        }
        return false;
    }

    // ===== Statement parsing =====

    std::unique_ptr<Statement> parseStatement() {
        switch (curToken.type) {
            case TokenType::SET:      return parseSetStatement(false);
            case TokenType::SAY:      return parseSayStatement();
            case TokenType::IF:       return parseIfStatement();
            case TokenType::MATCH:    return parseMatchStatement();
            case TokenType::WHILE:    return parseWhileStatement(false);
            case TokenType::FOR:      return parseForStatement();
            case TokenType::BREAK:    return parseBreakStatement();
            case TokenType::CONTINUE: return parseContinueStatement();
            case TokenType::RETURN:   return parseReturnStatement();
            case TokenType::USE:      return parseUseStatement();
            case TokenType::CONST:    return parseSetStatement(true);
            case TokenType::TRY:      return parseTryStatement();
            case TokenType::THROW:    return parseThrowStatement();
            case TokenType::PASS:     return parsePassStatement();
            case TokenType::REPEAT:   return parseRepeatStatement();
            case TokenType::UNTIL:    return parseWhileStatement(true);
            case TokenType::ENUM:     return parseEnumStatement();
            case TokenType::STRUCT:   return parseStructStatement();
            case TokenType::ILLEGAL:
                addError(curToken.literal, curToken);
                return nullptr;
            case TokenType::ELSE:
                addError("'else' can only follow an 'if' block", curToken);
                return nullptr;
            case TokenType::INDENT:
                addError("unexpected indentation: this line belongs to no block", curToken);
                return nullptr;
            default:
                if (curToken.type != TokenType::NEWLINE &&
                    curToken.type != TokenType::END_OF_FILE &&
                    curToken.type != TokenType::DEDENT) {
                    return parseExpressionOrAssignStatement();
                }
                return nullptr;
        }
    }

    // Expression statement OR assignment: distinguishes expressions like "square(5)"
    // from assignments "x = 5", "x += 1", "list[0] = 9" (RFC-001).
    std::unique_ptr<Statement> parseExpressionOrAssignStatement() {
        Token firstToken = curToken;
        auto expr = parseExpression(Precedence::LOWEST);
        if (expr == nullptr) return nullptr;

        TokenType pt = peekToken.type;
        if (pt == TokenType::ASSIGN || pt == TokenType::PLUS_ASSIGN ||
            pt == TokenType::MINUS_ASSIGN || pt == TokenType::ASTERISK_ASSIGN ||
            pt == TokenType::SLASH_ASSIGN || pt == TokenType::PERCENT_ASSIGN ||
            pt == TokenType::AMP_ASSIGN || pt == TokenType::PIPE_ASSIGN ||
            pt == TokenType::CARET_ASSIGN || pt == TokenType::SHL_ASSIGN ||
            pt == TokenType::SHR_ASSIGN || pt == TokenType::QQ_ASSIGN) {

            if (expr->nodeType() != NodeType::IDENTIFIER &&
                expr->nodeType() != NodeType::INDEX_EXPRESSION &&
                expr->nodeType() != NodeType::MEMBER_EXPRESSION) {
                addError("assignment target must be a variable, an index (list[i]) or a member (object.field)", peekToken);
                return nullptr;
            }

            nextParserToken(); // advance to the assignment operator
            auto stmt = std::make_unique<AssignStatement>();
            stmt->token = curToken;
            stmt->op = curToken.literal;
            stmt->target = std::move(expr);

            nextParserToken(); // advance to the value
            stmt->value = parseExpression(Precedence::LOWEST);
            if (stmt->value == nullptr) return nullptr;
            return stmt;
        }

        auto stmt = std::make_unique<ExpressionStatement>();
        stmt->token = firstToken;
        stmt->expression = std::move(expr);
        return stmt;
    }

    std::unique_ptr<SetStatement> parseSetStatement(bool isConst) {
        auto stmt = std::make_unique<SetStatement>();
        stmt->token = curToken;
        stmt->isConst = isConst;

        if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;

        auto ident = std::make_unique<Identifier>();
        ident->token = curToken;
        ident->value = curToken.literal;
        stmt->name = std::move(ident);

        // Parallel targets: set a, b, c = ...
        while (peekToken.type == TokenType::COMMA) {
            nextParserToken(); // comma
            if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
            auto extra = std::make_unique<Identifier>();
            extra->token = curToken;
            extra->value = curToken.literal;
            stmt->extraNames.push_back(std::move(extra));
        }

        if (!expectPeek(TokenType::ASSIGN)) return nullptr;

        nextParserToken();
        stmt->value = parseExpression(Precedence::LOWEST);
        if (stmt->value == nullptr) return nullptr;

        // Parallel values: ... = 1, 2, 3
        while (peekToken.type == TokenType::COMMA) {
            nextParserToken(); // comma
            nextParserToken();
            auto extra = parseExpression(Precedence::LOWEST);
            if (extra == nullptr) return nullptr;
            stmt->extraValues.push_back(std::move(extra));
        }

        // Count check: n targets need n values, or a single value (list unpacking)
        size_t targets = 1 + stmt->extraNames.size();
        size_t values = 1 + stmt->extraValues.size();
        if (values != 1 && values != targets) {
            addError("parallel assignment mismatch: " + std::to_string(targets) +
                     " target(s) but " + std::to_string(values) + " value(s)", stmt->token);
            return nullptr;
        }

        return stmt;
    }

    // try: block catch err: block  (RFC-008)
    std::unique_ptr<TryStatement> parseTryStatement() {
        auto stmt = std::make_unique<TryStatement>();
        stmt->token = curToken;

        stmt->tryBlock = parseColonBlock();
        if (stmt->tryBlock == nullptr) return nullptr;

        // 'catch' is optional when a 'finally' clause is present (try/finally).
        if (peekToken.type == TokenType::CATCH) {
            nextParserToken(); // 'catch'
            if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
            stmt->catchName = std::make_unique<Identifier>();
            stmt->catchName->token = curToken;
            stmt->catchName->value = curToken.literal;

            stmt->catchBlock = parseColonBlock();
            if (stmt->catchBlock == nullptr) return nullptr;
        }

        if (peekToken.type == TokenType::FINALLY) {
            nextParserToken(); // 'finally'
            stmt->finallyBlock = parseColonBlock();
            if (stmt->finallyBlock == nullptr) return nullptr;
        }

        if (stmt->catchBlock == nullptr && stmt->finallyBlock == nullptr) {
            addError("'try' needs a 'catch' or a 'finally' clause", stmt->token);
            return nullptr;
        }
        return stmt;
    }

    // enum State: IDLE, WALK, ATTACK  (members separated by commas and/or newlines)
    std::unique_ptr<EnumStatement> parseEnumStatement() {
        auto stmt = std::make_unique<EnumStatement>();
        stmt->token = curToken;
        if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
        stmt->name = curToken.literal;
        if (!expectPeek(TokenType::COLON)) return nullptr;
        if (peekToken.type == TokenType::NEWLINE) {
            // Block form: one member per line (commas optional).
            nextParserToken(); // newline
            if (!expectPeek(TokenType::INDENT)) return nullptr;
            nextParserToken();
            while (curToken.type != TokenType::DEDENT && curToken.type != TokenType::END_OF_FILE) {
                if (curToken.type == TokenType::NEWLINE || curToken.type == TokenType::COMMA) {
                    nextParserToken();
                    continue;
                }
                if (curToken.type != TokenType::IDENTIFIER) {
                    addError("enum members must be identifiers", curToken);
                    return nullptr;
                }
                stmt->members.push_back(curToken.literal);
                nextParserToken();
            }
        } else {
            // Inline form: enum State: IDLE, WALK, ATTACK
            if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
            stmt->members.push_back(curToken.literal);
            while (peekToken.type == TokenType::COMMA) {
                nextParserToken(); // comma
                if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
                stmt->members.push_back(curToken.literal);
            }
        }
        if (stmt->members.empty()) {
            addError("enum needs at least one member", stmt->token);
            return nullptr;
        }
        return stmt;
    }

    // struct Player: field defaults + methods (RFC-003)
    std::unique_ptr<StructStatement> parseStructStatement() {
        auto stmt = std::make_unique<StructStatement>();
        stmt->token = curToken;
        if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
        stmt->name = curToken.literal;
        if (!expectPeek(TokenType::COLON)) return nullptr;
        if (peekToken.type == TokenType::NEWLINE) nextParserToken();
        if (!expectPeek(TokenType::INDENT)) return nullptr;
        nextParserToken();
        while (curToken.type != TokenType::DEDENT && curToken.type != TokenType::END_OF_FILE) {
            if (curToken.type == TokenType::NEWLINE) { nextParserToken(); continue; }
            if (curToken.type == TokenType::FN) {
                auto method = parseFunctionLiteral();
                if (method == nullptr) return nullptr;
                if (!method->name) { addError("struct methods must be named", stmt->token); return nullptr; }
                stmt->methods.push_back(std::move(method));
                nextParserToken();
                continue;
            }
            if (curToken.type == TokenType::IDENTIFIER) {
                stmt->fields.push_back(curToken.literal);
                if (peekToken.type == TokenType::ASSIGN) {
                    nextParserToken(); // '='
                    nextParserToken();
                    auto def = parseExpression(Precedence::LOWEST);
                    if (def == nullptr) return nullptr;
                    stmt->fieldDefaults.push_back(std::move(def));
                } else {
                    stmt->fieldDefaults.push_back(nullptr);
                }
                nextParserToken();
                continue;
            }
            addError("struct body expects fields or 'fn' methods", curToken);
            return nullptr;
        }
        return stmt;
    }

    std::unique_ptr<Expression> parseYieldExpression() {
        auto y = std::make_unique<YieldExpression>();
        y->token = curToken;
        // 'yield' alone yields null; 'yield <expr>' yields the value.
        TokenType nt = peekToken.type;
        if (nt != TokenType::NEWLINE && nt != TokenType::DEDENT &&
            nt != TokenType::END_OF_FILE && nt != TokenType::RPAREN &&
            nt != TokenType::RBRACKET && nt != TokenType::COMMA &&
            nt != TokenType::COLON) {
            nextParserToken();
            y->value = parseExpression(Precedence::LOWEST);
            if (y->value == nullptr) return nullptr;
        }
        return y;
    }

    std::unique_ptr<ThrowStatement> parseThrowStatement() {
        auto stmt = std::make_unique<ThrowStatement>();
        stmt->token = curToken;
        nextParserToken();
        stmt->value = parseExpression(Precedence::LOWEST);
        if (stmt->value == nullptr) return nullptr;
        return stmt;
    }

    std::unique_ptr<PassStatement> parsePassStatement() {
        auto stmt = std::make_unique<PassStatement>();
        stmt->token = curToken;
        return stmt;
    }

    std::unique_ptr<RepeatStatement> parseRepeatStatement() {
        auto stmt = std::make_unique<RepeatStatement>();
        stmt->token = curToken;
        nextParserToken();
        stmt->count = parseExpression(Precedence::LOWEST);
        if (stmt->count == nullptr) return nullptr;
        stmt->body = parseColonBlock();
        if (stmt->body == nullptr) return nullptr;
        return stmt;
    }

    std::unique_ptr<SayStatement> parseSayStatement() {
        auto stmt = std::make_unique<SayStatement>();
        stmt->token = curToken;

        nextParserToken();
        auto first = parseExpression(Precedence::LOWEST);
        if (first == nullptr) return nullptr;
        stmt->values.push_back(std::move(first));

        // Comma-separated values: say "hp:", hp
        while (peekToken.type == TokenType::COMMA) {
            nextParserToken(); // advance past the comma
            nextParserToken(); // advance to the value
            auto next = parseExpression(Precedence::LOWEST);
            if (next == nullptr) return nullptr;
            stmt->values.push_back(std::move(next));
        }

        return stmt;
    }

    std::unique_ptr<ReturnStatement> parseReturnStatement() {
        auto stmt = std::make_unique<ReturnStatement>();
        stmt->token = curToken;

        nextParserToken(); // skip the 'return' keyword

        if (curToken.type != TokenType::NEWLINE &&
            curToken.type != TokenType::END_OF_FILE &&
            curToken.type != TokenType::DEDENT) {
            stmt->returnValue = parseExpression(Precedence::LOWEST);
            // Multiple return values build a tuple: return a, b
            if (peekToken.type == TokenType::COMMA) {
                auto tup = std::make_unique<TupleLiteral>();
                tup->token = stmt->token;
                tup->elements.push_back(std::move(stmt->returnValue));
                while (peekToken.type == TokenType::COMMA) {
                    nextParserToken();   // onto the comma
                    nextParserToken();   // onto the next value
                    auto v = parseExpression(Precedence::LOWEST);
                    if (v == nullptr) return stmt;
                    tup->elements.push_back(std::move(v));
                }
                stmt->returnValue = std::move(tup);
            }
        }

        return stmt;
    }

    // use math / use math as m / use math: lerp, clamp / use "file.lov" [as x] [: names]
    std::unique_ptr<UseStatement> parseUseStatement() {
        auto stmt = std::make_unique<UseStatement>();
        stmt->token = curToken;

        if (peekToken.type == TokenType::IDENTIFIER) {
            nextParserToken();
            stmt->isFile = false;
            stmt->target = curToken.literal;
        } else if (peekToken.type == TokenType::STRING) {
            nextParserToken();
            stmt->isFile = true;
            stmt->target = curToken.literal; // raw path (no escapes/interpolation)
        } else {
            addError("use expects a module name or a \"path/to/file.lov\"", peekToken);
            return nullptr;
        }

        if (peekToken.type == TokenType::AS) {
            nextParserToken(); // 'as'
            if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
            stmt->alias = curToken.literal;
        }

        if (peekToken.type == TokenType::COLON) {
            if (!stmt->alias.empty()) {
                addError("'as' and ': names' cannot be combined in a use statement", peekToken);
                return nullptr;
            }
            nextParserToken(); // ':'
            do {
                if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
                stmt->names.push_back(curToken.literal);
                if (peekToken.type != TokenType::COMMA) break;
                nextParserToken(); // ','
            } while (true);
            if (stmt->names.empty()) {
                addError("use ... : requires at least one name", peekToken);
                return nullptr;
            }
        }

        return stmt;
    }

    std::unique_ptr<BreakStatement> parseBreakStatement() {
        auto stmt = std::make_unique<BreakStatement>();
        stmt->token = curToken;
        return stmt;
    }

    std::unique_ptr<ContinueStatement> parseContinueStatement() {
        auto stmt = std::make_unique<ContinueStatement>();
        stmt->token = curToken;
        return stmt;
    }

    // ':' + newline + INDENT + block — shared by if/else/while/for/fn
    std::unique_ptr<BlockStatement> parseColonBlock() {
        if (!expectPeek(TokenType::COLON)) return nullptr;
        if (peekToken.type == TokenType::NEWLINE) {
            nextParserToken();
        }
        if (!expectPeek(TokenType::INDENT)) return nullptr;
        return parseBlockStatement();
    }

    // Conditional (if / else if / else) parsing engine
    std::unique_ptr<IfStatement> parseIfStatement() {
        auto stmt = std::make_unique<IfStatement>();
        stmt->token = curToken; // 'if' veya 'elif'

        nextParserToken(); // skip the 'if' token
        stmt->condition = parseExpression(Precedence::LOWEST);
        if (stmt->condition == nullptr) return nullptr;

        stmt->consequence = parseColonBlock();
        if (stmt->consequence == nullptr) return nullptr;

        // After the block, curToken sits on DEDENT
        if (peekToken.type == TokenType::ELSE) {
            nextParserToken(); // move onto ELSE
            // 'else if': the chain nests as an inner if (plain English, RFC-004)
            if (peekToken.type == TokenType::IF) {
                nextParserToken(); // IF konumuna gel
                stmt->alternative = parseIfStatement();
            } else {
                stmt->alternative = parseColonBlock();
            }
        }

        return stmt;
    }

    // match <expr>: + indented pattern branches (RFC-004)
    //   match durum:
    //       "a", "b":
    //           ...
    //       _:
    //           ...
    std::unique_ptr<MatchStatement> parseMatchStatement() {
        auto stmt = std::make_unique<MatchStatement>();
        stmt->token = curToken;

        nextParserToken(); // skip the 'match' token
        stmt->subject = parseExpression(Precedence::LOWEST);
        if (stmt->subject == nullptr) return nullptr;

        if (!expectPeek(TokenType::COLON)) return nullptr;
        if (peekToken.type == TokenType::NEWLINE) nextParserToken();
        if (!expectPeek(TokenType::INDENT)) return nullptr;

        nextParserToken(); // focus on the first pattern

        while (curToken.type != TokenType::DEDENT &&
               curToken.type != TokenType::END_OF_FILE) {
            if (errorList.size() >= MAX_ERRORS) break;
            if (curToken.type == TokenType::NEWLINE) {
                nextParserToken();
                continue;
            }

            MatchCase mcase;

            // '_' wildcard pattern: matches anything
            if (curToken.type == TokenType::IDENTIFIER && curToken.literal == "_") {
                mcase.isDefault = true;
            } else {
                auto pattern = parseExpression(Precedence::LOWEST);
                if (pattern == nullptr) return nullptr;
                mcase.patterns.push_back(std::move(pattern));

                // Comma-separated multi-pattern: "a", "b":
                while (peekToken.type == TokenType::COMMA) {
                    nextParserToken(); // advance past the comma
                    nextParserToken(); // advance to the next pattern
                    auto next = parseExpression(Precedence::LOWEST);
                    if (next == nullptr) return nullptr;
                    mcase.patterns.push_back(std::move(next));
                }
            }

            mcase.body = parseColonBlock();
            if (mcase.body == nullptr) return nullptr;

            stmt->cases.push_back(std::move(mcase));
            nextParserToken(); // skip the branch's DEDENT
        }

        if (stmt->cases.empty()) {
            addError("match requires at least one pattern branch", stmt->token);
            return nullptr;
        }

        return stmt;
    }

    std::unique_ptr<WhileStatement> parseWhileStatement(bool untilForm) {
        auto stmt = std::make_unique<WhileStatement>();
        stmt->token = curToken;
        stmt->untilForm = untilForm;

        nextParserToken(); // skip the 'while'/'until' token
        stmt->condition = parseExpression(Precedence::LOWEST);
        if (stmt->condition == nullptr) return nullptr;

        stmt->body = parseColonBlock();
        if (stmt->body == nullptr) return nullptr;

        return stmt;
    }

    // for <name> in <expr>: body
    std::unique_ptr<ForStatement> parseForStatement() {
        auto stmt = std::make_unique<ForStatement>();
        stmt->token = curToken;

        if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;

        stmt->variable = std::make_unique<Identifier>();
        stmt->variable->token = curToken;
        stmt->variable->value = curToken.literal;

        // Pair form: for i, x in ... / for k, v in ...
        if (peekToken.type == TokenType::COMMA) {
            nextParserToken(); // comma
            if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
            stmt->variable2 = std::make_unique<Identifier>();
            stmt->variable2->token = curToken;
            stmt->variable2->value = curToken.literal;
        }

        if (!expectPeek(TokenType::IN)) return nullptr;

        nextParserToken(); // advance to the iterable expression
        stmt->iterable = parseExpression(Precedence::LOWEST);
        if (stmt->iterable == nullptr) return nullptr;

        stmt->body = parseColonBlock();
        if (stmt->body == nullptr) return nullptr;

        return stmt;
    }

    // Parses an indented code block
    std::unique_ptr<BlockStatement> parseBlockStatement() {
        auto block = std::make_unique<BlockStatement>();
        block->token = curToken;

        nextParserToken(); // Skip the INDENT token and focus on the first statement

        while (curToken.type != TokenType::DEDENT &&
               curToken.type != TokenType::END_OF_FILE) {
            if (errorList.size() >= MAX_ERRORS) break;
            if (curToken.type == TokenType::NEWLINE) {
                nextParserToken();
                continue;
            }

            size_t errsBefore = errorList.size();
            auto stmt = parseStatement();
            if (stmt != nullptr) {
                block->statements.push_back(std::move(stmt));
            } else if (errorList.size() > errsBefore) {
                synchronize();
            }
            nextParserToken();
        }

        return block;
    }

    std::unique_ptr<FunctionLiteral> parseFunctionLiteral() {
        auto lit = std::make_unique<FunctionLiteral>();
        lit->token = curToken;

        // Named: fn add(...) — Anonymous: set f = fn(...)
        if (peekToken.type == TokenType::IDENTIFIER) {
            nextParserToken();
            lit->name = std::make_unique<Identifier>();
            lit->name->token = curToken;
            lit->name->value = curToken.literal;
        }

        if (!expectPeek(TokenType::LPAREN)) return nullptr;

        // Parameters; each may carry an optional default: fn f(a, b = 10)
        bool seenDefault = false;
        if (peekToken.type != TokenType::RPAREN) {
            do {
                if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;

                for (const auto& p : lit->parameters) {
                    if (p->value == curToken.literal) {
                        addError("duplicate parameter name: '" + curToken.literal + "'", curToken);
                        return nullptr;
                    }
                }

                auto param = std::make_unique<Identifier>();
                param->token = curToken;
                param->value = curToken.literal;
                lit->parameters.push_back(std::move(param));

                // Variadic rest parameter: fn f(a, more...) — must be last, no default
                if (peekToken.type == TokenType::ELLIPSIS) {
                    nextParserToken(); // '...'
                    lit->variadic = true;
                    lit->defaults.push_back(nullptr);
                    if (peekToken.type == TokenType::COMMA) {
                        addError("the rest parameter (name...) must be the last parameter", peekToken);
                        return nullptr;
                    }
                    break;
                }

                if (peekToken.type == TokenType::ASSIGN) {
                    nextParserToken(); // '='
                    nextParserToken(); // advance to the default expression
                    auto defExpr = parseExpression(Precedence::LOWEST);
                    if (defExpr == nullptr) return nullptr;
                    lit->defaults.push_back(std::move(defExpr));
                    seenDefault = true;
                } else {
                    if (seenDefault) {
                        addError("a parameter without a default cannot follow one with a default: '" +
                                 lit->parameters.back()->value + "'", curToken);
                        return nullptr;
                    }
                    lit->defaults.push_back(nullptr);
                }

                if (peekToken.type == TokenType::COMMA) {
                    nextParserToken();
                } else {
                    break;
                }
            } while (true);
        }

        if (!expectPeek(TokenType::RPAREN)) return nullptr;

        // Arrow form: fn(x) -> expr  ==  fn(x): return expr
        if (peekToken.type == TokenType::ARROW) {
            nextParserToken(); // '->'
            nextParserToken(); // start of the expression
            auto body = std::make_unique<BlockStatement>();
            body->token = curToken;
            auto ret = std::make_unique<ReturnStatement>();
            ret->token = lit->token;
            ret->returnValue = parseExpression(Precedence::LOWEST);
            if (ret->returnValue == nullptr) return nullptr;
            body->statements.push_back(std::move(ret));
            lit->body = std::move(body);
            return lit;
        }

        lit->body = parseColonBlock();
        if (lit->body == nullptr) return nullptr;

        return lit;
    }

    // ===== Expression parsing (Pratt) =====

    std::unique_ptr<Expression> parseExpression(Precedence precedence) {
        DepthGuard guard(exprDepth);
        if (exprDepth > MAX_EXPR_DEPTH) {
            addError("expression too deep: nesting limit (" +
                     std::to_string(MAX_EXPR_DEPTH) + ") exceeded", curToken);
            return nullptr;
        }

        std::unique_ptr<Expression> leftExp = parsePrefix();
        if (leftExp == nullptr) return nullptr;

        while (peekToken.type != TokenType::END_OF_FILE && precedence < peekPrecedence()) {
            switch (peekToken.type) {
                case TokenType::LPAREN:
                    nextParserToken();
                    leftExp = parseCallExpression(std::move(leftExp));
                    break;
                case TokenType::LBRACKET:
                    nextParserToken();
                    leftExp = parseIndexExpression(std::move(leftExp));
                    break;
                case TokenType::DOT:
                    nextParserToken();
                    leftExp = parseMemberExpression(std::move(leftExp), false);
                    break;
                case TokenType::QDOT:
                    nextParserToken();
                    leftExp = parseMemberExpression(std::move(leftExp), true);
                    break;
                case TokenType::PLUS: case TokenType::MINUS:
                case TokenType::ASTERISK: case TokenType::SLASH: case TokenType::PERCENT:
                case TokenType::POWER:
                case TokenType::EQUAL: case TokenType::NOT_EQUAL:
                case TokenType::LESS_THAN: case TokenType::GREATER_THAN:
                case TokenType::LESS_EQUAL: case TokenType::GREATER_EQUAL:
                case TokenType::AND: case TokenType::OR: case TokenType::IN:
                case TokenType::IS: case TokenType::DOTDOT: case TokenType::QQ:
                case TokenType::AMPERSAND: case TokenType::PIPE: case TokenType::CARET:
                case TokenType::SHIFT_LEFT: case TokenType::SHIFT_RIGHT:
                    nextParserToken();
                    leftExp = parseInfixExpression(std::move(leftExp));
                    break;
                case TokenType::IF:
                    nextParserToken();
                    leftExp = parseConditionalExpression(std::move(leftExp));
                    break;
                default:
                    return leftExp;
            }
            if (leftExp == nullptr) return nullptr;
        }

        return leftExp;
    }

    // Parses tokens in prefix position
    std::unique_ptr<Expression> parsePrefix() {
        switch (curToken.type) {
            case TokenType::INT:        return parseIntegerLiteral();
            case TokenType::FLOAT:      return parseFloatLiteral();
            case TokenType::STRING:     return parseStringLiteral();
            case TokenType::IDENTIFIER: {
                auto ident = std::make_unique<Identifier>();
                ident->token = curToken;
                ident->value = curToken.literal;
                return ident;
            }
            case TokenType::TRUE:
            case TokenType::FALSE:      return parseBooleanLiteral();
            case TokenType::NULL_KW: {
                auto lit = std::make_unique<NullLiteral>();
                lit->token = curToken;
                return lit;
            }
            case TokenType::THIS: {
                auto ident = std::make_unique<Identifier>();
                ident->token = curToken;
                ident->value = "this";
                return ident;
            }
            case TokenType::FN:         return parseFunctionLiteral();
            case TokenType::YIELD:      return parseYieldExpression();
            case TokenType::MINUS:
            case TokenType::TILDE:
            case TokenType::NOT:        return parsePrefixExpression();
            case TokenType::LPAREN:     return parseGroupedExpression();
            case TokenType::LBRACKET:   return parseListLiteral();
            case TokenType::LBRACE:     return parseMapLiteral();
            case TokenType::ILLEGAL:
                addError(curToken.literal, curToken);
                return nullptr;
            default:
                addError("unexpected token: " + tokenTypeName(curToken.type) +
                         " (a value or expression is expected here)", curToken);
                return nullptr;
        }
    }

    std::unique_ptr<Expression> parsePrefixExpression() {
        auto expr = std::make_unique<PrefixExpression>();
        expr->token = curToken;
        expr->op = curToken.literal;

        // 'not' binds LOOSER than comparisons (Python rule):
        //   not x == y  ->  not (x == y)
        // Unary '-' binds tightest: -2 * 3 -> (-2) * 3
        Precedence operandPrec = (expr->op == "not") ? Precedence::P_AND
                                                     : Precedence::PREFIX;

        nextParserToken();
        expr->right = parseExpression(operandPrec);
        if (expr->right == nullptr) return nullptr;

        return expr;
    }

    std::unique_ptr<Expression> parseGroupedExpression() {
        Token lparen = curToken;
        // Empty tuple: ()
        if (peekToken.type == TokenType::RPAREN) {
            nextParserToken();
            auto tup = std::make_unique<TupleLiteral>();
            tup->token = lparen;
            return tup;
        }
        nextParserToken(); // skip the '(' token
        auto expr = parseExpression(Precedence::LOWEST);
        if (expr == nullptr) return nullptr;
        // A comma inside parens makes it a tuple: (a, b) / (a,)
        if (peekToken.type == TokenType::COMMA) {
            auto tup = std::make_unique<TupleLiteral>();
            tup->token = lparen;
            tup->elements.push_back(std::move(expr));
            while (peekToken.type == TokenType::COMMA) {
                nextParserToken();                    // onto the comma
                if (peekToken.type == TokenType::RPAREN) break;  // trailing: (a,)
                nextParserToken();                    // onto the next element
                auto elem = parseExpression(Precedence::LOWEST);
                if (elem == nullptr) return nullptr;
                tup->elements.push_back(std::move(elem));
            }
            if (!expectPeek(TokenType::RPAREN)) return nullptr;
            return tup;
        }
        if (!expectPeek(TokenType::RPAREN)) return nullptr;
        return expr;
    }

    std::unique_ptr<Expression> parseIntegerLiteral() {
        auto lit = std::make_unique<IntegerLiteral>();
        lit->token = curToken;
        const std::string& raw = curToken.literal;
        try {
            if (raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) {
                lit->value = std::stoll(raw.substr(2), nullptr, 16);
            } else if (raw.size() > 2 && raw[0] == '0' && (raw[1] == 'b' || raw[1] == 'B')) {
                lit->value = std::stoll(raw.substr(2), nullptr, 2);
            } else {
                lit->value = std::stoll(raw);
            }
        } catch (...) {
            addError("number too large: " + raw +
                     " (64-bit integer limit exceeded)", curToken);
            return nullptr;
        }
        return lit;
    }

    std::unique_ptr<Expression> parseFloatLiteral() {
        auto lit = std::make_unique<FloatLiteral>();
        lit->token = curToken;
        try {
            lit->value = std::stod(curToken.literal);
        } catch (...) {
            addError("could not parse float: " + curToken.literal, curToken);
            return nullptr;
        }
        return lit;
    }

    std::unique_ptr<Expression> parseBooleanLiteral() {
        auto lit = std::make_unique<BooleanLiteral>();
        lit->token = curToken;
        lit->value = (curToken.type == TokenType::TRUE);
        return lit;
    }

    // Processes the raw string content in one pass: escape sequences
    // and interpolation ("hp: {hp}") — RFC-002.
    std::unique_ptr<Expression> parseStringLiteral() {
        const std::string& raw = curToken.literal;
        Token strToken = curToken;

        std::vector<std::unique_ptr<Expression>> parts;
        std::string current; // plain-text chunk being accumulated

        auto flushLiteral = [&]() {
            if (!current.empty()) {
                auto lit = std::make_unique<StringLiteral>();
                lit->token = strToken;
                lit->value = current;
                parts.push_back(std::move(lit));
                current.clear();
            }
        };

        size_t i = 0;
        while (i < raw.size()) {
            char c = raw[i];

            if (c == '\\' && i + 1 < raw.size()) {
                char next = raw[i + 1];
                switch (next) {
                    case 'n':  current += '\n'; break;
                    case 't':  current += '\t'; break;
                    case 'r':  current += '\r'; break;
                    case '\\': current += '\\'; break;
                    case '"':  current += '"';  break;
                    case '{':  current += '{';  break;
                    case '}':  current += '}';  break;
                    default:
                        addError(std::string("unknown escape sequence: \\") + next, strToken);
                        return nullptr;
                }
                i += 2;
                continue;
            }

            if (c == '{') {
                // Embedded expression: find the matching '}' (ignore braces inside quotes)
                size_t start = i + 1;
                int braceDepth = 1;
                bool inQuote = false;
                size_t j = start;
                while (j < raw.size()) {
                    char cj = raw[j];
                    if (inQuote) {
                        if (cj == '\\' && j + 1 < raw.size()) { j += 2; continue; }
                        if (cj == '"') inQuote = false;
                    } else {
                        if (cj == '"') inQuote = true;
                        else if (cj == '{') braceDepth++;
                        else if (cj == '}') {
                            braceDepth--;
                            if (braceDepth == 0) break;
                        }
                    }
                    j++;
                }
                if (j >= raw.size()) {
                    addError("unclosed '{' inside string (interpolation)", strToken);
                    return nullptr;
                }

                std::string inner = raw.substr(start, j - start);
                if (inner.empty()) {
                    addError("empty interpolation inside string: {}", strToken);
                    return nullptr;
                }

                // Parse the inner expression with its own mini lexer+parser
                // (the constructor already primes curToken and peekToken)
                Lexer subLexer(inner);
                Parser subParser(subLexer);
                auto innerExpr = subParser.parseSingleExpression();
                if (innerExpr == nullptr || !subParser.errorList.empty()) {
                    addError("could not parse string interpolation: {" + inner + "}", strToken);
                    return nullptr;
                }

                flushLiteral();
                parts.push_back(std::move(innerExpr));
                i = j + 1;
                continue;
            }

            if (c == '}') {
                addError("unmatched '}' inside string (use \\} for a literal brace)", strToken);
                return nullptr;
            }

            current += c;
            i++;
        }

        // No interpolation at all: return a plain StringLiteral (fast path)
        if (parts.empty()) {
            auto lit = std::make_unique<StringLiteral>();
            lit->token = strToken;
            lit->value = current;
            return lit;
        }

        flushLiteral();
        auto interp = std::make_unique<InterpolatedString>();
        interp->token = strToken;
        interp->parts = std::move(parts);
        return interp;
    }

    // Entry point for mini expressions inside interpolation
    std::unique_ptr<Expression> parseSingleExpression() {
        return parseExpression(Precedence::LOWEST);
    }

    std::unique_ptr<Expression> parseListLiteral() {
        auto lit = std::make_unique<ListLiteral>();
        lit->token = curToken;

        // Empty list: []
        if (peekToken.type == TokenType::RBRACKET) {
            nextParserToken();
            return lit;
        }

        do {
            nextParserToken(); // advance to the element
            auto elem = parseExpression(Precedence::LOWEST);
            if (elem == nullptr) return nullptr;
            lit->elements.push_back(std::move(elem));

            if (peekToken.type == TokenType::COMMA) {
                nextParserToken(); // advance past the comma
                // Trailing commas are supported: [1, 2,]
                if (peekToken.type == TokenType::RBRACKET) break;
            } else {
                break;
            }
        } while (true);

        if (!expectPeek(TokenType::RBRACKET)) return nullptr;
        return lit;
    }

    std::unique_ptr<Expression> parseMapLiteral() {
        auto lit = std::make_unique<MapLiteral>();
        lit->token = curToken;

        // Empty map: {}
        if (peekToken.type == TokenType::RBRACE) {
            nextParserToken();
            return lit;
        }

        do {
            nextParserToken(); // advance to the key
            auto key = parseExpression(Precedence::LOWEST);
            if (key == nullptr) return nullptr;

            if (!expectPeek(TokenType::COLON)) return nullptr;

            nextParserToken(); // advance to the value
            auto val = parseExpression(Precedence::LOWEST);
            if (val == nullptr) return nullptr;

            lit->pairs.push_back({std::move(key), std::move(val)});

            if (peekToken.type == TokenType::COMMA) {
                nextParserToken();
                if (peekToken.type == TokenType::RBRACE) break; // trailing comma
            } else {
                break;
            }
        } while (true);

        if (!expectPeek(TokenType::RBRACE)) return nullptr;
        return lit;
    }

    // Member access: object.name (RFC-006)
    std::unique_ptr<Expression> parseMemberExpression(std::unique_ptr<Expression> object, bool safe) {
        auto expr = std::make_unique<MemberExpression>();
        expr->token = curToken; // '.' or '?.' token
        expr->object = std::move(object);
        expr->safe = safe;

        if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
        expr->property = curToken.literal;
        return expr;
    }

    std::unique_ptr<Expression> parseIndexExpression(std::unique_ptr<Expression> object) {
        auto expr = std::make_unique<IndexExpression>();
        expr->token = curToken; // '[' token
        expr->object = std::move(object);

        // Slice starting at 0: a[:end]
        if (peekToken.type == TokenType::COLON) {
            expr->isSlice = true;
            nextParserToken(); // ':'
        } else {
            nextParserToken(); // advance to the index expression
            expr->index = parseExpression(Precedence::LOWEST);
            if (expr->index == nullptr) return nullptr;
            if (peekToken.type == TokenType::COLON) {
                expr->isSlice = true;
                nextParserToken(); // ':'
            }
        }

        if (expr->isSlice && peekToken.type != TokenType::RBRACKET) {
            nextParserToken();
            expr->indexEnd = parseExpression(Precedence::LOWEST);
            if (expr->indexEnd == nullptr) return nullptr;
        }

        if (!expectPeek(TokenType::RBRACKET)) return nullptr;
        return expr;
    }

    std::unique_ptr<Expression> parseInfixExpression(std::unique_ptr<Expression> left) {
        auto expression = std::make_unique<InfixExpression>();
        expression->token = curToken;
        expression->op = curToken.literal;
        expression->left = std::move(left);

        Precedence precedence = curPrecedence();
        // '**' is right-associative: 2 ** 3 ** 2 = 2 ** (3 ** 2)
        if (expression->op == "**") {
            precedence = (Precedence)(precedence - 1);
        }
        nextParserToken();
        expression->right = parseExpression(precedence);
        if (expression->right == nullptr) return nullptr;

        return expression;
    }

    // Conditional expression (ternary): <value> if <condition> else <value>
    std::unique_ptr<Expression> parseConditionalExpression(std::unique_ptr<Expression> thenExpr) {
        auto expr = std::make_unique<ConditionalExpression>();
        expr->token = curToken; // 'if' jetonu
        expr->thenExpr = std::move(thenExpr);

        nextParserToken();
        expr->condition = parseExpression(Precedence::LOWEST);
        if (expr->condition == nullptr) return nullptr;

        if (!expectPeek(TokenType::ELSE)) return nullptr;

        nextParserToken();
        expr->elseExpr = parseExpression(Precedence::LOWEST);
        if (expr->elseExpr == nullptr) return nullptr;

        return expr;
    }

    std::unique_ptr<Expression> parseCallExpression(std::unique_ptr<Expression> function) {
        auto exp = std::make_unique<CallExpression>();
        exp->token = curToken; // '(' jetonu
        exp->function = std::move(function);
        if (!parseCallArguments(exp->arguments)) return nullptr;
        return exp;
    }

    bool parseCallArguments(std::vector<std::unique_ptr<Expression>>& args) {
        // If the parentheses are empty, close immediately
        if (peekToken.type == TokenType::RPAREN) {
            nextParserToken();
            return true;
        }

        nextParserToken();
        auto first = parseExpression(Precedence::LOWEST);
        if (first == nullptr) return false;
        args.push_back(std::move(first));

        while (peekToken.type == TokenType::COMMA) {
            nextParserToken(); // advance past the comma
            nextParserToken(); // advance to the next argument
            auto arg = parseExpression(Precedence::LOWEST);
            if (arg == nullptr) return false;
            args.push_back(std::move(arg));
        }

        if (!expectPeek(TokenType::RPAREN)) return false;
        return true;
    }
};

} // namespace Lovax

#endif // PARSER_HPP
