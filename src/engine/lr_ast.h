/*
 * LR_JS - JavaScript Engine AST and Parser
 * Pure C, ES2022-compatible AST node definitions and recursive descent parser.
 *
 * Defines the Abstract Syntax Tree node types and the parser functions
 * that tokenize and parse JavaScript source into an AST.
 */
#ifndef LR_AST_H
#define LR_AST_H

#include "lr_lexer.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── AST Node Types ──────────────────────────────────────────────────── */

typedef enum {
    AST_PROGRAM,
    AST_BLOCK,
    AST_EXPR_STMT,
    AST_IF, AST_FOR, AST_WHILE, AST_DO_WHILE,
    AST_FOR_IN, AST_FOR_OF,
    AST_SWITCH, AST_CASE, AST_DEFAULT,
    AST_BREAK, AST_CONTINUE, AST_RETURN, AST_THROW,
    AST_TRY, AST_CATCH,
    AST_LABEL, AST_WITH, AST_DEBUGGER,
    AST_VAR_DECL, AST_VAR_DECLARATOR,
    AST_FUNC_DECL, AST_FUNC_EXPR, AST_ARROW,
    AST_CLASS_DECL, AST_CLASS_BODY,
    AST_IMPORT, AST_EXPORT, AST_EXPORT_DEFAULT,
    AST_EXPORT_NAMED, AST_IMPORT_SPECIFIER,
    AST_LITERAL, AST_IDENTIFIER, AST_THIS, AST_SUPER,
    AST_BINARY, AST_UNARY, AST_CONDITIONAL,
    AST_CALL, AST_NEW, AST_MEMBER, AST_COMPUTED_MEMBER,
    AST_OPTIONAL_CALL, AST_OPTIONAL_MEMBER,
    AST_ASSIGN, AST_SEQUENCE,
    AST_ARRAY, AST_OBJECT, AST_PROPERTY,
    AST_SPREAD, AST_TEMPLATE, AST_TAGGED_TEMPLATE,
    AST_PATTERN, AST_REST, AST_DEFAULT_VALUE,
    AST_AWAIT, AST_YIELD, AST_SPREAD_ELEMENT,
    AST_TEMPLATE_PART,
    AST_IMPORT_NAMESPACE,
    AST_EXPORT_ALL,
} ASTNodeType;

/* Forward declaration */
typedef struct ASTNode ASTNode;

/* ── AST Node ─────────────────────────────────────────────────────────── */

struct ASTNode {
    ASTNodeType type;
    Token token;
    union {
        /* Literals */
        struct { double num; } number;
        struct { char *str; } string;
        struct { int val; } bool_val;
        struct { int64_t val; } bigint_val;

        /* Identifiers */
        struct { char *name; } ident;

        /* Lists */
        struct { ASTNode **items; int count; } list;

        /* Statements */
        struct { ASTNode *init, *test, *update, *body; } for_stmt;
        struct { ASTNode *cond, *body, *else_body; } if_stmt;
        struct { ASTNode *body; } block;
        struct { ASTNode *expr; } expr_stmt;
        struct { ASTNode *label; } break_stmt;
        struct { ASTNode *label; } continue_stmt;
        struct { ASTNode *arg; } return_stmt;
        struct { ASTNode *arg; } throw_stmt;
        struct { ASTNode *body, *catch_body, *finally_body; char *catch_var; } try_stmt;
        struct { ASTNode *test, *fallback, **cases; int ncases; } switch_stmt;
        struct { ASTNode *source, *each, *body; } for_in;
        struct { ASTNode *source, *each, *body; } for_of;
        struct { ASTNode *label; ASTNode *stmt; } label_stmt;
        struct { ASTNode *obj; ASTNode *body; } with_stmt;

        /* Declarations */
        struct { ASTNode **vars; int nvars; } var_decl;
        struct { ASTNode *var, *init; } declarator;
        struct { char *name; ASTNode *body, **params; int nparams; int is_async; int is_generator;
                 int is_static; int is_getter; int is_setter;
                 ASTNode *key_expr;    /* computed method key: [expr]() {} (owned) */
                 ASTNode *class_node;  /* back-pointer to owning class (NOT owned) */ } func;
        struct { char *name; ASTNode *body, **params; int nparams; int is_async; } arrow;
        struct { char *name; ASTNode *body; ASTNode *extends; ASTNode **methods; int nmethods; } class_decl;
        struct { ASTNode **methods; int nmethods; } class_body;
        struct { ASTNode *key; ASTNode *value; int is_static; int is_getter; int is_setter; char *name; } method;

        /* Expressions */
        struct { ASTNode *left, *right; char op[16]; } binary;
        struct { ASTNode *arg; char op[16]; int prefix; } unary;
        struct { ASTNode *cond, *consequent, *alternate; } conditional;
        struct { ASTNode *callee, **args; int argc; int is_optional; } call;
        struct { ASTNode *callee, **args; int argc; } new_expr;
        struct { ASTNode *obj, *prop; int is_optional; } member;
        struct { ASTNode *obj, *prop; } computed;
        struct { ASTNode *target, *value; char op[16]; } assign;
        struct { ASTNode **exprs; int count; } sequence;

        /* Array/Object literals */
        struct { ASTNode **elements; int nelem; } array;
        struct { ASTNode **props; int nprops; } object;
        struct { ASTNode *key, *val; int computed; int shorthand; int is_static; } property;
        struct { ASTNode *arg; } spread;

        /* Patterns (the two structs intentionally alias each other in the
         * union; is_object discriminates array vs object patterns) */
        struct { ASTNode **elements; int nelem; int is_object; } pattern_array;
        struct { ASTNode **props; int nprops; int is_object; } pattern_object;
        struct { ASTNode *arg; } rest_elem;
        struct { ASTNode *left, *right; } default_val;

        /* Template literals.
         * `parts` holds the nexp+1 verbatim text fragments between interpolations.
         * For plain templates `tag` is NULL; for tagged templates `tag` is the
         * callee expression node. cooked text is derived by unescaping `parts`. */
        struct {
            char    **parts;  /* nexp+1 verbatim source fragments */
            int       nparts;
            ASTNode  *tag;    /* tagged template callee (NULL for plain) */
            ASTNode **exprs;
            int       nexp;
        } template_lit;

        /* Import/Export */
        struct { ASTNode **specifiers; int nspec; ASTNode *source; } import_decl;
        struct { char *name; ASTNode *local; int is_default; } import_spec;
        struct { ASTNode *local; } import_namespace;
        struct { ASTNode **specifiers; int nspec; ASTNode *source; int is_default; } export_decl;
        struct { ASTNode *value; } export_default;
        struct { char *name; ASTNode *exported; } export_spec;
        struct { ASTNode *source; } export_all;

        /* Await/Yield */
        struct { ASTNode *arg; } await_expr;
        struct { ASTNode *arg; int is_delegate; } yield_expr;
    } u;
};

/* ── AST Node Pool ──────────────────────────────────────────────────────── */

#define LR_AST_NODE_POOL_SIZE 4096

typedef struct {
    ASTNode  nodes[LR_AST_NODE_POOL_SIZE];
    int      count;       /* number of nodes used so far */
    int      capacity;    /* allocated count (from pool) */
} ASTNodePool;

/* ── String Intern Table ───────────────────────────────────────────────── */

#define LR_STRING_INTERN_SIZE 256

typedef struct LRStringIntern {
    char   *str;
    size_t  len;
    struct LRStringIntern *next; /* hash chain */
} LRStringIntern;

/* ── Parser ───────────────────────────────────────────────────────────── */

typedef struct Parser {
    Lexer *lexer;
    int has_error;
    char error_msg[256];
    size_t error_line;
    size_t error_col;
    ASTNodePool   node_pool;       /* pre-allocated AST node pool */
    int           use_pool;        /* 1 = use pool, 0 = fallback to malloc */
    LRStringIntern *intern_table[LR_STRING_INTERN_SIZE]; /* string intern hash table */
} Parser;

/* ── Parser Functions ─────────────────────────────────────────────────── */

/* Initialize parser */
void parser_init(Parser *parser, Lexer *lexer);

/* Parse a complete program (top-level) */
ASTNode *parse_program(Parser *parser);

/* Parse a single statement */
ASTNode *parse_statement(Parser *parser);

/* Parse an expression */
ASTNode *parse_expression(Parser *parser);

/* Parse an assignment expression (lowest precedence except comma) */
ASTNode *parse_assignment_expr(Parser *parser);

/* Free an AST node and all its children.
 * If parser is non-NULL, pool-allocated nodes are handled correctly. */
void ast_free_ex(ASTNode *node, Parser *parser);
/* Free an AST node (backward-compatible, assumes no pool) */
static inline void ast_free(ASTNode *node) { ast_free_ex(node, NULL); }

/* Get error message */
const char *parser_get_error(Parser *parser, size_t *line, size_t *col);

/* Free parser resources (intern table, resets pool) */
void parser_free(Parser *parser);

/* AST node creation helpers */
ASTNode *ast_alloc(ASTNodeType type);
ASTNode *ast_literal_number(Parser *parser, double num);
ASTNode *ast_literal_string(Parser *parser, const char *str);
ASTNode *ast_literal_bool(Parser *parser, int val);
ASTNode *ast_literal_bigint(Parser *parser, int64_t val);
ASTNode *ast_identifier(Parser *parser, const char *name);
ASTNode *ast_binary(ASTNode *left, ASTNode *right, const char *op);
ASTNode *ast_unary(ASTNode *arg, const char *op, int prefix);

/* ── AST Serialization (bytecode cache) ─────────────────────────────────
 * Serialize the parse tree to a flat byte buffer so it can be persisted to
 * disk (.lrfile) and re-executed later without re-lexing/re-parsing. */
uint8_t *lr_ast_serialize(ASTNode *root, size_t *out_len);
ASTNode *lr_ast_deserialize(const uint8_t *buf, size_t len, Parser **out_parser);

#ifdef __cplusplus
}
#endif

#endif /* LR_AST_H */