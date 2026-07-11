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
        case TokenType::ILLEGAL:         return "GECERSIZ";
        case TokenType::END_OF_FILE:     return "dosya sonu";
        case TokenType::IDENTIFIER:      return "isim (identifier)";
        case TokenType::INT:             return "tam sayı";
        case TokenType::FLOAT:           return "ondalık sayı";
        case TokenType::STRING:          return "metin (string)";
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
        case TokenType::NEWLINE:         return "satır sonu";
        case TokenType::INDENT:          return "girinti (INDENT)";
        case TokenType::DEDENT:          return "girinti sonu (DEDENT)";
        case TokenType::SET:             return "'set'";
        case TokenType::SAY:             return "'say'";
        case TokenType::IF:              return "'if'";
        case TokenType::ELSE:            return "'else'";
        case TokenType::MATCH:           return "'match'";
        case TokenType::USE:             return "'use'";
        case TokenType::AS:              return "'as'";
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
    return "bilinmeyen";
}

} // namespace Lume

#endif // TOKEN_HPP
