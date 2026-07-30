/*
 * LR_JS - JavaScript Engine Lexer Implementation
 * Pure C, ES2022-compatible tokenizer.
 *
 * Tokenizes JavaScript source code into a stream of tokens.
 * Supports all JavaScript token types including template literals,
 * regular expressions, and all modern operators.
 */
#include "lr_lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ── Character Classification ─────────────────────────────────────────── */

static int is_js_ident_start(char c)
{
    unsigned char uc = (unsigned char)c;
    return isalpha(uc) || c == '_' || c == '$' ||
           (uc >= 0x80);  /* non-ASCII Unicode */
}

static int is_js_ident_continue(char c)
{
    unsigned char uc = (unsigned char)c;
    return isalnum(uc) || c == '_' || c == '$' ||
           (uc >= 0x80);
}

static int is_hex_char(char c)
{
    return isdigit((unsigned char)c) ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int is_octal_char(char c)
{
    return c >= '0' && c <= '7';
}

static int is_binary_char(char c)
{
    return c == '0' || c == '1';
}

static int is_line_terminator(char c)
{
    return c == '\n' || c == '\r';
}

/* ── Lexer Helpers ────────────────────────────────────────────────────── */

static char lexer_peek_char(Lexer *lex)
{
    if (lex->pos >= lex->src_len) return '\0';
    return lex->src[lex->pos];
}

static char lexer_peek_char_next(Lexer *lex)
{
    if (lex->pos + 1 >= lex->src_len) return '\0';
    return lex->src[lex->pos + 1];
}

static char lexer_advance(Lexer *lex)
{
    if (lex->pos >= lex->src_len) return '\0';
    char c = lex->src[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else if (c == '\r') {
        /* handle \r\n */
        lex->line++;
        lex->col = 1;
        if (lex->pos < lex->src_len && lex->src[lex->pos] == '\n') {
            lex->pos++;
        }
    } else {
        lex->col++;
    }
    return c;
}

static void lexer_skip_whitespace(Lexer *lex)
{
    char c;
    while ((c = lexer_peek_char(lex)) != '\0') {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            lexer_advance(lex);
        } else if (c == '/' && lexer_peek_char_next(lex) == '/') {
            /* Single-line comment */
            while (lex->pos < lex->src_len) {
                char nc = lexer_peek_char(lex);
                if (nc == '\n' || nc == '\r') break;
                lexer_advance(lex);
            }
        } else if (c == '/' && lexer_peek_char_next(lex) == '*') {
            /* Multi-line comment */
            lexer_advance(lex); /* skip / */
            lexer_advance(lex); /* skip * */
            while (lex->pos < lex->src_len) {
                if (lexer_peek_char(lex) == '*' &&
                    lexer_peek_char_next(lex) == '/') {
                    lexer_advance(lex); /* skip * */
                    lexer_advance(lex); /* skip / */
                    break;
                }
                lexer_advance(lex);
            }
        } else {
            break;
        }
    }
}

/* ── Token Construction ───────────────────────────────────────────────── */

static Token make_token_from(Lexer *lex, TokenType type, size_t start_pos)
{
    Token t;
    t.type = type;
    t.start = lex->src + start_pos;
    t.len = lex->pos - start_pos;
    t.num_val = 0.0;
    t.str_val = NULL;
    t.line = lex->line;
    t.col = lex->col;

    /* Centralised regexp-context rule: a '/' can start a regexp literal
     * whenever the previous token cannot end an expression. This runs
     * last, so it overrides any scattered allow_regexp assignments. */
    switch (type) {
    /* Tokens that can END an expression -> '/' is division */
    case TOK_NUMBER: case TOK_STRING: case TOK_IDENTIFIER: case TOK_REGEXP:
    case TOK_BOOL_LIT: case TOK_NULL_LIT: case TOK_UNDEFINED_LIT:
    case TOK_RPAREN: case TOK_RBRACKET: case TOK_RBRACE:
    case TOK_INC: case TOK_DEC:
    case TOK_TEMPLATE_END:
    case TOK_THIS: case TOK_SUPER:
    case TOK_PRIVATE_NAME:
    /* Contextual keywords frequently used as plain identifiers */
    case TOK_GET: case TOK_SET: case TOK_STATIC: case TOK_OF:
    case TOK_FROM: case TOK_AS: case TOK_ASYNC:
        lex->allow_regexp = 0;
        break;
    case TOK_EOF: case TOK_ERROR:
        break;
    /* Operators, punctuation and keywords -> expression follows */
    default:
        lex->allow_regexp = 1;
        break;
    }
    return t;
}

static Token make_error(Lexer *lex, const char *msg)
{
    Token t;
    t.type = TOK_ERROR;
    t.start = msg;
    t.len = strlen(msg);
    t.num_val = 0.0;
    t.str_val = NULL;
    t.line = lex->line;
    t.col = lex->col;
    return t;
}

/* ── Keyword Lookup ───────────────────────────────────────────────────── */

static TokenType lookup_keyword(const char *str, size_t len)
{
    if (len == 0) return TOK_IDENTIFIER;
    /* Use a simple switch on first char + length for speed */
    switch (str[0]) {
    case 'a':
        if (len == 2 && memcmp(str, "as", 2) == 0) return TOK_AS;
        if (len == 5 && memcmp(str, "async", 5) == 0) return TOK_ASYNC;
        if (len == 5 && memcmp(str, "await", 5) == 0) return TOK_AWAIT;
        break;
    case 'b':
        if (len == 5 && memcmp(str, "break", 5) == 0) return TOK_BREAK;
        break;
    case 'c':
        if (len == 4 && memcmp(str, "case", 4) == 0) return TOK_CASE;
        if (len == 5 && memcmp(str, "catch", 5) == 0) return TOK_CATCH;
        if (len == 5 && memcmp(str, "class", 5) == 0) return TOK_CLASS;
        if (len == 8 && memcmp(str, "continue", 8) == 0) return TOK_CONTINUE;
        if (len == 5 && memcmp(str, "const", 5) == 0) return TOK_CONST;
        break;
    case 'd':
        if (len == 2 && memcmp(str, "do", 2) == 0) return TOK_DO;
        if (len == 7 && memcmp(str, "default", 7) == 0) return TOK_DEFAULT;
        if (len == 6 && memcmp(str, "delete", 6) == 0) return TOK_DELETE;
        if (len == 8 && memcmp(str, "debugger", 8) == 0) return TOK_DEBUGGER;
        break;
    case 'e':
        if (len == 4 && memcmp(str, "else", 4) == 0) return TOK_ELSE;
        if (len == 7 && memcmp(str, "extends", 7) == 0) return TOK_EXTENDS;
        if (len == 6 && memcmp(str, "export", 6) == 0) return TOK_EXPORT;
        break;
    case 'f':
        if (len == 3 && memcmp(str, "for", 3) == 0) return TOK_FOR;
        if (len == 5 && memcmp(str, "false", 5) == 0) return TOK_BOOL_LIT;
        if (len == 8 && memcmp(str, "function", 8) == 0) return TOK_FUNCTION;
        if (len == 7 && memcmp(str, "finally", 7) == 0) return TOK_FINALLY;
        if (len == 4 && memcmp(str, "from", 4) == 0) return TOK_FROM;
        break;
    case 'g':
        if (len == 3 && memcmp(str, "get", 3) == 0) return TOK_GET;
        break;
    case 'i':
        if (len == 2 && memcmp(str, "if", 2) == 0) return TOK_IF;
        if (len == 2 && memcmp(str, "in", 2) == 0) return TOK_IN;
        if (len == 6 && memcmp(str, "import", 6) == 0) return TOK_IMPORT;
        if (len == 10 && memcmp(str, "instanceof", 10) == 0) return TOK_INSTANCEOF;
        break;
    case 'l':
        if (len == 3 && memcmp(str, "let", 3) == 0) return TOK_LET;
        break;
    case 'n':
        if (len == 4 && memcmp(str, "null", 4) == 0) return TOK_NULL_LIT;
        if (len == 3 && memcmp(str, "new", 3) == 0) return TOK_NEW;
        break;
    case 'o':
        if (len == 2 && memcmp(str, "of", 2) == 0) return TOK_OF;
        break;
    case 'r':
        if (len == 6 && memcmp(str, "return", 6) == 0) return TOK_RETURN;
        break;
    case 's':
        if (len == 5 && memcmp(str, "super", 5) == 0) return TOK_SUPER;
        if (len == 6 && memcmp(str, "switch", 6) == 0) return TOK_SWITCH;
        if (len == 6 && memcmp(str, "static", 6) == 0) return TOK_STATIC;
        if (len == 3 && memcmp(str, "set", 3) == 0) return TOK_SET;
        break;
    case 't':
        if (len == 4 && memcmp(str, "true", 4) == 0) return TOK_BOOL_LIT;
        if (len == 4 && memcmp(str, "this", 4) == 0) return TOK_THIS;
        if (len == 6 && memcmp(str, "typeof", 6) == 0) return TOK_TYPEOF;
        if (len == 3 && memcmp(str, "try", 3) == 0) return TOK_TRY;
        if (len == 5 && memcmp(str, "throw", 5) == 0) return TOK_THROW;
        break;
    case 'u':
        if (len == 9 && memcmp(str, "undefined", 9) == 0) return TOK_UNDEFINED_LIT;
        break;
    case 'v':
        if (len == 3 && memcmp(str, "var", 3) == 0) return TOK_VAR;
        if (len == 4 && memcmp(str, "void", 4) == 0) return TOK_VOID;
        break;
    case 'w':
        if (len == 5 && memcmp(str, "while", 5) == 0) return TOK_WHILE;
        if (len == 2 && memcmp(str, "do", 0) == 0) { /* wrong, do is 'd' */ }
        break;
    case 'y':
        if (len == 5 && memcmp(str, "yield", 5) == 0) return TOK_YIELD;
        break;
    }
    return TOK_IDENTIFIER;
}

/* ── Number Parsing ───────────────────────────────────────────────────── */

static Token lex_number(Lexer *lex)
{
    size_t start = lex->pos;
    char c = lexer_peek_char(lex);
    int is_hex = 0, is_octal = 0, is_binary = 0, is_bigint = 0;

    /* Check for 0x, 0o, 0b prefixes */
    if (c == '0' && lex->pos + 1 < lex->src_len) {
        char next = lex->src[lex->pos + 1];
        if (next == 'x' || next == 'X') {
            is_hex = 1;
            lexer_advance(lex); /* 0 */
            lexer_advance(lex); /* x */
            while (is_hex_char(lexer_peek_char(lex)) || lexer_peek_char(lex) == '_') {
                lexer_advance(lex);
            }
            goto done;
        }
        if (next == 'o' || next == 'O') {
            is_octal = 1;
            lexer_advance(lex); /* 0 */
            lexer_advance(lex); /* o */
            while (is_octal_char(lexer_peek_char(lex)) || lexer_peek_char(lex) == '_') {
                lexer_advance(lex);
            }
            goto done;
        }
        if (next == 'b' || next == 'B') {
            is_binary = 1;
            lexer_advance(lex); /* 0 */
            lexer_advance(lex); /* b */
            while (is_binary_char(lexer_peek_char(lex)) || lexer_peek_char(lex) == '_') {
                lexer_advance(lex);
            }
            goto done;
        }
    }

    /* Decimal number (numeric separators '_' allowed between digits) */
    while (isdigit((unsigned char)lexer_peek_char(lex)) ||
           (lexer_peek_char(lex) == '_' && isdigit((unsigned char)lexer_peek_char_next(lex)))) {
        lexer_advance(lex);
    }

    /* Fractional part */
    if (lexer_peek_char(lex) == '.' && lexer_peek_char_next(lex) != '.' &&
        !is_js_ident_start(lexer_peek_char_next(lex))) {
        lexer_advance(lex); /* . */
        while (isdigit((unsigned char)lexer_peek_char(lex)) ||
               (lexer_peek_char(lex) == '_' && isdigit((unsigned char)lexer_peek_char_next(lex)))) {
            lexer_advance(lex);
        }
    }

    /* Exponent */
    if (lexer_peek_char(lex) == 'e' || lexer_peek_char(lex) == 'E') {
        lexer_advance(lex); /* e */
        if (lexer_peek_char(lex) == '+' || lexer_peek_char(lex) == '-') {
            lexer_advance(lex);
        }
        while (isdigit((unsigned char)lexer_peek_char(lex)) ||
               (lexer_peek_char(lex) == '_' && isdigit((unsigned char)lexer_peek_char_next(lex)))) {
            lexer_advance(lex);
        }
    }

    /* BigInt suffix */
    if (lexer_peek_char(lex) == 'n') {
        lexer_advance(lex); /* n */
        is_bigint = 1;
    }

done: {
    /* Parse the number value */
    size_t len = lex->pos - start;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return make_error(lex, "out of memory");

    /* Copy without underscores */
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (lex->src[start + i] != '_') {
            buf[j++] = lex->src[start + i];
        }
    }
    /* Strip trailing 'n' for BigInt literals */
    if (is_bigint && j > 0 && buf[j-1] == 'n')
        buf[--j] = '\0';
    buf[j] = '\0';

    Token t = make_token_from(lex, is_bigint ? TOK_BIGINT_LIT : TOK_NUMBER, start);
    if (is_bigint) {
        t.bigint_val = strtoll(buf, NULL, 0);
    } else if (is_hex) {
        t.num_val = (double)strtoll(buf, NULL, 16);
    } else if (is_octal) {
        t.num_val = (double)strtoll(buf + 2, NULL, 8);
    } else if (is_binary) {
        t.num_val = (double)strtoll(buf + 2, NULL, 2);
    } else {
        t.num_val = strtod(buf, NULL);
    }
    free(buf);
    return t;
    }
}

/* ── String Parsing ───────────────────────────────────────────────────── */

static char *unescape_string(const char *start, size_t len, size_t *out_len)
{
    /* Allocate max possible size */
    char *result = (char *)malloc(len + 1);
    if (!result) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (start[i] == '\\' && i + 1 < len) {
            i++;
            switch (start[i]) {
            case 'n':  result[j++] = '\n'; break;
            case 't':  result[j++] = '\t'; break;
            case 'r':  result[j++] = '\r'; break;
            case 'b':  result[j++] = '\b'; break;
            case 'f':  result[j++] = '\f'; break;
            case 'v':  result[j++] = '\v'; break;
            case '0':  result[j++] = '\0'; break;
            case '\\': result[j++] = '\\'; break;
            case '\'': result[j++] = '\''; break;
            case '"':  result[j++] = '"'; break;
            case '`':  result[j++] = '`'; break;
            case 'x': {
                if (i + 2 < len && is_hex_char(start[i+1]) && is_hex_char(start[i+2])) {
                    char hex[3] = {start[i+1], start[i+2], 0};
                    result[j++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    result[j++] = 'x';
                }
                break;
            }
            case 'u': {
                if (i + 4 < len && start[i+1] == '{') {
                    /* Unicode escape with variable length: \u{...} */
                    i += 2; /* skip u{ */
                    unsigned long cp = 0;
                    while (i < len && start[i] != '}') {
                        cp = cp * 16 + (start[i] >= 'a' ? start[i] - 'a' + 10 :
                             start[i] >= 'A' ? start[i] - 'A' + 10 : start[i] - '0');
                        i++;
                    }
                    if (cp < 0x80) {
                        result[j++] = (char)cp;
                    } else if (cp < 0x800) {
                        result[j++] = (char)(0xC0 | (cp >> 6));
                        result[j++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        result[j++] = (char)(0xE0 | (cp >> 12));
                        result[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        result[j++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else if (i + 4 < len) {
                    char hex[5] = {start[i+1], start[i+2], start[i+3], start[i+4], 0};
                    unsigned long cp = strtoul(hex, NULL, 16);
                    if (cp < 0x80) {
                        result[j++] = (char)cp;
                    } else if (cp < 0x800) {
                        result[j++] = (char)(0xC0 | (cp >> 6));
                        result[j++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        result[j++] = (char)(0xE0 | (cp >> 12));
                        result[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        result[j++] = (char)(0x80 | (cp & 0x3F));
                    }
                    i += 4;
                } else {
                    result[j++] = 'u';
                }
                break;
            }
            case '\r':
                /* line continuation */
                if (i + 1 < len && start[i+1] == '\n') i++;
                break;
            case '\n':
                /* line continuation */
                break;
            default:
                result[j++] = start[i];
                break;
            }
        } else {
            result[j++] = start[i];
        }
    }
    result[j] = '\0';
    *out_len = j;
    return result;
}

static Token lex_string(Lexer *lex, char quote)
{
    size_t start = lex->pos;
    lexer_advance(lex); /* skip opening quote */

    /* Find the closing quote */
    size_t content_start = lex->pos;
    while (lex->pos < lex->src_len) {
        char c = lexer_peek_char(lex);
        if (c == quote) {
            size_t content_len = lex->pos - content_start;
            char *decoded = unescape_string(lex->src + content_start, content_len, &content_len);
            lexer_advance(lex); /* skip closing quote */
            Token t = make_token_from(lex, TOK_STRING, start);
            t.str_val = decoded;
            return t;
        }
        if (c == '\\') {
            lexer_advance(lex); /* skip escape char */
            if (lex->pos < lex->src_len) {
                /* Handle \r\n line continuation */
                if (lexer_peek_char(lex) == '\r') {
                    lexer_advance(lex);
                    if (lexer_peek_char(lex) == '\n') lexer_advance(lex);
                    continue;
                }
            }
        }
        if (is_line_terminator(c)) {
            /* Unterminated string literal */
            Token t = make_token_from(lex, TOK_ERROR, start);
            t.start = "unterminated string literal";
            t.len = 26;
            return t;
        }
        lexer_advance(lex);
    }

    Token t = make_token_from(lex, TOK_ERROR, start);
    t.start = "unterminated string literal";
    t.len = 26;
    return t;
}

/* ── Template Literal Parsing ─────────────────────────────────────────── */

static Token lex_template(Lexer *lex)
{
    size_t start = lex->pos;
    /* The very first char is the opening backtick only when we are not already
     * inside a template (i.e. this is the start of the whole literal). After an
     * interpolation we are invoked at the continuation text, or at the closing
     * backtick, which we must NOT treat as a new opening backtick. */
    int opening = (lexer_peek_char(lex) == '`') && !lex->in_template;
    if (opening) {
        lexer_advance(lex);      /* skip opening backtick */
        lex->in_template = 1;
    }
    size_t body_start = lex->pos; /* first char of the text fragment */

    while (lex->pos < lex->src_len) {
        char c = lexer_peek_char(lex);
        if (c == '`') {
            lexer_advance(lex); /* skip closing backtick */
            lex->in_template = 0;
            Token t = make_token_from(lex, TOK_TEMPLATE_END, start);
            /* Store the verbatim text between the backticks (no backticks). */
            size_t len = lex->pos - 1 - body_start;
            t.str_val = (char *)malloc(len + 1);
            if (t.str_val) {
                memcpy(t.str_val, lex->src + body_start, len);
                t.str_val[len] = '\0';
            }
            return t;
        }
        if (c == '$' && lexer_peek_char_next(lex) == '{') {
            lexer_advance(lex); /* skip $ */
            lexer_advance(lex); /* skip { */
            Token t = make_token_from(lex, TOK_TEMPLATE, start);
            /* Store the verbatim text up to (but not including) the '$'. */
            size_t len = (lex->pos - 2) - body_start;
            t.str_val = (char *)malloc(len + 1);
            if (t.str_val) {
                memcpy(t.str_val, lex->src + body_start, len);
                t.str_val[len] = '\0';
            }
            return t;
        }
        if (c == '\\') {
            lexer_advance(lex); /* skip escape */
            if (lex->pos < lex->src_len) lexer_advance(lex);
            continue;
        }
        if (is_line_terminator(c)) {
            /* Template literals can span multiple lines, but we handle line terminators */
            lexer_advance(lex);
            continue;
        }
        lexer_advance(lex);
    }

    Token t = make_token_from(lex, TOK_ERROR, start);
    t.start = "unterminated template literal";
    t.len = 28;
    return t;
}

/* ── Regular Expression Parsing ───────────────────────────────────────── */

static Token lex_regexp(Lexer *lex)
{
    size_t start = lex->pos;
    lexer_advance(lex); /* skip opening / */

    int in_class = 0;
    while (lex->pos < lex->src_len) {
        char c = lexer_peek_char(lex);
        if (c == '\\') {
            lexer_advance(lex); /* skip escape */
            if (lex->pos < lex->src_len) lexer_advance(lex);
            continue;
        }
        if (c == '[') {
            in_class = 1;
        } else if (c == ']') {
            in_class = 0;
        } else if (c == '/' && !in_class) {
            lexer_advance(lex); /* skip closing / */
            break;
        } else if (is_line_terminator(c)) {
            Token t = make_token_from(lex, TOK_ERROR, start);
            t.start = "unterminated regular expression";
            t.len = 30;
            return t;
        }
        lexer_advance(lex);
    }

    /* Parse flags */
    while (lex->pos < lex->src_len) {
        char c = lexer_peek_char(lex);
        if (is_js_ident_continue(c) && c != '\\') {
            lexer_advance(lex);
        } else {
            break;
        }
    }

    return make_token_from(lex, TOK_REGEXP, start);
}

/* ── Main Tokenizer ───────────────────────────────────────────────────── */

Token lexer_next(Lexer *lex)
{
    /* If we have a peeked token, return it */
    if (lex->has_peek) {
        lex->has_peek = 0;
        Token t = lex->peek;
        /* Clear peek to avoid double-free issues */
        memset(&lex->peek, 0, sizeof(Token));
        return t;
    }

    lexer_skip_whitespace(lex);

    if (lex->pos >= lex->src_len) {
        Token t;
        t.type = TOK_EOF;
        t.start = lex->src + lex->src_len;
        t.len = 0;
        t.num_val = 0.0;
        t.str_val = NULL;
        t.line = lex->line;
        t.col = lex->col;
        return t;
    }

    char c = lexer_peek_char(lex);
    size_t start = lex->pos;

    /* Handle private names: #name (class private fields/methods) */
    if (c == '#') {
        lexer_advance(lex); /* skip # */
        while (is_js_ident_continue(lexer_peek_char(lex))) {
            lexer_advance(lex);
        }
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_PRIVATE_NAME, start);
    }

    /* Handle identifiers and keywords */
    if (is_js_ident_start(c)) {
        lexer_advance(lex);
        while (is_js_ident_continue(lexer_peek_char(lex))) {
            lexer_advance(lex);
        }

        size_t ident_len = lex->pos - start;
        TokenType kw = lookup_keyword(lex->src + start, ident_len);

        if (kw == TOK_BOOL_LIT) {
            Token t = make_token_from(lex, TOK_BOOL_LIT, start);
            t.num_val = (double)(lex->src[start] == 't' ? 1 : 0);
            return t;
        }

        return make_token_from(lex, kw, start);
    }

    /* Handle numbers */
    if (isdigit((unsigned char)c)) {
        Token t = lex_number(lex);
        lex->allow_regexp = 0; /* / after number is division */
        return t;
    }

    /* Handle strings */
    if (c == '"' || c == '\'') {
        return lex_string(lex, c);
    }

    /* Handle template literals */
    if (c == '`') {
        lex->allow_regexp = 1;
        return lex_template(lex);
    }

    /* Handle template continuation after ${...} */
    /* This is handled by the parser calling lexer_next when it sees TOK_TEMPLATE */

    /* Handle single-character tokens / multi-character operators */
    switch (c) {
    case '(': lexer_advance(lex); lex->allow_regexp = 1; return make_token_from(lex, TOK_LPAREN, start);
    case ')': lexer_advance(lex); lex->allow_regexp = 0; return make_token_from(lex, TOK_RPAREN, start);
    case '[': lexer_advance(lex); lex->allow_regexp = 1; return make_token_from(lex, TOK_LBRACKET, start);
    case ']': lexer_advance(lex); lex->allow_regexp = 0; return make_token_from(lex, TOK_RBRACKET, start);
    case '{': lexer_advance(lex); lex->allow_regexp = 1; return make_token_from(lex, TOK_LBRACE, start);
    case '}': lexer_advance(lex); lex->allow_regexp = 0; return make_token_from(lex, TOK_RBRACE, start);
    case ';': lexer_advance(lex); lex->allow_regexp = 1; return make_token_from(lex, TOK_SEMICOLON, start);
    case ',': lexer_advance(lex); lex->allow_regexp = 1; return make_token_from(lex, TOK_COMMA, start);
    case ':': lexer_advance(lex); lex->allow_regexp = 1; return make_token_from(lex, TOK_COLON, start);
    case '?':
        if (lexer_peek_char_next(lex) == '?') {
            lexer_advance(lex); lexer_advance(lex); /* ?? */
            if (lexer_peek_char(lex) == '=') {
                lexer_advance(lex); /* = */
                lex->allow_regexp = 1;
                return make_token_from(lex, TOK_NULLISH_ASSIGN, start);
            }
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_NULLISH, start);
        }
        if (lexer_peek_char_next(lex) == '.') {
            lexer_advance(lex); lexer_advance(lex); /* ?. */
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_QUESTION_DOT, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 1;
        return make_token_from(lex, TOK_QUESTION, start);
    case '~':
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_BIT_NOT, start);
    case '.':
        if (lexer_peek_char_next(lex) == '.' && lex->pos + 2 < lex->src_len &&
            lex->src[lex->pos + 2] == '.') {
            lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_ELLIPSIS, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_DOT, start);

    /* Operators */
    case '+':
        if (lexer_peek_char_next(lex) == '+') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0; /* increment prefix, next is expr */
            return make_token_from(lex, TOK_INC, start);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1; /* assignment opens new expr */
            return make_token_from(lex, TOK_PLUS_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0; /* binary +, next is expr */
        return make_token_from(lex, TOK_PLUS, start);
    case '-':
        if (lexer_peek_char_next(lex) == '-') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0; /* decrement prefix, next is expr */
            return make_token_from(lex, TOK_DEC, start);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1; /* assignment opens new expr */
            return make_token_from(lex, TOK_MINUS_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0; /* binary -, next is expr */
        return make_token_from(lex, TOK_MINUS, start);
    case '*':
        if (lexer_peek_char_next(lex) == '*') {
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '=') {
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 1;
                return make_token_from(lex, TOK_POW_ASSIGN, start);
            }
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_POW, start);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_MUL_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_MUL, start);
    case '%':
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_MOD_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_MOD, start);
    case '=':
        if (lexer_peek_char_next(lex) == '=') {
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '=') {
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 0;
                return make_token_from(lex, TOK_STRICT_EQ, start);
            }
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_EQ, start);
        }
        if (lexer_peek_char_next(lex) == '>') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_ARROW, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 1; /* assignment opens new expression context */
        return make_token_from(lex, TOK_ASSIGN, start);
    case '!':
        if (lexer_peek_char_next(lex) == '=') {
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '=') {
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 0;
                return make_token_from(lex, TOK_STRICT_NEQ, start);
            }
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_NEQ, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_NOT, start);
    case '<':
        if (lexer_peek_char_next(lex) == '<') {
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '=') {
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 1;
                return make_token_from(lex, TOK_SHIFT_LEFT_ASSIGN, start);
            }
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_SHIFT_LEFT, start);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_LE, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_LT, start);
    case '>':
        if (lexer_peek_char_next(lex) == '>') {
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '>') {
                if (lex->pos + 3 < lex->src_len && lex->src[lex->pos + 3] == '=') {
                    lexer_advance(lex); lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                    lex->allow_regexp = 1;
                    return make_token_from(lex, TOK_USHIFT_RIGHT_ASSIGN, start);
                }
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 0;
                return make_token_from(lex, TOK_USHIFT_RIGHT, start);
            }
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '=') {
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 1;
                return make_token_from(lex, TOK_SHIFT_RIGHT_ASSIGN, start);
            }
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_SHIFT_RIGHT, start);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_GE, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_GT, start);
    case '&':
        if (lexer_peek_char_next(lex) == '&') {
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '=') {
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 1;
                return make_token_from(lex, TOK_AND_AND_ASSIGN, start);
            }
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_AND_AND, start);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_AND_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_BIT_AND, start);
    case '|':
        if (lexer_peek_char_next(lex) == '|') {
            if (lex->pos + 2 < lex->src_len && lex->src[lex->pos + 2] == '=') {
                lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
                lex->allow_regexp = 1;
                return make_token_from(lex, TOK_OR_OR_ASSIGN, start);
            }
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 0;
            return make_token_from(lex, TOK_OR_OR, start);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_OR_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_BIT_OR, start);
    case '^':
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_XOR_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_BIT_XOR, start);
    case '/':
        /* Handle regexp vs division */
        if (lex->allow_regexp) {
            return lex_regexp(lex);
        }
        if (lexer_peek_char_next(lex) == '=') {
            lexer_advance(lex); lexer_advance(lex);
            lex->allow_regexp = 1;
            return make_token_from(lex, TOK_DIV_ASSIGN, start);
        }
        lexer_advance(lex);
        lex->allow_regexp = 0;
        return make_token_from(lex, TOK_DIV, start);
    default:
        lexer_advance(lex);
        return make_error(lex, "unexpected character");
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void lexer_init(Lexer *lex, const char *src, size_t len)
{
    lex->src = src;
    lex->src_len = len;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->has_peek = 0;
    lex->allow_regexp = 1; /* start of file can have regexp */
    lex->in_template = 0;
    memset(&lex->current, 0, sizeof(Token));
    memset(&lex->peek, 0, sizeof(Token));
}

Token lexer_peek(Lexer *lex)
{
    if (!lex->has_peek) {
        lex->peek = lexer_next(lex);
        lex->has_peek = 1;
    }
    return lex->peek;
}

void lexer_skip(Lexer *lex)
{
    lexer_next(lex); /* consume and discard current token */
}

/* ── Utility Functions ────────────────────────────────────────────────── */

const char *token_type_name(TokenType type)
{
    switch (type) {
    case TOK_EOF: return "EOF";
    case TOK_NUMBER: return "number";
    case TOK_STRING: return "string";
    case TOK_IDENTIFIER: return "identifier";
    case TOK_REGEXP: return "regexp";
    case TOK_BOOL_LIT: return "boolean literal";
    case TOK_NULL_LIT: return "null";
    case TOK_UNDEFINED_LIT: return "undefined";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_LBRACKET: return "'['";
    case TOK_RBRACKET: return "']'";
    case TOK_LBRACE: return "'{'";
    case TOK_RBRACE: return "'}'";
    case TOK_DOT: return "'.'";
    case TOK_SEMICOLON: return "';'";
    case TOK_COMMA: return "','";
    case TOK_COLON: return "':'";
    case TOK_ARROW: return "'=>'";
    case TOK_ELLIPSIS: return "'...'";
    case TOK_ASSIGN: return "'='";
    case TOK_PLUS_ASSIGN: return "'+='";
    case TOK_MINUS_ASSIGN: return "'-='";
    case TOK_MUL_ASSIGN: return "'*='";
    case TOK_DIV_ASSIGN: return "'/='";
    case TOK_MOD_ASSIGN: return "'%='";
    case TOK_POW_ASSIGN: return "'**='";
    case TOK_EQ: return "'=='";
    case TOK_NEQ: return "'!='";
    case TOK_STRICT_EQ: return "'==='";
    case TOK_STRICT_NEQ: return "'!=='";
    case TOK_LT: return "'<'";
    case TOK_GT: return "'>'";
    case TOK_LE: return "'<='";
    case TOK_GE: return "'>='";
    case TOK_PLUS: return "'+'";
    case TOK_MINUS: return "'-'";
    case TOK_MUL: return "'*'";
    case TOK_DIV: return "'/'";
    case TOK_MOD: return "'%'";
    case TOK_POW: return "'**'";
    case TOK_INC: return "'++'";
    case TOK_DEC: return "'--'";
    case TOK_AND: return "'&'"; /* bitwise */
    case TOK_OR: return "'|'";  /* bitwise */
    case TOK_NOT: return "'!'";
    case TOK_BIT_AND: return "'&'";
    case TOK_BIT_OR: return "'|'";
    case TOK_BIT_XOR: return "'^'";
    case TOK_BIT_NOT: return "'~'";
    case TOK_AND_AND: return "'&&'";
    case TOK_OR_OR: return "'||'";
    case TOK_NULLISH: return "'\?\?'";
    case TOK_QUESTION: return "'?'";
    case TOK_QUESTION_DOT: return "'?.'";
    case TOK_TEMPLATE: return "template literal";
    case TOK_TEMPLATE_END: return "end of template";
    case TOK_LET: return "'let'";
    case TOK_CONST: return "'const'";
    case TOK_VAR: return "'var'";
    case TOK_FUNCTION: return "'function'";
    case TOK_IF: return "'if'";
    case TOK_ELSE: return "'else'";
    case TOK_FOR: return "'for'";
    case TOK_WHILE: return "'while'";
    case TOK_DO: return "'do'";
    case TOK_SWITCH: return "'switch'";
    case TOK_CASE: return "'case'";
    case TOK_DEFAULT: return "'default'";
    case TOK_BREAK: return "'break'";
    case TOK_CONTINUE: return "'continue'";
    case TOK_RETURN: return "'return'";
    case TOK_THROW: return "'throw'";
    case TOK_TRY: return "'try'";
    case TOK_CATCH: return "'catch'";
    case TOK_FINALLY: return "'finally'";
    case TOK_NEW: return "'new'";
    case TOK_THIS: return "'this'";
    case TOK_TYPEOF: return "'typeof'";
    case TOK_DELETE: return "'delete'";
    case TOK_VOID: return "'void'";
    case TOK_CLASS: return "'class'";
    case TOK_EXTENDS: return "'extends'";
    case TOK_SUPER: return "'super'";
    case TOK_IMPORT: return "'import'";
    case TOK_EXPORT: return "'export'";
    case TOK_FROM: return "'from'";
    case TOK_AS: return "'as'";
    case TOK_IN: return "'in'";
    case TOK_INSTANCEOF: return "'instanceof'";
    case TOK_OF: return "'of'";
    case TOK_ASYNC: return "'async'";
    case TOK_AWAIT: return "'await'";
    case TOK_YIELD: return "'yield'";
    case TOK_STATIC: return "'static'";
    case TOK_GET: return "'get'";
    case TOK_SET: return "'set'";
    case TOK_DEBUGGER: return "'debugger'";
    case TOK_ERROR: return "error";
    default: return "unknown";
    }
}

void token_free_data(Token *tok)
{
    if (tok->str_val) {
        free(tok->str_val);
        tok->str_val = NULL;
    }
}

void lexer_reset_peek(Lexer *lex)
{
    lex->has_peek = 0;
}

Token lexer_template_next(Lexer *lex)
{
    return lex_template(lex);
}