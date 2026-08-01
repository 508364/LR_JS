/*
 * LR_JS - Self-implemented JavaScript Engine
 * Pure C, ES2022-compatible bytecode interpreter
 *
 * This is a fully self-implemented JS engine. No external JS engine
 * (QuickJS, V8, etc.) source files are referenced or included.
 * Design follows standard JS engine architecture patterns.
 */
#ifndef LR_ENGINE_H
#define LR_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* AST node / parser types are defined in engine/lr_ast.h. We forward-declare
 * them here instead of including lr_ast.h, because lr_ast.h transitively
 * includes lr_lexer.h whose `TokenType` enum collides with winnt.h's
 * `TokenType` in any translation unit that also pulls in the Windows headers. */
typedef struct ASTNode ASTNode;
typedef struct Parser  Parser;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Value Types ──────────────────────────────────────────────────────── */

typedef enum {
    LR_TYPE_UNDEFINED = 0,
    LR_TYPE_NULL,
    LR_TYPE_BOOL,
    LR_TYPE_INT32,
    LR_TYPE_FLOAT64,
    LR_TYPE_STRING,
    LR_TYPE_OBJECT,
    LR_TYPE_SYMBOL,
    LR_TYPE_EXCEPTION,
} LRValueType;

/* Forward declarations */
typedef struct LRString    LRString;
typedef struct LRObject    LRObject;
typedef struct LRProperty  LRProperty;
typedef struct LRShape     LRShape;
typedef struct LRContext   LRContext;
typedef struct LRRuntime   LRRuntime;
typedef struct LRClass     LRClass;
typedef struct LRCFunction LRCFunction;
typedef struct LRClosureVar LRClosureVar;

/* LRValue: tagged value representation
 * Uses a simple struct with type tag and union payload.
 * Heap objects (strings, objects) are reference-counted. */
typedef struct LRValue {
    int32_t tag;  /* LRValueType */
    union {
        int32_t  int32;
        double   float64;
        uint8_t  bool_val;
        void    *ptr;
    } u;
} LRValue;

/* For compatibility with JSValueConst pattern */
typedef LRValue LRValueConst;

/* ── Constants ────────────────────────────────────────────────────────── */

#define LR_VALUE_UNDEFINED  ((LRValue){ .tag = LR_TYPE_UNDEFINED })
#define LR_VALUE_NULL       ((LRValue){ .tag = LR_TYPE_NULL })
#define LR_VALUE_FALSE      ((LRValue){ .tag = LR_TYPE_BOOL, .u.bool_val = 0 })
#define LR_VALUE_TRUE       ((LRValue){ .tag = LR_TYPE_BOOL, .u.bool_val = 1 })
#define LR_VALUE_EXCEPTION  ((LRValue){ .tag = LR_TYPE_EXCEPTION })

/* For QuickJS compatibility */
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* ── String ───────────────────────────────────────────────────────────── */

struct LRString {
    int32_t  ref_count;
    uint32_t len;
    uint8_t  is_atom;   /* interned atom? */
    char     str[];      /* null-terminated, flexible array */
};

/* ── Property ─────────────────────────────────────────────────────────── */

typedef enum {
    LR_PROP_NORMAL      = 0,
    LR_PROP_GETTER      = 1,
    LR_PROP_SETTER      = 2,
    LR_PROP_ACCESSOR    = 3,  /* getter + setter */
    LR_PROP_CONFIGURABLE = (1 << 2),
    LR_PROP_ENUMERABLE   = (1 << 3),
    LR_PROP_WRITABLE     = (1 << 4),
} LRPropFlags;

struct LRProperty {
    LRString    *key;       /* property name atom */
    LRValue      value;     /* value or getter function */
    LRValue      setter;    /* setter function (for accessors) */
    int32_t      flags;
    LRProperty  *next;      /* main prop_hash chain (enumeration) */
    LRProperty  *bnext;     /* bucket chain (fast lookup) */
};

/* ── Shape (hidden class) ─────────────────────────────────────────────── */

struct LRShape {
    int32_t    ref_count;
    LRString  *prop_name;   /* property added at this transition */
    LRShape   *prev;        /* previous shape in chain */
    LRShape   *next;        /* next in hash chain */
    uint32_t   slot_index;  /* property slot index */
    uint32_t   hash;
};

/* ── Object ────────────────────────────────────────────────────────────── */

typedef enum {
    LR_OBJ_PLAIN      = 0,
    LR_OBJ_ARRAY      = 1,
    LR_OBJ_FUNCTION   = 2,
    LR_OBJ_CFUNCTION  = 3,
    LR_OBJ_BYTECODE_FUNC = 4,
    LR_OBJ_ARRAY_BUFFER = 5,
    LR_OBJ_PROXY      = 6,
    LR_OBJ_PROMISE    = 7,
    LR_OBJ_REGEXP     = 8,
    LR_OBJ_DATE       = 9,
    LR_OBJ_ERROR      = 10,
    LR_OBJ_MODULE     = 11,
    LR_OBJ_TYPED_ARRAY = 12,
    LR_OBJ_DATA_VIEW   = 13,
    LR_OBJ_SCRIPT      = 14,  /* compiled script (COMPILE_ONLY result); owns an LREvalUnit */
    LR_OBJ_BIGINT      = 15,  /* BigInt: wraps int64_t in a heap object */
} LRObjectType;

struct LRObject {
    int32_t       ref_count;
    LRObjectType  type;
    LRShape      *shape;          /* current shape (hidden class) */
    LRClass      *class_def;      /* class definition */
    LRValue      *props;          /* property value array */
    uint32_t      prop_count;     /* allocated slots */
    LRValue       proto;          /* prototype */
    void         *extra;          /* type-specific data (array data, function code, etc.) */
    void         *def_scope;      /* captured lexical scope (interpreter closures) */
    void         *opaque;         /* user data (for JS_SetOpaque/JS_GetOpaque) */
    void         (*opaque_free)(void *opaque); /* destructor for opaque data, NULL = free() */
    LRContext    *ctx;            /* owning context */
    uint8_t       is_exotic;      /* has special behavior */
    uint8_t       is_extensible;  /* can add new properties */
    uint8_t       finalized;      /* finalized by GC */
    LRProperty   *prop_hash;      /* named property linked list */
    #define OBJ_PROP_BUCKETS 8
    LRProperty   *prop_buckets[OBJ_PROP_BUCKETS]; /* hash-bucketed fast lookup */
    /* GC fields */
    int           gc_mark;        /* mark bit for mark-and-sweep GC */
    struct LRObject *gc_next;     /* linked list for object tracking */
    /* Finalization / weak reference support */
    int           finalization_pending; /* deferred-free in progress */
    int           weak_ref_count;       /* number of WeakRefs targeting this object */
    struct LRObject *finalize_next;     /* pending-finalization list link */
};

/* Hook installed by the interpreter: releases a captured closure scope
 * (LRObject.def_scope) when a function object is destroyed. */
extern void (*lr_closure_scope_release)(void *scope, LRContext *ctx);

/* ── FinalizationRegistry support ─────────────────────────────────────── */

/* One registration: target -> (heldValue, callback, registry, token).
 * `target` is stored as a raw (non-owning) pointer. */
typedef struct LRFinalizationEntry {
    struct LRObject *target;   /* raw pointer to the registered target (weak) */
    LRValue          heldValue; /* strong reference to the held value */
    LRValue          callback;  /* the cleanup callback (strong) */
    LRValue          registry;  /* owning FinalizationRegistry (strong) */
    LRValue          token;     /* unregister token (or JS_UNDEFINED) */
    struct LRFinalizationEntry *next;
} LRFinalizationEntry;

/* Closure data for a scheduled finalization cleanup job. */
typedef struct LRFinalizationJobData {
    LRContext *ctx;
    LRValue    callback;
    LRValue    heldValue;
} LRFinalizationJobData;

/* ── BigInt ────────────────────────────────────────────────────────────── */

typedef struct {
    int64_t value;    /* currently 64-bit; upgradeable to arbitrary precision */
} LRBigIntData;

LRValue  lr_new_bigint(LRContext *ctx, int64_t value);
int      lr_is_bigint(LRValue v);
int      lr_to_bigint64(LRContext *ctx, int64_t *out, LRValue v);

/* ── C Function ───────────────────────────────────────────────────────── */

typedef LRValue (*LRCFunctionFunc)(LRContext *ctx, LRValue this_val,
                                   int argc, LRValue *argv);

struct LRCFunction {
    LRCFunctionFunc  func;
    const char      *name;
    int              length;  /* expected arg count */
    int              magic;   /* for magic-based dispatch */
    void            *data;    /* user data */
    void (*data_free)(void *data); /* destructor for data, NULL = don't free */
};

/* ── TypedArray Data (stored in opaque) ────────────────────────────────── */

typedef struct TypedArrayData {
    LRValue  buffer;         /* the underlying ArrayBuffer */
    size_t   byte_offset;
    size_t   byte_length;
    size_t   element_size;   /* BYTES_PER_ELEMENT */
    int      magic;          /* TA_MAGIC_* */
    const char *name;        /* e.g. "Uint8Array" */
} TypedArrayData;

/* ── DataView Data (stored in opaque) ──────────────────────────────────── */

typedef struct DataViewData {
    LRValue  buffer;
    size_t   byte_offset;
    size_t   byte_length;
} DataViewData;

/* ── Bytecode Function ────────────────────────────────────────────────── */

typedef struct LRBytecodeFunction {
    uint8_t  *bytecode;
    uint32_t  bytecode_len;
    LRValue  *constant_pool;
    uint32_t  constant_pool_size;
    int       stack_size;
    int       arg_count;
    int       var_count;
    char     *filename;
    int       line_num;
} LRBytecodeFunction;

/* ── Class ─────────────────────────────────────────────────────────────── */

struct LRClass {
    int32_t      ref_count;
    const char   *name;
    LRValue       constructor;
    LRValue       prototype;
    int32_t       class_id;
    void         *opaque;  /* for custom data */
};

/* ── Property Enumeration ──────────────────────────────────────────────── */

typedef struct LRPropertyEnum {
    LRString *atom;   /* property name as atom */
    int32_t   flags;  /* property flags */
} LRPropertyEnum;

/* ── Function List Entry ───────────────────────────────────────────────── */

typedef struct LRCFunctionListEntry {
    const char *name;
    uint8_t     prop_flags;
    uint8_t     def_type;  /* 0=func, 1=getset, 2=value */
    int16_t     magic;
    union {
        struct { uint8_t length; uint8_t cproto; LRCFunctionFunc generic; } func;
        struct { LRCFunctionFunc get; LRCFunctionFunc set; } getset;
        struct { int32_t value; } val;
    } u;
} LRCFunctionListEntry;

/* Property flags for function list entries */
#define JS_PROP_WRITABLE      (1 << 0)
#define JS_PROP_CONFIGURABLE  (1 << 1)
#define JS_PROP_ENUMERABLE    (1 << 2)
#define JS_DEF_CFUNC          0
#define JS_DEF_CGETSET        1
#define JS_DEF_PROP_VALUE     2

/* CFunction constructor flag */
#define JS_CFUNC_constructor  2
#define JS_CFUNC_generic      0
#define JS_CFUNC_generic_magic 1

/* Atom constants */
#define JS_ATOM_NULL ((LRString *)NULL)

/* Value tags */
#define JS_TAG_UNDEFINED     LR_TYPE_UNDEFINED
#define JS_TAG_NULL          LR_TYPE_NULL
#define JS_TAG_BOOL          LR_TYPE_BOOL
#define JS_TAG_INT           LR_TYPE_INT32
#define JS_TAG_FLOAT64       LR_TYPE_FLOAT64
#define JS_TAG_STRING        LR_TYPE_STRING
#define JS_TAG_OBJECT        LR_TYPE_OBJECT

/* GPN flags for GetOwnPropertyNames */
#define JS_GPN_STRING_MASK  1
#define JS_GPN_ENUM_ONLY    2

#define JS_CFUNC_DEF(name, length, func1) \
    { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, \
      .u = { .func = { length, 0, (LRCFunctionFunc)(func1) } } }
#define JS_CGETSET_DEF(name, getter, setter) \
    { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CGETSET, 0, \
      .u = { .getset = { (LRCFunctionFunc)(getter), (LRCFunctionFunc)(setter) } } }

/* ── Module Loader ─────────────────────────────────────────────────────── */

typedef struct LRModuleDef LRModuleDef;

typedef char *(*LRModuleNormalizeFunc)(LRContext *ctx,
    const char *base_name, const char *name, void *opaque);

typedef LRModuleDef *(*LRModuleLoaderFunc)(LRContext *ctx,
    const char *name, void *opaque);

/* ── Call Stack Frame ──────────────────────────────────────────────────── */

#define LR_MAX_CALL_STACK_DEPTH 256

typedef struct LRCallStackFrame {
    const char *function_name;  /* function name */
    const char *filename;       /* source filename */
    int         line_number;    /* current line number */
} LRCallStackFrame;

/* ── Context ───────────────────────────────────────────────────────────── */

struct LRContext {
    LRRuntime      *rt;
    LRContext      *next_ctx;        /* linked list for cleanup */
    LRValue          global_obj;
    LRValue          global_var_obj;
    LRValue          current_exception;
    char            *error_message;
    void            *opaque;          /* user data */
    LRValue          current_func;    /* function being called (for C function data access) */
    LRString       **atom_table;     /* interned string atoms */
    uint32_t         atom_count;
    uint32_t         atom_capacity;
    /* Direct-mapped atom hash cache.  Each entry holds the most recent
     * atom for that FNV-1a bucket. O(1) on hit, linear fallback on miss.
     * Thread-safe: read-only after initialization; LRString never freed. */
    #define ATOM_HASH_BITS 10
    #define ATOM_HASH_SIZE (1 << ATOM_HASH_BITS)
    LRString        *atom_hash[ATOM_HASH_SIZE];
    LRModuleNormalizeFunc module_normalize;
    LRModuleLoaderFunc    module_loader;
    void            *module_opaque;
    int              strict_mode;
    int              is_module;
    int              allow_await;
    /* Call stack for stack trace */
    int              call_stack_depth;
    LRCallStackFrame call_stack[LR_MAX_CALL_STACK_DEPTH];
    int              stack_trace_limit;  /* Error.stackTraceLimit, default 10 */
    /* Built-in prototypes for fast access (set during initialization) */
    LRValue          object_proto;      /* Object.prototype, used by lr_new_object */
    LRValue          array_proto;       /* Array.prototype, used by lr_new_array */
    LRValue          string_proto;      /* String.prototype */
    LRValue          number_proto;      /* Number.prototype */
    LRValue          function_proto;    /* Function.prototype */
    LRValue          bigint_proto;      /* BigInt.prototype */
    int              timeout_ms;        /* execution timeout (copied from LR_Runtime config) */
    /* Callback for calling JS interpreter functions from C builtins */
    LRValue          (*call_js_function)(struct LRContext *ctx, LRValue func,
                                         LRValue this_val, int argc, LRValue *argv);
    /* Opaque interpreter pointer for use by the callback */
    void             *opaque_interp;
    /* Persistent evaluation state (interpreter + retained ASTs/sources).
     * Keeps JS function objects callable after lr_engine_eval returns,
     * which is required for timers, workers and other async callbacks.
     * Owned by the context; freed in lr_free_context. */
    void             *persistent_interp;
    /* Resolved-module registry (LRModuleCacheEntry list), per context.
     * Managed by the module loader; freed in lr_free_context. */
    void             *module_registry;
};

/* ── Runtime ───────────────────────────────────────────────────────────── */

/* Cache / hash table size constants */
#ifndef LR_SMALL_STRING_CACHE_SIZE
#define LR_SMALL_STRING_CACHE_SIZE 64
#endif

#ifndef LR_SHAPE_CACHE_SIZE
#define LR_SHAPE_CACHE_SIZE 128
#endif

/* Shape cache entry — caches the EXACT property pointer for O(1) hit */
typedef struct LRShapeCacheEntry {
    int         valid;
    void       *obj;     /* LRObject pointer */
    LRString   *prop;    /* property atom */
    LRProperty *prop_ptr; /* cached property pointer (verified with key==atom) */
} LRShapeCacheEntry;

struct LRRuntime {
    /* Memory tracking */
    size_t       malloc_size;
    size_t       malloc_limit;
    size_t       gc_threshold;
    size_t       max_stack_size;

    /* GC */
    int          gc_mode;
    int          gc_generational;
    int          gc_incremental;
    size_t       gc_nursery_size;
    int64_t      gc_pause_target_ns;

    /* Object tracking */
    LRObject    *obj_list;       /* all live objects linked list */
    int32_t      obj_count;

    /* String tracking */
    LRString    *string_list;    /* all live strings linked list */
    int32_t      string_count;

    /* Memory stats */
    int64_t      malloc_count;
    int64_t      memory_used_count;
    int64_t      memory_used_size;
    int64_t      atom_count;
    int64_t      atom_size;
    int64_t      str_count;
    int64_t      str_size;
    int64_t      prop_count;
    int64_t      prop_size;
    int64_t      shape_count;
    int64_t      shape_size;
    int64_t      js_func_count;
    int64_t      js_func_size;
    int64_t      js_func_code_size;
    int64_t      c_func_count;
    int64_t      array_count;

    /* Job queue (for Promises) */
    struct LRJobEntry *job_list;
    struct LRJobEntry *job_list_tail;
    int          has_pending_jobs;

    /* Module loader */
    LRModuleNormalizeFunc module_normalize_func;
    LRModuleLoaderFunc    module_loader_func;
    void        *module_opaque;

    /* Context list */
    LRContext   *ctx_list;

    /* Small string cache: caches strings with length <= 32 */
    LRString    *small_string_cache[LR_SMALL_STRING_CACHE_SIZE];

    /* Shape cache for property access */
    LRShapeCacheEntry shape_cache[LR_SHAPE_CACHE_SIZE];
    int                shape_cache_index;  /* round-robin replacement index */

    /* Finalization / weak reference support */
    struct LRObject   *pending_finalize_list; /* objects awaiting finalization */
    struct LRFinalizationEntry *finalization_entries; /* registry registrations */
};

/* ── Job Entry ─────────────────────────────────────────────────────────── */

typedef struct LRJobEntry {
    LRContext       *ctx;
    LRValue          func;  /* function to call */
    struct LRJobEntry *next;
} LRJobEntry;

/* ── Module ────────────────────────────────────────────────────────────── */

struct LRModuleDef {
    LRObject  *obj;       /* module namespace object */
    char      *name;
    int        evaluated;
    LRValue    result;
    LRContext *ctx;
};

/* Resolved-module cache entry (per context). The def owns a reference to
 * def->obj (the module namespace object). */
typedef struct LRModuleCacheEntry {
    char                  *name;   /* resolved module path */
    struct LRModuleDef    *def;
    struct LRModuleCacheEntry *next;
} LRModuleCacheEntry;

/* ── Memory Usage Structure ────────────────────────────────────────────── */

typedef struct LRMemoryUsage {
    int64_t malloc_size;
    int64_t malloc_limit;
    int64_t memory_used_size;
    int64_t malloc_count;
    int64_t memory_used_count;
    int64_t atom_count, atom_size;
    int64_t str_count, str_size;
    int64_t obj_count, obj_size;
    int64_t prop_count, prop_size;
    int64_t shape_count, shape_size;
    int64_t js_func_count, js_func_size, js_func_code_size;
    int64_t c_func_count, array_count;
    int64_t fast_array_count, fast_array_elements;
    int64_t binary_object_count, binary_object_size;
} LRMemoryUsage;

/* ── Value Creation ───────────────────────────────────────────────────── */

LRValue lr_new_bool(LRContext *ctx, int val);
LRValue lr_new_int32(LRContext *ctx, int32_t val);
LRValue lr_new_float64(LRContext *ctx, double val);
LRValue lr_new_string(LRContext *ctx, const char *str);
LRValue lr_new_string_len(LRContext *ctx, const char *str, size_t len);

/* ── Object / Array Creation ──────────────────────────────────────────── */

LRValue lr_new_object(LRContext *ctx);
LRValue lr_new_object_proto(LRContext *ctx, LRValue proto);
LRValue lr_new_array(LRContext *ctx);

/* ── C Function Creation ──────────────────────────────────────────────── */

LRValue lr_new_cfunction(LRContext *ctx, LRCFunctionFunc func,
                         const char *name, int length);
LRValue lr_new_cfunction2(LRContext *ctx, LRCFunctionFunc func,
                          const char *name, int length,
                          uint8_t cproto, int magic);

/* ── Value Management ─────────────────────────────────────────────────── */

LRValue    lr_dup_value(LRContext *ctx, LRValue val);
void       lr_free_value(LRContext *ctx, LRValue val);
void       lr_free_object(LRRuntime *rt, struct LRObject *obj);

/* ── Weak Reference support ────────────────────────────────────────────── */

/* Record that a WeakRef now references `target` (does not keep it alive). */
void lr_weak_ref_retain(LRRuntime *rt, struct LRObject *target);
/* Drop one WeakRef reference to `target`; frees it if fully unreferenced. */
void lr_weak_ref_release(LRRuntime *rt, struct LRObject *target);
/* Return a strong reference to `target`, or JS_UNDEFINED if it was collected. */
LRValue lr_weak_ref_deref(LRContext *ctx, struct LRObject *target);

/* ── FinalizationRegistry support ─────────────────────────────────────── */

/* Register a target for finalization. `callback`/`heldValue`/`registry` are
 * taken (their references belong to the entry). `token` may be JS_UNDEFINED. */
void lr_register_finalization(LRRuntime *rt, struct LRObject *target,
                              LRValue callback, LRValue heldValue,
                              LRValue registry, LRValue token);
/* Remove all registrations whose unregister token is === `token`.
 * Returns the number of registrations removed. */
int lr_unregister_finalization(LRRuntime *rt, LRValue token);
const char *lr_to_cstring(LRContext *ctx, LRValue val);
void       lr_free_cstring(LRContext *ctx, const char *str);

/* ── Type Checking ────────────────────────────────────────────────────── */

int lr_is_undefined(LRValue val);
int lr_is_null(LRValue val);
int lr_is_bool(LRValue val);
int lr_is_number(LRValue val);
int lr_is_string(LRValue val);
int lr_is_object(LRValue val);
int lr_is_array(LRContext *ctx, LRValue val);
int lr_is_function(LRContext *ctx, LRValue val);
int lr_is_exception(LRValue val);
int lr_is_symbol(LRValue val);
int lr_is_array_buffer(LRContext *ctx, LRValue val);
int lr_is_promise(LRContext *ctx, LRValue val);

/* ── Type Conversion ──────────────────────────────────────────────────── */

int     lr_to_bool(LRContext *ctx, LRValue val);
int     lr_to_int32(LRContext *ctx, int32_t *pres, LRValue val);
int     lr_to_int64(LRContext *ctx, int64_t *pres, LRValue val);
int     lr_to_float64(LRContext *ctx, double *pres, LRValue val);

/* ── Proxy Data ────────────────────────────────────────────────────────── */

typedef struct LRProxyData {
    LRValue target;   /* [[ProxyTarget]] */
    LRValue handler;  /* [[ProxyHandler]] */
} LRProxyData;

/* ── Property Access ──────────────────────────────────────────────────── */

LRValue lr_get_property(LRContext *ctx, LRValue obj, LRString *atom);
LRValue lr_get_property_str(LRContext *ctx, LRValue obj, const char *name);
LRValue lr_get_property_uint32(LRContext *ctx, LRValue obj, uint32_t idx);

int     lr_set_property(LRContext *ctx, LRValue obj, LRString *atom, LRValue val);
int     lr_set_property_str(LRContext *ctx, LRValue obj, const char *name, LRValue val);
/* Mirror an external write to the global object back into the interpreter's
 * global scope (two-way global-object binding). Called from lr_set_property. */
void    interp_sync_global_binding(LRContext *ctx, const char *name, LRValue val);
int     lr_set_property_uint32(LRContext *ctx, LRValue obj, uint32_t idx, LRValue val);
/* Define an accessor (getter/setter) property. getter/setter are function
 * values (or LR_VALUE_UNDEFINED when absent). */
int     lr_set_accessor_property(LRContext *ctx, LRValue obj, LRString *atom,
                                 LRValue getter, LRValue setter);
int     lr_set_accessor_property_str(LRContext *ctx, LRValue obj, const char *name,
                                     LRValue getter, LRValue setter);

int     lr_delete_property(LRContext *ctx, LRValue obj, LRString *atom, int flags);

int     lr_define_property_value(LRContext *ctx, LRValue obj, LRString *atom,
                                  LRValue val, int flags);
int     lr_get_own_property_names(LRContext *ctx, LRPropertyEnum **ptab,
                                   uint32_t *plen, LRValue obj, int flags);
void    lr_free_property_enum(LRContext *ctx, LRPropertyEnum *tab, uint32_t len);

/* ── Has Property ──────────────────────────────────────────────────────── */

int     lr_has_property(LRContext *ctx, LRValue obj, LRString *atom);

/* ── Direct Property Access (bypass Proxy traps) ───────────────────────── */

LRValue lr_get_property_direct(LRContext *ctx, LRValue obj, LRString *atom);
int     lr_set_property_direct(LRContext *ctx, LRValue obj, LRString *atom, LRValue val);
int     lr_delete_property_direct(LRContext *ctx, LRValue obj, LRString *atom, int flags);
int     lr_has_property_direct(LRContext *ctx, LRValue obj, LRString *atom);
int     lr_get_own_property_names_direct(LRContext *ctx, LRPropertyEnum **ptab,
                                          uint32_t *plen, LRValue obj, int flags);
LRValue lr_call_direct(LRContext *ctx, LRValue func, LRValue this_val,
                       int argc, LRValue *argv);
LRValue lr_call_constructor_direct(LRContext *ctx, LRValue func,
                                    int argc, LRValue *argv);

/* ── Function Calls ───────────────────────────────────────────────────── */

LRValue lr_call(LRContext *ctx, LRValue func, LRValue this_val,
                int argc, LRValue *argv);
LRValue lr_call_constructor(LRContext *ctx, LRValue func,
                            int argc, LRValue *argv);

/* ── Property Function List ────────────────────────────────────────────── */

void lr_set_property_function_list(LRContext *ctx, LRValue obj,
    const LRCFunctionListEntry *tab, int count);

/* ── Prototype ─────────────────────────────────────────────────────────── */

LRValue lr_get_prototype(LRContext *ctx, LRValue obj);
int     lr_set_prototype(LRContext *ctx, LRValue obj, LRValue proto);

/* ── Global Object ────────────────────────────────────────────────────── */

LRValue lr_get_global_object(LRContext *ctx);

/* ── Error Handling ───────────────────────────────────────────────────── */

LRValue lr_throw_type_error(LRContext *ctx, const char *fmt, ...);
LRValue lr_throw_reference_error(LRContext *ctx, const char *fmt, ...);
LRValue lr_throw_range_error(LRContext *ctx, const char *fmt, ...);
LRValue lr_throw_syntax_error(LRContext *ctx, const char *fmt, ...);
LRValue lr_throw_internal_error(LRContext *ctx, const char *fmt, ...);
LRValue lr_get_exception(LRContext *ctx);
const char *lr_get_exception_str(LRContext *ctx);

/* ── Stack Trace Support ──────────────────────────────────────────────── */

/* Capture the current call stack, returns an array of formatted stack strings.
 * out_count will be set to the number of frames captured. */
char **lr_capture_stack_trace(LRContext *ctx, int *out_count);

/* Free a stack trace array returned by lr_capture_stack_trace */
void lr_free_stack_trace(LRContext *ctx, char **trace, int count);

/* Build a V8-style stack string from the current call stack.
 * The returned string must be freed with lr_free_cstring(). */
char *lr_build_stack_string(LRContext *ctx, const char *error_message);

/* Push/pop a call stack frame (used by interpreter) */
void lr_push_call_frame(LRContext *ctx, const char *function_name,
                         const char *filename, int line_number);
void lr_pop_call_frame(LRContext *ctx);

/* Built-in Error.captureStackTrace(target, constructorOpt) */
LRValue lr_error_capture_stack_trace(LRContext *ctx, LRValue this_val,
                                      int argc, LRValue *argv);

/* Built-in Error constructor */
LRValue lr_error_constructor(LRContext *ctx, LRValue this_val,
                              int argc, LRValue *argv);

/* Built-in getter for Error.prototype.stack */
LRValue lr_error_proto_stack_getter(LRContext *ctx, LRValue this_val,
                                     int argc, LRValue *argv);

/* ── Runtime / Context Lifecycle ──────────────────────────────────────── */

LRRuntime *lr_new_runtime(void);
void       lr_free_runtime(LRRuntime *rt);

LRContext *lr_new_context(LRRuntime *rt);
void       lr_free_context(LRContext *ctx);

void lr_set_memory_limit(LRRuntime *rt, size_t limit);
void lr_set_gc_threshold(LRRuntime *rt, size_t threshold);
void lr_set_max_stack_size(LRRuntime *rt, size_t size);

void lr_set_context_opaque(LRContext *ctx, void *opaque);
void *lr_get_context_opaque(LRContext *ctx);

/* Set/get opaque user data on a JS object. */
void  lr_set_opaque(LRValue obj, void *opaque);
void  lr_set_opaque_with_free(LRValue obj, void *opaque,
                               void (*free_func)(void *opaque));
void *lr_get_opaque(LRValue obj);

/* Define a property with getter/setter functions. */
int lr_define_property_getset(LRContext *ctx, LRValue obj, LRString *atom,
                               LRCFunctionFunc getter, LRCFunctionFunc setter, int flags);

/* ── Module Loader ────────────────────────────────────────────────────── */

void lr_set_module_loader_func(LRRuntime *rt,
    LRModuleNormalizeFunc normalize, LRModuleLoaderFunc loader, void *opaque);

/* ── Evaluation ───────────────────────────────────────────────────────── */

LRValue lr_engine_eval(LRContext *ctx, const char *input, size_t input_len,
                const char *filename, int flags);
/* Parse + (optionally serialize the AST into *out_bc / *out_bc_len) + execute.
 * If out_bc/out_bc_len are NULL, no serialization is performed. */
LRValue lr_engine_eval_source(LRContext *ctx, const char *input, size_t input_len,
                int is_module, const char *filename, uint8_t **out_bc, size_t *out_bc_len);
/* Execute an already-parsed AST (e.g. one loaded from the bytecode cache). */
LRValue lr_engine_eval_ast(LRContext *ctx, ASTNode *ast, Parser *parser,
                int is_module, const char *filename);
/* Execute a compiled-script object produced by lr_engine_eval with
 * JS_EVAL_FLAG_COMPILE_ONLY. */
LRValue lr_engine_eval_function(LRContext *ctx, LRValue func_obj);
/* Parse + execute a module from source and return its namespace object
 * (the set of exported bindings) in *out_ns. Used by the module loader so
 * that `import` can read another module's exports. */
LRValue lr_engine_run_module(LRContext *ctx, const char *input, size_t input_len,
                     const char *filename, LRValue *out_ns);
/* Build a Function object from param names + body string (new Function()). */
LRValue lr_engine_build_function(LRContext *ctx, int nparams,
                                  const char **params, const char *body);
/* AST (de)serialization used by the IOME586 result cache. */
uint8_t  *lr_ast_serialize(ASTNode *root, size_t *out_len);
ASTNode  *lr_ast_deserialize(const uint8_t *buf, size_t len, Parser **out_parser);

/* ── IOME586 support: split parse / execute phases ──────────────────────
 * lr_engine_parse_unit parses the source into an opaque retained unit and
 * (optionally) serializes its AST, WITHOUT executing it. This lets the
 * cache write a WRITING-state archive to disk before/while the script runs.
 * lr_engine_exec_unit_handle then executes the unit (ownership transfers to
 * the persistent interpreter). lr_engine_unit_ast_handle exposes the parsed
 * program node for result snapshotting. */
void    *lr_engine_parse_unit(LRContext *ctx, const char *input, size_t input_len,
                              int is_module, const char *filename,
                              uint8_t **out_bc, size_t *out_bc_len);
LRValue  lr_engine_exec_unit_handle(LRContext *ctx, void *unit_handle,
                                    int is_module, const char *filename);
const ASTNode *lr_engine_unit_ast_handle(void *unit_handle);

/* Expose serialized bytecode + length for IOME586 archiving. */
const uint8_t *lr_engine_unit_bc_data(void *unit_handle, size_t *out_len);

/* Deserialize bytecode from IOME586 warm load into the eval unit. */
int lr_engine_unit_load_bytecode(void *unit_handle,
                                  const uint8_t *data, size_t len);

/* Top-level program node inspection for IOME586 result snapshots.
 * (ASTNode internals stay private to the engine: lr_ast.h cannot be included
 * from TUs that pull in windows.h due to the TokenType name collision.)
 * Returns the number of top-level nodes, or fills type/line/binding-name for
 * node i (binding name is the declared identifier for func/class/var decls,
 * NULL otherwise). Returns 0/-1 respectively when program is not a program. */
int      lr_engine_program_count(const ASTNode *program);
int      lr_engine_program_node_info(const ASTNode *program, int i,
                                     uint16_t *out_type, uint32_t *out_line,
                                     const char **out_binding_name);
int     lr_engine_detect_module(const char *input, size_t input_len);

/* ── Bytecode Serialization ───────────────────────────────────────────── */

LRValue lr_read_object(LRContext *ctx, const uint8_t *buf, size_t buf_len, int flags);
uint8_t *lr_write_object(LRContext *ctx, size_t *pout_len, LRValue obj, int flags);

/* ── Job Queue (Promise microtasks) ───────────────────────────────────── */

int  lr_is_job_pending(LRRuntime *rt);
int  lr_execute_pending_job(LRRuntime *rt, LRContext **pctx);
void lr_enqueue_job(LRRuntime *rt, LRContext *ctx, LRValue func);

/* ── GC ───────────────────────────────────────────────────────────────── */

void lr_gc_run(LRRuntime *rt);
void lr_engine_compute_memory_usage(LRRuntime *rt, LRMemoryUsage *usage);

/* ── Atom Operations ──────────────────────────────────────────────────── */

LRString *lr_new_atom(LRContext *ctx, const char *str);
LRString *lr_new_atom_len(LRContext *ctx, const char *str, size_t len);
const char *lr_atom_to_cstring(LRContext *ctx, LRString *atom);
LRString *lr_to_atom(LRContext *ctx, LRValue val);
LRValue lr_atom_to_value(LRContext *ctx, LRString *atom);

/* ── Class System ─────────────────────────────────────────────────────── */

LRClass *lr_new_class(const char *name, LRValue ctor, LRValue proto);
LRClass *lr_dup_class(LRClass *cls);
void     lr_free_class(LRRuntime *rt, LRClass *cls);
LRClass *lr_get_object_class(LRValue obj);
int      lr_get_object_class_id(LRValue obj);
void     lr_set_object_class(LRValue obj, LRClass *cls);

/* ── Threading Support ────────────────────────────────────────────────── */

LRRuntime *lr_new_runtime2(void *mem_opaque, void *alloc_opaque, void *free_opaque, void *realloc_opaque);
void lr_set_can_block(LRRuntime *rt, int can_block);

/* ── Macros for compatibility ─────────────────────────────────────────── */

#define lr_value_get_ptr(v)    ((v).u.ptr)
#define lr_value_get_int(v)    ((v).u.int32)
#define lr_value_get_bool(v)   ((v).u.bool_val)
#define lr_value_get_float(v)  ((v).u.float64)
#define lr_value_get_tag(v)    ((v).tag)

/* ── JS_* compatibility macros ────────────────────────────────────────── */

#define JSValue               LRValue
#define JSValueConst          LRValue
#define JSContext             LRContext
#define JSRuntime             LRRuntime
#define JSObject              LRObject
#define JSAtom                LRString *
#define JSString              LRString
#define JSModuleDef           LRModuleDef
#define JSPropertyEnum        LRPropertyEnum
#define JSCFunctionListEntry  LRCFunctionListEntry
#define JSClass               LRClass
#define JS_UNDEFINED          LR_VALUE_UNDEFINED
#define JS_NULL               LR_VALUE_NULL
#define JS_TRUE               LR_VALUE_TRUE
#define JS_FALSE              LR_VALUE_FALSE
#define JS_EXCEPTION          LR_VALUE_EXCEPTION

#define JS_NewBool(ctx, v)          lr_new_bool(ctx, v)
#define JS_NewInt32(ctx, v)         lr_new_int32(ctx, v)
#define JS_NewFloat64(ctx, v)       lr_new_float64(ctx, v)
#define JS_NewString(ctx, s)        lr_new_string(ctx, s)
#define JS_NewStringLen(ctx, s, l)  lr_new_string_len(ctx, s, l)
#define JS_NewObject(ctx)           lr_new_object(ctx)
#define JS_NewObjectProto(ctx, p)   lr_new_object_proto(ctx, p)
#define JS_NewArray(ctx)            lr_new_array(ctx)

#define JS_NewCFunction(ctx, fn, name, len) \
    lr_new_cfunction(ctx, (LRCFunctionFunc)(fn), name, len)
#define JS_NewCFunction2(ctx, fn, name, len, cproto, magic) \
    lr_new_cfunction2(ctx, (LRCFunctionFunc)(fn), name, len, cproto, magic)

#define JS_DupValue(ctx, v)         lr_dup_value(ctx, v)
#define JS_FreeValue(ctx, v)        lr_free_value(ctx, v)
#define JS_ToCString(ctx, v)        lr_to_cstring(ctx, v)
#define JS_FreeCString(ctx, s)      lr_free_cstring(ctx, s)

#define JS_IsUndefined(v)           lr_is_undefined(v)
#define JS_IsNull(v)                lr_is_null(v)
#define JS_IsBool(v)                lr_is_bool(v)
#define JS_IsNumber(v)              lr_is_number(v)
#define JS_IsString(v)              lr_is_string(v)
#define JS_IsObject(v)              lr_is_object(v)
#define JS_IsArray(ctx, v)          lr_is_array(ctx, v)
#define JS_IsFunction(ctx, v)       lr_is_function(ctx, v)
#define JS_IsException(v)           lr_is_exception(v)
#define JS_IsSymbol(v)              lr_is_symbol(v)
#define JS_IsArrayBuffer(ctx, v)    lr_is_array_buffer(ctx, v)
#define JS_IsPromise(ctx, v)        lr_is_promise(ctx, v)

#define JS_ToBool(ctx, v)           lr_to_bool(ctx, v)
#define JS_ToInt32(ctx, p, v)       lr_to_int32(ctx, p, v)
#define JS_ToFloat64(ctx, p, v)     lr_to_float64(ctx, p, v)

#define JS_GetProperty(ctx, o, a)     lr_get_property(ctx, o, a)
#define JS_GetPropertyStr(ctx, o, s)  lr_get_property_str(ctx, o, s)
#define JS_GetPropertyUint32(ctx, o, i) lr_get_property_uint32(ctx, o, i)

#define JS_SetProperty(ctx, o, a, v)     lr_set_property(ctx, o, a, v)
#define JS_SetPropertyStr(ctx, o, s, v)  lr_set_property_str(ctx, o, s, v)
#define JS_SetPropertyUint32(ctx, o, i, v) lr_set_property_uint32(ctx, o, i, v)

#define JS_DeleteProperty(ctx, o, a, f)   lr_delete_property(ctx, o, a, f)
#define JS_HasProperty(ctx, o, a)         lr_has_property(ctx, o, a)
#define JS_DefinePropertyValue(ctx, o, a, v, f) lr_define_property_value(ctx, o, a, v, f)
#define JS_DefinePropertyValueStr(ctx, obj, name, val, flags) \
    lr_define_property_value(ctx, obj, lr_new_atom(ctx, name), val, flags)
#define JS_GetOwnPropertyNames(ctx, t, l, o, f) lr_get_own_property_names(ctx, t, l, o, f)
#define JS_FreePropertyEnum(ctx, t, l)    lr_free_property_enum(ctx, t, l)

#define JS_Call(ctx, f, t, c, a)       lr_call(ctx, f, t, c, (LRValue *)(a))
#define JS_CallConstructor(ctx, f, c, a) lr_call_constructor(ctx, f, c, (LRValue *)(a))

#define JS_SetPropertyFunctionList(ctx, o, t, c) lr_set_property_function_list(ctx, o, t, c)

#define JS_GetPrototype(ctx, o)       lr_get_prototype(ctx, o)
#define JS_SetPrototype(ctx, o, p)    lr_set_prototype(ctx, o, p)

#define JS_GetGlobalObject(ctx)       lr_get_global_object(ctx)

#define JS_ThrowTypeError      lr_throw_type_error
#define JS_ThrowReferenceError lr_throw_reference_error
#define JS_ThrowRangeError    lr_throw_range_error
#define JS_ThrowSyntaxError   lr_throw_syntax_error
#define JS_ThrowInternalError lr_throw_internal_error

#define JS_GetException(ctx)          lr_get_exception(ctx)
#define JS_GetExceptionStr(ctx)       lr_get_exception_str(ctx)

#define JS_NewRuntime()               lr_new_runtime()
#define JS_FreeRuntime(rt)            lr_free_runtime(rt)
#define JS_NewContext(rt)             lr_new_context(rt)
#define JS_FreeContext(ctx)           lr_free_context(ctx)

#define JS_SetMemoryLimit(rt, l)      lr_set_memory_limit(rt, l)
#define JS_SetGCThreshold(rt, t)       lr_set_gc_threshold(rt, t)
#define JS_SetMaxStackSize(rt, s)     lr_set_max_stack_size(rt, s)

#define JS_SetContextOpaque(ctx, o)   lr_set_context_opaque(ctx, o)
#define JS_GetContextOpaque(ctx)      lr_get_context_opaque(ctx)

#define JS_SetModuleLoaderFunc(rt, n, l, o) lr_set_module_loader_func(rt, n, l, o)

#define JS_Eval(ctx, in, len, fn, fl)  lr_engine_eval(ctx, in, len, fn, fl)
#define JS_EvalFunction(ctx, f)        lr_engine_eval_function(ctx, f)
#define JS_DetectModule(in, len)       lr_engine_detect_module(in, len)

#define JS_ReadObject(ctx, b, bl, fl)  lr_read_object(ctx, b, bl, fl)
#define JS_WriteObject(ctx, ol, o, fl) lr_write_object(ctx, ol, o, fl)

#define JS_IsJobPending(rt)            lr_is_job_pending(rt)
#define JS_ExecutePendingJob(rt, pc)   lr_execute_pending_job(rt, pc)

#define JS_ComputeMemoryUsage(rt, u)   lr_engine_compute_memory_usage(rt, u)

#define JS_NewAtom(ctx, s)             lr_new_atom(ctx, s)
#define JS_NewAtomLen(ctx, s, l)       lr_new_atom_len(ctx, s, l)
#define JS_AtomToCString(ctx, a)       lr_atom_to_cstring(ctx, a)
#define JS_ToAtom(ctx, v)              lr_to_atom(ctx, v)
#define JS_AtomToValue(ctx, a)         lr_atom_to_value(ctx, a)

#define JS_VALUE_GET_PTR(v)            lr_value_get_ptr(v)
#define JS_VALUE_GET_INT(v)            lr_value_get_int(v)
#define JS_VALUE_GET_BOOL(v)           lr_value_get_bool(v)
#define JS_VALUE_GET_FLOAT64(v)        lr_value_get_float(v)
#define JS_VALUE_GET_TAG(v)            lr_value_get_tag(v)

#define JS_NewRuntime2(a,b,c,d)        lr_new_runtime2(a,b,c,d)
#define JS_SetCanBlock(rt, b)          lr_set_can_block(rt, b)

#define JS_NewClass                    lr_new_class
#define JS_DupClass                    lr_dup_class
#define JS_FreeClass                   lr_free_class
#define JS_GetObjectClass              lr_get_object_class
#define JS_GetObjectClassID            lr_get_object_class_id
#define JS_SetObjectClass              lr_set_object_class

/* ── GC macros ────────────────────────────────────────────────────────── */

#define JS_GC(rt)                      lr_gc_run(rt)
#define JS_GetGCCount(rt)              ((rt)->obj_count)

/* ── Additional macros for LR_JS internal use ──────────────────────────── */

#define JS_AddPromiseRejectionCallback(ctx, fn, opaque) ((void)(ctx), (void)(fn), (void)(opaque), 0)
#define JS_ToUint32(ctx, p, v)         lr_to_int32(ctx, p, v)  /* simplified */
#define JS_ToInt64(ctx, p, v)          lr_to_int64(ctx, p, v)
#define JS_ToUint64(ctx, p, v)         ({ int64_t _tmp; int _r = lr_to_int64(ctx, &_tmp, v); *(p) = (uint64_t)_tmp; _r; })
#define JS_ToString(ctx, v)            lr_new_string(ctx, lr_to_cstring(ctx, v))
#define JS_ToNumber(ctx, v)            ({ double _d; lr_to_float64(ctx, &_d, v); lr_new_float64(ctx, _d); })
#define JS_ToObject(ctx, v)            lr_new_object(ctx)  /* simplified */

/* Eval flags */
#define JS_EVAL_TYPE_GLOBAL      0
#define JS_EVAL_TYPE_MODULE      1
#define JS_EVAL_FLAG_STRICT      2
#define JS_EVAL_FLAG_COMPILE_ONLY 4
#define JS_EVAL_FLAG_STRIP       8

/* Job queue */
#define JS_ThrowOutOfMemory(ctx) lr_throw_internal_error(ctx, "out of memory")
#define JS_EnqueueJob(ctx, handler, argc, argv) \
    do { (void)(argc); (void)(argv); lr_enqueue_job((ctx)->rt, ctx, handler); } while(0)

/* Bytecode flags */
#define JS_READ_OBJ_BYTECODE     1
#define JS_WRITE_OBJ_BYTECODE    1

/* Module import meta stub */
#define js_module_set_import_meta(ctx, func_val, use_realpath, is_main) \
    ((void)(ctx), (void)(func_val), (void)(use_realpath), (void)(is_main))

#define JS_NewArrayBuffer(ctx, buf, len, free_func, opaque, is_shared) \
    lr_new_array_buffer(ctx, buf, len, free_func, opaque, is_shared)

/* Forward declare array buffer support */
LRValue lr_new_array_buffer(LRContext *ctx, uint8_t *buf, size_t len,
    void (*free_func)(void *opaque, void *ptr), void *opaque, int is_shared);

/* Free the persistent interpreter + retained ASTs of a context.
 * Called automatically by lr_free_context. */
void lr_context_free_persistent_interp(LRContext *ctx);

/* Returns 1 if the value is an ArrayBuffer created with is_shared=1
 * (i.e. a SharedArrayBuffer), 0 otherwise. */
int lr_array_buffer_is_shared(LRValue obj);
/* Returns the opaque pointer passed to lr_new_array_buffer (the free_func
 * argument), or NULL. Used by SharedArrayBuffer to retrieve its shared
 * control block. */
void *lr_array_buffer_get_opaque(LRValue obj);

#define JS_GetArrayBuffer(ctx, psize, obj) lr_get_array_buffer(ctx, psize, obj)
uint8_t *lr_get_array_buffer(LRContext *ctx, size_t *psize, LRValue obj);

#define JS_GetTypedArrayBuffer(ctx, obj, poffset, plen, pbytes) \
    lr_get_typed_array_buffer(ctx, obj, poffset, plen, pbytes)
LRValue lr_get_typed_array_buffer(LRContext *ctx, LRValue obj,
    size_t *poffset, size_t *plen, size_t *pbytes);

#define JS_NewArrayBufferCopy(ctx, buf, len) \
    lr_new_array_buffer_copy(ctx, buf, len)
LRValue lr_new_array_buffer_copy(LRContext *ctx, const uint8_t *buf, size_t len);
void lr_array_buffer_free(void *opaque, void *ptr);

#define JS_NewInt64(ctx, val)       lr_new_int32(ctx, (int32_t)(val))  /* simplified */
#define JS_NewUint32(ctx, val)      lr_new_int32(ctx, (int32_t)(val))

#define JS_GetPropertyInternal(ctx, obj, atom, receiver) \
    lr_get_property(ctx, obj, atom)
#define JS_SetPropertyInternal(ctx, obj, atom, val, flags) \
    lr_set_property(ctx, obj, atom, val)

#define JS_NewPromiseCapability(ctx, resolving_funcs) \
    lr_new_promise_capability(ctx, resolving_funcs)
LRValue lr_new_promise_capability(LRContext *ctx, LRValue *resolving_funcs);

/* Get the internal [[PromiseResult]] of a Promise object */
LRValue lr_promise_get_result(LRValue promise);

#define JS_NewError(ctx)             lr_new_object(ctx)  /* stub */

/* ── Object opaque data ────────────────────────────────────────────────── */

#define JS_SetOpaque(obj, data)      lr_set_opaque(obj, data)
#define JS_GetOpaque(obj, cls)       lr_get_opaque(obj)

/* ── Property getter/setter definition ─────────────────────────────────── */

#define JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter, flags) \
    lr_define_property_getset(ctx, obj, atom, getter, setter, flags)

#ifdef __cplusplus
}
#endif

#endif /* LR_ENGINE_H */