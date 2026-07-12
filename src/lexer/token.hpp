#ifndef TOKEN_HPP
#define TOKEN_HPP

#include <string>

namespace Lume {

enum class TokenType {
    ILLEGAL,
    END_OF_FILE,

    // Identifiers and numbers
    IDENTIFIER,
    INT,
    FLOAT,
    STRING,

    // Operators
    ASSIGN,          // =
    PLUS,            // +
    MINUS,           // -
    ASTERISK,        // *
    POWER,           // **
    SLASH,           // /
    PERCENT,         // %
    AMPERSAND,       // &  (bitwise and)
    PIPE,            // |  (bitwise or)
    CARET,           // ^  (bitwise xor)
    TILDE,           // ~  (bitwise not)
    SHIFT_LEFT,      // <<
    SHIFT_RIGHT,     // >>

    // Compound assignment operators
    PLUS_ASSIGN,     // +=
    MINUS_ASSIGN,    // -=
    ASTERISK_ASSIGN, // *=
    SLASH_ASSIGN,    // /=
    PERCENT_ASSIGN,  // %=
    AMP_ASSIGN,      // &=
    PIPE_ASSIGN,     // |=
    CARET_ASSIGN,    // ^=
    SHL_ASSIGN,      // <<=
    SHR_ASSIGN,      // >>=
    QQ_ASSIGN,       // ??=

    // Comparison operators
    EQUAL,           // ==
    NOT_EQUAL,       // !=
    LESS_THAN,       // <
    GREATER_THAN,    // >
    LESS_EQUAL,      // <=
    GREATER_EQUAL,   // >=

    // Delimiters
    COLON,           // :
    COMMA,           // ,
    LPAREN,          // (
    RPAREN,          // )
    LBRACKET,        // [
    RBRACKET,        // ]
    LBRACE,          // {
    RBRACE,          // }
    DOT,             // .  (member access: math.lerp, player.hp)
    DOTDOT,          // .. (range literal: 0..10, end-exclusive)
    ELLIPSIS,        // ... (variadic parameter: fn f(a, rest...))
    QQ,              // ?? (null-coalescing: a ?? b)
    QDOT,            // ?. (null-safe member access)
    ARROW,           // -> (single-expression lambda body)
    NEWLINE,         // \n
    INDENT,
    DEDENT,

    // Keywords
    SET,
    SAY,
    IF,
    ELSE,
    MATCH,
    USE,    // module import: use math / use "file.lm" (RFC-006)
    AS,     // alias: use math as m
    TRY,     // error handling: try / catch / throw (RFC-008)
    CATCH,
    THROW,
    CONST,   // immutable binding
    PASS,    // empty-block no-op
    REPEAT,  // repeat N: (loop N times)
    UNTIL,   // until cond: (loop while NOT cond)
    IS,      // type test: x is "int"
    STRUCT,  // struct Player: field definitions + methods (RFC-003)
    ENUM,    // enum State: named integer constants
    THIS,    // the current struct instance inside a method
    NEW,     // reserved (struct instantiation is Name(...) — keeps the surface clean)
    FINALLY, // try/catch/finally
    WHILE,
    FOR,
    IN,
    BREAK,
    CONTINUE,
    FN,
    RETURN,
    TRUE,
    FALSE,
    NULL_KW,

    AND,
    OR,
    NOT
};

struct Token {
    TokenType type = TokenType::ILLEGAL;
    std::string literal;
    int line = 0;    // 1-based line number
    int column = 0;  // 1-based column number (byte-based)
};

// Prints a human-readable token name instead of a number in error messages
inline std::string tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::ILLEGAL:         return "ILLEGAL";
        case TokenType::END_OF_FILE:     return "end of file";
        case TokenType::IDENTIFIER:      return "identifier";
        case TokenType::INT:             return "integer";
        case TokenType::FLOAT:           return "float";
        case TokenType::STRING:          return "string";
        case TokenType::ASSIGN:          return "'='";
        case TokenType::PLUS:            return "'+'";
        case TokenType::MINUS:           return "'-'";
        case TokenType::ASTERISK:        return "'*'";
        case TokenType::POWER:           return "'**'";
        case TokenType::SLASH:           return "'/'";
        case TokenType::PERCENT:         return "'%'";
        case TokenType::AMPERSAND:       return "'&'";
        case TokenType::PIPE:            return "'|'";
        case TokenType::CARET:           return "'^'";
        case TokenType::TILDE:           return "'~'";
        case TokenType::SHIFT_LEFT:      return "'<<'";
        case TokenType::SHIFT_RIGHT:     return "'>>'";
        case TokenType::PLUS_ASSIGN:     return "'+='";
        case TokenType::MINUS_ASSIGN:    return "'-='";
        case TokenType::ASTERISK_ASSIGN: return "'*='";
        case TokenType::SLASH_ASSIGN:    return "'/='";
        case TokenType::PERCENT_ASSIGN:  return "'%='";
        case TokenType::AMP_ASSIGN:      return "'&='";
        case TokenType::PIPE_ASSIGN:     return "'|='";
        case TokenType::CARET_ASSIGN:    return "'^='";
        case TokenType::SHL_ASSIGN:      return "'<<='";
        case TokenType::SHR_ASSIGN:      return "'>>='";
        case TokenType::QQ_ASSIGN:       return "'?\?='";
        case TokenType::EQUAL:           return "'=='";
        case TokenType::NOT_EQUAL:       return "'!='";
        case TokenType::LESS_THAN:       return "'<'";
        case TokenType::GREATER_THAN:    return "'>'";
        case TokenType::LESS_EQUAL:      return "'<='";
        case TokenType::GREATER_EQUAL:   return "'>='";
        case TokenType::COLON:           return "':'";
        case TokenType::COMMA:           return "','";
        case TokenType::LPAREN:          return "'('";
        case TokenType::RPAREN:          return "')'";
        case TokenType::LBRACKET:        return "'['";
        case TokenType::RBRACKET:        return "']'";
        case TokenType::LBRACE:          return "'{'";
        case TokenType::RBRACE:          return "'}'";
        case TokenType::DOT:             return "'.'";
        case TokenType::DOTDOT:          return "'..'";
        case TokenType::ELLIPSIS:        return "'...'";
        case TokenType::QQ:              return "'?\?'";
        case TokenType::QDOT:            return "'?.'";
        case TokenType::ARROW:           return "'->'";
        case TokenType::NEWLINE:         return "newline";
        case TokenType::INDENT:          return "indent (INDENT)";
        case TokenType::DEDENT:          return "dedent (DEDENT)";
        case TokenType::SET:             return "'set'";
        case TokenType::SAY:             return "'say'";
        case TokenType::IF:              return "'if'";
        case TokenType::ELSE:            return "'else'";
        case TokenType::MATCH:           return "'match'";
        case TokenType::USE:             return "'use'";
        case TokenType::AS:              return "'as'";
        case TokenType::TRY:             return "'try'";
        case TokenType::CATCH:           return "'catch'";
        case TokenType::THROW:           return "'throw'";
        case TokenType::CONST:           return "'const'";
        case TokenType::PASS:            return "'pass'";
        case TokenType::REPEAT:          return "'repeat'";
        case TokenType::UNTIL:           return "'until'";
        case TokenType::IS:              return "'is'";
        case TokenType::STRUCT:          return "'struct'";
        case TokenType::ENUM:            return "'enum'";
        case TokenType::THIS:            return "'this'";
        case TokenType::NEW:             return "'new'";
        case TokenType::FINALLY:         return "'finally'";
        case TokenType::WHILE:           return "'while'";
        case TokenType::FOR:             return "'for'";
        case TokenType::IN:              return "'in'";
        case TokenType::BREAK:           return "'break'";
        case TokenType::CONTINUE:        return "'continue'";
        case TokenType::FN:              return "'fn'";
        case TokenType::RETURN:          return "'return'";
        case TokenType::TRUE:            return "'true'";
        case TokenType::FALSE:           return "'false'";
        case TokenType::NULL_KW:         return "'null'";
        case TokenType::AND:             return "'and'";
        case TokenType::OR:              return "'or'";
        case TokenType::NOT:             return "'not'";
    }
    return "unknown";
}

} // namespace Lume

#endif // TOKEN_HPP
