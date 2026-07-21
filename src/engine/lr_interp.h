/*
 * LR_JS - JavaScript Engine AST Tree-Walking Interpreter
 * Pure C, ES2022-compatible.
 *
 * Evaluates AST nodes produced by the parser using a tree-walking approach.
 * Manages scope chains, control flow, and all JavaScript semantics.
 */
#ifndef LR_INTERP_H
#define LR_INTERP_H

#include "lr_ast.h"
#include "lr_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Scope ─────────────────────────────────────────────────────────────── */

typedef struct InterpScope {
    struct InterpScope *parent;
    char             **names;
    LRValue           *values;
    int               *is_const;    /* 1 = const declaration */
    int                count;
    int                capacity;
    int                is_function_scope;
    int                is_global_scope;
} InterpScope;

/* ── Inline Cache for Property Access ──────────────────────────────────── */

#define LR_IC_SIZE 64

typedef struct {
    const char *prop_name;   /* cached property name pointer */
    LRString   *prop_atom;   /* cached atom (for fast lookup) */
    int         hit_count;   /* how many times this cache has been used */
    int         is_active;   /* whether the cache slot is valid */
} LRInlineCache;

/* ── Interpreter State ─────────────────────────────────────────────────── */

typedef struct {
    LRContext   *ctx;
    InterpScope *global_scope;
    InterpScope *current_scope;
    int          break_target;      /* non-zero during break */
    int          continue_target;   /* non-zero during continue */
    int          return_target;     /* non-zero during return */
    LRValue      return_value;
    int          has_returned;
    int          error_flag;
    char         error_message[512];
    int          is_module;
    int          depth;             /* call depth for stack limit */
    /* For try/catch: exception state */
    int          exception_pending;
    LRValue      exception_value;
    /* Inline cache for property access (direct threading optimization) */
    LRInlineCache member_cache[LR_IC_SIZE];
    int           cache_index;      /* round-robin index for cache replacement */
} Interpreter;

/* ── API ───────────────────────────────────────────────────────────────── */

/* Initialize interpreter */
void interp_init(Interpreter *interp, LRContext *ctx, int is_module);

/* Evaluate an AST node and return the result value.
 * Caller must free the returned value with lr_free_value. */
LRValue interp_eval(Interpreter *interp, ASTNode *node);

/* Free all resources held by the interpreter */
void interp_free(Interpreter *interp);

#ifdef __cplusplus
}
#endif

#endif /* LR_INTERP_H */