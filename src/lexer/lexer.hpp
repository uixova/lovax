#ifndef LEXER_HPP
#define LEXER_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include "token.hpp"

namespace Lume {

class Lexer {
private:
    std::string input;
    size_t position = 0;
    size_t readPosition = 0;
    char ch = 0;

    // Position tracking (for error messages)
    int line = 1;
    int column = 0; // readChar increments each call; the first character lands on column 1

    // State for indentation tracking
    std::vector<int> indentStack = {0}; // We start at indentation level 0
    std::queue<Token> tokenQueue;       // Holds generated implicit INDENT/DEDENT tokens
    bool isAtLineStart = true;          // Are we at the start of a line?

    // Newlines are ignored inside parens/brackets/braces (multiline literals)
    int bracketDepth = 0;

    std::unordered_map<std::string, TokenType> keywords = {
        {"set", TokenType::SET},
        {"say", TokenType::SAY},
        {"if", TokenType::IF},
        {"else", TokenType::ELSE},
        {"match", TokenType::MATCH},
        {"use", TokenType::USE},
        {"as", TokenType::AS},
        {"try", TokenType::TRY},
        {"catch", TokenType::CATCH},
        {"throw", TokenType::THROW},
        {"const", TokenType::CONST},
        {"pass", TokenType::PASS},
        {"repeat", TokenType::REPEAT},
        {"until", TokenType::UNTIL},
        {"is", TokenType::IS},
        {"struct", TokenType::STRUCT},
        {"enum", TokenType::ENUM},
        {"this", TokenType::THIS},
        {"new", TokenType::NEW},
        {"finally", TokenType::FINALLY},
        {"while", TokenType::WHILE},
        {"for", TokenType::FOR},
        {"in", TokenType::IN},
        {"break", TokenType::BREAK},
        {"continue", TokenType::CONTINUE},
        {"fn", TokenType::FN},
        {"return", TokenType::RETURN},
        {"and", TokenType::AND},
        {"or", TokenType::OR},
        {"not", TokenType::NOT},
        {"true", TokenType::TRUE},
        {"false", TokenType::FALSE},
        {"null", TokenType::NULL_KW}
    };

public:
    Lexer(const std::string& src) : input(src) {
        readChar();
    }

    void readChar() {
        if (ch == '\n') {
            line++;
            column = 0;
        }
        if (readPosition >= input.length()) {
            ch = 0;
        } else {
            ch = input[readPosition];
        }
        position = readPosition;
        readPosition++;
        column++;
    }

    char peekChar() {
        if (readPosition >= input.length()) {
            return 0;
        }
        return input[readPosition];
    }

    char peekChar2() {
        if (readPosition + 1 >= input.length()) {
            return 0;
        }
        return input[readPosition + 1];
    }

    Token nextToken() {
        // 1. If INDENT/DEDENT tokens are queued, emit them first
        if (!tokenQueue.empty()) {
            Token tok = tokenQueue.front();
            tokenQueue.pop();
            return tok;
        }

        // 2. Indentation handling (disabled inside brackets)
        if (isAtLineStart && bracketDepth == 0) {
            isAtLineStart = false;
            int spaces = 0;

            // Count leading spaces or tabs
            while (ch == ' ' || ch == '\t') {
                if (ch == ' ') spaces++;
                else spaces += 4; // Count 1 tab as 4 spaces for stability
                readChar();
            }

            // Skip blank lines, lone newlines, and comment lines
            // (so indentation is preserved; lines starting with '#' are ignored entirely)
            if (ch == '\n' || ch == '\r' || ch == '#') {
                if (ch == '#' && peekChar() == '#' && peekChar2() == '#') {
                    skipBlockComment();
                    isAtLineStart = true;
                    return nextToken();
                }
                // If it is a comment line, advance to end of line
                while (ch != '\n' && ch != '\r' && ch != 0) {
                    readChar();
                }
                if (ch == '\n' || ch == '\r') {
                    isAtLineStart = true;
                    readChar();
                }
                return nextToken(); // Skip the blank/comment line and move on
            }

            // At end of input, emit DEDENTs for any blocks still open
            if (ch == 0) {
                while (indentStack.size() > 1) {
                    indentStack.pop_back();
                    tokenQueue.push(makeToken(TokenType::DEDENT, ""));
                }
                tokenQueue.push(makeToken(TokenType::END_OF_FILE, ""));
                return nextToken();
            }

            int currentIndent = indentStack.back();
            if (spaces > currentIndent) {
                // Indent detected! Record the new level and emit INDENT
                indentStack.push_back(spaces);
                tokenQueue.push(makeToken(TokenType::INDENT, ""));
            }
            else if (spaces < currentIndent) {
                // Dedent detected! Emit DEDENTs until we reach a previous level
                while (spaces < indentStack.back()) {
                    indentStack.pop_back();
                    tokenQueue.push(makeToken(TokenType::DEDENT, ""));
                }
                // If the spacing matches no level (alignment error)
                if (spaces != indentStack.back()) {
                    return makeToken(TokenType::ILLEGAL,
                        "invalid indentation level: matches no enclosing block");
                }
            }

            // If anything was queued, return it immediately
            if (!tokenQueue.empty()) {
                Token tok = tokenQueue.front();
                tokenQueue.pop();
                return tok;
            }
        }
        if (isAtLineStart) isAtLineStart = false; // clear the flag while bracketDepth > 0

        // Skip ordinary whitespace
        skipWhitespace();

        // Inline comment: on '#', skip to end of line without disturbing NEWLINE emission.
        // '###' opens a block comment that runs until the next '###'.
        if (ch == '#') {
            if (peekChar() == '#' && peekChar2() == '#') {
                skipBlockComment();
                return nextToken();
            }
            while (ch != '\n' && ch != 0) {
                readChar();
            }
            // ch is now '\n' or 0; the switch below handles it as NEWLINE/EOF
        }

        Token tok;
        switch (ch) {
            case '=':
                if (peekChar() == '=') { tok = twoCharToken(TokenType::EQUAL); }
                else                   { tok = makeToken(TokenType::ASSIGN, "="); }
                break;
            case '!':
                if (peekChar() == '=') { tok = twoCharToken(TokenType::NOT_EQUAL); }
                else                   { tok = makeToken(TokenType::ILLEGAL, "unexpected character '!' (use 'not' for negation)"); }
                break;
            case '+':
                if (peekChar() == '=') { tok = twoCharToken(TokenType::PLUS_ASSIGN); }
                else                   { tok = makeToken(TokenType::PLUS, "+"); }
                break;
            case '-':
                if (peekChar() == '>')      { tok = twoCharToken(TokenType::ARROW); }
                else if (peekChar() == '=') { tok = twoCharToken(TokenType::MINUS_ASSIGN); }
                else                        { tok = makeToken(TokenType::MINUS, "-"); }
                break;
            case '*':
                if (peekChar() == '*')      { tok = twoCharToken(TokenType::POWER); }
                else if (peekChar() == '=') { tok = twoCharToken(TokenType::ASTERISK_ASSIGN); }
                else                        { tok = makeToken(TokenType::ASTERISK, "*"); }
                break;
            case '/':
                if (peekChar() == '=') { tok = twoCharToken(TokenType::SLASH_ASSIGN); }
                else                   { tok = makeToken(TokenType::SLASH, "/"); }
                break;
            case '%':
                if (peekChar() == '=') { tok = twoCharToken(TokenType::PERCENT_ASSIGN); }
                else                   { tok = makeToken(TokenType::PERCENT, "%"); }
                break;
            case '&':
                if (peekChar() == '=')      { tok = twoCharToken(TokenType::AMP_ASSIGN); }
                else if (peekChar() == '&') { tok = makeToken(TokenType::ILLEGAL, "there is no '&&'; use 'and' for logical and"); readChar(); }
                else                        { tok = makeToken(TokenType::AMPERSAND, "&"); }
                break;
            case '|':
                if (peekChar() == '=')      { tok = twoCharToken(TokenType::PIPE_ASSIGN); }
                else if (peekChar() == '|') { tok = makeToken(TokenType::ILLEGAL, "there is no '||'; use 'or' for logical or"); readChar(); }
                else                        { tok = makeToken(TokenType::PIPE, "|"); }
                break;
            case '^':
                if (peekChar() == '=') { tok = twoCharToken(TokenType::CARET_ASSIGN); }
                else                   { tok = makeToken(TokenType::CARET, "^"); }
                break;
            case '~': tok = makeToken(TokenType::TILDE, "~"); break;
            case '<':
                if (peekChar() == '<' && peekChar2() == '=') { tok = threeCharToken(TokenType::SHL_ASSIGN); }
                else if (peekChar() == '<')                  { tok = twoCharToken(TokenType::SHIFT_LEFT); }
                else if (peekChar() == '=')                  { tok = twoCharToken(TokenType::LESS_EQUAL); }
                else                                         { tok = makeToken(TokenType::LESS_THAN, "<"); }
                break;
            case '>':
                if (peekChar() == '>' && peekChar2() == '=') { tok = threeCharToken(TokenType::SHR_ASSIGN); }
                else if (peekChar() == '>')                  { tok = twoCharToken(TokenType::SHIFT_RIGHT); }
                else if (peekChar() == '=')                  { tok = twoCharToken(TokenType::GREATER_EQUAL); }
                else                                         { tok = makeToken(TokenType::GREATER_THAN, ">"); }
                break;
            case ':': tok = makeToken(TokenType::COLON, ":"); break;
            case '.':
                if (peekChar() == '.' && peekChar2() == '.') {
                    tok = makeToken(TokenType::ELLIPSIS, "...");
                    readChar(); readChar();
                } else if (peekChar() == '.') {
                    tok = twoCharToken(TokenType::DOTDOT);
                } else {
                    tok = makeToken(TokenType::DOT, ".");
                }
                break;
            case '?':
                if (peekChar() == '?' && peekChar2() == '=') { tok = threeCharToken(TokenType::QQ_ASSIGN); }
                else if (peekChar() == '?')                  { tok = twoCharToken(TokenType::QQ); }
                else if (peekChar() == '.')                  { tok = twoCharToken(TokenType::QDOT); }
                else { tok = makeToken(TokenType::ILLEGAL, "unexpected character '?' (did you mean '?\?' or '?.')"); }
                break;
            case ',': tok = makeToken(TokenType::COMMA, ","); break;
            case '(': bracketDepth++; tok = makeToken(TokenType::LPAREN, "("); break;
            case ')': if (bracketDepth > 0) bracketDepth--; tok = makeToken(TokenType::RPAREN, ")"); break;
            case '[': bracketDepth++; tok = makeToken(TokenType::LBRACKET, "["); break;
            case ']': if (bracketDepth > 0) bracketDepth--; tok = makeToken(TokenType::RBRACKET, "]"); break;
            case '{': bracketDepth++; tok = makeToken(TokenType::LBRACE, "{"); break;
            case '}': if (bracketDepth > 0) bracketDepth--; tok = makeToken(TokenType::RBRACE, "}"); break;
            case '"':
                if (peekChar() == '"' && peekChar2() == '"') {
                    return readMultilineString();
                }
                return readString(); // manages its own readChar calls
            case '\n':
                if (bracketDepth > 0) {
                    // Inside parens/brackets/braces: newlines become invisible
                    readChar();
                    return nextToken();
                }
                tok = makeToken(TokenType::NEWLINE, "\\n");
                isAtLineStart = true; // A newline was consumed, so the next round starts a fresh line
                break;
            case 0:
                // If the file ends abruptly, safely close any open indentation levels
                while (indentStack.size() > 1) {
                    indentStack.pop_back();
                    tokenQueue.push(makeToken(TokenType::DEDENT, ""));
                }
                tokenQueue.push(makeToken(TokenType::END_OF_FILE, ""));
                return nextToken();
            default:
                if (isLetter(ch)) {
                    int startLine = line, startCol = column;
                    std::string ident = readIdentifier();
                    Token t;
                    t.type = lookupIdent(ident);
                    t.literal = ident;
                    t.line = startLine;
                    t.column = startCol;
                    return t;
                } else if (isDigit(ch)) {
                    return readNumber();
                } else {
                    tok = makeToken(TokenType::ILLEGAL,
                        std::string("unexpected character '") + ch + "'");
                }
                break;
        }

        readChar();
        return tok;
    }

private:
    Token makeToken(TokenType type, const std::string& lit) {
        Token t;
        t.type = type;
        t.literal = lit;
        t.line = line;
        t.column = column;
        return t;
    }

    // For three-character operators (<<=, >>=, ??=)
    Token threeCharToken(TokenType type) {
        char c1 = ch; int startCol = column;
        readChar(); char c2 = ch;
        readChar();
        Token t;
        t.type = type;
        t.literal = std::string(1, c1) + c2 + ch;
        t.line = line; t.column = startCol;
        return t;
    }

    // For two-character operators: joins the current and next characters
    Token twoCharToken(TokenType type) {
        char first = ch;
        int startCol = column;
        readChar();
        Token t;
        t.type = type;
        t.literal = std::string(1, first) + ch;
        t.line = line;
        t.column = startCol;
        return t;
    }

    // Block comment: ### ... ### (may span lines; unterminated simply runs to EOF)
    void skipBlockComment() {
        readChar(); readChar(); readChar(); // consume opening ###
        while (ch != 0) {
            if (ch == '#' && peekChar() == '#' && peekChar2() == '#') {
                readChar(); readChar(); readChar(); // consume closing ###
                return;
            }
            readChar();
        }
    }

    void skipWhitespace() {
        // Skips inline whitespace but leaves \n alone
        while (ch == ' ' || ch == '\t' || ch == '\r') {
            readChar();
        }
    }

    std::string readIdentifier() {
        size_t startPos = position;
        while (isLetter(ch) || isDigit(ch) || ch == '_') {
            readChar();
        }
        return input.substr(startPos, position - startPos);
    }

    // Number literals: 42, 3.14, 1_000_000, 0xFF, 0b1010, 1e6, 2.5e-3
    Token readNumber() {
        int startLine = line, startCol = column;
        Token t;
        t.line = startLine;
        t.column = startCol;

        auto isHexDigit = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };

        // Hexadecimal (0x) and binary (0b) literals
        if (ch == '0' && (peekChar() == 'x' || peekChar() == 'X' ||
                          peekChar() == 'b' || peekChar() == 'B')) {
            bool isHex = (peekChar() == 'x' || peekChar() == 'X');
            std::string lit = "0";
            lit += peekChar();
            readChar(); // skip '0'
            readChar(); // skip 'x'/'b'
            int digits = 0;
            while ((isHex ? isHexDigit(ch) : (ch == '0' || ch == '1')) || ch == '_') {
                if (ch != '_') { lit += ch; digits++; }
                readChar();
            }
            if (digits == 0) {
                t.type = TokenType::ILLEGAL;
                t.literal = "invalid number format: " + lit + " (digits missing)";
                return t;
            }
            // Adjacent invalid character like '0b12' or '0xFG': error instead of a silently wrong value
            if (isDigit(ch) || isLetter(ch)) {
                t.type = TokenType::ILLEGAL;
                t.literal = std::string("invalid number format: ") + lit + ch;
                return t;
            }
            t.type = TokenType::INT;
            t.literal = lit; // with '_' separators stripped
            return t;
        }

        std::string lit;
        bool isFloat = false;
        bool badFormat = false;

        while (isDigit(ch) || ch == '.' || ch == '_') {
            if (ch == '_') { readChar(); continue; } // readability separator: 1_000_000
            if (ch == '.') {
                // A dot is part of the number only if a DIGIT follows;
                // otherwise it is a member-access token: 5.lerp -> INT(5) DOT lerp
                if (!isDigit(peekChar())) break;
                if (isFloat) badFormat = true; // '1.2.3'
                isFloat = true;
            }
            lit += ch;
            readChar();
        }

        // Scientific notation: 1e6, 2.5e-3 (digit, or sign+digit, must follow 'e')
        if ((ch == 'e' || ch == 'E') &&
            (isDigit(peekChar()) ||
             ((peekChar() == '+' || peekChar() == '-') && isDigit(peekChar2())))) {
            isFloat = true;
            lit += ch;
            readChar(); // skip 'e'
            if (ch == '+' || ch == '-') { lit += ch; readChar(); }
            while (isDigit(ch) || ch == '_') {
                if (ch != '_') lit += ch;
                readChar();
            }
        }

        if (badFormat) {
            t.type = TokenType::ILLEGAL;
            t.literal = "invalid number format: " + lit;
            return t;
        }

        t.type = isFloat ? TokenType::FLOAT : TokenType::INT;
        t.literal = lit;
        return t;
    }

    bool isLetter(char c) {
        // ASCII letters, underscore, and UTF-8 multi-byte characters (Turkish: ş, ç, ğ, ı, ö, ü, İ ...)
        // char may be signed; cast to unsigned and treat high-bit bytes (>=0x80) as letters
        unsigned char uc = static_cast<unsigned char>(c);
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || uc >= 0x80;
    }

    bool isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    TokenType lookupIdent(const std::string& ident) {
        auto it = keywords.find(ident);
        if (it != keywords.end()) {
            return it->second;
        }
        return TokenType::IDENTIFIER;
    }

    // Reads the string RAW (escapes are processed in one pass by the parser —
    // so escapes never clash with '{expression}' interpolation).
    // Inner quotes inside a '{...}' interpolation region ("{m["a"]}") do NOT terminate the string.
    Token readString() {
        int startLine = line, startCol = column;
        size_t startPos = position + 1;
        int braceDepth = 0;   // interpolation depth
        bool innerQuote = false; // inner string inside interpolation

        while (true) {
            readChar();
            if (ch == '\\') {
                // Skip the escaped character (so it is not mistaken for the closing quote: "a\"b")
                readChar();
                if (ch == 0) break;
                continue;
            }
            if (ch == 0 || ch == '\n') break;

            if (innerQuote) {
                if (ch == '"') innerQuote = false;
                continue;
            }
            if (ch == '{') { braceDepth++; continue; }
            if (ch == '}') { if (braceDepth > 0) braceDepth--; continue; }
            if (ch == '"') {
                if (braceDepth > 0) { innerQuote = true; continue; }
                break; // real closing quote
            }
        }

        Token t;
        t.line = startLine;
        t.column = startCol;

        if (ch != '"') {
            // Hit end of line or end of file: unterminated string
            t.type = TokenType::ILLEGAL;
            t.literal = "unterminated string (closing '\"' missing)";
            // if we sit on '\n', keep the line-start logic intact
            if (ch == '\n') isAtLineStart = true;
            if (ch != 0) readChar();
            return t;
        }

        t.type = TokenType::STRING;
        t.literal = input.substr(startPos, position - startPos);
        readChar(); // skip the closing quote
        return t;
    }

    // Multiline string: """ ... """ — for game dialogue.
    // Content is captured raw; escapes and {interpolation} are processed by the parser like a normal string.
    Token readMultilineString() {
        int startLine = line, startCol = column;
        readChar(); // 2nd quote
        readChar(); // 3rd quote
        readChar(); // first content character
        size_t startPos = position;

        int braceDepth = 0;
        bool innerQuote = false;

        while (true) {
            if (ch == 0) {
                Token t;
                t.type = TokenType::ILLEGAL;
                t.literal = "unterminated multiline string (closing \"\"\" missing)";
                t.line = startLine;
                t.column = startCol;
                return t;
            }
            if (ch == '\\') {
                readChar();
                if (ch != 0) readChar();
                continue;
            }
            if (innerQuote) {
                if (ch == '"') innerQuote = false;
                readChar();
                continue;
            }
            if (ch == '{') { braceDepth++; readChar(); continue; }
            if (ch == '}') { if (braceDepth > 0) braceDepth--; readChar(); continue; }
            if (ch == '"') {
                if (braceDepth > 0) { innerQuote = true; readChar(); continue; }
                if (peekChar() == '"' && peekChar2() == '"') break; // real closing delimiter
            }
            readChar();
        }

        Token t;
        t.type = TokenType::STRING;
        t.literal = input.substr(startPos, position - startPos);
        t.line = startLine;
        t.column = startCol;
        readChar(); // 1st closing quote
        readChar(); // 2.
        readChar(); // 3.
        return t;
    }
};

} // namespace Lume

#endif // LEXER_HPP
