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
    int               *is_lexical;  /* 1 = let/const/class (declarative);
                                     * 0 = var/function. Global-scope var and
                                     * function bindings in Script mode are
                                     * mirrored onto the global object per
                                     * the ES spec (GlobalDeclarationInstantiation) */
    int                count;
    int                capacity;
    int                is_function_scope;
    int                is_global_scope;
    int                mirror_globals;/* for the global scope only: non-zero means
                                     * top-level var/function bindings are mirrored
                                     * onto the global object (Script mode). Zero in
                                     * module mode so top-level declarations stay
                                     * declarative (ES spec GlobalDeclarationInstantiation) */
    int                refcount;      /* scopes are refcounted so closures can
                                       * keep their defining chain alive */
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
    char         break_label[64];   /* target label of pending break ("" = innermost) */
    char         continue_label[64];/* target label of pending continue */
    const char  *pending_label;     /* label to attach to the next loop statement */
    int          return_target;     /* non-zero during return */
    LRValue      return_value;
    int          has_returned;
    int          error_flag;
    char         error_message[512];
    int          is_module;
    const char  *filename;          /* current script/module path (import.meta) */
    LRValue      import_meta;       /* lazily created per-unit import.meta object */
    LRObject    *module_ns;         /* current module namespace object; set while
                                     * a module is being evaluated so that
                                     * eval_export can populate it and eval_import
                                     * can read other modules' namespaces */
    int          depth;             /* call depth for stack limit */
    /* For try/catch: exception state */
    int          exception_pending;
    LRValue      exception_value;
    /* Inline cache for property access (direct threading optimization) */
    LRInlineCache member_cache[LR_IC_SIZE];
    int           cache_index;      /* round-robin index for cache replacement */
    /* Closure scope handoff: set right before invoking an interpreted
     * function object; consumed by interp_call_function */
    void         *pending_closure;
    /* Generator support (eager evaluation): while a generator body runs,
     * yields append to gen_items */
    int           gen_active;
    LRValue       gen_items;    /* JS array of yielded values */
    int           gen_count;
    /* Generator lazy mode: when non-zero, yields are collected into
     * gen_items as an eager substep of the current top-level statement,
     * then returned one-at-a-time by gen_next. */
    int           gen_lazy;      /* 1 = running generator lazily */
    int           gen_resume_pc; /* next statement index in body */
    /* Execution timeout (ms). 0 = no limit. Checked every N statements. */
    int           timeout_ms;
    int           stmt_counter;
    /* Global object property cache: small LRU of frequently-accessed
     * global names → values, for O(1) miss in scope chain. */
    #define GLOBAL_CACHE_SIZE 16
    struct { const char *name; LRValue val; } global_cache[GLOBAL_CACHE_SIZE];
} Interpreter;

/* ── API ───────────────────────────────────────────────────────────────── */

/* Initialize interpreter */
void interp_init(Interpreter *interp, LRContext *ctx, int is_module);

/* Evaluate an AST node and return the result value.
 * Caller must free the returned value with lr_free_value. */
LRValue interp_eval(Interpreter *interp, ASTNode *node);

/* Free all resources held by the interpreter */
void interp_free(Interpreter *interp);

/* Re-attach the interpreter's JS-call callback to the context
 * (used by the persistent interpreter in lr_engine_eval) */
void interp_reattach(Interpreter *interp, LRContext *ctx);

/* ── Bytecode VM bridge ────────────────────────────────────────────────
 * The bytecode VM (lr_bytecode.c) executes on top of the *same*
 * interpreter state: one scope chain, one error/exception state, one set
 * of closures. These entry points expose exactly the pieces of the
 * tree-walking interpreter the VM needs, so that both execution engines
 * share a single implementation of the JavaScript semantics.            */

/* Evaluate an arbitrary AST subtree with the tree-walker (VM fallback). */
LRValue interp_bc_eval_node(Interpreter *interp, ASTNode *node);

/* Identifier read. Returns 1 on success; on failure sets a ReferenceError
 * and returns 0 (*out is undefined). */
int  interp_bc_load_var(Interpreter *interp, const char *name, LRValue *out);

/* Push `this` from the current scope chain onto the stack. */
void interp_bc_push_this(Interpreter *interp, LRValue *out);

/* typeof-style read: never throws. Returns 1 if the binding exists. */
int  interp_bc_typeof_var(Interpreter *interp, const char *name, LRValue *out);

/* Assignment to an existing binding; creates an implicit global when the
 * name is unbound (sloppy-mode semantics, same as the tree-walker). */
void interp_bc_store_var(Interpreter *interp, const char *name, LRValue val);

/* Declaration. kind: 0 = var/function, 1 = let, 2 = const. */
void interp_bc_declare_var(Interpreter *interp, const char *name,
                           LRValue val, int kind);

/* Invoke a callable value (C function, interpreted function, class). */
LRValue interp_bc_call(Interpreter *interp, LRValue callee, LRValue this_val,
                       int argc, LRValue *argv);

/* `new callee(...)` */
LRValue interp_bc_construct(Interpreter *interp, LRValue callee,
                            int argc, LRValue *argv);

/* Raise a JS exception from the VM (`throw expr`). */
void interp_bc_throw(Interpreter *interp, LRValue value);

/* Lexical scope management for VM-compiled blocks. */
void interp_bc_push_scope(Interpreter *interp);
void interp_bc_pop_scope(Interpreter *interp);

/* Cook (unescape) a raw template-literal fragment. Caller frees. */
char *interp_bc_cook_template(const char *raw);

/* ── Parallel body precompilation (IOME586 CAS work-stealing) ─────────── */

void interp_precompile_all_bodies(ASTNode *ast);
int  interp_precompile_bodies_cas(ASTNode *ast);
int  interp_collect_all_bodies(ASTNode *ast, ASTNode ***bodies_out, int *count_out);
void *interp_compile_body_cas(ASTNode *body);

#ifdef __cplusplus
}
#endif

#endif /* LR_INTERP_H */