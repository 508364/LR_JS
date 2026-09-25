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
#include "../lr_platform.h"

#include <string.h>  /* memcpy for scope_new_inline_fast */

/* ── Generator lazy-driver state ──────────────────────────────────────────
 * Stored in the generator object's opaque pointer.  Each next() performs
 * a single yield incrementally (lazy driven), with a small result-object
 * pool avoiding per-call allocation of {value, done}. */
#define GEN_RESULT_POOL_SIZE 16

typedef struct LREagerGenData {
    ASTNode     *body;      /* AST_BLOCK: function body statements */
    struct InterpScope *scope; /* scope at creation time */
    int          pc;        /* next statement index in body (lazy mode) */
    int          done;
    LRContext   *ctx;       /* needed by scope_release at cleanup */
    int          freed;     /* guard against double-free */
    LRValue      ret;       /* generator return value */
    LRValue      items;     /* buffered yields (same array as GEN_ITEMS_PROP) */
    int32_t      count;     /* number of buffered yields */
    int32_t      idx;       /* next index to drain */
    /* Result object pool: small fixed-size array of cached {value, done}
     * objects.  gen_make_result recycles from this pool when non-full,
     * otherwise falls back to lr_new_object().  No per-call malloc. */
    LRValue      result_pool[GEN_RESULT_POOL_SIZE];
    int          result_pool_count;  /* how many slots are currently checked out */
    /* Lazy yield tracking */
    int          yield_target;   /* which yield number we're waiting for (0-based) */
} LREagerGenData;

/* ── Cached debug env-flag (getenv is a locked env scan on MSVCRT; calling
 * it per bytecode-instruction/call in hot loops measured ~7-14µs each on
 * Windows.  Each flag is read once on first use. ───────────────────────── */
static inline int lr_env_flag(int *flag, const char *name)
{
    if (LR_UNLIKELY(*flag < 0)) *flag = (getenv(name) != NULL);
    return *flag;
}

/* ── Fast value release (inlineable) ─────────────────────────────────────
 * Only heap-backed tags need a call.  Inlines the refcount decrement for
 * the common case (refcount > 0 after decrement) and only calls
 * lr_free_value() for the edge case where the refcount reaches 0
 * (triggering GC work).  This avoids the function call overhead for every
 * object/string reference release in hot paths like scope destruction —
 * primitive (int32/float64/bool) scope slots, which dominate small-function
 * parameter scopes, become no-ops.  Duplicated from lr_bytecode.c and
 * lr_interp.c — we consolidate here. */
#define FREE_IF_HEAP(ctx, v) do {                                      \
    int _tag = (v).tag;                                                \
    if (unlikely(_tag == LR_TYPE_OBJECT)) {                            \
        LRObject *_o = (LRObject *)(v).u.ptr;                          \
        if (unlikely(_o && --_o->ref_count <= 0))                      \
            lr_free_value(ctx, v);                                     \
    } else if (unlikely(_tag == LR_TYPE_STRING)) {                     \
        LRString *_s = (LRString *)(v).u.ptr;                          \
        if (unlikely(_s && --_s->ref_count <= 0))                      \
            lr_free_value(ctx, v);                                     \
    } else if (unlikely(_tag == LR_TYPE_SYMBOL)) {                     \
        lr_free_value(ctx, v);                                         \
    }                                                                  \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

/* ── Scope ─────────────────────────────────────────────────────────────── */

/* Hot fields at the top, cold fields at the bottom.
 * Frequently accessed together (parent, names, values, count, cache_gen)
 * live in the first cache line.  Read-only/rarely-modified fields are
 * packed to minimise working-set footprint. */
typedef struct InterpScope {
    /* ── Hot path: accessed on every var lookup ───────────────────────── */
    struct InterpScope *parent;       /* 8  */
    char             **names;         /* 8  */
    LRValue           *values;        /* 8  */
    int                count;         /* 4  */
    int                capacity;      /* 4  */
    uint32_t           cache_gen;     /* 4  (32 bytes so far) */
    /* ── Warm path: accessed on scope creation / release ──────────────── */
    int               *is_const;      /* 8  */
    int               *is_lexical;    /* 8  */
    int                refcount;      /* 4  */
    int                packed_alloc;  /* 4  (56 bytes so far) */
    /* ── Cold path: flags set once at creation, rarely read ───────────── */
    int                is_function_scope;  /* 4 */
    int                is_global_scope;    /* 4 */
    int                mirror_globals;     /* 4 */
    int                borrowed_count;     /* 4 — first N names are borrowed
                                           * from AST, skip free in release */
} LR_CACHE_ALIGNED InterpScope;

/* ── Inline Cache for Property Access ──────────────────────────────────── */

#define LR_IC_SIZE 64

typedef struct {
    const char *prop_name;   /* cached property name pointer */
    LRString   *prop_atom;   /* cached atom (for fast lookup) */
    LRShape    *shape;       /* cached shape pointer (hidden class) */
    uint32_t    slot_index;  /* cached slot index in props[] (O(1) access) */
    uint32_t    shape_version; /* shape->version at cache time (invalidation) */
    int         hit_count;   /* how many times this cache has been used */
    int         is_active;   /* whether the cache slot is valid */
} LR_CACHE_ALIGNED LRInlineCache;

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
    /* Generator support (lazy evaluation): each next() executes body
     * incrementally, pausing at each yield. */
    int           gen_active;
    LRValue       gen_items;    /* JS array of yielded values */
    int           gen_count;
    /* Lazy mode: when non-zero, gen_next runs body incrementally
     * instead of eagerly executing all statements at once. */
    int           gen_lazy;      /* 1 = lazy yield-driven mode */
    int           gen_resume_pc; /* next statement index in body */
    /* Yield pending: set by eval_yield_expr in lazy mode to signal
     * eval_block to pause execution after the current yield. */
    int           yield_pending;
    /* Execution timeout (ms). 0 = no limit. Checked every N statements. */
    int           timeout_ms;
    int           stmt_counter;
    /* Global object property cache: small LRU of frequently-accessed
     * global names → values, for O(1) miss in scope chain.
     * Aligned to avoid false sharing with adjacent fields. */
    #define GLOBAL_CACHE_SIZE 16
    LR_CACHE_ALIGNED struct { const char *name; LRValue val; } global_cache[GLOBAL_CACHE_SIZE];
    /* Math object reference (set during init, used by BC_CALL fast path) */
    LRObject    *math_obj;
    /* NOTE: Function scope cache was removed because it caused recursive
     * function calls to corrupt parameter values (shared scope between
     * outer and inner calls).  The scope pool + scope_declare_name_direct
     * already provide sufficient performance. */
    /* Scope lookup cache: maps (name_hash, scope_ptr) → index in scope->names[].
     * Avoids O(n) strcmp linear search on every variable access.
     * Invalidate by setting scope_cache_gen when current_scope changes. */
    #define SCOPE_CACHE_SIZE 256
    struct { intptr_t key; int index; } scope_cache[SCOPE_CACHE_SIZE];
    uint32_t scope_cache_gen; /* incremented when scope chain changes */

    /* ── JIT runtime state (accessed from native JIT code) ───────────── */
    int           jit_loop_counter;     /* loop iteration counter for hot loop detection */
    LRValue      *jit_stack_base;       /* JIT stack base (r12 value) for BC_CALL inline */
    int           jit_sp;               /* JIT stack pointer (r13 value) */
    void         *jit_code_ptr;         /* current LRJITCode* for JIT runtime access */
    LRValue      *jit_args_base;        /* pre-staged arg spill base for recursive JIT calls */
} Interpreter;

/* ── API ───────────────────────────────────────────────────────────────── */

/* Initialize interpreter */
void interp_init(Interpreter *interp, LRContext *ctx, int is_module);

/* Evaluate an AST node and return the result value.
 * Caller must free the returned value with lr_free_value. */
LRValue interp_eval(Interpreter *interp, ASTNode *node);

/* Free all resources held by the interpreter */
void interp_free(Interpreter *interp);

/* Drain the thread-local scope pool: free all cached scopes.
 * Safe to call multiple times; idempotent when pool is empty. */
void interp_drain_scope_pool(void);

/* Generator lazy-data destructor.  Releases the saved scope and
 * frees the GenLazyData struct.  Exposed so that lr_runtime_free
 * can identify generator objects and nullify their scope reference
 * before freeing the interpreter scope chain. */
void gen_lazy_data_free(void *ptr);

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

/* Does the given AST subtree reference the `arguments` object?
 * The bytecode inline-call fast path does not bind `arguments`, so any
 * function body returning 1 here must take the non-inlined call path. */
int lr_ast_scans_arguments(ASTNode *n);

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

/* Invoke a callable AST node (function, arrow, or class). */
LRValue interp_invoke_function_ast(Interpreter *interp, ASTNode *ast,
                                   LRValue this_val, int argc, LRValue *argv);

/* Direct call to a JS function body (skips interp_invoke_function_ast wrapper).
 * Used by the bytecode VM's BC_CALL fast path for LR_OBJ_FUNCTION. */
LRValue interp_call_function(Interpreter *interp, ASTNode *func_node,
                              LRValue this_val, int argc, LRValue *argv);

/* Lightweight function call path for the bytecode VM.
 * Skips call frame push/pop, function name extraction, and lazy error
 * save/restore that are unnecessary for the bytecode execution path.
 * closure_scope is passed directly (not via interp->pending_closure).
 * Generator and async functions should use interp_call_function instead. */
LRValue interp_bc_call_function(Interpreter *interp, ASTNode *func_node,
                                InterpScope *closure_scope, LRValue this_val,
                                int argc, LRValue *argv);

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

/* ── Scope API (used by the bytecode VM for inline function calls) ───── */
InterpScope *scope_new(InterpScope *parent, int is_function, int is_global);
void         scope_release(InterpScope *scope, LRContext *ctx);
void         scope_declare_name_direct(InterpScope *scope, const char *name,
                                       LRValue value, int kind);

/* Optimized scope creation for bytecode inline call path.
 * Creates a function scope with `count` entries (this + params) and
 * directly populates all arrays in one shot.  Values are MOVED (not dup'd).
 * names[0] should be "this", names[1..count-1] are parameter names.       */
InterpScope *scope_new_inline(InterpScope *parent, int count,
                               const char **names, LRValue *values);
/* Move-semantics variant: transfers ownership of the references in
 * `values[]` to the scope (no refcount bump).  Used by the inline-call
 * fast path to avoid per-arg dup + later free (which used to leak).     */
InterpScope *scope_new_inline_move(InterpScope *parent, int count,
                                   const char **names, LRValue *values);
/* ── Thread-local scope pool (defined in lr_interp.c) ──────────────── */
#define SCOPE_FUNC_CAP 32  /* params+this+args+super+locals */
#define SCOPE_POOL_SIZE 64 /* thread-local pool of reusable scopes */

typedef struct ScopePool {
    InterpScope *scopes[SCOPE_POOL_SIZE];
    int          count;
} ScopePool;

extern LR_THREAD_LOCAL ScopePool scope_pool;

/* ── Inline pool accessors ─────────────────────────────────────────── */
static inline InterpScope *scope_pool_pop(void) {
    if (scope_pool.count > 0) {
        return scope_pool.scopes[--scope_pool.count];
    }
    return NULL;
}
static inline void scope_pool_push(InterpScope *s) {
    s->cache_gen++;
    if (scope_pool.count < SCOPE_POOL_SIZE) {
        scope_pool.scopes[scope_pool.count++] = s;
    } else {
        if (s->packed_alloc) free(s);
    }
}

/* Ultra-lean scope for PURE inline calls (IOME586): the callee is pure,
 * so the body never does name-based lookups and the scope can never
 * escape (no closures).  Skips writing names[]/is_const[]/is_lexical[]
 * entirely (borrowed_count = count → scope_release never frees names);
 * only the values that BC_LOAD_LOCAL reads are populated.  Must be
 * released with scope_release_inline.  Inlined into the hot call path. */
static inline InterpScope *scope_new_inline_fast(InterpScope *parent, int count,
                                                  LRValue *values) {
    int cap = count < SCOPE_FUNC_CAP ? SCOPE_FUNC_CAP : count;
    InterpScope *s;

    s = scope_pool_pop();
    if (LR_LIKELY(s != NULL)) {
        s->mirror_globals = 0;
        if (LR_UNLIKELY(s->capacity < cap)) {
            free(s);
            s = NULL;
        }
    }
    if (!s) {
        size_t sz = sizeof(InterpScope) +
                    (size_t)cap * (sizeof(char *) + sizeof(LRValue) + 2 * sizeof(int));
        s = (InterpScope *)calloc(1, sz);
        if (!s) return NULL;
        char *p = (char *)(s + 1);
        s->names      = (char **)p;           p += (size_t)cap * sizeof(char *);
        s->values     = (LRValue *)p;          p += (size_t)cap * sizeof(LRValue);
        s->is_const   = (int *)p;              p += (size_t)cap * sizeof(int);
        s->is_lexical = (int *)p;
        s->packed_alloc = 1;
        s->capacity = cap;
    }

    s->parent = parent;
    if (parent) parent->refcount++;
    s->refcount = 1;
    s->is_function_scope = 1;
    s->is_global_scope = 0;
    s->count = count;
    s->borrowed_count = count;

    /* Use memcpy for the value copy (faster than loop on most compilers).
     * If values is NULL, skip the copy (caller will populate directly). */
    if (likely(values != NULL))
        memcpy(s->values, values, (size_t)count * sizeof(LRValue));
    return s;
}

/* Ultra-lean release for the scope created by scope_new_inline_fast.
 * Inlined into the hot return path.  Falls back to scope_release() for
 * captured scopes (refcount > 1) or scopes with un-borrowed names. */
static inline void scope_release_inline(InterpScope *scope, LRContext *ctx) {
    if (unlikely(scope->refcount > 1 ||
                 scope->borrowed_count != scope->count)) {
        scope_release(scope, ctx);
        return;
    }
    int cnt = scope->count;
    InterpScope *parent = scope->parent;
    scope->count = 0;
    scope->parent = NULL;
    scope->refcount = -1;   /* sentinel: never double-released */
    for (int i = 0; i < cnt; i++) {
        FREE_IF_HEAP(ctx, scope->values[i]);
        scope->values[i] = LR_VALUE_UNDEFINED;
    }
    scope_pool_push(scope);
    if (parent) {
        if (--parent->refcount <= 0)
            scope_release(parent, ctx);
    }
}
void         interp_pop_scope(Interpreter *interp);

/* Pre-populate the global scope with the global object's data properties
 * (Math, Date, JSON, console, ...) so bare identifier reads hit the
 * bytecode variable cache instead of the slow global-object lookup. */
void         interp_prepopulate_global_scope(Interpreter *interp);

/* ── BCProgram cache (used by the bytecode VM for inline function calls) */
#include "lr_bytecode.h"
BCProgram *bc_get_or_compile_body(struct ASTNode *body);
/* Function-aware variant: pre-binds "this" + parameter slots so direct
 * local-slot access matches the runtime function-scope layout. */
BCProgram *bc_get_or_compile_func(struct ASTNode *func_node);

/* Cook (unescape) a raw template-literal fragment. Caller frees. */
char *interp_bc_cook_template(const char *raw);

/* ── Parallel body precompilation (IOME586 CAS work-stealing) ─────────── */

void interp_precompile_all_bodies(ASTNode *ast);
int  interp_precompile_bodies_cas(ASTNode *ast);
int  interp_collect_all_bodies(ASTNode *ast, ASTNode ***bodies_out, int *count_out);
void *interp_compile_body_cas(ASTNode *body);

/* Iterate over all precompiled body→BCProgram pairs via callback. */
void interp_iterate_precompiled_bodies(
    void (*callback)(ASTNode *ast_body, BCProgram *prog, void *userdata),
    void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* LR_INTERP_H */