/*
 * LR_JS - JavaScript Engine Lexer
 * Pure C recursive descent lexer for JavaScript/ES2022 source code.
 *
 * Tokenizes JS source into a stream of tokens for the parser.
 * Supports all ES2022 syntax including template literals, regexps,
 * and all operator/keyword types.
 */
#ifndef LR_LEXER_H
#define LR_LEXER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Token Types ──────────────────────────────────────────────────────── */

typedef enum {
    TOK_EOF = 0,
    TOK_NUMBER, TOK_STRING, TOK_IDENTIFIER, TOK_REGEXP,
    TOK_BOOL_LIT, TOK_NULL_LIT, TOK_UNDEFINED_LIT,
    /* Punctuators */
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE, TOK_DOT, TOK_SEMICOLON,
    TOK_COMMA, TOK_COLON, TOK_ARROW, TOK_ELLIPSIS,
    /* Operators */
    TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_MUL_ASSIGN, TOK_DIV_ASSIGN,
    TOK_MOD_ASSIGN, TOK_POW_ASSIGN, TOK_AND_ASSIGN, TOK_OR_ASSIGN, TOK_XOR_ASSIGN,
    TOK_SHIFT_LEFT_ASSIGN, TOK_SHIFT_RIGHT_ASSIGN, TOK_USHIFT_RIGHT_ASSIGN,
    TOK_AND_AND_ASSIGN,  /* &&= */
    TOK_OR_OR_ASSIGN,    /* ||= */
    TOK_NULLISH_ASSIGN,  /* ??= */
    TOK_EQ, TOK_NEQ, TOK_STRICT_EQ, TOK_STRICT_NEQ,
    TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_MOD, TOK_POW,
    TOK_INC, TOK_DEC,
    TOK_AND, TOK_OR, TOK_NOT, TOK_BIT_AND, TOK_BIT_OR, TOK_BIT_XOR, TOK_BIT_NOT,
    TOK_AND_AND, TOK_OR_OR, TOK_NULLISH,
    TOK_SHIFT_LEFT, TOK_SHIFT_RIGHT, TOK_USHIFT_RIGHT,
    TOK_QUESTION, TOK_QUESTION_DOT,
    TOK_TEMPLATE, TOK_TEMPLATE_END,
    /* Keywords */
    TOK_LET, TOK_CONST, TOK_VAR, TOK_FUNCTION, TOK_IF, TOK_ELSE,
    TOK_FOR, TOK_WHILE, TOK_DO, TOK_SWITCH, TOK_CASE, TOK_DEFAULT,
    TOK_BREAK, TOK_CONTINUE, TOK_RETURN, TOK_THROW, TOK_TRY, TOK_CATCH, TOK_FINALLY,
    TOK_NEW, TOK_THIS, TOK_TYPEOF, TOK_DELETE, TOK_VOID,
    TOK_CLASS, TOK_EXTENDS, TOK_SUPER,
    TOK_IMPORT, TOK_EXPORT, TOK_FROM, TOK_AS,
    TOK_IN, TOK_INSTANCEOF, TOK_OF,
    TOK_ASYNC, TOK_AWAIT, TOK_YIELD,
    TOK_STATIC, TOK_GET, TOK_SET,
    TOK_PRIVATE_NAME,
    TOK_DEBUGGER,
    TOK_ERROR,
} TokenType;

/* ── Token ────────────────────────────────────────────────────────────── */

typedef struct {
    TokenType type;
    const char *start;  /* pointer into source */
    size_t len;
    double num_val;     /* for TOK_NUMBER */
    char *str_val;      /* for TOK_STRING (allocated) */
    size_t line;
    size_t col;
} Token;

/* ── Lexer ────────────────────────────────────────────────────────────── */

typedef struct {
    const char *src;
    size_t src_len;
    size_t pos;
    size_t line;
    size_t col;
    Token current;
    Token peek;
    int has_peek;
    int allow_regexp;   /* context flag: 1 = next / starts regexp, 0 = division */
} Lexer;

/* ── Functions ────────────────────────────────────────────────────────── */

void lexer_init(Lexer *lex, const char *src, size_t len);
Token lexer_next(Lexer *lex);
Token lexer_peek(Lexer *lex);
void lexer_skip(Lexer *lex);

/* Utility: get string representation of a token type */
const char *token_type_name(TokenType type);

/* Free string data allocated in a token */
void token_free_data(Token *tok);

/* Reset the peek cache (call after restoring lexer position) */
void lexer_reset_peek(Lexer *lex);

#ifdef __cplusplus
}
#endif

#endif /* LR_LEXER_H */