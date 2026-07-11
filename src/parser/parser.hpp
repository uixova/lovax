#ifndef PARSER_HPP
#define PARSER_HPP

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include "../lexer/lexer.hpp"
#include "../ast/ast.hpp"

namespace Lume {

enum Precedence {
    LOWEST = 1,
    TERNARY,       // a if condition else b
    P_OR,          // or (lowest-precedence logical operator)
    P_AND,         // and
    EQUALS,        // == and !=
    LESSGREATER,   // < > <= >= in
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
        return "[Sözdizimi Hatası] satır " + std::to_string(line) +
               ", sütun " + std::to_string(column) + ": " + message;
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
        {TokenType::DOT, Precedence::INDEX}
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
                addError("çok fazla hata, ayrıştırma durduruldu", curToken);
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
            addError("beklenen " + tokenTypeName(t) + " fakat " +
                     tokenTypeName(peekToken.type) + " geldi", peekToken);
        }
        return false;
    }

    // ===== Statement parsing =====

    std::unique_ptr<Statement> parseStatement() {
        switch (curToken.type) {
            case TokenType::SET:      return parseSetStatement();
            case TokenType::SAY:      return parseSayStatement();
            case TokenType::IF:       return parseIfStatement();
            case TokenType::MATCH:    return parseMatchStatement();
            case TokenType::WHILE:    return parseWhileStatement();
            case TokenType::FOR:      return parseForStatement();
            case TokenType::BREAK:    return parseBreakStatement();
            case TokenType::CONTINUE: return parseContinueStatement();
            case TokenType::RETURN:   return parseReturnStatement();
            case TokenType::USE:      return parseUseStatement();
            case TokenType::ILLEGAL:
                addError(curToken.literal, curToken);
                return nullptr;
            case TokenType::ELSE:
                addError("'else' yalnızca bir 'if' bloğundan sonra gelebilir", curToken);
                return nullptr;
            case TokenType::INDENT:
                addError("beklenmeyen girinti: bu satır bir bloğa ait değil", curToken);
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
            pt == TokenType::SLASH_ASSIGN || pt == TokenType::PERCENT_ASSIGN) {

            if (expr->nodeType() != NodeType::IDENTIFIER &&
                expr->nodeType() != NodeType::INDEX_EXPRESSION &&
                expr->nodeType() != NodeType::MEMBER_EXPRESSION) {
                addError("atama hedefi bir değişken, indeks (liste[i]) veya üye (nesne.alan) olmalı", peekToken);
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

    std::unique_ptr<SetStatement> parseSetStatement() {
        auto stmt = std::make_unique<SetStatement>();
        stmt->token = curToken;

        if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;

        auto ident = std::make_unique<Identifier>();
        ident->token = curToken;
        ident->value = curToken.literal;
        stmt->name = std::move(ident);

        if (!expectPeek(TokenType::ASSIGN)) return nullptr;

        nextParserToken();
        stmt->value = parseExpression(Precedence::LOWEST);
        if (stmt->value == nullptr) return nullptr;

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
        }

        return stmt;
    }

    // use math / use math as m / use math: lerp, clamp / use "file.lm" [as x] [: names]
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
            addError("use bir modül ismi veya \"dosya/yolu.lm\" ister", peekToken);
            return nullptr;
        }

        if (peekToken.type == TokenType::AS) {
            nextParserToken(); // 'as'
            if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
            stmt->alias = curToken.literal;
        }

        if (peekToken.type == TokenType::COLON) {
            if (!stmt->alias.empty()) {
                addError("use ifadesinde 'as' ile ': isimler' birlikte kullanılamaz", peekToken);
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
                addError("use ... : sonrasında en az bir isim gerekli", peekToken);
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
            addError("match en az bir desen dalı ister", stmt->token);
            return nullptr;
        }

        return stmt;
    }

    std::unique_ptr<WhileStatement> parseWhileStatement() {
        auto stmt = std::make_unique<WhileStatement>();
        stmt->token = curToken;

        nextParserToken(); // skip the 'while' token
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
                        addError("parametre ismi tekrarlı: '" + curToken.literal + "'", curToken);
                        return nullptr;
                    }
                }

                auto param = std::make_unique<Identifier>();
                param->token = curToken;
                param->value = curToken.literal;
                lit->parameters.push_back(std::move(param));

                if (peekToken.type == TokenType::ASSIGN) {
                    nextParserToken(); // '='
                    nextParserToken(); // advance to the default expression
                    auto defExpr = parseExpression(Precedence::LOWEST);
                    if (defExpr == nullptr) return nullptr;
                    lit->defaults.push_back(std::move(defExpr));
                    seenDefault = true;
                } else {
                    if (seenDefault) {
                        addError("varsayılanlı parametreden sonra varsayılansız parametre gelemez: '" +
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

        lit->body = parseColonBlock();
        if (lit->body == nullptr) return nullptr;

        return lit;
    }

    // ===== Expression parsing (Pratt) =====

    std::unique_ptr<Expression> parseExpression(Precedence precedence) {
        DepthGuard guard(exprDepth);
        if (exprDepth > MAX_EXPR_DEPTH) {
            addError("ifade çok derin: iç içe geçme limiti (" +
                     std::to_string(MAX_EXPR_DEPTH) + ") aşıldı", curToken);
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
                    leftExp = parseMemberExpression(std::move(leftExp));
                    break;
                case TokenType::PLUS: case TokenType::MINUS:
                case TokenType::ASTERISK: case TokenType::SLASH: case TokenType::PERCENT:
                case TokenType::POWER:
                case TokenType::EQUAL: case TokenType::NOT_EQUAL:
                case TokenType::LESS_THAN: case TokenType::GREATER_THAN:
                case TokenType::LESS_EQUAL: case TokenType::GREATER_EQUAL:
                case TokenType::AND: case TokenType::OR: case TokenType::IN:
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
            case TokenType::FN:         return parseFunctionLiteral();
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
                addError("beklenmeyen jeton: " + tokenTypeName(curToken.type) +
                         " (burada bir değer veya ifade olmalı)", curToken);
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
        nextParserToken(); // skip the '(' token
        auto expr = parseExpression(Precedence::LOWEST);
        if (expr == nullptr) return nullptr;
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
            addError("sayı çok büyük: " + raw +
                     " (64-bit tam sayı sınırı aşıldı)", curToken);
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
            addError("ondalık sayı çevrilemedi: " + curToken.literal, curToken);
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
                        addError(std::string("bilinmeyen kaçış dizisi: \\") + next, strToken);
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
                    addError("string içinde kapatılmamış '{' (interpolasyon)", strToken);
                    return nullptr;
                }

                std::string inner = raw.substr(start, j - start);
                if (inner.empty()) {
                    addError("string içinde boş interpolasyon: {}", strToken);
                    return nullptr;
                }

                // Parse the inner expression with its own mini lexer+parser
                // (the constructor already primes curToken and peekToken)
                Lexer subLexer(inner);
                Parser subParser(subLexer);
                auto innerExpr = subParser.parseSingleExpression();
                if (innerExpr == nullptr || !subParser.errorList.empty()) {
                    addError("string interpolasyonu çözümlenemedi: {" + inner + "}", strToken);
                    return nullptr;
                }

                flushLiteral();
                parts.push_back(std::move(innerExpr));
                i = j + 1;
                continue;
            }

            if (c == '}') {
                addError("string içinde eşleşmeyen '}' (düz süslü için \\} kullan)", strToken);
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
    std::unique_ptr<Expression> parseMemberExpression(std::unique_ptr<Expression> object) {
        auto expr = std::make_unique<MemberExpression>();
        expr->token = curToken; // '.' token
        expr->object = std::move(object);

        if (!expectPeek(TokenType::IDENTIFIER)) return nullptr;
        expr->property = curToken.literal;
        return expr;
    }

    std::unique_ptr<Expression> parseIndexExpression(std::unique_ptr<Expression> object) {
        auto expr = std::make_unique<IndexExpression>();
        expr->token = curToken; // '[' token
        expr->object = std::move(object);

        nextParserToken(); // advance to the index expression
        expr->index = parseExpression(Precedence::LOWEST);
        if (expr->index == nullptr) return nullptr;

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

} // namespace Lume

#endif // PARSER_HPP
