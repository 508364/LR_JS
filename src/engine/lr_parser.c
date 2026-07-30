/*
 * LR_JS - JavaScript Engine Recursive Descent Parser
 * Pure C, ES2022-compatible.
 *
 * Parses JavaScript source code into an AST using recursive descent
 * with operator precedence climbing for expressions.
 */
#include "lr_ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

/* ── Memory Allocation Helpers ────────────────────────────────────────── */

/* Check if a token type can be used as a property name (identifier or contextual keyword) */
static int is_prop_name_token(TokenType type)
{
    /* Identifiers, private names, literal keywords, and ALL reserved
     * words are valid property names after '.' (e.g. p.catch, o.default).
     * Keywords are contiguous in the enum: TOK_LET .. TOK_SET. */
    if (type == TOK_IDENTIFIER || type == TOK_PRIVATE_NAME) return 1;
    if (type == TOK_BOOL_LIT || type == TOK_NULL_LIT || type == TOK_UNDEFINED_LIT) return 1;
    if (type >= TOK_LET && type <= TOK_SET) return 1;
    return 0;
}

static void *p_malloc(size_t size)
{
    return malloc(size);
}

static void *p_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}

static void p_free(void *ptr)
{
    free(ptr);
}

static void *p_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

/* ── String Interning ──────────────────────────────────────────────────── */

/* Simple DJB2 hash for strings */
static unsigned int str_intern_hash(const char *str, size_t len)
{
    unsigned int hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)str[i];
    }
    return hash % LR_STRING_INTERN_SIZE;
}

/* Intern a string: returns a pointer to the interned copy (stable across calls).
 * The interned string is freed when parser_free is called (or at parse end). */
static const char *parser_intern_string(Parser *parser, const char *str, size_t len)
{
    if (!str || len == 0) return "";
    unsigned int idx = str_intern_hash(str, len);

    /* Look for existing entry */
    LRStringIntern *entry = parser->intern_table[idx];
    while (entry) {
        if (entry->len == len && memcmp(entry->str, str, len) == 0) {
            return entry->str;
        }
        entry = entry->next;
    }

    /* Create new entry */
    entry = (LRStringIntern *)p_malloc(sizeof(LRStringIntern));
    if (!entry) {
        /* Fallback: duplicate the string */
        char *dup = (char *)p_malloc(len + 1);
        if (dup) { memcpy(dup, str, len); dup[len] = '\0'; }
        return dup;
    }
    entry->str = (char *)p_malloc(len + 1);
    if (entry->str) {
        memcpy(entry->str, str, len);
        entry->str[len] = '\0';
    }
    entry->len = len;
    entry->next = parser->intern_table[idx];
    parser->intern_table[idx] = entry;
    return entry->str ? entry->str : "";
}

/* Convenience wrapper for null-terminated strings */
static const char *parser_intern(Parser *parser, const char *str)
{
    if (!str) return "";
    return parser_intern_string(parser, str, strlen(str));
}

/* ── Parser Error Helpers ─────────────────────────────────────────────── */

static void parser_error(Parser *parser, const char *fmt, ...)
{
    if (parser->has_error) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(parser->error_msg, sizeof(parser->error_msg), fmt, ap);
    va_end(ap);
    parser->has_error = 1;
    parser->error_line = parser->lexer->line;
    parser->error_col = parser->lexer->col;
}

static void parser_error_token(Parser *parser, Token tok, const char *fmt, ...)
{
    if (parser->has_error) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(parser->error_msg, sizeof(parser->error_msg), fmt, ap);
    va_end(ap);
    parser->has_error = 1;
    parser->error_line = tok.line;
    parser->error_col = tok.col;
}

/* ── Token Matching ───────────────────────────────────────────────────── */

static Token expect_token(Parser *parser, TokenType type)
{
    Token t = lexer_next(parser->lexer);
    if (t.type != type) {
        parser_error_token(parser, t, "expected %s but got %s",
            token_type_name(type), token_type_name(t.type));
    }
    return t;
}

static int match_token(Parser *parser, TokenType type)
{
    Token t = lexer_peek(parser->lexer);
    if (t.type == type) {
        lexer_skip(parser->lexer);
        return 1;
    }
    return 0;
}

static int peek_token(Parser *parser, TokenType type)
{
    Token t = lexer_peek(parser->lexer);
    return t.type == type;
}

/* ── Lexer State Save/Restore (for speculative parsing) ────────────────── */

typedef struct {
    size_t pos;
    size_t line;
    size_t col;
    Token  peek;
    int    has_peek;
    int    allow_regexp;
    int    in_template;
} LexState;

static LexState lex_state_save(Lexer *lex)
{
    LexState st;
    st.pos = lex->pos;
    st.line = lex->line;
    st.col = lex->col;
    st.peek = lex->peek;
    st.has_peek = lex->has_peek;
    st.allow_regexp = lex->allow_regexp;
    st.in_template = lex->in_template;
    return st;
}

static void lex_state_restore(Lexer *lex, LexState st)
{
    lex->pos = st.pos;
    lex->line = st.line;
    lex->col = st.col;
    lex->peek = st.peek;
    lex->has_peek = st.has_peek;
    lex->allow_regexp = st.allow_regexp;
    lex->in_template = st.in_template;
}

/* Check if current token is an assignment operator */
static int is_assign_op(TokenType type)
{
    switch (type) {
    case TOK_ASSIGN:
    case TOK_PLUS_ASSIGN:
    case TOK_MINUS_ASSIGN:
    case TOK_MUL_ASSIGN:
    case TOK_DIV_ASSIGN:
    case TOK_MOD_ASSIGN:
    case TOK_POW_ASSIGN:
    case TOK_AND_ASSIGN:
    case TOK_OR_ASSIGN:
    case TOK_XOR_ASSIGN:
    case TOK_SHIFT_LEFT_ASSIGN:
    case TOK_SHIFT_RIGHT_ASSIGN:
    case TOK_USHIFT_RIGHT_ASSIGN:
    case TOK_AND_AND_ASSIGN:
    case TOK_OR_OR_ASSIGN:
    case TOK_NULLISH_ASSIGN:
        return 1;
    default:
        return 0;
    }
}

/* Get operator string from token type */
static const char *get_op_str(TokenType type)
{
    switch (type) {
    case TOK_PLUS: return "+";
    case TOK_MINUS: return "-";
    case TOK_MUL: return "*";
    case TOK_DIV: return "/";
    case TOK_MOD: return "%";
    case TOK_POW: return "**";
    case TOK_EQ: return "==";
    case TOK_NEQ: return "!=";
    case TOK_STRICT_EQ: return "===";
    case TOK_STRICT_NEQ: return "!==";
    case TOK_LT: return "<";
    case TOK_GT: return ">";
    case TOK_LE: return "<=";
    case TOK_GE: return ">=";
    case TOK_BIT_AND: return "&";
    case TOK_BIT_OR: return "|";
    case TOK_BIT_XOR: return "^";
    case TOK_AND_AND: return "&&";
    case TOK_OR_OR: return "||";
    case TOK_NULLISH: return "??";
    case TOK_SHIFT_LEFT: return "<<";
    case TOK_SHIFT_RIGHT: return ">>";
    case TOK_USHIFT_RIGHT: return ">>>";
    case TOK_IN: return "in";
    case TOK_INSTANCEOF: return "instanceof";
    case TOK_ASSIGN: return "=";
    case TOK_PLUS_ASSIGN: return "+=";
    case TOK_MINUS_ASSIGN: return "-=";
    case TOK_MUL_ASSIGN: return "*=";
    case TOK_DIV_ASSIGN: return "/=";
    case TOK_MOD_ASSIGN: return "%=";
    case TOK_POW_ASSIGN: return "**=";
    case TOK_AND_ASSIGN: return "&=";
    case TOK_OR_ASSIGN: return "|=";
    case TOK_XOR_ASSIGN: return "^=";
    case TOK_SHIFT_LEFT_ASSIGN: return "<<=";
    case TOK_SHIFT_RIGHT_ASSIGN: return ">>=";
    case TOK_USHIFT_RIGHT_ASSIGN: return ">>>=";
    case TOK_AND_AND_ASSIGN: return "&&=";
    case TOK_OR_OR_ASSIGN: return "||=";
    case TOK_NULLISH_ASSIGN: return "?" "?=";
    default: return "";
    }
}

/* Get binary operator precedence (higher = binds tighter) */
static int get_precedence(TokenType type, int is_left)
{
    (void)is_left;
    switch (type) {
    case TOK_COMMA: /* comma operator in expression */
        return 0;
    case TOK_ASSIGN: case TOK_PLUS_ASSIGN: case TOK_MINUS_ASSIGN:
    case TOK_MUL_ASSIGN: case TOK_DIV_ASSIGN: case TOK_MOD_ASSIGN:
    case TOK_POW_ASSIGN: case TOK_AND_ASSIGN: case TOK_OR_ASSIGN:
    case TOK_XOR_ASSIGN: case TOK_SHIFT_LEFT_ASSIGN: case TOK_SHIFT_RIGHT_ASSIGN:
    case TOK_USHIFT_RIGHT_ASSIGN:
    case TOK_AND_AND_ASSIGN: case TOK_OR_OR_ASSIGN: case TOK_NULLISH_ASSIGN:
        return 1;
    case TOK_NULLISH:
        return 2;
    case TOK_QUESTION: /* ternary, right-associative, same level as nullish */
        return 2;
    case TOK_OR_OR:
        return 3;
    case TOK_AND_AND:
        return 4;
    case TOK_BIT_OR:
        return 5;
    case TOK_BIT_XOR:
        return 6;
    case TOK_BIT_AND:
        return 7;
    case TOK_EQ: case TOK_NEQ: case TOK_STRICT_EQ: case TOK_STRICT_NEQ:
        return 8;
    case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE:
    case TOK_IN: case TOK_INSTANCEOF:
        return 9;
    case TOK_SHIFT_LEFT: case TOK_SHIFT_RIGHT: case TOK_USHIFT_RIGHT:
        return 10;
    case TOK_PLUS: case TOK_MINUS:
        return 11;
    case TOK_MUL: case TOK_DIV: case TOK_MOD:
        return 12;
    case TOK_POW:
        return 13; /* right-associative */
    default:
        return -1;
    }
}

/* ── AST Node Creation ────────────────────────────────────────────────── */

/* Pool-based allocation: get a node from the pre-allocated pool, or fall back to malloc. */
static ASTNode *ast_alloc_from_pool(Parser *parser, ASTNodeType type)
{
    ASTNode *node;
    if (parser->use_pool && parser->node_pool.count < LR_AST_NODE_POOL_SIZE) {
        node = &parser->node_pool.nodes[parser->node_pool.count++];
        memset(node, 0, sizeof(ASTNode));
    } else {
        node = (ASTNode *)p_calloc(1, sizeof(ASTNode));
    }
    if (node) {
        node->type = type;
    }
    return node;
}

ASTNode *ast_alloc(ASTNodeType type)
{
    /* Global version (used when parser is not available) - falls back to malloc */
    ASTNode *node = (ASTNode *)p_calloc(1, sizeof(ASTNode));
    if (node) {
        node->type = type;
    }
    return node;
}

/* Pool-aware version used by parser functions */
#define parser_ast_alloc(parser, type) ast_alloc_from_pool(parser, type)

/* Cached literal nodes for common values (singletons, used to avoid allocations).
 * These are allocated once and reused. */
static ASTNode *cached_true_node = NULL;
static ASTNode *cached_false_node = NULL;
static ASTNode *cached_null_node = NULL;
static ASTNode *cached_undefined_node = NULL;
static ASTNode *cached_zero_node = NULL;
static ASTNode *cached_one_node = NULL;

/* Initialize cached literal nodes (called once at first parse) */
static void init_cached_literals(void)
{
    if (!cached_true_node) {
        cached_true_node = ast_alloc(AST_LITERAL);
        if (cached_true_node) {
            cached_true_node->token.type = TOK_BOOL_LIT;
            cached_true_node->u.bool_val.val = 1;
        }
    }
    if (!cached_false_node) {
        cached_false_node = ast_alloc(AST_LITERAL);
        if (cached_false_node) {
            cached_false_node->token.type = TOK_BOOL_LIT;
            cached_false_node->u.bool_val.val = 0;
        }
    }
    if (!cached_null_node) {
        cached_null_node = ast_alloc(AST_LITERAL);
        if (cached_null_node) {
            cached_null_node->token.type = TOK_NULL_LIT;
            cached_null_node->u.number.num = 0;
        }
    }
    if (!cached_undefined_node) {
        cached_undefined_node = ast_alloc(AST_LITERAL);
        if (cached_undefined_node) {
            cached_undefined_node->token.type = TOK_UNDEFINED_LIT;
            cached_undefined_node->u.number.num = -1;
        }
    }
    if (!cached_zero_node) {
        cached_zero_node = ast_alloc(AST_LITERAL);
        if (cached_zero_node) {
            cached_zero_node->token.type = TOK_NUMBER;
            cached_zero_node->u.number.num = 0.0;
        }
    }
    if (!cached_one_node) {
        cached_one_node = ast_alloc(AST_LITERAL);
        if (cached_one_node) {
            cached_one_node->token.type = TOK_NUMBER;
            cached_one_node->u.number.num = 1.0;
        }
    }
}

ASTNode *ast_literal_number(Parser *parser, double num)
{
    /* Return cached nodes for common values */
    if (num == 0.0) return cached_zero_node;
    if (num == 1.0) return cached_one_node;

    ASTNode *node = parser_ast_alloc(parser, AST_LITERAL);
    if (node) {
        node->token.type = TOK_NUMBER;
        node->u.number.num = num;
    }
    return node;
}

ASTNode *ast_literal_string(Parser *parser, const char *str)
{
    ASTNode *node = parser_ast_alloc(parser, AST_LITERAL);
    if (node) {
        node->token.type = TOK_STRING;
        /* Intern the string to avoid duplicate strdup */
        if (parser && str) {
            node->u.string.str = (char *)parser_intern(parser, str);
        } else {
            node->u.string.str = str ? strdup(str) : NULL;
        }
    }
    return node;
}

ASTNode *ast_literal_bool(Parser *parser, int val)
{
    /* Return cached nodes for boolean literals */
    return val ? cached_true_node : cached_false_node;
}

ASTNode *ast_identifier(Parser *parser, const char *name)
{
    ASTNode *node = parser_ast_alloc(parser, AST_IDENTIFIER);
    if (node) {
        /* Use interned string for identifiers (important for repeated name lookups) */
        if (parser && name) {
            node->u.ident.name = (char *)parser_intern(parser, name);
        } else {
            node->u.ident.name = name ? strdup(name) : NULL;
        }
    }
    return node;
}

ASTNode *ast_binary(ASTNode *left, ASTNode *right, const char *op)
{
    ASTNode *node = ast_alloc(AST_BINARY);
    if (node) {
        node->u.binary.left = left;
        node->u.binary.right = right;
        strncpy(node->u.binary.op, op, sizeof(node->u.binary.op) - 1);
    }
    return node;
}

ASTNode *ast_unary(ASTNode *arg, const char *op, int prefix)
{
    ASTNode *node = ast_alloc(AST_UNARY);
    if (node) {
        node->u.unary.arg = arg;
        strncpy(node->u.unary.op, op, sizeof(node->u.unary.op) - 1);
        node->u.unary.prefix = prefix;
    }
    return node;
}

/* ── Forward Declarations of Parser Functions ──────────────────────────── */

static ASTNode *parse_expr(Parser *parser, int min_prec);
static ASTNode *parse_primary_expr(Parser *parser);
static ASTNode *parse_postfix_expr(Parser *parser, ASTNode *left);
static ASTNode *parse_unary_expr(Parser *parser);
static ASTNode *parse_block(Parser *parser);
static ASTNode *parse_var_declaration(Parser *parser, TokenType decl_type, int allow_no_init);
static ASTNode *parse_function(Parser *parser, int is_async, int is_generator, int is_decl);
static ASTNode **parse_params(Parser *parser);
static ASTNode *parse_array_literal(Parser *parser);
static ASTNode *parse_object_literal(Parser *parser);
static ASTNode *parse_pattern(Parser *parser);
static ASTNode *parse_import_decl(Parser *parser);
static ASTNode *parse_export_decl(Parser *parser);
static ASTNode *parse_template_literal(Parser *parser, ASTNode *tag);
static int parse_template_body(Parser *parser, Token first,
                               char ***out_parts, int *out_nparts,
                               ASTNode ***out_exprs, int *out_nexp);
static ASTNode *parse_class_decl(Parser *parser, int is_decl);

/* ── Expression Parsing ───────────────────────────────────────────────── */

/* Arrow function body: `{ ... }` is a block, otherwise an expression. */
static ASTNode *parse_arrow_body(Parser *parser)
{
    if (peek_token(parser, TOK_LBRACE)) {
        return parse_block(parser);
    }
    return parse_assignment_expr(parser);
}

/* Check if a token can start an expression */
static int is_expr_start(Parser *parser)
{
    Token t = lexer_peek(parser->lexer);
    switch (t.type) {
    case TOK_NUMBER: case TOK_STRING: case TOK_IDENTIFIER:
    case TOK_BOOL_LIT: case TOK_NULL_LIT: case TOK_UNDEFINED_LIT:
    case TOK_THIS: case TOK_SUPER:
    case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE:
    case TOK_FUNCTION: case TOK_CLASS:
    case TOK_NEW: case TOK_TYPEOF: case TOK_DELETE: case TOK_VOID:
    case TOK_PLUS: case TOK_MINUS: case TOK_NOT: case TOK_BIT_NOT:
    case TOK_INC: case TOK_DEC:
    case TOK_AWAIT: case TOK_YIELD:
    case TOK_ASYNC:
    case TOK_TEMPLATE: case TOK_TEMPLATE_END:
    case TOK_ELLIPSIS:
    case TOK_IMPORT:
    case TOK_REGEXP:
    case TOK_PRIVATE_NAME:
    case TOK_GET: case TOK_SET: case TOK_STATIC: case TOK_OF:
    case TOK_FROM: case TOK_AS:
        return 1;
    default:
        return 0;
    }
}

/* Parse primary expression (literals, identifiers, etc.) */
static ASTNode *parse_primary_expr(Parser *parser)
{
    Token t = lexer_next(parser->lexer);

    switch (t.type) {
    case TOK_NUMBER: {
        ASTNode *n = ast_literal_number(parser,t.num_val);
        if (n) n->token = t;
        return n;
    }
    case TOK_STRING: {
        ASTNode *n = ast_literal_string(parser,t.str_val ? t.str_val : "");
        if (n) {
            n->token = t;
            token_free_data(&t);
            n->token.str_val = NULL; /* already freed above */
        }
        return n;
    }
    case TOK_REGEXP: {
        /* Regex literal /pattern/flags -> new RegExp("pattern", "flags") */
        const char *src = t.start;
        size_t len = t.len;
        char *pattern = NULL, *flags = NULL;
        if (src && len >= 2 && src[0] == '/') {
            /* Find the closing '/' scanning from the end (flags are idents) */
            size_t close = len - 1;
            while (close > 0 && src[close] != '/') close--;
            if (close > 0) {
                pattern = (char *)p_malloc(close);
                if (pattern) {
                    memcpy(pattern, src + 1, close - 1);
                    pattern[close - 1] = '\0';
                }
                size_t flen = len - close - 1;
                flags = (char *)p_malloc(flen + 1);
                if (flags) {
                    memcpy(flags, src + close + 1, flen);
                    flags[flen] = '\0';
                }
            }
        }
        if (!pattern) {
            parser_error_token(parser, t, "invalid regular expression literal");
            return NULL;
        }
        ASTNode *n = ast_alloc(AST_NEW);
        if (n) {
            n->token = t;
            n->u.new_expr.callee = ast_identifier(parser, "RegExp");
            n->u.new_expr.args = (ASTNode **)p_malloc(2 * sizeof(ASTNode *));
            if (n->u.new_expr.args) {
                n->u.new_expr.args[0] = ast_literal_string(parser, pattern);
                n->u.new_expr.args[1] = ast_literal_string(parser, flags ? flags : "");
                n->u.new_expr.argc = 2;
            } else {
                n->u.new_expr.argc = 0;
            }
        }
        p_free(pattern);
        if (flags) p_free(flags);
        return n;
    }
    case TOK_PRIVATE_NAME: {
        /* Ergonomic brand check: `#field in obj`. Private fields are stored
         * as regular properties named "#field", so treat the private name
         * as a string literal for the `in` operator. */
        char buf[128];
        size_t len = t.len < sizeof(buf) - 1 ? t.len : sizeof(buf) - 1;
        if (t.start) memcpy(buf, t.start, len); else len = 0;
        buf[len] = '\0';
        ASTNode *n = ast_literal_string(parser, buf);
        if (n) {
            /* Keep position info but preserve TOK_STRING so the literal
             * evaluates as a string (not an unknown token -> null). */
            n->token.line = t.line;
            n->token.col = t.col;
        }
        return n;
    }
    case TOK_BOOL_LIT: {
        ASTNode *n = ast_literal_bool(parser,(int)t.num_val);
        if (n) n->token = t;
        return n;
    }
    case TOK_NULL_LIT: {
        ASTNode *n = ast_alloc(AST_LITERAL);
        if (n) {
            n->token = t;
            n->u.number.num = 0; /* mark as null via token type */
        }
        return n;
    }
    case TOK_UNDEFINED_LIT: {
        ASTNode *n = ast_alloc(AST_LITERAL);
        if (n) {
            n->token = t;
            n->u.number.num = -1; /* mark as undefined */
        }
        return n;
    }
    case TOK_THIS: {
        ASTNode *n = ast_alloc(AST_THIS);
        if (n) n->token = t;
        return n;
    }
    case TOK_SUPER: {
        ASTNode *n = ast_alloc(AST_SUPER);
        if (n) n->token = t;
        return n;
    }
    case TOK_IDENTIFIER: {
        /* Extract the identifier name */
        char *name = NULL;
        if (t.start && t.len > 0) {
            name = (char *)p_malloc(t.len + 1);
            if (name) {
                memcpy(name, t.start, t.len);
                name[t.len] = '\0';
            }
        }
        ASTNode *n = ast_identifier(parser,name ? name : "");
        if (n) n->token = t;
        if (name) p_free(name);

        /* Check for template literal after identifier (tagged template) */
        if (peek_token(parser, TOK_TEMPLATE) || peek_token(parser, TOK_TEMPLATE_END)) {
            return parse_template_literal(parser, n);
        }
        return n;
    }
    case TOK_TEMPLATE:
    case TOK_TEMPLATE_END: {
        ASTNode *n = ast_alloc(AST_TEMPLATE);
        if (n) {
            n->token = t;
            char **parts = NULL;
            int nparts = 0;
            ASTNode **exprs = NULL;
            int nexp = 0;
            parse_template_body(parser, t, &parts, &nparts, &exprs, &nexp);
            n->u.template_lit.parts = parts;
            n->u.template_lit.nparts = nparts;
            n->u.template_lit.tag = NULL;
            n->u.template_lit.exprs = exprs;
            n->u.template_lit.nexp = nexp;
            /* The first fragment (t.str_val) is now owned by parts[0]; clear the
             * token copy so ast_free_ex's generic token_free_data is a no-op. */
            n->token.str_val = NULL;
        }
        return n;
    }
    case TOK_LPAREN: {
        /* Parenthesized expression or arrow function params */
        /* Try to parse as expression first */
        /* We need to handle the case where this is (x) vs (x) => ... */
        /* For arrow functions, we need to look ahead for => */

        /* Handle empty params: () => ... */
        if (peek_token(parser, TOK_RPAREN)) {
            match_token(parser, TOK_RPAREN); /* consume ) */
            if (peek_token(parser, TOK_ARROW)) {
                lexer_skip(parser->lexer); /* consume => */
                ASTNode *body = parse_arrow_body(parser);
                ASTNode *arrow = ast_alloc(AST_ARROW);
                if (arrow) {
                    arrow->u.arrow.params = NULL;
                    arrow->u.arrow.nparams = 0;
                    arrow->u.arrow.body = body;
                    arrow->u.arrow.is_async = 0;
                }
                return arrow;
            }
            /* Not an arrow function, return a syntax error for empty parens */
            parser_error(parser, "unexpected token ')' in expression");
            return NULL;
        }

        ASTNode *expr = parse_expression(parser);
        if (!match_token(parser, TOK_RPAREN)) {
            parser_error(parser, "expected ')' after expression");
            if (expr) ast_free_ex(expr, parser);
            return NULL;
        }

        /* Check for arrow function (expr) => ... */
        if (peek_token(parser, TOK_ARROW)) {
            /* This is an arrow function with a single parenthesized expression parameter */
            /* But we already parsed the expression. We need to backtrack or handle differently */
            /* For simplicity, we'll re-parse this as arrow function params */
            /* Actually, for a single-param arrow like (x) => x+1, we can just use the identifier */
            if (expr->type == AST_IDENTIFIER) {
                lexer_skip(parser->lexer); /* consume => */
                ASTNode *body = parse_arrow_body(parser);
                ASTNode *arrow = ast_alloc(AST_ARROW);
                if (arrow) {
                    arrow->u.arrow.params = (ASTNode **)p_malloc(sizeof(ASTNode *));
                    arrow->u.arrow.params[0] = expr;
                    arrow->u.arrow.nparams = 1;
                    arrow->u.arrow.body = body;
                    arrow->u.arrow.is_async = 0;
                }
                return arrow;
            }
            /* Multi-param: (a, b) => ... */
            /* We need to reconstruct from the sequence expression */
            lexer_skip(parser->lexer); /* consume => */
            ASTNode *body = parse_arrow_body(parser);

            ASTNode *arrow = ast_alloc(AST_ARROW);
            if (arrow) {
                /* Count the params from the sequence */
                ASTNode *seq = expr;
                int nparams = 0;
                if (seq->type == AST_SEQUENCE) {
                    nparams = seq->u.sequence.count;
                    arrow->u.arrow.params = (ASTNode **)p_malloc(nparams * sizeof(ASTNode *));
                    for (int i = 0; i < nparams; i++) {
                        arrow->u.arrow.params[i] = seq->u.sequence.exprs[i];
                    }
                    seq->u.sequence.exprs = NULL;
                    seq->u.sequence.count = 0;
                    ast_free_ex(seq, parser);
                }
                arrow->u.arrow.nparams = nparams;
                arrow->u.arrow.body = body;
                arrow->u.arrow.is_async = 0;
            }
            return arrow;
        }

        return expr;
    }
    case TOK_LBRACKET: {
        return parse_array_literal(parser);
    }
    case TOK_LBRACE: {
        /* Could be object literal or block statement */
        /* In expression context, it's always an object literal */
        return parse_object_literal(parser);
    }
    case TOK_FUNCTION: {
        ASTNode *n = parse_function(parser, 0, 0, 0);
        if (n) n->token = t;
        return n;
    }
    case TOK_CLASS: {
        return parse_class_decl(parser, 0);
    }
    case TOK_NEW: {
        /* new expression
         * The callee must be a MemberExpression (primary expr + member access only),
         * NOT a CallExpression. So we parse primary expr, then handle . and []
         * member access manually, but NOT function calls. */
        ASTNode *callee = parse_primary_expr(parser);
        /* Handle member access (. and []) but not function calls */
        while (!parser->has_error) {
            Token t = lexer_peek(parser->lexer);
            if (t.type == TOK_DOT) {
                lexer_skip(parser->lexer);
                Token prop = lexer_next(parser->lexer);
                if (!is_prop_name_token(prop.type)) {
                    parser_error_token(parser, prop, "expected property name after '.'");
                    return callee;
                }
                char *prop_name = NULL;
                if (prop.start && prop.len > 0) {
                    prop_name = (char *)p_malloc(prop.len + 1);
                    if (prop_name) {
                        memcpy(prop_name, prop.start, prop.len);
                        prop_name[prop.len] = '\0';
                    }
                }
                ASTNode *n = ast_alloc(AST_MEMBER);
                if (n) {
                    n->token = t;
                    n->u.member.obj = callee;
                    n->u.member.prop = ast_identifier(parser, prop_name ? prop_name : "");
                    n->u.member.is_optional = 0;
                }
                if (prop_name) p_free(prop_name);
                callee = n;
            } else if (t.type == TOK_LBRACKET) {
                lexer_skip(parser->lexer);
                ASTNode *prop = parse_assignment_expr(parser);
                ASTNode *n = ast_alloc(AST_COMPUTED_MEMBER);
                if (n) {
                    n->token = t;
                    n->u.computed.obj = callee;
                    n->u.computed.prop = prop;
                }
                callee = n;
                expect_token(parser, TOK_RBRACKET);
            } else {
                break;
            }
        }
        ASTNode *n = ast_alloc(AST_NEW);
        if (n) {
            n->token = t;
            n->u.new_expr.callee = callee;
            n->u.new_expr.args = NULL;
            n->u.new_expr.argc = 0;

            if (peek_token(parser, TOK_LPAREN)) {
                /* new Foo(args) */
                lexer_skip(parser->lexer); /* skip ( */
                {
                    ASTNode **args = NULL;
                    int argc = 0;
                    int cap = 0;
                    if (!peek_token(parser, TOK_RPAREN)) {
                        while (1) {
                            ASTNode *arg = parse_assignment_expr(parser);
                            if (argc >= cap) {
                                cap = cap ? cap * 2 : 4;
                                args = (ASTNode **)p_realloc(args, cap * sizeof(ASTNode *));
                            }
                            args[argc++] = arg;
                            if (!match_token(parser, TOK_COMMA)) break;
                        }
                    }
                    expect_token(parser, TOK_RPAREN);
                    n->u.new_expr.args = args;
                    n->u.new_expr.argc = argc;
                }
            }
        }
        return n;
    }
    case TOK_TYPEOF: {
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "typeof", 1);
    }
    case TOK_DELETE: {
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "delete", 1);
    }
    case TOK_VOID: {
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "void", 1);
    }
    case TOK_PLUS: {
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "+", 1);
    }
    case TOK_MINUS: {
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "-", 1);
    }
    case TOK_NOT: {
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "!", 1);
    }
    case TOK_BIT_NOT: {
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "~", 1);
    }
    case TOK_INC: {
        /* prefix ++ */
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "++", 1);
    }
    case TOK_DEC: {
        /* prefix -- */
        ASTNode *arg = parse_unary_expr(parser);
        return ast_unary(arg, "--", 1);
    }
    case TOK_AWAIT: {
        ASTNode *arg = parse_unary_expr(parser);
        ASTNode *n = ast_alloc(AST_AWAIT);
        if (n) {
            n->token = t;
            n->u.await_expr.arg = arg;
        }
        return n;
    }
    case TOK_YIELD: {
        ASTNode *n = ast_alloc(AST_YIELD);
        if (n) {
            n->token = t;
            n->u.yield_expr.is_delegate = 0;
            n->u.yield_expr.arg = NULL;
            if (peek_token(parser, TOK_MUL)) {
                n->u.yield_expr.is_delegate = 1;
                lexer_skip(parser->lexer);
            }
            if (is_expr_start(parser)) {
                n->u.yield_expr.arg = parse_assignment_expr(parser);
            }
        }
        return n;
    }
    case TOK_ASYNC: {
        /* Could be async function or async () => ... */
        /* Look ahead to determine */
        /* async function, async () =>, async x => */
        if (peek_token(parser, TOK_FUNCTION)) {
            lexer_skip(parser->lexer); /* consume function */
            return parse_function(parser, 1, 0, 0);
        }
        /* async arrow function: async (params) => body  |  async x => body */
        {
            Token nt = lexer_peek(parser->lexer);
            if (nt.type == TOK_LPAREN) {
                size_t save_pos = (size_t)(nt.start - parser->lexer->src);
                ASTNode *sub = parse_primary_expr(parser);
                if (sub && sub->type == AST_ARROW && !parser->has_error) {
                    sub->u.arrow.is_async = 1;
                    return sub;
                }
                /* Not an arrow: it was a call like async(...). Rewind and
                 * treat 'async' as a plain identifier. */
                if (sub) ast_free_ex(sub, parser);
                parser->has_error = 0;
                parser->lexer->pos = save_pos;
                lexer_reset_peek(parser->lexer);
            } else if (nt.type == TOK_IDENTIFIER) {
                size_t save_pos = (size_t)(nt.start - parser->lexer->src);
                Token name_tok = lexer_next(parser->lexer);
                if (peek_token(parser, TOK_ARROW)) {
                    lexer_skip(parser->lexer); /* consume => */
                    ASTNode *param = ast_alloc(AST_IDENTIFIER);
                    if (param) {
                        param->u.ident.name = (char *)p_malloc(name_tok.len + 1);
                        if (param->u.ident.name) {
                            memcpy(param->u.ident.name, name_tok.start, name_tok.len);
                            param->u.ident.name[name_tok.len] = '\0';
                        }
                    }
                    ASTNode *body = parse_arrow_body(parser);
                    ASTNode *arrow = ast_alloc(AST_ARROW);
                    if (arrow) {
                        arrow->u.arrow.params = (ASTNode **)p_malloc(sizeof(ASTNode *));
                        if (arrow->u.arrow.params) arrow->u.arrow.params[0] = param;
                        arrow->u.arrow.nparams = 1;
                        arrow->u.arrow.body = body;
                        arrow->u.arrow.is_async = 1;
                    }
                    return arrow;
                }
                /* Not an arrow; rewind to before the identifier */
                parser->lexer->pos = save_pos;
                lexer_reset_peek(parser->lexer);
            }
        }
        /* Fall back: treat async as identifier */
        ASTNode *n = ast_identifier(parser,"async");
        if (n) n->token = t;
        return n;
    }
    case TOK_IMPORT: {
        /* import() dynamic import or import.meta */
        if (peek_token(parser, TOK_DOT)) {
            /* import.meta */
            lexer_skip(parser->lexer); /* skip . */
            expect_token(parser, TOK_IDENTIFIER); /* 'meta' */
            /* Create member expression: import.meta */
            ASTNode *import_node = ast_identifier(parser,"import");
            ASTNode *meta_node = ast_identifier(parser,"meta");
            ASTNode *n = ast_alloc(AST_MEMBER);
            if (n) {
                n->token = t;
                n->u.member.obj = import_node;
                n->u.member.prop = meta_node;
                n->u.member.is_optional = 0;
            }
            return n;
        }
        if (peek_token(parser, TOK_LPAREN)) {
            /* import() dynamic import */
            lexer_skip(parser->lexer); /* skip ( */
            ASTNode *arg = parse_assignment_expr(parser);
            expect_token(parser, TOK_RPAREN);
            ASTNode *n = ast_alloc(AST_CALL);
            if (n) {
                n->token = t;
                n->u.call.callee = ast_identifier(parser,"import");
                n->u.call.args = (ASTNode **)p_malloc(sizeof(ASTNode *));
                n->u.call.args[0] = arg;
                n->u.call.argc = 1;
                n->u.call.is_optional = 0;
            }
            return n;
        }
        /* Fall through to identifier */
        ASTNode *n = ast_identifier(parser,"import");
        if (n) n->token = t;
        return n;
    }
    case TOK_ELLIPSIS: {
        ASTNode *n = ast_alloc(AST_SPREAD_ELEMENT);
        if (n) {
            n->token = t;
            n->u.spread.arg = parse_assignment_expr(parser);
        }
        return n;
    }
    default:
        parser_error_token(parser, t, "unexpected token '%s' in expression",
            token_type_name(t.type));
        return NULL;
    }
}

/* Parse unary expression */
static ASTNode *parse_unary_expr(Parser *parser)
{
    ASTNode *left = parse_primary_expr(parser);
    /* Handle postfix operations (member access, calls, etc.) to ensure
     * correct precedence: typeof a.b should be typeof (a.b), not (typeof a).b */
    return parse_postfix_expr(parser, left);
}

/* Parse postfix expression (member access, calls, etc.) */
static ASTNode *parse_postfix_expr(Parser *parser, ASTNode *left)
{
    while (!parser->has_error) {
        Token t = lexer_peek(parser->lexer);

        switch (t.type) {
        case TOK_DOT: {
            lexer_skip(parser->lexer); /* skip . */
            Token prop = lexer_next(parser->lexer);
            if (!is_prop_name_token(prop.type)) {
                parser_error_token(parser, prop, "expected property name after '.'");
                return left;
            }
            char *prop_name = NULL;
            if (prop.start && prop.len > 0) {
                prop_name = (char *)p_malloc(prop.len + 1);
                if (prop_name) {
                    memcpy(prop_name, prop.start, prop.len);
                    prop_name[prop.len] = '\0';
                }
            }
            ASTNode *n = ast_alloc(AST_MEMBER);
            if (n) {
                n->token = t;
                n->u.member.obj = left;
                n->u.member.prop = ast_identifier(parser,prop_name ? prop_name : "");
                n->u.member.is_optional = 0;
            }
            if (prop_name) p_free(prop_name);
            left = n;
            break;
        }
        case TOK_QUESTION_DOT: {
            /* Optional chaining: ?. */
            lexer_skip(parser->lexer); /* skip ?. */
            Token prop = lexer_next(parser->lexer);

            if (prop.type == TOK_LPAREN) {
                /* ?.(args) - optional call */
                ASTNode **args = NULL;
                int argc = 0, cap = 0;
                if (!peek_token(parser, TOK_RPAREN)) {
                    while (1) {
                        ASTNode *arg = parse_assignment_expr(parser);
                        if (argc >= cap) {
                            cap = cap ? cap * 2 : 4;
                            args = (ASTNode **)p_realloc(args, cap * sizeof(ASTNode *));
                        }
                        args[argc++] = arg;
                        if (!match_token(parser, TOK_COMMA)) break;
                    }
                }
                expect_token(parser, TOK_RPAREN);
                ASTNode *n = ast_alloc(AST_OPTIONAL_CALL);
                if (n) {
                    n->token = t;
                    n->u.call.callee = left;
                    n->u.call.args = args;
                    n->u.call.argc = argc;
                    n->u.call.is_optional = 1;
                }
                left = n;
            } else if (prop.type == TOK_TEMPLATE || prop.type == TOK_TEMPLATE_END) {
                /* ?.`template` - tagged template with optional chaining */
                ASTNode *n = parse_template_literal(parser, left);
                if (n) {
                    /* The template literal already has the tag */
                    /* We need to wrap it as optional member */
                }
                left = n;
            } else {
                /* ?.property */
                char *prop_name = NULL;
                if (prop.start && prop.len > 0) {
                    prop_name = (char *)p_malloc(prop.len + 1);
                    if (prop_name) {
                        memcpy(prop_name, prop.start, prop.len);
                        prop_name[prop.len] = '\0';
                    }
                }
                ASTNode *n = ast_alloc(AST_OPTIONAL_MEMBER);
                if (n) {
                    n->token = t;
                    n->u.member.obj = left;
                    n->u.member.prop = ast_identifier(parser,prop_name ? prop_name : "");
                    n->u.member.is_optional = 1;
                }
                if (prop_name) p_free(prop_name);
                left = n;
            }
            break;
        }
        case TOK_LBRACKET: {
            /* Computed member access: expr[expr] */
            lexer_skip(parser->lexer); /* skip [ */
            ASTNode *prop = parse_expression(parser);
            expect_token(parser, TOK_RBRACKET);
            ASTNode *n = ast_alloc(AST_COMPUTED_MEMBER);
            if (n) {
                n->token = t;
                n->u.member.obj = left;
                n->u.member.prop = prop;
                n->u.member.is_optional = 0;
            }
            left = n;
            break;
        }
        case TOK_LPAREN: {
            /* Function call: expr(args) */
            lexer_skip(parser->lexer); /* skip ( */
            ASTNode **args = NULL;
            int argc = 0, cap = 0;
            if (!peek_token(parser, TOK_RPAREN)) {
                while (1) {
                    ASTNode *arg = parse_assignment_expr(parser);
                    if (argc >= cap) {
                        cap = cap ? cap * 2 : 4;
                        args = (ASTNode **)p_realloc(args, cap * sizeof(ASTNode *));
                    }
                    args[argc++] = arg;
                    if (!match_token(parser, TOK_COMMA)) break;
                }
            }
            expect_token(parser, TOK_RPAREN);
            ASTNode *n = ast_alloc(AST_CALL);
            if (n) {
                n->token = t;
                n->u.call.callee = left;
                n->u.call.args = args;
                n->u.call.argc = argc;
                n->u.call.is_optional = 0;
            }
            left = n;
            break;
        }
        case TOK_INC: {
            /* Postfix ++ */
            lexer_skip(parser->lexer);
            left = ast_unary(left, "++", 0);
            break;
        }
        case TOK_DEC: {
            /* Postfix -- */
            lexer_skip(parser->lexer);
            left = ast_unary(left, "--", 0);
            break;
        }
        case TOK_TEMPLATE:
        case TOK_TEMPLATE_END: {
            /* Tagged template literal */
            left = parse_template_literal(parser, left);
            break;
        }
        default:
            return left;
        }
    }
    return left;
}

/* Parse expression with precedence climbing */
static ASTNode *parse_expr(Parser *parser, int min_prec)
{
    if (parser->has_error) return NULL;

    ASTNode *left = parse_postfix_expr(parser, parse_unary_expr(parser));
    if (parser->has_error) return left;

    while (!parser->has_error) {
        Token t = lexer_peek(parser->lexer);

        /* Check for arrow function BEFORE precedence check, since TOK_ARROW
         * has no binary precedence but must be handled for single-param arrows
         * like: x => x * 2 */
        if (t.type == TOK_ARROW) {
            if (left->type == AST_IDENTIFIER) {
                lexer_skip(parser->lexer); /* consume => */
                ASTNode *body = parse_arrow_body(parser);
                ASTNode *arrow = ast_alloc(AST_ARROW);
                if (arrow) {
                    arrow->u.arrow.params = (ASTNode **)p_malloc(sizeof(ASTNode *));
                    arrow->u.arrow.params[0] = left;
                    arrow->u.arrow.nparams = 1;
                    arrow->u.arrow.body = body;
                    arrow->u.arrow.is_async = 0;
                }
                return arrow;
            }
            break;
        }

        int prec = get_precedence(t.type, 1);

        if (prec < min_prec) break;

        /* Handle right-associative operators */
        int next_min = prec;
        if (t.type == TOK_POW || is_assign_op(t.type)) {
            next_min = prec; /* right-associative */
        } else {
            next_min = prec + 1;
        }

        if (t.type == TOK_QUESTION) {
            /* Ternary operator: cond ? consequent : alternate */
            lexer_skip(parser->lexer); /* skip ? */
            ASTNode *consequent = parse_assignment_expr(parser);
            expect_token(parser, TOK_COLON);
            ASTNode *alternate = parse_assignment_expr(parser);
            ASTNode *n = ast_alloc(AST_CONDITIONAL);
            if (n) {
                n->token = t;
                n->u.conditional.cond = left;
                n->u.conditional.consequent = consequent;
                n->u.conditional.alternate = alternate;
            }
            left = n;
            continue;
        }

        if (is_assign_op(t.type)) {
            /* Assignment expression */
            lexer_skip(parser->lexer);
            const char *op = get_op_str(t.type);
            ASTNode *right = parse_expr(parser, next_min);
            ASTNode *n = ast_alloc(AST_ASSIGN);
            if (n) {
                n->token = t;
                n->u.assign.target = left;
                n->u.assign.value = right;
                strncpy(n->u.assign.op, op, sizeof(n->u.assign.op) - 1);
            }
            left = n;
            continue;
        }

        /* Binary operator */
        lexer_skip(parser->lexer);
        const char *op = get_op_str(t.type);
        ASTNode *right = parse_expr(parser, next_min);
        left = ast_binary(left, right, op);
        if (left) left->token = t;
    }

    return left;
}

/* Parse assignment expression (entry point for expression parsing) */
/* Does NOT include comma operator - comma is handled by parse_expression */
ASTNode *parse_assignment_expr(Parser *parser)
{
    return parse_expr(parser, 1);
}

/* Parse full expression (including comma operator) */
ASTNode *parse_expression(Parser *parser)
{
    ASTNode *left = parse_assignment_expr(parser);
    if (parser->has_error) return left;

    /* Handle comma operator */
    if (peek_token(parser, TOK_COMMA)) {
        int cap = 4;
        int count = 1;
        ASTNode **exprs = (ASTNode **)p_malloc(cap * sizeof(ASTNode *));
        exprs[0] = left;

        while (match_token(parser, TOK_COMMA)) {
            ASTNode *next = parse_assignment_expr(parser);
            if (count >= cap) {
                cap *= 2;
                exprs = (ASTNode **)p_realloc(exprs, cap * sizeof(ASTNode *));
            }
            exprs[count++] = next;
        }

        ASTNode *n = ast_alloc(AST_SEQUENCE);
        if (n) {
            n->u.sequence.exprs = exprs;
            n->u.sequence.count = count;
        }
        return n;
    }

    return left;
}

/* ── Array Literal ────────────────────────────────────────────────────── */

static ASTNode *parse_array_literal(Parser *parser)
{
    ASTNode *n = ast_alloc(AST_ARRAY);
    if (!n) return NULL;

    n->u.array.elements = NULL;
    n->u.array.nelem = 0;
    int cap = 0;

    while (!parser->has_error && !peek_token(parser, TOK_RBRACKET)) {
        if (peek_token(parser, TOK_COMMA)) {
            /* Elision (hole in array) */
            lexer_skip(parser->lexer);
            if (n->u.array.nelem >= cap) {
                cap = cap ? cap * 2 : 4;
                n->u.array.elements = (ASTNode **)p_realloc(n->u.array.elements,
                    cap * sizeof(ASTNode *));
            }
            n->u.array.elements[n->u.array.nelem++] = NULL;
            continue;
        }

        if (peek_token(parser, TOK_ELLIPSIS)) {
            /* Spread element */
            lexer_skip(parser->lexer); /* skip ... */
            ASTNode *expr = parse_assignment_expr(parser);
            ASTNode *spread = ast_alloc(AST_SPREAD_ELEMENT);
            if (spread) spread->u.spread.arg = expr;

            if (n->u.array.nelem >= cap) {
                cap = cap ? cap * 2 : 4;
                n->u.array.elements = (ASTNode **)p_realloc(n->u.array.elements,
                    cap * sizeof(ASTNode *));
            }
            n->u.array.elements[n->u.array.nelem++] = spread;
        } else {
            if (n->u.array.nelem >= cap) {
                cap = cap ? cap * 2 : 4;
                n->u.array.elements = (ASTNode **)p_realloc(n->u.array.elements,
                    cap * sizeof(ASTNode *));
            }
            n->u.array.elements[n->u.array.nelem++] = parse_assignment_expr(parser);
        }

        if (!match_token(parser, TOK_COMMA)) break;
    }

    expect_token(parser, TOK_RBRACKET);
    return n;
}

/* ── Object Literal ───────────────────────────────────────────────────── */

static ASTNode *parse_object_literal(Parser *parser)
{
    ASTNode *n = ast_alloc(AST_OBJECT);
    if (!n) return NULL;

    n->u.object.props = NULL;
    n->u.object.nprops = 0;
    int cap = 0;

    while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
        if (peek_token(parser, TOK_ELLIPSIS)) {
            /* Spread property */
            lexer_skip(parser->lexer);
            ASTNode *expr = parse_assignment_expr(parser);
            ASTNode *spread = ast_alloc(AST_SPREAD);
            if (spread) spread->u.spread.arg = expr;

            if (n->u.object.nprops >= cap) {
                cap = cap ? cap * 2 : 4;
                n->u.object.props = (ASTNode **)p_realloc(n->u.object.props,
                    cap * sizeof(ASTNode *));
            }
            n->u.object.props[n->u.object.nprops++] = spread;
        } else {
            ASTNode *prop = NULL;

            /* Detect accessor syntax: get name() {} / set name(v) {}
             * ('get'/'set' followed by a property name, not by ':'/'('/','/'}'/'=') */
            int accessor = 0, accessor_is_get = 0;
            if (peek_token(parser, TOK_GET) || peek_token(parser, TOK_SET)) {
                accessor_is_get = peek_token(parser, TOK_GET);
                LexState st = lex_state_save(parser->lexer);
                lexer_skip(parser->lexer);
                if (!(peek_token(parser, TOK_COLON) || peek_token(parser, TOK_LPAREN) ||
                      peek_token(parser, TOK_COMMA) || peek_token(parser, TOK_RBRACE) ||
                      peek_token(parser, TOK_ASSIGN))) {
                    accessor = 1;
                }
                lex_state_restore(parser->lexer, st);
            }

            if (accessor) {
                lexer_skip(parser->lexer); /* consume 'get'/'set' */

                /* Accessor property name */
                char *pname = NULL;
                if (peek_token(parser, TOK_STRING) || peek_token(parser, TOK_NUMBER)) {
                    Token kt = lexer_next(parser->lexer);
                    if (kt.type == TOK_STRING) {
                        if (kt.str_val) pname = strdup(kt.str_val);
                        token_free_data(&kt);
                    } else {
                        char numbuf[40];
                        snprintf(numbuf, sizeof(numbuf), "%.17g", kt.num_val);
                        pname = strdup(numbuf);
                    }
                } else {
                    Token kt = lexer_next(parser->lexer);
                    if (kt.start && kt.len > 0) {
                        pname = (char *)p_malloc(kt.len + 1);
                        if (pname) {
                            memcpy(pname, kt.start, kt.len);
                            pname[kt.len] = '\0';
                        }
                    } else {
                        parser_error_token(parser, kt, "expected accessor property name");
                        break;
                    }
                }

                /* Parameters + body */
                ASTNode **params = parse_params(parser);
                int nparams = 0;
                if (params) {
                    while (params[nparams]) nparams++;
                }
                ASTNode *body = parse_block(parser);

                ASTNode *func = ast_alloc(AST_FUNC_EXPR);
                if (func) {
                    func->u.func.name = pname ? strdup(pname) : NULL;
                    func->u.func.params = params;
                    func->u.func.nparams = nparams;
                    func->u.func.body = body;
                    func->u.func.is_getter = accessor_is_get ? 1 : 0;
                    func->u.func.is_setter = accessor_is_get ? 0 : 1;
                }

                prop = ast_alloc(AST_PROPERTY);
                if (prop) {
                    prop->u.property.key = ast_identifier(parser, pname ? pname : "");
                    prop->u.property.val = func;
                    prop->u.property.computed = 0;
                    prop->u.property.shorthand = 0;
                }
                if (pname) p_free(pname);

                if (prop) {
                    if (n->u.object.nprops >= cap) {
                        cap = cap ? cap * 2 : 4;
                        n->u.object.props = (ASTNode **)p_realloc(n->u.object.props,
                            cap * sizeof(ASTNode *));
                    }
                    n->u.object.props[n->u.object.nprops++] = prop;
                }

                if (!match_token(parser, TOK_COMMA)) break;
                continue;
            }

            /* Shorthand property name or computed property */
            int computed = match_token(parser, TOK_LBRACKET);
            if (computed) {
                ASTNode *key = parse_expression(parser);
                expect_token(parser, TOK_RBRACKET);
                if (peek_token(parser, TOK_LPAREN) || peek_token(parser, TOK_TEMPLATE)) {
                    /* Computed method shorthand: [key]() {} */
                    ASTNode *func = parse_function(parser, 0, 0, 0);
                    prop = ast_alloc(AST_PROPERTY);
                    if (prop) {
                        prop->u.property.key = key;
                        prop->u.property.val = func;
                        prop->u.property.computed = 1;
                        prop->u.property.shorthand = 0;
                    }
                } else {
                    expect_token(parser, TOK_COLON);
                    ASTNode *val = parse_assignment_expr(parser);

                    prop = ast_alloc(AST_PROPERTY);
                    if (prop) {
                        prop->u.property.key = key;
                        prop->u.property.val = val;
                        prop->u.property.computed = 1;
                        prop->u.property.shorthand = 0;
                    }
                }
            } else if (peek_token(parser, TOK_STRING) || peek_token(parser, TOK_NUMBER)) {
                /* Property with string/number key */
                Token key_tok = lexer_next(parser->lexer);
                ASTNode *key = NULL;
                if (key_tok.type == TOK_STRING) {
                    key = ast_literal_string(parser,key_tok.str_val ? key_tok.str_val : "");
                    token_free_data(&key_tok);
                } else {
                    key = ast_literal_number(parser,key_tok.num_val);
                }
                expect_token(parser, TOK_COLON);
                ASTNode *val = parse_assignment_expr(parser);

                prop = ast_alloc(AST_PROPERTY);
                if (prop) {
                    prop->u.property.key = key;
                    prop->u.property.val = val;
                    prop->u.property.computed = 0;
                    prop->u.property.shorthand = 0;
                }
            } else {
                /* Regular property: identifier or shorthand */
                Token id_tok = lexer_next(parser->lexer);
                if (id_tok.type != TOK_IDENTIFIER) {
                    if (id_tok.type == TOK_GET || id_tok.type == TOK_SET ||
                        id_tok.type == TOK_STATIC || id_tok.type == TOK_ASYNC) {
                        /* These can be method names or keywords */
                        /* For simplicity, treat as identifier */
                    } else {
                        parser_error_token(parser, id_tok, "expected property name");
                        if (prop) ast_free_ex(prop, parser);
                        break;
                    }
                }
                char *prop_name = NULL;
                if (id_tok.start && id_tok.len > 0) {
                    prop_name = (char *)p_malloc(id_tok.len + 1);
                    if (prop_name) {
                        memcpy(prop_name, id_tok.start, id_tok.len);
                        prop_name[id_tok.len] = '\0';
                    }
                }

                if (peek_token(parser, TOK_COLON)) {
                    /* Regular property: key: value */
                    lexer_skip(parser->lexer); /* skip : */
                    ASTNode *val = parse_assignment_expr(parser);
                    prop = ast_alloc(AST_PROPERTY);
                    if (prop) {
                        prop->u.property.key = ast_identifier(parser,prop_name ? prop_name : "");
                        prop->u.property.val = val;
                        prop->u.property.computed = 0;
                        prop->u.property.shorthand = 0;
                    }
                } else if (peek_token(parser, TOK_LPAREN) || peek_token(parser, TOK_TEMPLATE)) {
                    /* Method definition: key() {} */
                    /* Re-parse as function expression */
                    if (prop_name) {
                        ASTNode *func = parse_function(parser, 0, 0, 0);
                        if (func && func->type == AST_FUNC_EXPR) {
                            func->u.func.name = strdup(prop_name);
                        }
                        prop = ast_alloc(AST_PROPERTY);
                        if (prop) {
                            prop->u.property.key = ast_identifier(parser,prop_name ? prop_name : "");
                            prop->u.property.val = func;
                            prop->u.property.computed = 0;
                            prop->u.property.shorthand = 0;
                        }
                    }
                } else if (peek_token(parser, TOK_COMMA) || peek_token(parser, TOK_RBRACE) ||
                           peek_token(parser, TOK_ASSIGN)) {
                    /* Shorthand property: { x } or { x = default } */
                    ASTNode *val = ast_identifier(parser,prop_name ? prop_name : "");
                    if (match_token(parser, TOK_ASSIGN)) {
                        ASTNode *default_val = parse_assignment_expr(parser);
                        ASTNode *default_node = ast_alloc(AST_DEFAULT_VALUE);
                        if (default_node) {
                            default_node->u.default_val.left = val;
                            default_node->u.default_val.right = default_val;
                        }
                        val = default_node;
                    }
                    prop = ast_alloc(AST_PROPERTY);
                    if (prop) {
                        prop->u.property.key = ast_identifier(parser,prop_name ? prop_name : "");
                        prop->u.property.val = val;
                        prop->u.property.computed = 0;
                        prop->u.property.shorthand = 1;
                    }
                }
                if (prop_name) p_free(prop_name);
            }

            if (prop) {
                if (n->u.object.nprops >= cap) {
                    cap = cap ? cap * 2 : 4;
                    n->u.object.props = (ASTNode **)p_realloc(n->u.object.props,
                        cap * sizeof(ASTNode *));
                }
                n->u.object.props[n->u.object.nprops++] = prop;
            }
        }

        if (!match_token(parser, TOK_COMMA)) break;
    }

    expect_token(parser, TOK_RBRACE);
    return n;
}

/* ── Template Literal ─────────────────────────────────────────────────── */

/* Collect the parts/exprs of a template literal.
 * `first` is the template token the caller already consumed (TOK_TEMPLATE or
 * TOK_TEMPLATE_END). Its str_val is transferred into the returned parts array
 * and must NOT be freed by the caller. Returns 1 on success. */
static int parse_template_body(Parser *parser, Token first,
                               char ***out_parts, int *out_nparts,
                               ASTNode ***out_exprs, int *out_nexp)
{
    char **parts = (char **)p_malloc(4 * sizeof(char *));
    int nparts = 0, parts_cap = 4;
    ASTNode **exprs = NULL;
    int nexp = 0, exprs_cap = 0;

    parts[nparts++] = first.str_val; /* transfer ownership; never freed here */

    if (first.type == TOK_TEMPLATE) {
        /* A TOK_TEMPLATE segment is always followed by an interpolation. */
        while (!parser->has_error) {
            ASTNode *expr = parse_expression(parser);
            if (!expr) {
                parser_error(parser, "expected template interpolation expression");
                break;
            }
            if (nexp >= exprs_cap) {
                exprs_cap = exprs_cap ? exprs_cap * 2 : 4;
                exprs = (ASTNode **)p_realloc(exprs, exprs_cap * sizeof(ASTNode *));
            }
            exprs[nexp++] = expr;

            if (!match_token(parser, TOK_RBRACE)) {
                if (!parser->has_error)
                    parser_error(parser, "expected '}' after template expression");
                break;
            }

            Token next = lexer_template_next(parser->lexer);
            if (next.type == TOK_TEMPLATE) {
                if (nparts >= parts_cap) {
                    parts_cap *= 2;
                    parts = (char **)p_realloc(parts, parts_cap * sizeof(char *));
                }
                parts[nparts++] = next.str_val; /* transfer ownership */
                /* another interpolation follows: continue */
            } else if (next.type == TOK_TEMPLATE_END) {
                parts[nparts++] = next.str_val; /* transfer ownership */
                break;
            } else if (next.type == TOK_EOF) {
                parser_error(parser, "unterminated template literal");
                break;
            } else {
                parser_error(parser, "expected template literal continuation");
                token_free_data(&next);
                break;
            }
        }
    }

    *out_parts = parts;
    *out_nparts = nparts;
    *out_exprs = exprs;
    *out_nexp = nexp;
    return 1;
}

static ASTNode *parse_template_literal(Parser *parser, ASTNode *tag)
{
    Token t = lexer_next(parser->lexer);
    if (t.type != TOK_TEMPLATE && t.type != TOK_TEMPLATE_END) {
        parser_error_token(parser, t, "expected template literal");
        token_free_data(&t);
        return tag;
    }

    ASTNode *n = ast_alloc(AST_TAGGED_TEMPLATE);
    if (!n) {
        token_free_data(&t);
        return tag;
    }

    n->token = t;
    char **parts = NULL;
    int nparts = 0;
    ASTNode **exprs = NULL;
    int nexp = 0;
    parse_template_body(parser, t, &parts, &nparts, &exprs, &nexp);
    n->u.template_lit.parts = parts;
    n->u.template_lit.nparts = nparts;
    n->u.template_lit.tag = tag;
    n->u.template_lit.exprs = exprs;
    n->u.template_lit.nexp = nexp;
    /* The first fragment (t.str_val) is now owned by parts[0]; clear the token
     * copy so ast_free_ex's generic token_free_data is a no-op. */
    n->token.str_val = NULL;
    return n;
}

/* ── Function Parsing ─────────────────────────────────────────────────── */

static ASTNode *parse_function(Parser *parser, int is_async, int is_generator, int is_decl)
{
    ASTNode *n = ast_alloc(is_decl ? AST_FUNC_DECL : AST_FUNC_EXPR);
    if (!n) return NULL;

    n->u.func.is_async = is_async;
    n->u.func.is_generator = is_generator;
    n->u.func.name = NULL;
    n->u.func.params = NULL;
    n->u.func.nparams = 0;
    n->u.func.body = NULL;

    /* Generator asterisk comes BEFORE the name: function* g() {} */
    if (peek_token(parser, TOK_MUL)) {
        lexer_skip(parser->lexer);
        n->u.func.is_generator = 1;
    }

    /* Parse function name (optional for expressions) */
    if (peek_token(parser, TOK_IDENTIFIER)) {
        Token name_tok = lexer_next(parser->lexer);
        if (name_tok.start && name_tok.len > 0) {
            n->u.func.name = (char *)p_malloc(name_tok.len + 1);
            if (n->u.func.name) {
                memcpy(n->u.func.name, name_tok.start, name_tok.len);
                n->u.func.name[name_tok.len] = '\0';
            }
        }
    } else if (is_decl) {
        parser_error(parser, "function declaration requires a name");
    }

    /* Parse parameters */
    n->u.func.params = parse_params(parser);
    n->u.func.nparams = 0;
    if (n->u.func.params) {
        /* Count params */
        while (n->u.func.params[n->u.func.nparams]) n->u.func.nparams++;
    }

    /* Parse body */
    n->u.func.body = parse_block(parser);

    return n;
}

static ASTNode **parse_params(Parser *parser)
{
    ASTNode **params = NULL;
    int count = 0, cap = 0;

    expect_token(parser, TOK_LPAREN);

    if (peek_token(parser, TOK_RPAREN)) {
        lexer_skip(parser->lexer); /* skip ) */
        params = (ASTNode **)p_calloc(1, sizeof(ASTNode *));
        return params;
    }

    while (!parser->has_error) {
        ASTNode *param = NULL;

        if (peek_token(parser, TOK_ELLIPSIS)) {
            /* Rest parameter: ...name */
            lexer_skip(parser->lexer);
            Token name_tok = lexer_next(parser->lexer);
            if (name_tok.type != TOK_IDENTIFIER) {
                parser_error_token(parser, name_tok, "expected parameter name after '...'");
            }
            char *name = NULL;
            if (name_tok.start && name_tok.len > 0) {
                name = (char *)p_malloc(name_tok.len + 1);
                if (name) { memcpy(name, name_tok.start, name_tok.len); name[name_tok.len] = '\0'; }
            }
            ASTNode *rest = ast_alloc(AST_REST);
            if (rest) rest->u.rest_elem.arg = ast_identifier(parser,name ? name : "");
            if (name) p_free(name);
            param = rest;
        } else if (peek_token(parser, TOK_LBRACKET) || peek_token(parser, TOK_LBRACE)) {
            /* Destructuring pattern as parameter */
            param = parse_pattern(parser);

            /* Default value */
            if (match_token(parser, TOK_ASSIGN)) {
                ASTNode *default_val = parse_assignment_expr(parser);
                ASTNode *dv = ast_alloc(AST_DEFAULT_VALUE);
                if (dv) {
                    dv->u.default_val.left = param;
                    dv->u.default_val.right = default_val;
                }
                param = dv;
            }
        } else {
            /* Regular parameter */
            Token name_tok = lexer_next(parser->lexer);
            if (name_tok.type != TOK_IDENTIFIER) {
                parser_error_token(parser, name_tok, "expected parameter name");
            }
            char *name = NULL;
            if (name_tok.start && name_tok.len > 0) {
                name = (char *)p_malloc(name_tok.len + 1);
                if (name) { memcpy(name, name_tok.start, name_tok.len); name[name_tok.len] = '\0'; }
            }
            param = ast_identifier(parser,name ? name : "");
            if (name) p_free(name);

            /* Default value */
            if (match_token(parser, TOK_ASSIGN)) {
                ASTNode *default_val = parse_assignment_expr(parser);
                ASTNode *dv = ast_alloc(AST_DEFAULT_VALUE);
                if (dv) {
                    dv->u.default_val.left = param;
                    dv->u.default_val.right = default_val;
                }
                param = dv;
            }
        }

        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            params = (ASTNode **)p_realloc(params, cap * sizeof(ASTNode *));
        }
        params[count++] = param;

        if (!match_token(parser, TOK_COMMA)) break;
    }

    expect_token(parser, TOK_RPAREN);

    /* Null-terminate */
    params = (ASTNode **)p_realloc(params, (count + 1) * sizeof(ASTNode *));
    params[count] = NULL;

    return params;
}

/* ── Pattern Parsing (Destructuring) ──────────────────────────────────── */

static ASTNode *parse_pattern(Parser *parser)
{
    Token t = lexer_peek(parser->lexer);

    if (t.type == TOK_LBRACKET) {
        /* Array destructuring pattern */
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_PATTERN);
        if (!n) return NULL;

        n->u.pattern_array.elements = NULL;
        n->u.pattern_array.nelem = 0;
        n->u.pattern_array.is_object = 0;
        int cap = 0;

        while (!parser->has_error && !peek_token(parser, TOK_RBRACKET)) {
            if (peek_token(parser, TOK_COMMA)) {
                /* Hole */
                lexer_skip(parser->lexer);
                if (n->u.pattern_array.nelem >= cap) {
                    cap = cap ? cap * 2 : 4;
                    n->u.pattern_array.elements = (ASTNode **)p_realloc(
                        n->u.pattern_array.elements, cap * sizeof(ASTNode *));
                }
                n->u.pattern_array.elements[n->u.pattern_array.nelem++] = NULL;
                continue;
            }

            ASTNode *elem = NULL;
            if (peek_token(parser, TOK_ELLIPSIS)) {
                lexer_skip(parser->lexer);
                ASTNode *rest_target = parse_pattern(parser);
                elem = ast_alloc(AST_REST);
                if (elem) elem->u.rest_elem.arg = rest_target;
            } else if (peek_token(parser, TOK_LBRACKET) || peek_token(parser, TOK_LBRACE)) {
                elem = parse_pattern(parser);
            } else {
                Token id_tok = lexer_next(parser->lexer);
                if (id_tok.type != TOK_IDENTIFIER) {
                    parser_error_token(parser, id_tok, "expected identifier in destructuring pattern");
                }
                char *name = NULL;
                if (id_tok.start && id_tok.len > 0) {
                    name = (char *)p_malloc(id_tok.len + 1);
                    if (name) { memcpy(name, id_tok.start, id_tok.len); name[id_tok.len] = '\0'; }
                }
                elem = ast_identifier(parser,name ? name : "");
                if (name) p_free(name);
            }

            /* Default value */
            if (elem && match_token(parser, TOK_ASSIGN)) {
                ASTNode *default_val = parse_assignment_expr(parser);
                ASTNode *dv = ast_alloc(AST_DEFAULT_VALUE);
                if (dv) {
                    dv->u.default_val.left = elem;
                    dv->u.default_val.right = default_val;
                }
                elem = dv;
            }

            if (n->u.pattern_array.nelem >= cap) {
                cap = cap ? cap * 2 : 4;
                n->u.pattern_array.elements = (ASTNode **)p_realloc(
                    n->u.pattern_array.elements, cap * sizeof(ASTNode *));
            }
            n->u.pattern_array.elements[n->u.pattern_array.nelem++] = elem;

            if (!match_token(parser, TOK_COMMA)) break;
        }

        expect_token(parser, TOK_RBRACKET);
        return n;
    }

    if (t.type == TOK_LBRACE) {
        /* Object destructuring pattern */
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_PATTERN);
        /* We'll reuse the pattern type but store object destructuring info */
        if (!n) return NULL;

        n->u.pattern_object.props = NULL;
        n->u.pattern_object.nprops = 0;
        n->u.pattern_object.is_object = 1;
        int cap = 0;

        while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
            ASTNode *prop = NULL;

            if (peek_token(parser, TOK_ELLIPSIS)) {
                /* Rest in object pattern */
                lexer_skip(parser->lexer);
                ASTNode *rest_target = NULL;
                if (peek_token(parser, TOK_IDENTIFIER)) {
                    Token id_tok = lexer_next(parser->lexer);
                    char *name = NULL;
                    if (id_tok.start && id_tok.len > 0) {
                        name = (char *)p_malloc(id_tok.len + 1);
                        if (name) { memcpy(name, id_tok.start, id_tok.len); name[id_tok.len] = '\0'; }
                    }
                    rest_target = ast_identifier(parser,name ? name : "");
                    if (name) p_free(name);
                }
                prop = ast_alloc(AST_REST);
                if (prop) prop->u.rest_elem.arg = rest_target;
            } else {
                /* key: value or shorthand */
                Token key_tok = lexer_next(parser->lexer);
                if (key_tok.type != TOK_IDENTIFIER) {
                    parser_error_token(parser, key_tok, "expected property name in destructuring pattern");
                }
                char *key_name = NULL;
                if (key_tok.start && key_tok.len > 0) {
                    key_name = (char *)p_malloc(key_tok.len + 1);
                    if (key_name) { memcpy(key_name, key_tok.start, key_tok.len); key_name[key_tok.len] = '\0'; }
                }

                if (match_token(parser, TOK_COLON)) {
                    /* { key: target } */
                    ASTNode *target = NULL;
                    if (peek_token(parser, TOK_LBRACKET) || peek_token(parser, TOK_LBRACE)) {
                        target = parse_pattern(parser);
                    } else {
                        Token id_tok = lexer_next(parser->lexer);
                        if (id_tok.type != TOK_IDENTIFIER) {
                            parser_error_token(parser, id_tok, "expected identifier in destructuring pattern");
                        }
                        char *name = NULL;
                        if (id_tok.start && id_tok.len > 0) {
                            name = (char *)p_malloc(id_tok.len + 1);
                            if (name) { memcpy(name, id_tok.start, id_tok.len); name[id_tok.len] = '\0'; }
                        }
                        target = ast_identifier(parser,name ? name : "");
                        if (name) p_free(name);
                    }

                    /* Default value */
                    if (match_token(parser, TOK_ASSIGN)) {
                        ASTNode *default_val = parse_assignment_expr(parser);
                        ASTNode *dv = ast_alloc(AST_DEFAULT_VALUE);
                        if (dv) {
                            dv->u.default_val.left = target;
                            dv->u.default_val.right = default_val;
                        }
                        target = dv;
                    }

                    prop = ast_alloc(AST_PROPERTY);
                    if (prop) {
                        prop->u.property.key = ast_identifier(parser,key_name ? key_name : "");
                        prop->u.property.val = target;
                        prop->u.property.computed = 0;
                        prop->u.property.shorthand = 0;
                    }
                } else {
                    /* Shorthand: { key } or { key = default } */
                    ASTNode *target = ast_identifier(parser,key_name ? key_name : "");
                    if (match_token(parser, TOK_ASSIGN)) {
                        ASTNode *default_val = parse_assignment_expr(parser);
                        ASTNode *dv = ast_alloc(AST_DEFAULT_VALUE);
                        if (dv) {
                            dv->u.default_val.left = target;
                            dv->u.default_val.right = default_val;
                        }
                        target = dv;
                    }
                    prop = ast_alloc(AST_PROPERTY);
                    if (prop) {
                        prop->u.property.key = ast_identifier(parser,key_name ? key_name : "");
                        prop->u.property.val = target;
                        prop->u.property.computed = 0;
                        prop->u.property.shorthand = 1;
                    }
                }
                if (key_name) p_free(key_name);
            }

            if (prop) {
                if (n->u.pattern_object.nprops >= cap) {
                    cap = cap ? cap * 2 : 4;
                    n->u.pattern_object.props = (ASTNode **)p_realloc(
                        n->u.pattern_object.props, cap * sizeof(ASTNode *));
                }
                n->u.pattern_object.props[n->u.pattern_object.nprops++] = prop;
            }

            if (!match_token(parser, TOK_COMMA)) break;
        }

        expect_token(parser, TOK_RBRACE);
        return n;
    }

    /* Fallback: just parse an identifier */
    Token id_tok = lexer_next(parser->lexer);
    if (id_tok.type != TOK_IDENTIFIER) {
        parser_error_token(parser, id_tok, "expected identifier in pattern");
        return NULL;
    }
    char *name = NULL;
    if (id_tok.start && id_tok.len > 0) {
        name = (char *)p_malloc(id_tok.len + 1);
        if (name) { memcpy(name, id_tok.start, id_tok.len); name[id_tok.len] = '\0'; }
    }
    ASTNode *result = ast_identifier(parser,name ? name : "");
    if (name) p_free(name);
    return result;
}

/* ── Block Parsing ────────────────────────────────────────────────────── */

static ASTNode *parse_block(Parser *parser)
{
    ASTNode *n = ast_alloc(AST_BLOCK);
    if (!n) return NULL;

    n->u.block.body = NULL;
    int cap = 0, count = 0;
    ASTNode **stmts = NULL;

    expect_token(parser, TOK_LBRACE);

    while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
        ASTNode *stmt = parse_statement(parser);
        if (stmt) {
            if (count >= cap) {
                cap = cap ? cap * 2 : 8;
                stmts = (ASTNode **)p_realloc(stmts, cap * sizeof(ASTNode *));
            }
            stmts[count++] = stmt;
        } else if (!parser->has_error) {
            break;
        }
    }

    expect_token(parser, TOK_RBRACE);
    n->u.list.items = stmts;
    n->u.list.count = count;
    return n;
}

/* ── Variable Declaration ─────────────────────────────────────────────── */

static ASTNode *parse_var_declaration(Parser *parser, TokenType decl_type, int allow_no_init)
{
    ASTNode *n = ast_alloc(AST_VAR_DECL);
    if (!n) return NULL;

    /* Record the declaration keyword (var/let/const) so the interpreter can
     * distinguish them (e.g. only var/function bind to the global object). */
    n->token.type = decl_type;

    n->u.var_decl.vars = NULL;
    n->u.var_decl.nvars = 0;
    int cap = 0;

    while (!parser->has_error) {
        ASTNode *var = NULL;
        ASTNode *init = NULL;

        if (peek_token(parser, TOK_LBRACKET) || peek_token(parser, TOK_LBRACE)) {
            /* Destructuring declaration */
            var = parse_pattern(parser);
        } else {
            /* Simple identifier */
            Token name_tok = lexer_next(parser->lexer);
            if (name_tok.type != TOK_IDENTIFIER) {
                parser_error_token(parser, name_tok, "expected variable name in declaration");
                break;
            }
            char *name = NULL;
            if (name_tok.start && name_tok.len > 0) {
                name = (char *)p_malloc(name_tok.len + 1);
                if (name) { memcpy(name, name_tok.start, name_tok.len); name[name_tok.len] = '\0'; }
            }
            var = ast_identifier(parser,name ? name : "");
            if (name) p_free(name);
        }

        /* Optional initializer */
        if (match_token(parser, TOK_ASSIGN)) {
            init = parse_assignment_expr(parser);
        } else if (decl_type == TOK_CONST && !allow_no_init) {
            parser_error(parser, "const declaration requires an initializer");
        }

        ASTNode *declarator = ast_alloc(AST_VAR_DECLARATOR);
        if (declarator) {
            declarator->u.declarator.var = var;
            declarator->u.declarator.init = init;
        }

        if (n->u.var_decl.nvars >= cap) {
            cap = cap ? cap * 2 : 4;
            n->u.var_decl.vars = (ASTNode **)p_realloc(n->u.var_decl.vars,
                cap * sizeof(ASTNode *));
        }
        n->u.var_decl.vars[n->u.var_decl.nvars++] = declarator;

        if (!match_token(parser, TOK_COMMA)) break;
    }

    return n;
}

/* ── Statement Parsing ────────────────────────────────────────────────── */

ASTNode *parse_statement(Parser *parser)
{
    if (parser->has_error) return NULL;

    Token t = lexer_peek(parser->lexer);

    switch (t.type) {
    case TOK_LBRACE:
        return parse_block(parser);

    case TOK_LET:
    case TOK_CONST:
    case TOK_VAR: {
        TokenType decl = t.type;
        lexer_skip(parser->lexer);
        ASTNode *n = parse_var_declaration(parser, decl, 0);
        /* Semicolon is optional for var declarations (ASI) */
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    case TOK_FUNCTION: {
        lexer_skip(parser->lexer);
        return parse_function(parser, 0, 0, 1);
    }

    case TOK_ASYNC: {
        /* async function or async () => or async identifier */
        /* Check lookahead for 'function' */
        size_t save_pos = (size_t)(t.start - parser->lexer->src);
        lexer_skip(parser->lexer); /* consume async */

        if (peek_token(parser, TOK_FUNCTION)) {
            lexer_skip(parser->lexer); /* consume function */
            return parse_function(parser, 1, 0, 1);
        }

        /* Not async function, restore and parse as expression statement */
        parser->lexer->pos = save_pos; lexer_reset_peek(parser->lexer);
        /* Fall through to expression statement */
        break;
    }

    case TOK_CLASS: {
        lexer_skip(parser->lexer);
        return parse_class_decl(parser, 1);
    }

    case TOK_IF: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_IF);
        if (!n) return NULL;

        expect_token(parser, TOK_LPAREN);
        n->u.if_stmt.cond = parse_expression(parser);
        expect_token(parser, TOK_RPAREN);
        n->u.if_stmt.body = parse_statement(parser);
        n->u.if_stmt.else_body = NULL;

        if (match_token(parser, TOK_ELSE)) {
            n->u.if_stmt.else_body = parse_statement(parser);
        }
        return n;
    }

    case TOK_FOR: {
        lexer_skip(parser->lexer);
        expect_token(parser, TOK_LPAREN);

        ASTNode *n = ast_alloc(AST_FOR);
        if (!n) return NULL;

        /* Check for var/let/const declaration */
        if (peek_token(parser, TOK_VAR) || peek_token(parser, TOK_LET) ||
            peek_token(parser, TOK_CONST)) {
            TokenType decl_type = lexer_next(parser->lexer).type;
            n->u.for_stmt.init = parse_var_declaration(parser, decl_type, 1);
        } else if (!peek_token(parser, TOK_SEMICOLON)) {
            n->u.for_stmt.init = parse_expression(parser);
        } else {
            n->u.for_stmt.init = NULL;
        }

        /* Check for for-in or for-of */
        if (peek_token(parser, TOK_IN)) {
            lexer_skip(parser->lexer);
            ASTNode *in_node = ast_alloc(AST_FOR_IN);
            if (in_node) {
                in_node->u.for_in.each = n->u.for_stmt.init;
                in_node->u.for_in.source = parse_expression(parser);
            }
            /* The init node (each) is transferred to in_node; prevent
             * ast_free_ex(n) from freeing it (would be a use-after-free /
             * double-free since for_in frees it again at teardown). */
            n->u.for_stmt.init = NULL;
            expect_token(parser, TOK_RPAREN);
            in_node->u.for_in.body = parse_statement(parser);
            ast_free_ex(n, parser);
            return in_node;
        }
        if (peek_token(parser, TOK_OF)) {
            lexer_skip(parser->lexer);
            ASTNode *of_node = ast_alloc(AST_FOR_OF);
            if (of_node) {
                of_node->u.for_of.each = n->u.for_stmt.init;
                of_node->u.for_of.source = parse_expression(parser);
            }
            /* The init node (each) is transferred to of_node; prevent
             * ast_free_ex(n) from freeing it (would be a use-after-free /
             * double-free since for_of frees it again at teardown). */
            n->u.for_stmt.init = NULL;
            expect_token(parser, TOK_RPAREN);
            of_node->u.for_of.body = parse_statement(parser);
            ast_free_ex(n, parser);
            return of_node;
        }

        expect_token(parser, TOK_SEMICOLON);

        /* Test */
        if (!peek_token(parser, TOK_SEMICOLON)) {
            n->u.for_stmt.test = parse_expression(parser);
        }
        expect_token(parser, TOK_SEMICOLON);

        /* Update */
        if (!peek_token(parser, TOK_RPAREN)) {
            n->u.for_stmt.update = parse_expression(parser);
        }
        expect_token(parser, TOK_RPAREN);

        n->u.for_stmt.body = parse_statement(parser);
        return n;
    }

    case TOK_WHILE: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_WHILE);
        if (!n) return NULL;

        expect_token(parser, TOK_LPAREN);
        n->u.if_stmt.cond = parse_expression(parser); /* reuse if_stmt fields */
        expect_token(parser, TOK_RPAREN);
        n->u.if_stmt.body = parse_statement(parser);
        return n;
    }

    case TOK_DO: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_DO_WHILE);
        if (!n) return NULL;

        n->u.if_stmt.body = parse_statement(parser);
        expect_token(parser, TOK_WHILE);
        expect_token(parser, TOK_LPAREN);
        n->u.if_stmt.cond = parse_expression(parser);
        expect_token(parser, TOK_RPAREN);
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    case TOK_SWITCH: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_SWITCH);
        if (!n) return NULL;

        expect_token(parser, TOK_LPAREN);
        n->u.switch_stmt.test = parse_expression(parser);
        expect_token(parser, TOK_RPAREN);
        expect_token(parser, TOK_LBRACE);

        n->u.switch_stmt.cases = NULL;
        n->u.switch_stmt.ncases = 0;
        n->u.switch_stmt.fallback = NULL;
        int cap = 0;

        while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
            ASTNode *case_node = NULL;

            if (match_token(parser, TOK_CASE)) {
                case_node = ast_alloc(AST_CASE);
                if (case_node) {
                    case_node->u.if_stmt.cond = parse_expression(parser); /* reuse: test expr */
                    expect_token(parser, TOK_COLON);

                    /* Get the body statements */
                    int sc = 0, scap = 0;
                    ASTNode **stmts = NULL;
                    while (!parser->has_error && !peek_token(parser, TOK_RBRACE) &&
                           !peek_token(parser, TOK_CASE) && !peek_token(parser, TOK_DEFAULT)) {
                        ASTNode *s = parse_statement(parser);
                        if (s) {
                            if (sc >= scap) { scap = scap ? scap * 2 : 4; stmts = (ASTNode **)p_realloc(stmts, scap * sizeof(ASTNode *)); }
                            stmts[sc++] = s;
                        }
                    }
                    /* Reuse body field for the statement list */
                    ASTNode *body_block = ast_alloc(AST_BLOCK);
                    if (body_block) {
                        body_block->u.list.items = stmts;
                        body_block->u.list.count = sc;
                    }
                    case_node->u.if_stmt.body = body_block;
                }
            } else if (match_token(parser, TOK_DEFAULT)) {
                case_node = ast_alloc(AST_DEFAULT);
                expect_token(parser, TOK_COLON);
                if (case_node) {
                    int sc = 0, scap = 0;
                    ASTNode **stmts = NULL;
                    while (!parser->has_error && !peek_token(parser, TOK_RBRACE) &&
                           !peek_token(parser, TOK_CASE) && !peek_token(parser, TOK_DEFAULT)) {
                        ASTNode *s = parse_statement(parser);
                        if (s) {
                            if (sc >= scap) { scap = scap ? scap * 2 : 4; stmts = (ASTNode **)p_realloc(stmts, scap * sizeof(ASTNode *)); }
                            stmts[sc++] = s;
                        }
                    }
                    ASTNode *body_block = ast_alloc(AST_BLOCK);
                    if (body_block) {
                        body_block->u.list.items = stmts;
                        body_block->u.list.count = sc;
                    }
                    case_node->u.if_stmt.body = body_block;
                }
            } else {
                break;
            }

            if (case_node) {
                if (n->u.switch_stmt.ncases >= cap) {
                    cap = cap ? cap * 2 : 8;
                    n->u.switch_stmt.cases = (ASTNode **)p_realloc(n->u.switch_stmt.cases,
                        cap * sizeof(ASTNode *));
                }
                n->u.switch_stmt.cases[n->u.switch_stmt.ncases++] = case_node;
            }
        }

        expect_token(parser, TOK_RBRACE);
        return n;
    }

    case TOK_BREAK: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_BREAK);
        if (!n) return NULL;
        n->u.break_stmt.label = NULL;
        if (peek_token(parser, TOK_IDENTIFIER)) {
            Token label = lexer_next(parser->lexer);
            char *name = NULL;
            if (label.start && label.len > 0) {
                name = (char *)p_malloc(label.len + 1);
                if (name) { memcpy(name, label.start, label.len); name[label.len] = '\0'; }
            }
            n->u.break_stmt.label = ast_identifier(parser,name ? name : "");
            if (name) p_free(name);
        }
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    case TOK_CONTINUE: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_CONTINUE);
        if (!n) return NULL;
        n->u.continue_stmt.label = NULL;
        if (peek_token(parser, TOK_IDENTIFIER)) {
            Token label = lexer_next(parser->lexer);
            char *name = NULL;
            if (label.start && label.len > 0) {
                name = (char *)p_malloc(label.len + 1);
                if (name) { memcpy(name, label.start, label.len); name[label.len] = '\0'; }
            }
            n->u.continue_stmt.label = ast_identifier(parser,name ? name : "");
            if (name) p_free(name);
        }
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    case TOK_RETURN: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_RETURN);
        if (!n) return NULL;
        n->u.return_stmt.arg = NULL;
        if (is_expr_start(parser) && !peek_token(parser, TOK_SEMICOLON) &&
            !peek_token(parser, TOK_RBRACE) && !peek_token(parser, TOK_EOF)) {
            n->u.return_stmt.arg = parse_expression(parser);
        }
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    case TOK_THROW: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_THROW);
        if (!n) return NULL;
        n->u.throw_stmt.arg = parse_expression(parser);
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    case TOK_TRY: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_TRY);
        if (!n) return NULL;

        n->u.try_stmt.body = parse_block(parser);
        n->u.try_stmt.catch_body = NULL;
        n->u.try_stmt.catch_var = NULL;
        n->u.try_stmt.finally_body = NULL;

        if (match_token(parser, TOK_CATCH)) {
            if (match_token(parser, TOK_LPAREN)) {
                Token catch_var = lexer_next(parser->lexer);
                if (catch_var.type == TOK_IDENTIFIER && catch_var.start && catch_var.len > 0) {
                    n->u.try_stmt.catch_var = (char *)p_malloc(catch_var.len + 1);
                    if (n->u.try_stmt.catch_var) {
                        memcpy(n->u.try_stmt.catch_var, catch_var.start, catch_var.len);
                        n->u.try_stmt.catch_var[catch_var.len] = '\0';
                    }
                }
                expect_token(parser, TOK_RPAREN);
            }
            n->u.try_stmt.catch_body = parse_block(parser);
        }

        if (match_token(parser, TOK_FINALLY)) {
            n->u.try_stmt.finally_body = parse_block(parser);
        }

        if (!n->u.try_stmt.catch_body && !n->u.try_stmt.finally_body) {
            parser_error(parser, "try requires catch or finally");
        }
        return n;
    }

    case TOK_DEBUGGER: {
        lexer_skip(parser->lexer);
        ASTNode *n = ast_alloc(AST_DEBUGGER);
        if (n) n->token = t;
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    case TOK_IMPORT: {
        /* import declaration or import() expression */
        /* import() is handled in primary expression */
        /* Here we handle import declarations at statement level */
        size_t save_pos = (size_t)(t.start - parser->lexer->src);
        lexer_skip(parser->lexer);
        if (peek_token(parser, TOK_LPAREN) || peek_token(parser, TOK_DOT)) {
            /* import() or import.meta - restore and parse as expression */
            /* Since we consumed import, we need to handle this differently */
            /* Actually, at statement level, import() is an expression statement */
            /* Let's restore and parse as expression */
            parser->lexer->pos = save_pos; lexer_reset_peek(parser->lexer);
            /* Fall through to expression statement */
            break;
        }
        /* import declaration */
        /* Restore the import token and re-parse */
        parser->lexer->pos = save_pos; lexer_reset_peek(parser->lexer);
        return parse_import_decl(parser);
    }

    case TOK_EXPORT: {
        return parse_export_decl(parser);
    }

    case TOK_SEMICOLON: {
        /* Empty statement */
        lexer_skip(parser->lexer);
        return NULL;
    }

    default: {
        /* Check for label: identifier : statement */
        if (t.type == TOK_IDENTIFIER) {
            size_t save_pos = (size_t)(t.start - parser->lexer->src);
            lexer_skip(parser->lexer);
            if (peek_token(parser, TOK_COLON)) {
                /* It's a label */
                char *label_name = NULL;
                if (t.start && t.len > 0) {
                    label_name = (char *)p_malloc(t.len + 1);
                    if (label_name) { memcpy(label_name, t.start, t.len); label_name[t.len] = '\0'; }
                }
                lexer_skip(parser->lexer); /* skip : */
                ASTNode *stmt = parse_statement(parser);
                ASTNode *n = ast_alloc(AST_LABEL);
                if (n) {
                    n->u.label_stmt.label = ast_identifier(parser,label_name ? label_name : "");
                    n->u.label_stmt.stmt = stmt;
                }
                if (label_name) p_free(label_name);
                return n;
            }
            /* Restore and parse as expression statement */
            parser->lexer->pos = save_pos; lexer_reset_peek(parser->lexer);
        }

        /* Expression statement */
        if (is_expr_start(parser)) {
            ASTNode *expr = parse_expression(parser);
            if (expr) {
                ASTNode *n = ast_alloc(AST_EXPR_STMT);
                if (n) {
                    n->u.expr_stmt.expr = expr;
                }
                match_token(parser, TOK_SEMICOLON);
                return n;
            }
        }

        if (!parser->has_error) {
            parser_error(parser, "unexpected token '%s' in statement",
                token_type_name(t.type));
        }
        return NULL;
    }
    }

    return NULL;
}

/* ── Import Declaration ───────────────────────────────────────────────── */

static ASTNode *parse_import_decl(Parser *parser)
{
    expect_token(parser, TOK_IMPORT);
    ASTNode *n = ast_alloc(AST_IMPORT);
    if (!n) return NULL;

    n->u.import_decl.specifiers = NULL;
    n->u.import_decl.nspec = 0;
    n->u.import_decl.source = NULL;
    int cap = 0;

    if (peek_token(parser, TOK_STRING)) {
        /* import "module" (side-effect import) */
        Token src = lexer_next(parser->lexer);
        n->u.import_decl.source = ast_literal_string(parser,src.str_val ? src.str_val : "");
        token_free_data(&src);
        goto done;
    }

    if (match_token(parser, TOK_MUL)) {
        /* import * as name from "module" */
        ASTNode *ns = ast_alloc(AST_IMPORT_NAMESPACE);
        if (ns) {
            expect_token(parser, TOK_AS);
            Token name = lexer_next(parser->lexer);
            if (name.type == TOK_IDENTIFIER && name.start && name.len > 0) {
                char *nname = (char *)p_malloc(name.len + 1);
                if (nname) { memcpy(nname, name.start, name.len); nname[name.len] = '\0'; }
                ns->u.import_namespace.local = ast_identifier(parser,nname ? nname : "");
                if (nname) p_free(nname);
            }
        }
        if (n->u.import_decl.nspec >= cap) {
            cap = cap ? cap * 2 : 4;
            n->u.import_decl.specifiers = (ASTNode **)p_realloc(n->u.import_decl.specifiers,
                cap * sizeof(ASTNode *));
        }
        n->u.import_decl.specifiers[n->u.import_decl.nspec++] = ns;
        goto from_clause;
    }

    if (match_token(parser, TOK_LBRACE)) {
        /* import { spec1, spec2 } from "module" */
        while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
            ASTNode *spec = ast_alloc(AST_IMPORT_SPECIFIER);
            if (spec) {
                Token name = lexer_next(parser->lexer);
                if (name.type == TOK_IDENTIFIER && name.start && name.len > 0) {
                    char *sname = (char *)p_malloc(name.len + 1);
                    if (sname) { memcpy(sname, name.start, name.len); sname[name.len] = '\0'; }
                    spec->u.import_spec.name = sname;
                    spec->u.import_spec.local = NULL;

                    if (match_token(parser, TOK_AS)) {
                        Token alias = lexer_next(parser->lexer);
                        if (alias.type == TOK_IDENTIFIER && alias.start && alias.len > 0) {
                            char *aname = (char *)p_malloc(alias.len + 1);
                            if (aname) { memcpy(aname, alias.start, alias.len); aname[alias.len] = '\0'; }
                            spec->u.import_spec.local = ast_identifier(parser,aname ? aname : "");
                            if (aname) p_free(aname);
                        }
                    } else {
                        spec->u.import_spec.local = ast_identifier(parser,sname);
                    }
                }

                if (n->u.import_decl.nspec >= cap) {
                    cap = cap ? cap * 2 : 4;
                    n->u.import_decl.specifiers = (ASTNode **)p_realloc(n->u.import_decl.specifiers,
                        cap * sizeof(ASTNode *));
                }
                n->u.import_decl.specifiers[n->u.import_decl.nspec++] = spec;
            }

            if (!match_token(parser, TOK_COMMA)) break;
        }
        expect_token(parser, TOK_RBRACE);
        goto from_clause;
    }

    /* Default import: import name from "module" */
    if (peek_token(parser, TOK_IDENTIFIER)) {
        Token name = lexer_next(parser->lexer);
        ASTNode *spec = ast_alloc(AST_IMPORT_SPECIFIER);
        if (spec) {
            spec->u.import_spec.is_default = 1;  /* default import */
            if (name.start && name.len > 0) {
                char *sname = (char *)p_malloc(name.len + 1);
                if (sname) { memcpy(sname, name.start, name.len); sname[name.len] = '\0'; }
                spec->u.import_spec.name = sname;
                spec->u.import_spec.local = ast_identifier(parser,sname);
            }
        }
        if (n->u.import_decl.nspec >= cap) {
            cap = cap ? cap * 2 : 4;
            n->u.import_decl.specifiers = (ASTNode **)p_realloc(n->u.import_decl.specifiers,
                cap * sizeof(ASTNode *));
        }
        n->u.import_decl.specifiers[n->u.import_decl.nspec++] = spec;

        if (peek_token(parser, TOK_COMMA)) {
            /* import default, { ... } from "module" */
            lexer_skip(parser->lexer);
            if (peek_token(parser, TOK_LBRACE)) {
                lexer_skip(parser->lexer);
                while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
                    ASTNode *spec2 = ast_alloc(AST_IMPORT_SPECIFIER);
                    if (spec2) {
                        Token name2 = lexer_next(parser->lexer);
                        if (name2.type == TOK_IDENTIFIER && name2.start && name2.len > 0) {
                            char *sname2 = (char *)p_malloc(name2.len + 1);
                            if (sname2) { memcpy(sname2, name2.start, name2.len); sname2[name2.len] = '\0'; }
                            spec2->u.import_spec.name = sname2;
                            spec2->u.import_spec.local = ast_identifier(parser,sname2);
                            if (match_token(parser, TOK_AS)) {
                                Token alias = lexer_next(parser->lexer);
                                if (alias.type == TOK_IDENTIFIER && alias.start && alias.len > 0) {
                                    char *aname = (char *)p_malloc(alias.len + 1);
                                    if (aname) { memcpy(aname, alias.start, alias.len); aname[alias.len] = '\0'; }
                                    spec2->u.import_spec.local = ast_identifier(parser,aname ? aname : "");
                                    if (aname) p_free(aname);
                                }
                            }
                        }
                        if (n->u.import_decl.nspec >= cap) {
                            cap = cap ? cap * 2 : 4;
                            n->u.import_decl.specifiers = (ASTNode **)p_realloc(n->u.import_decl.specifiers,
                                cap * sizeof(ASTNode *));
                        }
                        n->u.import_decl.specifiers[n->u.import_decl.nspec++] = spec2;
                    }
                    if (!match_token(parser, TOK_COMMA)) break;
                }
                expect_token(parser, TOK_RBRACE);
            }
        }
        goto from_clause;
    }

from_clause:
    if (match_token(parser, TOK_FROM)) {
        Token src = lexer_next(parser->lexer);
        if (src.type == TOK_STRING) {
            n->u.import_decl.source = ast_literal_string(parser,src.str_val ? src.str_val : "");
            token_free_data(&src);
        } else {
            parser_error_token(parser, src, "expected module specifier string");
        }
    } else {
        /* Without 'from' clause, it's just an import declaration */
        /* This is actually a syntax error in standard JS, but we allow it */
    }

done:
    match_token(parser, TOK_SEMICOLON);
    return n;
}

/* ── Export Declaration ───────────────────────────────────────────────── */

static ASTNode *parse_export_decl(Parser *parser)
{
    expect_token(parser, TOK_EXPORT);
    ASTNode *n = ast_alloc(AST_EXPORT);
    if (!n) return NULL;

    n->u.export_decl.specifiers = NULL;
    n->u.export_decl.nspec = 0;
    n->u.export_decl.source = NULL;
    n->u.export_decl.is_default = 0;
    int cap = 0;

    if (match_token(parser, TOK_DEFAULT)) {
        /* export default ... */
        ASTNode *def_node = ast_alloc(AST_EXPORT_DEFAULT);
        if (def_node) {
            if (peek_token(parser, TOK_FUNCTION)) {
                lexer_skip(parser->lexer);
                def_node->u.export_default.value = parse_function(parser, 0, 0, 0);
            } else if (peek_token(parser, TOK_ASYNC)) {
                size_t save_pos = parser->lexer->pos;
                lexer_skip(parser->lexer);
                if (peek_token(parser, TOK_FUNCTION)) {
                    lexer_skip(parser->lexer);
                    def_node->u.export_default.value = parse_function(parser, 1, 0, 0);
                } else {
                    parser->lexer->pos = save_pos; lexer_reset_peek(parser->lexer);
                    def_node->u.export_default.value = parse_assignment_expr(parser);
                }
            } else if (peek_token(parser, TOK_CLASS)) {
                lexer_skip(parser->lexer);
                def_node->u.export_default.value = parse_class_decl(parser, 0);
            } else {
                def_node->u.export_default.value = parse_assignment_expr(parser);
            }
        }
        n->u.export_decl.is_default = 1;
        if (n->u.export_decl.nspec >= cap) {
            cap = cap ? cap * 2 : 4;
            n->u.export_decl.specifiers = (ASTNode **)p_realloc(n->u.export_decl.specifiers,
                cap * sizeof(ASTNode *));
        }
        n->u.export_decl.specifiers[n->u.export_decl.nspec++] = def_node;
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    if (match_token(parser, TOK_MUL)) {
        /* export * from "module" */
        ASTNode *all = ast_alloc(AST_EXPORT_ALL);
        if (all) {
            if (match_token(parser, TOK_AS)) {
                /* export * as name from "module" */
                lexer_next(parser->lexer); /* consume the name */
                /* Handle as a named export with namespace */
            }
        }
        if (match_token(parser, TOK_FROM)) {
            Token src = lexer_next(parser->lexer);
            if (src.type == TOK_STRING) {
                n->u.export_decl.source = ast_literal_string(parser,src.str_val ? src.str_val : "");
                token_free_data(&src);
            }
        }
        if (n->u.export_decl.nspec >= cap) {
            cap = cap ? cap * 2 : 4;
            n->u.export_decl.specifiers = (ASTNode **)p_realloc(n->u.export_decl.specifiers,
                cap * sizeof(ASTNode *));
        }
        n->u.export_decl.specifiers[n->u.export_decl.nspec++] = all;
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    if (match_token(parser, TOK_LBRACE)) {
        /* export { spec1, spec2 } from "module" */
        while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
            ASTNode *spec = ast_alloc(AST_EXPORT_NAMED);
            if (spec) {
                Token name = lexer_next(parser->lexer);
                if (name.type == TOK_IDENTIFIER && name.start && name.len > 0) {
                    char *sname = (char *)p_malloc(name.len + 1);
                    if (sname) { memcpy(sname, name.start, name.len); sname[name.len] = '\0'; }
                    spec->u.export_spec.name = sname;
                    spec->u.export_spec.exported = NULL;

                    if (match_token(parser, TOK_AS)) {
                        Token alias = lexer_next(parser->lexer);
                        if (alias.type == TOK_IDENTIFIER && alias.start && alias.len > 0) {
                            char *aname = (char *)p_malloc(alias.len + 1);
                            if (aname) { memcpy(aname, alias.start, alias.len); aname[alias.len] = '\0'; }
                            spec->u.export_spec.exported = ast_identifier(parser,aname ? aname : "");
                            if (aname) p_free(aname);
                        }
                    }
                }

                if (n->u.export_decl.nspec >= cap) {
                    cap = cap ? cap * 2 : 4;
                    n->u.export_decl.specifiers = (ASTNode **)p_realloc(n->u.export_decl.specifiers,
                        cap * sizeof(ASTNode *));
                }
                n->u.export_decl.specifiers[n->u.export_decl.nspec++] = spec;
            }

            if (!match_token(parser, TOK_COMMA)) break;
        }
        expect_token(parser, TOK_RBRACE);

        if (match_token(parser, TOK_FROM)) {
            Token src = lexer_next(parser->lexer);
            if (src.type == TOK_STRING) {
                n->u.export_decl.source = ast_literal_string(parser,src.str_val ? src.str_val : "");
                token_free_data(&src);
            }
        }
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    /* export var/let/const/function/class declaration */
    if (peek_token(parser, TOK_VAR) || peek_token(parser, TOK_LET) ||
        peek_token(parser, TOK_CONST)) {
        TokenType decl_type = lexer_next(parser->lexer).type;
        n->u.export_decl.specifiers = (ASTNode **)p_malloc(sizeof(ASTNode *));
        n->u.export_decl.specifiers[0] = parse_var_declaration(parser, decl_type, 0);
        n->u.export_decl.nspec = 1;
        match_token(parser, TOK_SEMICOLON);
        return n;
    }

    if (peek_token(parser, TOK_FUNCTION)) {
        lexer_skip(parser->lexer);
        n->u.export_decl.specifiers = (ASTNode **)p_malloc(sizeof(ASTNode *));
        n->u.export_decl.specifiers[0] = parse_function(parser, 0, 0, 1);
        n->u.export_decl.nspec = 1;
        return n;
    }

    if (peek_token(parser, TOK_ASYNC)) {
        size_t save_pos = parser->lexer->pos;
        lexer_skip(parser->lexer);
        if (peek_token(parser, TOK_FUNCTION)) {
            lexer_skip(parser->lexer);
            n->u.export_decl.specifiers = (ASTNode **)p_malloc(sizeof(ASTNode *));
            n->u.export_decl.specifiers[0] = parse_function(parser, 1, 0, 1);
            n->u.export_decl.nspec = 1;
            return n;
        }
        parser->lexer->pos = save_pos; lexer_reset_peek(parser->lexer);
    }

    if (peek_token(parser, TOK_CLASS)) {
        lexer_skip(parser->lexer);
        n->u.export_decl.specifiers = (ASTNode **)p_malloc(sizeof(ASTNode *));
        n->u.export_decl.specifiers[0] = parse_class_decl(parser, 1);
        n->u.export_decl.nspec = 1;
        return n;
    }

    parser_error(parser, "expected declaration after 'export'");
    return n;
}

/* ── Class Declaration ────────────────────────────────────────────────── */

static ASTNode *parse_class_decl(Parser *parser, int is_decl)
{
    ASTNode *cls = ast_alloc(AST_CLASS_DECL);
    if (!cls) return NULL;

    cls->u.class_decl.name = NULL;
    cls->u.class_decl.body = NULL;
    cls->u.class_decl.extends = NULL;
    cls->u.class_decl.methods = NULL;
    cls->u.class_decl.nmethods = 0;

    /* Class name */
    if (peek_token(parser, TOK_IDENTIFIER)) {
        Token name = lexer_next(parser->lexer);
        if (name.start && name.len > 0) {
            cls->u.class_decl.name = (char *)p_malloc(name.len + 1);
            if (cls->u.class_decl.name) {
                memcpy(cls->u.class_decl.name, name.start, name.len);
                cls->u.class_decl.name[name.len] = '\0';
            }
        }
    }

    /* extends clause */
    if (match_token(parser, TOK_EXTENDS)) {
        cls->u.class_decl.extends = parse_expression(parser);
    }

    /* Class body */
    expect_token(parser, TOK_LBRACE);

    int cap = 0;
    while (!parser->has_error && !peek_token(parser, TOK_RBRACE)) {
        /* Skip stray semicolons between class members */
        if (match_token(parser, TOK_SEMICOLON)) continue;

        int is_static_m = 0;
        int is_async_method = 0;
        int is_generator_method = 0;
        int is_getter = 0;
        int is_setter = 0;

        /* 'static' modifier (unless used as a member name) */
        if (peek_token(parser, TOK_STATIC)) {
            LexState st = lex_state_save(parser->lexer);
            lexer_skip(parser->lexer);
            if (peek_token(parser, TOK_LPAREN) || peek_token(parser, TOK_ASSIGN) ||
                peek_token(parser, TOK_SEMICOLON)) {
                /* 'static' is actually the member name */
                lex_state_restore(parser->lexer, st);
            } else {
                is_static_m = 1;
            }
        }

        /* Static initialization block: static { ... } */
        if (is_static_m && peek_token(parser, TOK_LBRACE)) {
            ASTNode *body = parse_block(parser);
            ASTNode *func = ast_alloc(AST_FUNC_EXPR);
            if (func) {
                func->u.func.name = strdup("__static_block__");
                func->u.func.params = NULL;
                func->u.func.nparams = 0;
                func->u.func.body = body;
                func->u.func.is_static = 1;
                func->u.func.class_node = cls;
            } else if (body) {
                ast_free_ex(body, parser);
            }
            if (func) {
                if (cls->u.class_decl.nmethods >= cap) {
                    cap = cap ? cap * 2 : 8;
                    cls->u.class_decl.methods = (ASTNode **)p_realloc(cls->u.class_decl.methods,
                        cap * sizeof(ASTNode *));
                }
                cls->u.class_decl.methods[cls->u.class_decl.nmethods++] = func;
            }
            continue;
        }

        /* 'async' modifier (unless used as a member name) */
        if (peek_token(parser, TOK_ASYNC)) {
            LexState st = lex_state_save(parser->lexer);
            lexer_skip(parser->lexer);
            if (peek_token(parser, TOK_LPAREN) || peek_token(parser, TOK_ASSIGN) ||
                peek_token(parser, TOK_SEMICOLON)) {
                lex_state_restore(parser->lexer, st);
            } else {
                is_async_method = 1;
            }
        }

        /* Generator star */
        if (match_token(parser, TOK_MUL)) is_generator_method = 1;

        /* 'get' / 'set' modifier (unless used as a member name) */
        if (peek_token(parser, TOK_GET) || peek_token(parser, TOK_SET)) {
            int is_get_tok = peek_token(parser, TOK_GET);
            LexState st = lex_state_save(parser->lexer);
            lexer_skip(parser->lexer);
            if (peek_token(parser, TOK_LPAREN) || peek_token(parser, TOK_ASSIGN) ||
                peek_token(parser, TOK_SEMICOLON)) {
                lex_state_restore(parser->lexer, st);
            } else {
                if (is_get_tok) is_getter = 1;
                else is_setter = 1;
            }
        }

        /* Member name */
        ASTNode *key_expr = NULL;    /* computed key */
        char *method_name = NULL;

        if (match_token(parser, TOK_LBRACKET)) {
            key_expr = parse_expression(parser);
            expect_token(parser, TOK_RBRACKET);
        } else if (peek_token(parser, TOK_STRING) || peek_token(parser, TOK_NUMBER)) {
            Token kt = lexer_next(parser->lexer);
            if (kt.type == TOK_STRING) {
                if (kt.str_val) method_name = strdup(kt.str_val);
                token_free_data(&kt);
            } else {
                char numbuf[40];
                snprintf(numbuf, sizeof(numbuf), "%.17g", kt.num_val);
                method_name = strdup(numbuf);
            }
        } else {
            Token kt = lexer_next(parser->lexer);
            int name_ok = (kt.type == TOK_IDENTIFIER || kt.type == TOK_GET ||
                           kt.type == TOK_SET || kt.type == TOK_STATIC ||
                           kt.type == TOK_ASYNC || kt.type == TOK_PRIVATE_NAME);
            /* Also allow keywords used as member names (e.g. delete()) */
            if (!name_ok && kt.start && kt.len > 0) {
                char c0 = kt.start[0];
                if ((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') ||
                    c0 == '_' || c0 == '$' || c0 == '#') {
                    name_ok = 1;
                }
            }
            if (!name_ok) {
                parser_error_token(parser, kt, "expected class member name");
                break;
            }
            if (kt.start && kt.len > 0) {
                method_name = (char *)p_malloc(kt.len + 1);
                if (method_name) {
                    memcpy(method_name, kt.start, kt.len);
                    method_name[kt.len] = '\0';
                }
            }
        }

        ASTNode *member = NULL;

        if (peek_token(parser, TOK_LPAREN)) {
            /* Method definition */
            ASTNode **params = parse_params(parser);
            int nparams = 0;
            if (params) {
                while (params[nparams]) nparams++;
            }
            ASTNode *body = parse_block(parser);

            ASTNode *func = ast_alloc(AST_FUNC_EXPR);
            if (func) {
                func->u.func.name = method_name ? strdup(method_name) : NULL;
                func->u.func.params = params;
                func->u.func.nparams = nparams;
                func->u.func.body = body;
                func->u.func.is_async = is_async_method;
                func->u.func.is_generator = is_generator_method;
                func->u.func.is_static = is_static_m;
                func->u.func.is_getter = is_getter;
                func->u.func.is_setter = is_setter;
                func->u.func.key_expr = key_expr;
                func->u.func.class_node = cls;
            }
            member = func;
        } else {
            /* Class field: name [= initializer] ; */
            ASTNode *init = NULL;
            if (match_token(parser, TOK_ASSIGN)) {
                init = parse_assignment_expr(parser);
            }
            match_token(parser, TOK_SEMICOLON);

            ASTNode *prop = ast_alloc(AST_PROPERTY);
            if (prop) {
                prop->u.property.key = key_expr ? key_expr
                    : ast_identifier(parser, method_name ? method_name : "");
                prop->u.property.val = init;
                prop->u.property.computed = key_expr ? 1 : 0;
                prop->u.property.shorthand = 0;
                prop->u.property.is_static = is_static_m;
            }
            member = prop;
        }

        if (member) {
            if (cls->u.class_decl.nmethods >= cap) {
                cap = cap ? cap * 2 : 8;
                cls->u.class_decl.methods = (ASTNode **)p_realloc(cls->u.class_decl.methods,
                    cap * sizeof(ASTNode *));
            }
            cls->u.class_decl.methods[cls->u.class_decl.nmethods++] = member;
        }

        if (method_name) p_free(method_name);
    }

    expect_token(parser, TOK_RBRACE);

    /* If the original declaration was for a function expression (class expression), return the class */
    if (!is_decl) {
        /* Return the class as an expression */
        return cls;
    }

    return cls;
}

/* ── Program Parsing ──────────────────────────────────────────────────── */

ASTNode *parse_program(Parser *parser)
{
    ASTNode *prog = ast_alloc(AST_PROGRAM);
    if (!prog) return NULL;

    int cap = 16;
    int count = 0;
    ASTNode **stmts = (ASTNode **)p_malloc(cap * sizeof(ASTNode *));

    while (!parser->has_error && !peek_token(parser, TOK_EOF)) {
        ASTNode *stmt = parse_statement(parser);
        if (stmt) {
            if (count >= cap) {
                cap *= 2;
                stmts = (ASTNode **)p_realloc(stmts, cap * sizeof(ASTNode *));
            }
            stmts[count++] = stmt;
        } else if (!parser->has_error) {
            /* Skip to next statement on empty/null */
            Token t = lexer_next(parser->lexer);
            if (t.type == TOK_EOF) break;
            /* Skip this token and continue */
        }
    }

    prog->u.list.items = stmts;
    prog->u.list.count = count;
    return prog;
}

/* ── Parser API ───────────────────────────────────────────────────────── */

void parser_init(Parser *parser, Lexer *lexer)
{
    parser->lexer = lexer;
    parser->has_error = 0;
    parser->error_msg[0] = '\0';
    parser->error_line = 0;
    parser->error_col = 0;

    /* Initialize AST node pool */
    parser->node_pool.count = 0;
    parser->node_pool.capacity = LR_AST_NODE_POOL_SIZE;
    parser->use_pool = 1;

    /* Initialize string intern table */
    memset(parser->intern_table, 0, sizeof(parser->intern_table));

    /* Initialize cached literal nodes (once) */
    static int cached_literals_initialized = 0;
    if (!cached_literals_initialized) {
        init_cached_literals();
        cached_literals_initialized = 1;
    }
}

/* Free the string intern table */
void parser_free(Parser *parser)
{
    for (int i = 0; i < LR_STRING_INTERN_SIZE; i++) {
        LRStringIntern *entry = parser->intern_table[i];
        while (entry) {
            LRStringIntern *next = entry->next;
            if (entry->str) p_free(entry->str);
            p_free(entry);
            entry = next;
        }
        parser->intern_table[i] = NULL;
    }
    /* Reset pool so it can be reused */
    parser->node_pool.count = 0;
}

const char *parser_get_error(Parser *parser, size_t *line, size_t *col)
{
    if (parser->has_error) {
        if (line) *line = parser->error_line;
        if (col) *col = parser->error_col;
        return parser->error_msg;
    }
    return NULL;
}

/* ── AST Free ─────────────────────────────────────────────────────────── */

/* Helper: check if a node is in the parser's pool */
static inline int is_pool_node(Parser *parser, ASTNode *node)
{
    if (!parser || !node) return 0;
    return (node >= &parser->node_pool.nodes[0] &&
            node < &parser->node_pool.nodes[LR_AST_NODE_POOL_SIZE]);
}

void ast_free_ex(ASTNode *node, Parser *parser)
{
    if (!node) return;

    /* Skip cached literal nodes (global singletons, never freed) */
    if (node == cached_true_node || node == cached_false_node ||
        node == cached_null_node || node == cached_undefined_node ||
        node == cached_zero_node || node == cached_one_node) {
        return;
    }

    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
        if (node->u.list.items) {
            for (int i = 0; i < node->u.list.count; i++) {
                ast_free_ex(node->u.list.items[i], parser);
            }
            p_free(node->u.list.items);
        }
        break;

    case AST_EXPR_STMT:
        ast_free_ex(node->u.expr_stmt.expr, parser);
        break;

    case AST_IF:
    case AST_WHILE:
    case AST_DO_WHILE:
        ast_free_ex(node->u.if_stmt.cond, parser);
        ast_free_ex(node->u.if_stmt.body, parser);
        ast_free_ex(node->u.if_stmt.else_body, parser);
        break;

    case AST_FOR:
        ast_free_ex(node->u.for_stmt.init, parser);
        ast_free_ex(node->u.for_stmt.test, parser);
        ast_free_ex(node->u.for_stmt.update, parser);
        ast_free_ex(node->u.for_stmt.body, parser);
        break;

    case AST_FOR_IN:
        ast_free_ex(node->u.for_in.each, parser);
        ast_free_ex(node->u.for_in.source, parser);
        ast_free_ex(node->u.for_in.body, parser);
        break;

    case AST_FOR_OF:
        ast_free_ex(node->u.for_of.each, parser);
        ast_free_ex(node->u.for_of.source, parser);
        ast_free_ex(node->u.for_of.body, parser);
        break;

    case AST_SWITCH:
        ast_free_ex(node->u.switch_stmt.test, parser);
        if (node->u.switch_stmt.cases) {
            for (int i = 0; i < node->u.switch_stmt.ncases; i++) {
                ast_free_ex(node->u.switch_stmt.cases[i], parser);
            }
            p_free(node->u.switch_stmt.cases);
        }
        break;

    case AST_CASE:
    case AST_DEFAULT:
        ast_free_ex(node->u.if_stmt.cond, parser);
        ast_free_ex(node->u.if_stmt.body, parser);
        break;

    case AST_BREAK:
        ast_free_ex(node->u.break_stmt.label, parser);
        break;

    case AST_CONTINUE:
        ast_free_ex(node->u.continue_stmt.label, parser);
        break;

    case AST_RETURN:
        ast_free_ex(node->u.return_stmt.arg, parser);
        break;

    case AST_THROW:
        ast_free_ex(node->u.throw_stmt.arg, parser);
        break;

    case AST_TRY:
        ast_free_ex(node->u.try_stmt.body, parser);
        ast_free_ex(node->u.try_stmt.catch_body, parser);
        ast_free_ex(node->u.try_stmt.finally_body, parser);
        if (node->u.try_stmt.catch_var) p_free(node->u.try_stmt.catch_var);
        break;

    case AST_LABEL:
        ast_free_ex(node->u.label_stmt.label, parser);
        ast_free_ex(node->u.label_stmt.stmt, parser);
        break;

    case AST_WITH:
        ast_free_ex(node->u.with_stmt.obj, parser);
        ast_free_ex(node->u.with_stmt.body, parser);
        break;

    case AST_DEBUGGER:
        break;

    case AST_VAR_DECL:
        if (node->u.var_decl.vars) {
            for (int i = 0; i < node->u.var_decl.nvars; i++) {
                ast_free_ex(node->u.var_decl.vars[i], parser);
            }
            p_free(node->u.var_decl.vars);
        }
        break;

    case AST_VAR_DECLARATOR:
        ast_free_ex(node->u.declarator.var, parser);
        ast_free_ex(node->u.declarator.init, parser);
        break;

    case AST_FUNC_DECL:
    case AST_FUNC_EXPR:
        if (node->u.func.name) p_free(node->u.func.name);
        if (node->u.func.params) {
            for (int i = 0; i < node->u.func.nparams; i++) {
                ast_free_ex(node->u.func.params[i], parser);
            }
            p_free(node->u.func.params);
        }
        ast_free_ex(node->u.func.key_expr, parser);
        /* NOTE: u.func.class_node is a non-owning back-pointer, do not free */
        ast_free_ex(node->u.func.body, parser);
        break;

    case AST_ARROW:
        if (node->u.arrow.params) {
            for (int i = 0; i < node->u.arrow.nparams; i++) {
                ast_free_ex(node->u.arrow.params[i], parser);
            }
            p_free(node->u.arrow.params);
        }
        ast_free_ex(node->u.arrow.body, parser);
        break;

    case AST_CLASS_DECL:
        if (node->u.class_decl.name) p_free(node->u.class_decl.name);
        ast_free_ex(node->u.class_decl.body, parser);
        ast_free_ex(node->u.class_decl.extends, parser);
        if (node->u.class_decl.methods) {
            for (int i = 0; i < node->u.class_decl.nmethods; i++) {
                ast_free_ex(node->u.class_decl.methods[i], parser);
            }
            p_free(node->u.class_decl.methods);
        }
        break;

    case AST_LITERAL:
        /* Skip freeing the string: it's interned in the parser's intern table
         * and will be freed by parser_free(). */
        break;

    case AST_IDENTIFIER:
        /* Skip freeing the name: it's interned in the parser's intern table
         * and will be freed by parser_free(). */
        break;

    case AST_THIS:
    case AST_SUPER:
        break;

    case AST_BINARY:
        ast_free_ex(node->u.binary.left, parser);
        ast_free_ex(node->u.binary.right, parser);
        break;

    case AST_UNARY:
        ast_free_ex(node->u.unary.arg, parser);
        break;

    case AST_CONDITIONAL:
        ast_free_ex(node->u.conditional.cond, parser);
        ast_free_ex(node->u.conditional.consequent, parser);
        ast_free_ex(node->u.conditional.alternate, parser);
        break;

    case AST_CALL:
    case AST_OPTIONAL_CALL:
        ast_free_ex(node->u.call.callee, parser);
        if (node->u.call.args) {
            for (int i = 0; i < node->u.call.argc; i++) {
                ast_free_ex(node->u.call.args[i], parser);
            }
            p_free(node->u.call.args);
        }
        break;

    case AST_NEW:
        ast_free_ex(node->u.new_expr.callee, parser);
        if (node->u.new_expr.args) {
            for (int i = 0; i < node->u.new_expr.argc; i++) {
                ast_free_ex(node->u.new_expr.args[i], parser);
            }
            p_free(node->u.new_expr.args);
        }
        break;

    case AST_MEMBER:
    case AST_COMPUTED_MEMBER:
    case AST_OPTIONAL_MEMBER:
        ast_free_ex(node->u.member.obj, parser);
        ast_free_ex(node->u.member.prop, parser);
        break;

    case AST_ASSIGN:
        ast_free_ex(node->u.assign.target, parser);
        ast_free_ex(node->u.assign.value, parser);
        break;

    case AST_SEQUENCE:
        if (node->u.sequence.exprs) {
            for (int i = 0; i < node->u.sequence.count; i++) {
                ast_free_ex(node->u.sequence.exprs[i], parser);
            }
            p_free(node->u.sequence.exprs);
        }
        break;

    case AST_ARRAY:
        if (node->u.array.elements) {
            for (int i = 0; i < node->u.array.nelem; i++) {
                ast_free_ex(node->u.array.elements[i], parser);
            }
            p_free(node->u.array.elements);
        }
        break;

    case AST_OBJECT:
        if (node->u.object.props) {
            for (int i = 0; i < node->u.object.nprops; i++) {
                ast_free_ex(node->u.object.props[i], parser);
            }
            p_free(node->u.object.props);
        }
        break;

    case AST_PROPERTY:
        ast_free_ex(node->u.property.key, parser);
        ast_free_ex(node->u.property.val, parser);
        break;

    case AST_SPREAD:
    case AST_SPREAD_ELEMENT:
        ast_free_ex(node->u.spread.arg, parser);
        break;

    case AST_TEMPLATE:
    case AST_TAGGED_TEMPLATE:
        if (node->u.template_lit.parts) {
            for (int i = 0; i < node->u.template_lit.nparts; i++) {
                /* parts[i] are verbatim source fragments malloc'd by the lexer */
                if (node->u.template_lit.parts[i]) free(node->u.template_lit.parts[i]);
            }
            p_free(node->u.template_lit.parts);
        }
        if (node->u.template_lit.tag)
            ast_free_ex(node->u.template_lit.tag, parser);
        if (node->u.template_lit.exprs) {
            for (int i = 0; i < node->u.template_lit.nexp; i++) {
                ast_free_ex(node->u.template_lit.exprs[i], parser);
            }
            p_free(node->u.template_lit.exprs);
        }
        break;

    case AST_PATTERN:
        /* pattern_array/pattern_object alias the same union storage;
         * free the item array exactly once. */
        if (node->u.pattern_array.elements) {
            for (int i = 0; i < node->u.pattern_array.nelem; i++) {
                ast_free_ex(node->u.pattern_array.elements[i], parser);
            }
            p_free(node->u.pattern_array.elements);
        }
        break;

    case AST_REST:
        ast_free_ex(node->u.rest_elem.arg, parser);
        break;

    case AST_DEFAULT_VALUE:
        ast_free_ex(node->u.default_val.left, parser);
        ast_free_ex(node->u.default_val.right, parser);
        break;

    case AST_AWAIT:
        ast_free_ex(node->u.await_expr.arg, parser);
        break;

    case AST_YIELD:
        ast_free_ex(node->u.yield_expr.arg, parser);
        break;

    case AST_IMPORT:
        if (node->u.import_decl.specifiers) {
            for (int i = 0; i < node->u.import_decl.nspec; i++) {
                ast_free_ex(node->u.import_decl.specifiers[i], parser);
            }
            p_free(node->u.import_decl.specifiers);
        }
        ast_free_ex(node->u.import_decl.source, parser);
        break;

    case AST_EXPORT:
        if (node->u.export_decl.specifiers) {
            for (int i = 0; i < node->u.export_decl.nspec; i++) {
                ast_free_ex(node->u.export_decl.specifiers[i], parser);
            }
            p_free(node->u.export_decl.specifiers);
        }
        ast_free_ex(node->u.export_decl.source, parser);
        break;

    case AST_EXPORT_NAMED:
        /* Specifier node: uses the export_spec view (name string +
         * exported identifier), NOT the export_decl list view. */
        if (node->u.export_spec.name) p_free(node->u.export_spec.name);
        ast_free_ex(node->u.export_spec.exported, parser);
        break;

    case AST_EXPORT_DEFAULT:
        ast_free_ex(node->u.export_default.value, parser);
        break;

    case AST_EXPORT_ALL:
        ast_free_ex(node->u.export_all.source, parser);
        break;

    case AST_IMPORT_SPECIFIER:
        if (node->u.import_spec.name) p_free(node->u.import_spec.name);
        ast_free_ex(node->u.import_spec.local, parser);
        break;

    case AST_IMPORT_NAMESPACE:
        ast_free_ex(node->u.import_namespace.local, parser);
        break;

    default:
        break;
    }

    /* Free token string data */
    token_free_data(&node->token);

    /* Skip p_free for pool-allocated nodes (managed by the pool) and cached literals */
    if (!is_pool_node(parser, node) &&
        node != cached_true_node && node != cached_false_node &&
        node != cached_null_node && node != cached_undefined_node &&
        node != cached_zero_node && node != cached_one_node) {
        p_free(node);
    }
}

/* ── AST Serialization (bytecode cache support) ──────────────────────────
 * Serializes the parse tree into a flat byte buffer so it can be persisted to
 * disk (.lrfile) and re-executed later without re-lexing/re-parsing.
 *
 * Design notes:
 *  - A node is serialized as: type(u16), token(type/line/col/num_val), then
 *    type-specific union fields (children recursively, strings, op[16]).
 *  - NULL child pointers use the sentinel type 0xFFFF.
 *  - Strings that the interpreter resolves by name (identifiers, literal
 *    strings, func/class/property names) are interned on deserialize so that
 *    ast_free_ex / parser_free clean them up exactly like a normal parse.
 *  - Template literal part fragments are freed with free() by ast_free_ex, so
 *    they are copied with malloc() (not interned) on deserialize.
 *  - The func.class_node back-pointer is restored during deserialize by
 *    pointing each class method's func node at its owning class declaration
 *    (required at runtime for `super` in derived classes). */

#define LR_AST_NULL_NODE 0xFFFF

/* ── Serialize buffer ─────────────────────────────────────────────────── */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    int      error;
} SerBuf;

static void ser_reserve(SerBuf *b, size_t extra)
{
    if (b->error) return;
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
    if (!nd) { b->error = 1; return; }
    b->data = nd;
    b->cap = ncap;
}

static void ser_u8(SerBuf *b, uint8_t v)  { ser_reserve(b, 1); if (!b->error) b->data[b->len++] = v; }
static void ser_u16(SerBuf *b, uint16_t v)
{
    ser_reserve(b, 2); if (b->error) return;
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
}
static void ser_u32(SerBuf *b, uint32_t v)
{
    ser_reserve(b, 4); if (b->error) return;
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 24) & 0xFF);
}
static void ser_f64(SerBuf *b, double v)
{
    uint64_t bits; memcpy(&bits, &v, sizeof(bits));
    ser_u32(b, (uint32_t)(bits & 0xFFFFFFFF));
    ser_u32(b, (uint32_t)(bits >> 32));
}
static void ser_str(SerBuf *b, const char *s)
{
    if (!s) { ser_u32(b, 0); return; }
    size_t n = strlen(s);
    if (n > 0xFFFFFFFF) n = 0xFFFFFFFF;
    ser_u32(b, (uint32_t)n);
    ser_reserve(b, n);
    if (!b->error && n) { memcpy(b->data + b->len, s, n); b->len += n; }
}
/* Operators live in fixed char[16]; store the meaningful prefix (<=15 bytes). */
static void ser_op(SerBuf *b, const char *op)
{
    size_t n = 0;
    while (n < 15 && op && op[n]) n++;
    ser_u8(b, (uint8_t)n);
    ser_reserve(b, n);
    if (!b->error && n) { memcpy(b->data + b->len, op, n); b->len += n; }
}

static void ser_node(SerBuf *b, ASTNode *node);
static void ser_node_list(SerBuf *b, ASTNode **items, int count)
{
    ser_u32(b, (uint32_t)(count > 0 ? count : 0));
    for (int i = 0; i < count; i++) ser_node(b, items[i]);
}

static void ser_node(SerBuf *b, ASTNode *node)
{
    if (!node) { ser_u16(b, LR_AST_NULL_NODE); return; }
    ser_u16(b, (uint16_t)node->type);
    ser_u16(b, (uint16_t)node->token.type);
    ser_u32(b, (uint32_t)node->token.line);
    ser_u32(b, (uint32_t)node->token.col);
    ser_f64(b, node->token.num_val);

    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
        ser_node_list(b, node->u.list.items, node->u.list.count);
        break;
    case AST_EXPR_STMT:
        ser_node(b, node->u.expr_stmt.expr);
        break;
    case AST_IF:
    case AST_WHILE:
    case AST_DO_WHILE:
        ser_node(b, node->u.if_stmt.cond);
        ser_node(b, node->u.if_stmt.body);
        ser_node(b, node->u.if_stmt.else_body);
        break;
    case AST_FOR:
        ser_node(b, node->u.for_stmt.init);
        ser_node(b, node->u.for_stmt.test);
        ser_node(b, node->u.for_stmt.update);
        ser_node(b, node->u.for_stmt.body);
        break;
    case AST_FOR_IN:
        ser_node(b, node->u.for_in.each);
        ser_node(b, node->u.for_in.source);
        ser_node(b, node->u.for_in.body);
        break;
    case AST_FOR_OF:
        ser_node(b, node->u.for_of.each);
        ser_node(b, node->u.for_of.source);
        ser_node(b, node->u.for_of.body);
        break;
    case AST_SWITCH:
        ser_node(b, node->u.switch_stmt.test);
        ser_node_list(b, node->u.switch_stmt.cases, node->u.switch_stmt.ncases);
        break;
    case AST_CASE:
    case AST_DEFAULT:
        ser_node(b, node->u.if_stmt.cond);
        ser_node(b, node->u.if_stmt.body);
        break;
    case AST_BREAK:
        ser_node(b, node->u.break_stmt.label);
        break;
    case AST_CONTINUE:
        ser_node(b, node->u.continue_stmt.label);
        break;
    case AST_RETURN:
        ser_node(b, node->u.return_stmt.arg);
        break;
    case AST_THROW:
        ser_node(b, node->u.throw_stmt.arg);
        break;
    case AST_TRY:
        ser_node(b, node->u.try_stmt.body);
        ser_node(b, node->u.try_stmt.catch_body);
        ser_node(b, node->u.try_stmt.finally_body);
        ser_str(b, node->u.try_stmt.catch_var);
        break;
    case AST_LABEL:
        ser_node(b, node->u.label_stmt.label);
        ser_node(b, node->u.label_stmt.stmt);
        break;
    case AST_WITH:
        ser_node(b, node->u.with_stmt.obj);
        ser_node(b, node->u.with_stmt.body);
        break;
    case AST_DEBUGGER:
        break;
    case AST_VAR_DECL:
        ser_node_list(b, node->u.var_decl.vars, node->u.var_decl.nvars);
        break;
    case AST_VAR_DECLARATOR:
        ser_node(b, node->u.declarator.var);
        ser_node(b, node->u.declarator.init);
        break;
    case AST_FUNC_DECL:
    case AST_FUNC_EXPR:
        ser_str(b, node->u.func.name);
        ser_node_list(b, node->u.func.params, node->u.func.nparams);
        ser_node(b, node->u.func.key_expr);
        ser_node(b, node->u.func.body);
        ser_u8(b, (uint8_t)node->u.func.is_async);
        ser_u8(b, (uint8_t)node->u.func.is_generator);
        ser_u8(b, (uint8_t)node->u.func.is_static);
        ser_u8(b, (uint8_t)node->u.func.is_getter);
        ser_u8(b, (uint8_t)node->u.func.is_setter);
        /* class_node is a non-owning back-pointer; restored on deserialize */
        break;
    case AST_ARROW:
        ser_str(b, node->u.arrow.name);
        ser_node_list(b, node->u.arrow.params, node->u.arrow.nparams);
        ser_node(b, node->u.arrow.body);
        ser_u8(b, (uint8_t)node->u.arrow.is_async);
        break;
    case AST_CLASS_DECL:
        ser_str(b, node->u.class_decl.name);
        ser_node(b, node->u.class_decl.extends);
        ser_node_list(b, node->u.class_decl.methods, node->u.class_decl.nmethods);
        break;
    case AST_CLASS_BODY:
        ser_node_list(b, node->u.class_body.methods, node->u.class_body.nmethods);
        break;
    case AST_LITERAL: {
        /* Serialize an explicit type tag so the deserializer can pick the
         * correct value reader AND restore the exact lexical token type. The
         * node's token.type is NOT reliable on the receiving side (it is
         * overwritten with the AST node-type enum during deserialization), so a
         * discriminator is required. 0=bool, 1=string, 2=number, 3=null,
         * 4=undefined. Without it a bool (ser_u32, 4 bytes) would be read back
         * as an f64 (8 bytes), misaligning the whole stream and corrupting every
         * following node; and null/undefined (which the parser stores as num=0
         * / num=-1) would otherwise collapse into TOK_NUMBER and evaluate as
         * int32(0)/int32(-1) on warm runs. */
        uint8_t ltag;
        switch (node->token.type) {
            case TOK_BOOL_LIT:      ltag = 0; break;
            case TOK_STRING:        ltag = 1; break;
            case TOK_NULL_LIT:      ltag = 3; break;
            case TOK_UNDEFINED_LIT: ltag = 4; break;
            default:                ltag = 2; break; /* TOK_NUMBER + safety */
        }
        ser_u8(b, ltag);
        if (ltag == 0)      ser_u32(b, (uint32_t)node->u.bool_val.val);
        else if (ltag == 1) ser_str(b, node->u.string.str);
        else if (ltag == 2) ser_f64(b, node->u.number.num);
        /* ltag 3 (null) and 4 (undefined) carry no extra payload */
        break;
    }
    case AST_IDENTIFIER:
        ser_str(b, node->u.ident.name);
        break;
    case AST_THIS:
    case AST_SUPER:
        break;
    case AST_BINARY:
        ser_node(b, node->u.binary.left);
        ser_node(b, node->u.binary.right);
        ser_op(b, node->u.binary.op);
        break;
    case AST_UNARY:
        ser_node(b, node->u.unary.arg);
        ser_op(b, node->u.unary.op);
        ser_u8(b, (uint8_t)node->u.unary.prefix);
        break;
    case AST_CONDITIONAL:
        ser_node(b, node->u.conditional.cond);
        ser_node(b, node->u.conditional.consequent);
        ser_node(b, node->u.conditional.alternate);
        break;
    case AST_CALL:
    case AST_OPTIONAL_CALL:
        ser_node(b, node->u.call.callee);
        ser_node_list(b, node->u.call.args, node->u.call.argc);
        ser_u8(b, (uint8_t)node->u.call.is_optional);
        break;
    case AST_NEW:
        ser_node(b, node->u.new_expr.callee);
        ser_node_list(b, node->u.new_expr.args, node->u.new_expr.argc);
        break;
    case AST_MEMBER:
    case AST_COMPUTED_MEMBER:
    case AST_OPTIONAL_MEMBER:
        ser_node(b, node->u.member.obj);
        ser_node(b, node->u.member.prop);
        ser_u8(b, (uint8_t)node->u.member.is_optional);
        break;
    case AST_ASSIGN:
        ser_node(b, node->u.assign.target);
        ser_node(b, node->u.assign.value);
        ser_op(b, node->u.assign.op);
        break;
    case AST_SEQUENCE:
        ser_node_list(b, node->u.sequence.exprs, node->u.sequence.count);
        break;
    case AST_ARRAY:
        ser_node_list(b, node->u.array.elements, node->u.array.nelem);
        break;
    case AST_OBJECT:
        ser_node_list(b, node->u.object.props, node->u.object.nprops);
        break;
    case AST_PROPERTY:
        ser_node(b, node->u.property.key);
        ser_node(b, node->u.property.val);
        ser_u8(b, (uint8_t)node->u.property.computed);
        ser_u8(b, (uint8_t)node->u.property.shorthand);
        ser_u8(b, (uint8_t)node->u.property.is_static);
        break;
    case AST_SPREAD:
    case AST_SPREAD_ELEMENT:
        ser_node(b, node->u.spread.arg);
        break;
    case AST_TEMPLATE:
    case AST_TAGGED_TEMPLATE: {
        ser_u32(b, (uint32_t)node->u.template_lit.nparts);
        for (int i = 0; i < node->u.template_lit.nparts; i++)
            ser_str(b, node->u.template_lit.parts[i]);
        ser_node(b, node->u.template_lit.tag);
        ser_u32(b, (uint32_t)node->u.template_lit.nexp);
        for (int i = 0; i < node->u.template_lit.nexp; i++)
            ser_node(b, node->u.template_lit.exprs[i]);
        break;
    }
    case AST_PATTERN:
        ser_node_list(b, node->u.pattern_array.elements, node->u.pattern_array.nelem);
        ser_u8(b, (uint8_t)node->u.pattern_array.is_object);
        break;
    case AST_REST:
        ser_node(b, node->u.rest_elem.arg);
        break;
    case AST_DEFAULT_VALUE:
        ser_node(b, node->u.default_val.left);
        ser_node(b, node->u.default_val.right);
        break;
    case AST_AWAIT:
        ser_node(b, node->u.await_expr.arg);
        break;
    case AST_YIELD:
        ser_node(b, node->u.yield_expr.arg);
        ser_u8(b, (uint8_t)node->u.yield_expr.is_delegate);
        break;
    case AST_IMPORT:
        ser_node_list(b, node->u.import_decl.specifiers, node->u.import_decl.nspec);
        ser_node(b, node->u.import_decl.source);
        break;
    case AST_EXPORT:
        ser_node_list(b, node->u.export_decl.specifiers, node->u.export_decl.nspec);
        ser_node(b, node->u.export_decl.source);
        ser_u8(b, (uint8_t)node->u.export_decl.is_default);
        break;
    case AST_EXPORT_NAMED:
        /* export_spec view: local name + exported alias node */
        ser_str(b, node->u.export_spec.name);
        ser_node(b, node->u.export_spec.exported);
        break;
    case AST_EXPORT_DEFAULT:
        ser_node(b, node->u.export_default.value);
        break;
    case AST_EXPORT_ALL:
        ser_node(b, node->u.export_all.source);
        break;
    case AST_IMPORT_SPECIFIER:
        ser_str(b, node->u.import_spec.name);
        ser_node(b, node->u.import_spec.local);
        ser_u8(b, (uint8_t)node->u.import_spec.is_default);
        break;
    case AST_IMPORT_NAMESPACE:
        ser_node(b, node->u.import_namespace.local);
        break;
    default:
        break;
    }
}

uint8_t *lr_ast_serialize(ASTNode *root, size_t *out_len)
{
    SerBuf b = {0};
    /* Magic + format version so incompatible / stale cache files (we used to
     * serialize a JS value, not an AST) are rejected on deserialize. */
    ser_u8(&b, 'L');
    ser_u8(&b, 'R');
    ser_u8(&b, 'A');
    ser_u8(&b, (uint8_t)3); /* format version (3: literals carry full type tag incl. null/undefined) */
    ser_node(&b, root);
    if (b.error) { free(b.data); if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = b.len;
    return b.data;
}

/* ── Deserialize ───────────────────────────────────────────────────────── */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    int            error;
} DeserState;

static uint8_t  read_u8(DeserState *st)
{
    if (st->pos + 1 > st->len) { st->error = 1; return 0; }
    return st->buf[st->pos++];
}
static uint16_t read_u16(DeserState *st)
{
    if (st->pos + 2 > st->len) { st->error = 1; return 0; }
    uint16_t v = (uint16_t)st->buf[st->pos] | ((uint16_t)st->buf[st->pos + 1] << 8);
    st->pos += 2; return v;
}
static uint32_t read_u32(DeserState *st)
{
    if (st->pos + 4 > st->len) { st->error = 1; return 0; }
    uint32_t v = (uint32_t)st->buf[st->pos] | ((uint32_t)st->buf[st->pos + 1] << 8)
               | ((uint32_t)st->buf[st->pos + 2] << 16) | ((uint32_t)st->buf[st->pos + 3] << 24);
    st->pos += 4; return v;
}
static double read_f64(DeserState *st)
{
    if (st->pos + 8 > st->len) { st->error = 1; return 0.0; }
    uint64_t bits = (uint64_t)st->buf[st->pos] | ((uint64_t)st->buf[st->pos + 1] << 8)
                  | ((uint64_t)st->buf[st->pos + 2] << 16) | ((uint64_t)st->buf[st->pos + 3] << 24)
                  | ((uint64_t)st->buf[st->pos + 4] << 32) | ((uint64_t)st->buf[st->pos + 5] << 40)
                  | ((uint64_t)st->buf[st->pos + 6] << 48) | ((uint64_t)st->buf[st->pos + 7] << 56);
    st->pos += 8; double d; memcpy(&d, &bits, sizeof(d)); return d;
}
/* Interned copy (freed by parser_free via the intern table). */
static const char *deser_str(DeserState *st, Parser *intern)
{
    uint32_t n = read_u32(st);
    if (st->error || n == 0) return "";
    if (st->pos + n > st->len) { st->error = 1; return ""; }
    const char *s = parser_intern_string(intern, (const char *)(st->buf + st->pos), n);
    st->pos += n;
    return s ? s : "";
}
/* Plain malloc'd copy (freed with free() by ast_free_ex, e.g. template parts). */
static char *deser_str_raw(DeserState *st)
{
    uint32_t n = read_u32(st);
    if (st->error || n == 0) return NULL;
    if (st->pos + n > st->len) { st->error = 1; return NULL; }
    char *s = (char *)malloc(n + 1);
    if (!s) { st->error = 1; return NULL; }
    memcpy(s, st->buf + st->pos, n); s[n] = '\0'; st->pos += n;
    return s;
}
static void deser_op(char *dst, DeserState *st)
{
    uint8_t n = read_u8(st);
    if (n > 15) n = 15;
    if (st->pos + n > st->len) { st->error = 1; n = 0; }
    for (uint8_t i = 0; i < n; i++) dst[i] = (char)st->buf[st->pos++];
    for (int i = n; i < 16; i++) dst[i] = '\0';
}

static ASTNode *deser_node(DeserState *st, Parser *intern);
static ASTNode **deser_node_list(DeserState *st, Parser *intern, int *out_count)
{
    uint32_t count = read_u32(st);
    *out_count = (int)count;
    if (count == 0) return NULL;
    ASTNode **arr = (ASTNode **)p_malloc(count * sizeof(ASTNode *));
    if (!arr) { st->error = 1; return NULL; }
    for (uint32_t i = 0; i < count; i++) arr[i] = deser_node(st, intern);
    return arr;
}

static ASTNode *deser_node(DeserState *st, Parser *intern)
{
    uint16_t t = read_u16(st);
    if (st->error) return NULL;
    if (t == LR_AST_NULL_NODE) return NULL;
    ASTNode *node = ast_alloc((ASTNodeType)t);
    if (!node) { st->error = 1; return NULL; }

    node->token.type    = (TokenType)read_u16(st);
    node->token.line    = read_u32(st);
    node->token.col     = read_u32(st);
    node->token.num_val = read_f64(st);
    node->token.str_val = NULL;
    node->token.start   = NULL;
    node->token.len     = 0;

    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
        node->u.list.items = deser_node_list(st, intern, &node->u.list.count);
        break;
    case AST_EXPR_STMT:
        node->u.expr_stmt.expr = deser_node(st, intern);
        break;
    case AST_IF:
    case AST_WHILE:
    case AST_DO_WHILE:
        node->u.if_stmt.cond      = deser_node(st, intern);
        node->u.if_stmt.body      = deser_node(st, intern);
        node->u.if_stmt.else_body = deser_node(st, intern);
        break;
    case AST_FOR:
        node->u.for_stmt.init  = deser_node(st, intern);
        node->u.for_stmt.test  = deser_node(st, intern);
        node->u.for_stmt.update = deser_node(st, intern);
        node->u.for_stmt.body  = deser_node(st, intern);
        break;
    case AST_FOR_IN:
        node->u.for_in.each   = deser_node(st, intern);
        node->u.for_in.source = deser_node(st, intern);
        node->u.for_in.body   = deser_node(st, intern);
        break;
    case AST_FOR_OF:
        node->u.for_of.each   = deser_node(st, intern);
        node->u.for_of.source = deser_node(st, intern);
        node->u.for_of.body   = deser_node(st, intern);
        break;
    case AST_SWITCH:
        node->u.switch_stmt.test  = deser_node(st, intern);
        node->u.switch_stmt.cases = deser_node_list(st, intern, &node->u.switch_stmt.ncases);
        break;
    case AST_CASE:
    case AST_DEFAULT:
        node->u.if_stmt.cond = deser_node(st, intern);
        node->u.if_stmt.body = deser_node(st, intern);
        break;
    case AST_BREAK:
        node->u.break_stmt.label = deser_node(st, intern);
        break;
    case AST_CONTINUE:
        node->u.continue_stmt.label = deser_node(st, intern);
        break;
    case AST_RETURN:
        node->u.return_stmt.arg = deser_node(st, intern);
        break;
    case AST_THROW:
        node->u.throw_stmt.arg = deser_node(st, intern);
        break;
    case AST_TRY:
        node->u.try_stmt.body       = deser_node(st, intern);
        node->u.try_stmt.catch_body = deser_node(st, intern);
        node->u.try_stmt.finally_body = deser_node(st, intern);
        /* Owned copy: ast_free_ex frees catch_var with p_free. */
        node->u.try_stmt.catch_var  = deser_str_raw(st);
        break;
    case AST_LABEL:
        node->u.label_stmt.label = deser_node(st, intern);
        node->u.label_stmt.stmt  = deser_node(st, intern);
        break;
    case AST_WITH:
        node->u.with_stmt.obj  = deser_node(st, intern);
        node->u.with_stmt.body = deser_node(st, intern);
        break;
    case AST_DEBUGGER:
        break;
    case AST_VAR_DECL:
        node->u.var_decl.vars = deser_node_list(st, intern, &node->u.var_decl.nvars);
        break;
    case AST_VAR_DECLARATOR:
        node->u.declarator.var  = deser_node(st, intern);
        node->u.declarator.init = deser_node(st, intern);
        break;
    case AST_FUNC_DECL:
    case AST_FUNC_EXPR:
        /* Owned copy: ast_free_ex frees func.name with p_free. */
        node->u.func.name       = deser_str_raw(st);
        node->u.func.params     = deser_node_list(st, intern, &node->u.func.nparams);
        node->u.func.key_expr   = deser_node(st, intern);
        node->u.func.body       = deser_node(st, intern);
        node->u.func.is_async   = (int)read_u8(st);
        node->u.func.is_generator = (int)read_u8(st);
        node->u.func.is_static  = (int)read_u8(st);
        node->u.func.is_getter  = (int)read_u8(st);
        node->u.func.is_setter  = (int)read_u8(st);
        node->u.func.class_node = NULL; /* restored by owning class below */
        break;
    case AST_ARROW:
        node->u.arrow.name    = (char *)deser_str(st, intern);
        node->u.arrow.params  = deser_node_list(st, intern, &node->u.arrow.nparams);
        node->u.arrow.body    = deser_node(st, intern);
        node->u.arrow.is_async = (int)read_u8(st);
        break;
    case AST_CLASS_DECL:
        /* Owned copy: ast_free_ex frees class name with p_free. */
        node->u.class_decl.name    = deser_str_raw(st);
        node->u.class_decl.body    = NULL;
        node->u.class_decl.extends = deser_node(st, intern);
        node->u.class_decl.methods = deser_node_list(st, intern, &node->u.class_decl.nmethods);
        /* Restore the func.class_node back-pointer for derivation `super`. */
        for (int i = 0; i < node->u.class_decl.nmethods; i++) {
            ASTNode *m = node->u.class_decl.methods ? node->u.class_decl.methods[i] : NULL;
            if (m && (m->type == AST_FUNC_DECL || m->type == AST_FUNC_EXPR))
                m->u.func.class_node = node;
        }
        break;
    case AST_CLASS_BODY:
        node->u.class_body.methods = deser_node_list(st, intern, &node->u.class_body.nmethods);
        break;
    case AST_LITERAL: {
        /* Read the explicit type tag written by ser_node (see above). Do NOT
         * branch on node->token.type here: it was set from the AST node-type
         * enum during deserialization, not the original lexical token type. */
        uint8_t ltag = read_u8(st);
        if (ltag == 0) {
            uint32_t bv = read_u32(st);
            node->token.type      = TOK_BOOL_LIT;
            /* NOTE: u.bool_val and u.number alias in the union. Set bool_val
             * only — do NOT also write u.number.num, or the double store would
             * clobber bool_val.val (low 4 bytes of 1.0 are zero), turning a
             * deserialized `true` into `false`. This matches the parsed
             * cached_true_node, which only sets bool_val.val. */
            node->u.bool_val.val  = (int)bv;
        } else if (ltag == 1) {
            node->token.type      = TOK_STRING;
            node->u.string.str    = (char *)deser_str(st, intern);
        } else if (ltag == 2) {
            node->token.type      = TOK_NUMBER;
            node->u.number.num    = read_f64(st);
        } else if (ltag == 3) {
            node->token.type      = TOK_NULL_LIT;
            node->u.number.num    = 0.0;
        } else { /* ltag == 4 */
            node->token.type      = TOK_UNDEFINED_LIT;
            node->u.number.num    = -1.0;
        }
        break;
    }
    case AST_IDENTIFIER:
        node->u.ident.name = (char *)deser_str(st, intern);
        break;
    case AST_THIS:
    case AST_SUPER:
        break;
    case AST_BINARY:
        node->u.binary.left  = deser_node(st, intern);
        node->u.binary.right = deser_node(st, intern);
        deser_op(node->u.binary.op, st);
        break;
    case AST_UNARY:
        node->u.unary.arg    = deser_node(st, intern);
        deser_op(node->u.unary.op, st);
        node->u.unary.prefix = (int)read_u8(st);
        break;
    case AST_CONDITIONAL:
        node->u.conditional.cond       = deser_node(st, intern);
        node->u.conditional.consequent = deser_node(st, intern);
        node->u.conditional.alternate  = deser_node(st, intern);
        break;
    case AST_CALL:
    case AST_OPTIONAL_CALL:
        node->u.call.callee    = deser_node(st, intern);
        node->u.call.args       = deser_node_list(st, intern, &node->u.call.argc);
        node->u.call.is_optional = (int)read_u8(st);
        break;
    case AST_NEW:
        node->u.new_expr.callee = deser_node(st, intern);
        node->u.new_expr.args   = deser_node_list(st, intern, &node->u.new_expr.argc);
        break;
    case AST_MEMBER:
    case AST_COMPUTED_MEMBER:
    case AST_OPTIONAL_MEMBER:
        node->u.member.obj         = deser_node(st, intern);
        node->u.member.prop        = deser_node(st, intern);
        node->u.member.is_optional = (int)read_u8(st);
        break;
    case AST_ASSIGN:
        node->u.assign.target = deser_node(st, intern);
        node->u.assign.value  = deser_node(st, intern);
        deser_op(node->u.assign.op, st);
        break;
    case AST_SEQUENCE:
        node->u.sequence.exprs = deser_node_list(st, intern, &node->u.sequence.count);
        break;
    case AST_ARRAY:
        node->u.array.elements = deser_node_list(st, intern, &node->u.array.nelem);
        break;
    case AST_OBJECT:
        node->u.object.props = deser_node_list(st, intern, &node->u.object.nprops);
        break;
    case AST_PROPERTY:
        node->u.property.key      = deser_node(st, intern);
        node->u.property.val      = deser_node(st, intern);
        node->u.property.computed = (int)read_u8(st);
        node->u.property.shorthand = (int)read_u8(st);
        node->u.property.is_static = (int)read_u8(st);
        break;
    case AST_SPREAD:
    case AST_SPREAD_ELEMENT:
        node->u.spread.arg = deser_node(st, intern);
        break;
    case AST_TEMPLATE:
    case AST_TAGGED_TEMPLATE: {
        node->u.template_lit.nparts = (int)read_u32(st);
        if (node->u.template_lit.nparts > 0) {
            node->u.template_lit.parts = (char **)p_malloc(node->u.template_lit.nparts * sizeof(char *));
            for (int i = 0; i < node->u.template_lit.nparts; i++)
                node->u.template_lit.parts[i] = deser_str_raw(st);
        } else {
            node->u.template_lit.parts = NULL;
        }
        node->u.template_lit.tag  = deser_node(st, intern);
        node->u.template_lit.nexp = (int)read_u32(st);
        if (node->u.template_lit.nexp > 0) {
            node->u.template_lit.exprs = (ASTNode **)p_malloc(node->u.template_lit.nexp * sizeof(ASTNode *));
            for (int i = 0; i < node->u.template_lit.nexp; i++)
                node->u.template_lit.exprs[i] = deser_node(st, intern);
        } else {
            node->u.template_lit.exprs = NULL;
        }
        break;
    }
    case AST_PATTERN: {
        int nelem = 0;
        node->u.pattern_array.elements  = deser_node_list(st, intern, &nelem);
        node->u.pattern_array.nelem    = nelem;
        node->u.pattern_array.is_object = (int)read_u8(st);
        /* pattern_object aliases pattern_array: keep both views consistent */
        node->u.pattern_object.props     = node->u.pattern_array.elements;
        node->u.pattern_object.nprops    = nelem;
        node->u.pattern_object.is_object = node->u.pattern_array.is_object;
        break;
    }
    case AST_REST:
        node->u.rest_elem.arg = deser_node(st, intern);
        break;
    case AST_DEFAULT_VALUE:
        node->u.default_val.left  = deser_node(st, intern);
        node->u.default_val.right = deser_node(st, intern);
        break;
    case AST_AWAIT:
        node->u.await_expr.arg = deser_node(st, intern);
        break;
    case AST_YIELD:
        node->u.yield_expr.arg       = deser_node(st, intern);
        node->u.yield_expr.is_delegate = (int)read_u8(st);
        break;
    case AST_IMPORT:
        node->u.import_decl.specifiers = deser_node_list(st, intern, &node->u.import_decl.nspec);
        node->u.import_decl.source     = deser_node(st, intern);
        break;
    case AST_EXPORT:
        node->u.export_decl.specifiers = deser_node_list(st, intern, &node->u.export_decl.nspec);
        node->u.export_decl.source     = deser_node(st, intern);
        node->u.export_decl.is_default = (int)read_u8(st);
        break;
    case AST_EXPORT_NAMED:
        /* export_spec view; owned copy freed with p_free by ast_free_ex. */
        node->u.export_spec.name     = deser_str_raw(st);
        node->u.export_spec.exported = deser_node(st, intern);
        break;
    case AST_EXPORT_DEFAULT:
        node->u.export_default.value = deser_node(st, intern);
        break;
    case AST_EXPORT_ALL:
        node->u.export_all.source = deser_node(st, intern);
        break;
    case AST_IMPORT_SPECIFIER:
        /* Owned copy: ast_free_ex frees import spec name with p_free. */
        node->u.import_spec.name  = deser_str_raw(st);
        node->u.import_spec.local = deser_node(st, intern);
        node->u.import_spec.is_default = (int)read_u8(st);
        break;
    case AST_IMPORT_NAMESPACE:
        node->u.import_namespace.local = deser_node(st, intern);
        break;
    default:
        break;
    }

    if (st->error) {
        ast_free_ex(node, intern);
        return NULL;
    }
    return node;
}

ASTNode *lr_ast_deserialize(const uint8_t *buf, size_t len, Parser **out_parser)
{
    /* Reject malformed / stale (pre-format-version) cache files up front. */
    if (!buf || len < 5 ||
        buf[0] != 'L' || buf[1] != 'R' || buf[2] != 'A' || buf[3] != 3) {
        return NULL;
    }

    Parser *intern = (Parser *)calloc(1, sizeof(Parser));
    if (!intern) return NULL;
    parser_init(intern, NULL);
    intern->use_pool = 0;  /* deserialized nodes are malloc'd, freed via p_free */

    DeserState st = { buf + 4, len - 4, 0, 0 };
    ASTNode *root = deser_node(&st, intern);
    if (!root || st.error) {
        if (root) ast_free_ex(root, intern);
        parser_free(intern);
        free(intern);
        return NULL;
    }
    *out_parser = intern;
    return root;
}