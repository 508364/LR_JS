/*
 * LR_JS - JavaScript Engine AST Tree-Walking Interpreter
 * Pure C implementation.
 *
 * Evaluates AST nodes produced by the parser using a tree-walking approach.
 * Handles all ES2022 expressions, statements, declarations, and control flow.
 */
#include "lr_interp.h"
#include "../lr_promise.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

/* ── Constants ─────────────────────────────────────────────────────────── */

#define MAX_CALL_DEPTH 256
#define SCOPE_INIT_CAP 8

/* ── Forward Declarations ──────────────────────────────────────────────── */

static LRValue interp_eval_node(Interpreter *interp, ASTNode *node);
static LRValue eval_literal(Interpreter *interp, ASTNode *node);
static LRValue eval_identifier(Interpreter *interp, ASTNode *node);
static LRValue eval_binary(Interpreter *interp, ASTNode *node);
static LRValue eval_unary(Interpreter *interp, ASTNode *node);
static LRValue eval_assign(Interpreter *interp, ASTNode *node);
static LRValue eval_member(Interpreter *interp, ASTNode *node);
static LRValue eval_computed_member(Interpreter *interp, ASTNode *node);
static LRValue eval_call(Interpreter *interp, ASTNode *node);
static LRValue eval_new(Interpreter *interp, ASTNode *node);
static LRValue eval_conditional(Interpreter *interp, ASTNode *node);
static LRValue eval_array(Interpreter *interp, ASTNode *node);
static LRValue eval_object(Interpreter *interp, ASTNode *node);
static LRValue eval_func_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_arrow(Interpreter *interp, ASTNode *node);
static LRValue eval_template(Interpreter *interp, ASTNode *node);
static LRValue eval_sequence(Interpreter *interp, ASTNode *node);
static LRValue eval_spread(Interpreter *interp, ASTNode *node);
static LRValue eval_await(Interpreter *interp, ASTNode *node);
static LRValue eval_class_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_pattern(Interpreter *interp, ASTNode *node, LRValue value);

static LRValue interp_eval_stmt(Interpreter *interp, ASTNode *node);
static LRValue eval_block(Interpreter *interp, ASTNode *node);
static LRValue eval_if(Interpreter *interp, ASTNode *node);
static LRValue eval_for(Interpreter *interp, ASTNode *node);
static LRValue eval_for_in(Interpreter *interp, ASTNode *node);
static LRValue eval_for_of(Interpreter *interp, ASTNode *node);
static LRValue eval_while(Interpreter *interp, ASTNode *node);
static LRValue eval_do_while(Interpreter *interp, ASTNode *node);
static LRValue eval_switch(Interpreter *interp, ASTNode *node);
static LRValue eval_break(Interpreter *interp, ASTNode *node);
static LRValue eval_continue(Interpreter *interp, ASTNode *node);
static LRValue eval_return(Interpreter *interp, ASTNode *node);
static LRValue eval_throw(Interpreter *interp, ASTNode *node);
static LRValue eval_try(Interpreter *interp, ASTNode *node);
static LRValue eval_var_decl(Interpreter *interp, ASTNode *node);
static LRValue eval_func_decl(Interpreter *interp, ASTNode *node);
static LRValue eval_class_decl(Interpreter *interp, ASTNode *node);
static LRValue eval_import(Interpreter *interp, ASTNode *node);
static LRValue eval_export(Interpreter *interp, ASTNode *node);

/* Call a JS function (closure-style) with given args */
static LRValue interp_call_function(Interpreter *interp, ASTNode *func_node,
                                     LRValue this_val, int argc, LRValue *argv);

/* Call a class "constructor" (field init + explicit/implicit constructor) */
static LRValue interp_call_class_function(Interpreter *interp, ASTNode *class_node,
                                          LRValue this_val, int argc, LRValue *argv);

/* Dispatch any callable AST node (function, arrow, or class) */
static LRValue interp_invoke_function_ast(Interpreter *interp, ASTNode *ast,
                                          LRValue this_val, int argc, LRValue *argv);

/* ── Scope Management ──────────────────────────────────────────────────── */

static InterpScope *scope_new(InterpScope *parent, int is_function, int is_global)
{
    InterpScope *s = (InterpScope *)calloc(1, sizeof(InterpScope));
    if (!s) return NULL;
    s->parent = parent;
    if (parent) parent->refcount++;   /* child keeps its parent chain alive */
    s->refcount = 1;
    s->is_function_scope = is_function;
    s->is_global_scope = is_global;
    s->capacity = SCOPE_INIT_CAP;
    s->names = (char **)calloc(s->capacity, sizeof(char *));
    s->values = (LRValue *)calloc(s->capacity, sizeof(LRValue));
    s->is_const = (int *)calloc(s->capacity, sizeof(int));
    s->is_lexical = (int *)calloc(s->capacity, sizeof(int));
    return s;
}

/* Drop one reference; frees the scope (and cascades up the parent chain)
 * when the count reaches zero. */
static void scope_release(InterpScope *scope, LRContext *ctx)
{
    while (scope) {
        if (--scope->refcount > 0) return;
        InterpScope *parent = scope->parent;
        for (int i = 0; i < scope->count; i++) {
            if (scope->names[i]) free(scope->names[i]);
            lr_free_value(ctx, scope->values[i]);
        }
        free(scope->names);
        free(scope->values);
        free(scope->is_const);
        free(scope->is_lexical);
        free(scope);
        scope = parent;   /* release the reference this child held */
    }
}

/* Hook target for lr_engine: release a function object's captured scope */
static void interp_closure_release_hook(void *scope, LRContext *ctx)
{
    scope_release((InterpScope *)scope, ctx);
}

/* Capture the current scope into a function object for lexical closures */
static void interp_capture_closure(Interpreter *interp, LRValue fn_obj)
{
    if (fn_obj.tag != LR_TYPE_OBJECT || !interp->current_scope) return;
    LRObject *o = (LRObject *)fn_obj.u.ptr;
    if (o->def_scope) return;
    interp->current_scope->refcount++;
    o->def_scope = interp->current_scope;
}

static void interp_push_scope(Interpreter *interp, int is_function_scope)
{
    InterpScope *s = scope_new(interp->current_scope, is_function_scope, 0);
    if (interp->current_scope == NULL) {
        s->is_global_scope = 1;
        s->mirror_globals = !interp->is_module;
    }
    interp->current_scope = s;
}

static void interp_pop_scope(Interpreter *interp)
{
    if (!interp->current_scope) return;
    InterpScope *old = interp->current_scope;
    interp->current_scope = old->parent;
    scope_release(old, interp->ctx);
}

/* Find the nearest function scope (or global) - for var hoisting */
static InterpScope *find_function_scope(Interpreter *interp)
{
    InterpScope *s = interp->current_scope;
    while (s) {
        if (s->is_function_scope || s->is_global_scope) return s;
        s = s->parent;
    }
    return interp->global_scope;
}

    /* Look up a name in the scope chain.
     * Returns 1 if found (value set), 0 if not found.
     * For mirrored script-mode global var/function bindings the value is
     * read authoritatively from the global object so that bare `x` and
     * `globalThis.x` are the same binding (two-way consistency). */
static int scope_lookup_internal(InterpScope *scope, LRContext *ctx,
                                 const char *name, LRValue *value)
{
    (void)ctx;   /* reserved; bindings remain the source of truth for reads */
    while (scope) {
        for (int i = 0; i < scope->count; i++) {
            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                if (value) *value = lr_dup_value(NULL, scope->values[i]);
                return 1;
            }
        }
        scope = scope->parent;
    }
    return 0;
}

/* ES spec (GlobalDeclarationInstantiation): in Script mode, top-level var
 * and function declarations create properties of the global object.
 * Mirror such bindings onto the global object (scope stays source of truth
 * for reads; writes are kept in sync by scope_set_name). */
static void mirror_global_binding(Interpreter *interp, const char *name, LRValue value)
{
    LRValue global = lr_get_global_object(interp->ctx);
    lr_set_property_str(interp->ctx, global, name,
                        lr_dup_value(interp->ctx, value));
    lr_free_value(interp->ctx, global);
}

/* Declare a variable in the current scope (for let/const) or function scope (for var).
 * kind: 0=var, 1=let, 2=const */
static void scope_declare_name(Interpreter *interp, const char *name, LRValue value, int kind)
{
    /* NOTE: the engine currently treats let/const like var for scoping and
     * const-enforcement purposes (pre-existing behavior, preserved here so we
     * only change what the user asked for). We therefore always hoist the
     * binding to the nearest function/global scope. The *only* thing driven by
     * the real declaration kind is the global-object mirror: per the ES spec,
     * only top-level var/function (kind == 0) become properties of the global
     * object in Script mode; let/const/class do not. */
    InterpScope *scope = find_function_scope(interp);

    int mirror = (kind == 0 && scope->is_global_scope && scope->mirror_globals);

    /* Check if already declared in this scope */
    for (int i = 0; i < scope->count; i++) {
        if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
            /* Redeclaration in same scope - update value */
            lr_free_value(interp->ctx, scope->values[i]);
            scope->values[i] = lr_dup_value(interp->ctx, value);
            if (mirror && !scope->is_lexical[i])
                mirror_global_binding(interp, name, value);
            return;
        }
    }

    /* Add new entry */
    if (scope->count >= scope->capacity) {
        scope->capacity *= 2;
        scope->names = (char **)realloc(scope->names, scope->capacity * sizeof(char *));
        scope->values = (LRValue *)realloc(scope->values, scope->capacity * sizeof(LRValue));
        scope->is_const = (int *)realloc(scope->is_const, scope->capacity * sizeof(int));
        scope->is_lexical = (int *)realloc(scope->is_lexical, scope->capacity * sizeof(int));
        /* Zero out new entries */
        for (int i = scope->count; i < scope->capacity; i++) {
            scope->names[i] = NULL;
            scope->values[i] = LR_VALUE_UNDEFINED;
            scope->is_const[i] = 0;
            scope->is_lexical[i] = 0;
        }
    }
    scope->names[scope->count] = strdup(name);
    scope->values[scope->count] = lr_dup_value(interp->ctx, value);
    scope->is_const[scope->count] = 0;   /* const enforcement left as the
                                           * engine's pre-existing behavior */
    scope->is_lexical[scope->count] = (kind != 0) ? 1 : 0;
    scope->count++;

    if (mirror)
        mirror_global_binding(interp, name, value);
}

/* Set a variable's value in the scope chain.
 * Returns 1 if found and set, 0 if not found. */
static int scope_set_name(Interpreter *interp, const char *name, LRValue value)
{
    InterpScope *scope = interp->current_scope;
    while (scope) {
        for (int i = 0; i < scope->count; i++) {
            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                if (scope->is_const[i]) {
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "Assignment to constant variable '%s'", name);
                    interp->error_flag = 1;
                    return 0;
                }
                lr_free_value(interp->ctx, scope->values[i]);
                scope->values[i] = lr_dup_value(interp->ctx, value);
                /* Keep the ES-spec global-object mirror in sync for
                 * script-mode top-level var/function bindings */
                if (scope->is_global_scope && !scope->is_lexical[i] &&
                    scope->mirror_globals)
                    mirror_global_binding(interp, name, value);
                return 1;
            }
        }
        scope = scope->parent;
    }
    return 0;
}

/* ── Helper Functions ──────────────────────────────────────────────────── */

static double to_number(LRContext *ctx, LRValue val)
{
    double d;
    lr_to_float64(ctx, &d, val);
    return d;
}

static int32_t to_int32(LRContext *ctx, LRValue val)
{
    int32_t i;
    lr_to_int32(ctx, &i, val);
    return i;
}

/* Strict equality comparison */
static int strict_eq(LRValue a, LRValue b)
{
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case LR_TYPE_UNDEFINED: return 1;
    case LR_TYPE_NULL:      return 1;
    case LR_TYPE_BOOL:      return a.u.bool_val == b.u.bool_val;
    case LR_TYPE_INT32:     return a.u.int32 == b.u.int32;
    case LR_TYPE_FLOAT64:   return a.u.float64 == b.u.float64;
    case LR_TYPE_STRING: {
        LRString *sa = (LRString *)a.u.ptr;
        LRString *sb = (LRString *)b.u.ptr;
        if (sa == sb) return 1;
        if (sa->len != sb->len) return 0;
        return memcmp(sa->str, sb->str, sa->len) == 0;
    }
    case LR_TYPE_OBJECT:
        return a.u.ptr == b.u.ptr;
    case LR_TYPE_SYMBOL:
        return a.u.ptr == b.u.ptr;
    default:
        return 0;
    }
}

/* Abstract equality comparison (==) */
static int abstract_eq(LRContext *ctx, LRValue a, LRValue b)
{
    /* Same type - use strict equality */
    if (a.tag == b.tag) return strict_eq(a, b);

    /* undefined == null */
    if ((a.tag == LR_TYPE_UNDEFINED && b.tag == LR_TYPE_NULL) ||
        (a.tag == LR_TYPE_NULL && b.tag == LR_TYPE_UNDEFINED)) return 1;

    /* Number vs String - convert string to number */
    if (a.tag == LR_TYPE_STRING && (b.tag == LR_TYPE_INT32 || b.tag == LR_TYPE_FLOAT64)) {
        double da = to_number(ctx, a);
        double db = to_number(ctx, b);
        return da == db;
    }
    if ((a.tag == LR_TYPE_INT32 || a.tag == LR_TYPE_FLOAT64) && b.tag == LR_TYPE_STRING) {
        double da = to_number(ctx, a);
        double db = to_number(ctx, b);
        return da == db;
    }

    /* Bool vs anything - convert bool to number */
    if (a.tag == LR_TYPE_BOOL) {
        LRValue na = lr_new_int32(ctx, a.u.bool_val ? 1 : 0);
        int r = abstract_eq(ctx, na, b);
        lr_free_value(ctx, na);
        return r;
    }
    if (b.tag == LR_TYPE_BOOL) {
        LRValue nb = lr_new_int32(ctx, b.u.bool_val ? 1 : 0);
        int r = abstract_eq(ctx, a, nb);
        lr_free_value(ctx, nb);
        return r;
    }

    /* Object vs String/Number - convert to primitive */
    if (a.tag == LR_TYPE_OBJECT && (b.tag == LR_TYPE_STRING || b.tag == LR_TYPE_INT32 || b.tag == LR_TYPE_FLOAT64)) {
        const char *str = lr_to_cstring(ctx, a);
        LRValue prim = lr_new_string(ctx, str);
        lr_free_cstring(ctx, str);
        int r = abstract_eq(ctx, prim, b);
        lr_free_value(ctx, prim);
        return r;
    }
    if ((a.tag == LR_TYPE_STRING || a.tag == LR_TYPE_INT32 || a.tag == LR_TYPE_FLOAT64) && b.tag == LR_TYPE_OBJECT) {
        const char *str = lr_to_cstring(ctx, b);
        LRValue prim = lr_new_string(ctx, str);
        lr_free_cstring(ctx, str);
        int r = abstract_eq(ctx, a, prim);
        lr_free_value(ctx, prim);
        return r;
    }

    /* Number vs undefined/null */
    if ((a.tag == LR_TYPE_INT32 || a.tag == LR_TYPE_FLOAT64) &&
        (b.tag == LR_TYPE_UNDEFINED || b.tag == LR_TYPE_NULL)) return 0;
    if ((a.tag == LR_TYPE_UNDEFINED || a.tag == LR_TYPE_NULL) &&
        (b.tag == LR_TYPE_INT32 || b.tag == LR_TYPE_FLOAT64)) return 0;

    return 0;
}

/* ── Expression Evaluators ─────────────────────────────────────────────── */

static LRValue eval_literal(Interpreter *interp, ASTNode *node)
{
    (void)interp;
    TokenType tt = node->token.type;
    if (tt == TOK_NUMBER) {
        double d = node->u.number.num;
        if (d == (double)(int32_t)d && !isnan(d) && !isinf(d)) {
            return lr_new_int32(interp->ctx, (int32_t)d);
        }
        return lr_new_float64(interp->ctx, d);
    }
    if (tt == TOK_STRING) {
        const char *s = node->u.string.str;
        return lr_new_string(interp->ctx, s ? s : "");
    }
    if (tt == TOK_BOOL_LIT) {
        return lr_new_bool(interp->ctx, node->u.bool_val.val);
    }
    if (tt == TOK_NULL_LIT) {
        return LR_VALUE_NULL;
    }
    if (tt == TOK_UNDEFINED_LIT) {
        return LR_VALUE_UNDEFINED;
    }
    /* Fallback: check token type via string comparison */
    if (node->u.number.num == -1) return LR_VALUE_UNDEFINED;
    return LR_VALUE_NULL;
}

static LRValue eval_identifier(Interpreter *interp, ASTNode *node)
{
    const char *name = node->u.ident.name;
    if (!name) return LR_VALUE_UNDEFINED;

    LRValue val;
    if (scope_lookup_internal(interp->current_scope, interp->ctx, name, &val)) {
        return val;
    }

    /* Check global object */
    LRValue global = lr_get_global_object(interp->ctx);
    val = lr_get_property_str(interp->ctx, global, name);
    lr_free_value(interp->ctx, global);
    if (!lr_is_undefined(val)) {
        return val;
    }
    lr_free_value(interp->ctx, val);

    /* ReferenceError for undeclared identifiers */
    snprintf(interp->error_message, sizeof(interp->error_message),
             "'%s' is not defined", name);
    interp->error_flag = 1;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_binary(Interpreter *interp, ASTNode *node)
{
    const char *op = node->u.binary.op;

    /* Short-circuit logical operators */
    if (strcmp(op, "&&") == 0) {
        LRValue left = interp_eval_node(interp, node->u.binary.left);
        if (interp->error_flag) return left;
        int truthy = lr_to_bool(interp->ctx, left);
        if (!truthy) return left;
        lr_free_value(interp->ctx, left);
        return interp_eval_node(interp, node->u.binary.right);
    }
    if (strcmp(op, "||") == 0) {
        LRValue left = interp_eval_node(interp, node->u.binary.left);
        if (interp->error_flag) return left;
        int truthy = lr_to_bool(interp->ctx, left);
        if (truthy) return left;
        lr_free_value(interp->ctx, left);
        return interp_eval_node(interp, node->u.binary.right);
    }
    if (strcmp(op, "??") == 0) {
        LRValue left = interp_eval_node(interp, node->u.binary.left);
        if (interp->error_flag) return left;
        if (!lr_is_null(left) && !lr_is_undefined(left)) return left;
        lr_free_value(interp->ctx, left);
        return interp_eval_node(interp, node->u.binary.right);
    }

    /* Evaluate both sides */
    LRValue left = interp_eval_node(interp, node->u.binary.left);
    if (interp->error_flag) return left;
    LRValue right = interp_eval_node(interp, node->u.binary.right);
    if (interp->error_flag) {
        lr_free_value(interp->ctx, left);
        return right;
    }

    LRValue result = LR_VALUE_UNDEFINED;

    /* ── Integer Fast Path ──────────────────────────────────────────────── */
    /* For common arithmetic operations on two int32 values, avoid boxing.
     * This is a critical optimization for tight loops. */
    if (left.tag == LR_TYPE_INT32 && right.tag == LR_TYPE_INT32) {
        int32_t a = left.u.int32;
        int32_t b = right.u.int32;
        int32_t res;
        int fast = 1;

        if (strcmp(op, "+") == 0) {
            res = a + b;
        } else if (strcmp(op, "-") == 0) {
            res = a - b;
        } else if (strcmp(op, "*") == 0) {
            res = a * b;
        } else if (strcmp(op, "&") == 0) {
            res = a & b;
        } else if (strcmp(op, "|") == 0) {
            res = a | b;
        } else if (strcmp(op, "^") == 0) {
            res = a ^ b;
        } else if (strcmp(op, "<<") == 0) {
            res = a << (b & 31);
        } else if (strcmp(op, ">>") == 0) {
            res = a >> (b & 31);
        } else if (strcmp(op, ">>>") == 0) {
            res = (int32_t)((uint32_t)a >> (b & 31));
        } else if (strcmp(op, "<") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a < b);
        } else if (strcmp(op, ">") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a > b);
        } else if (strcmp(op, "<=") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a <= b);
        } else if (strcmp(op, ">=") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a >= b);
        } else {
            fast = 0;
        }

        if (fast) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_int32(interp->ctx, res);
        }
    }

    /* ── End Integer Fast Path ──────────────────────────────────────────── */

    /* String concatenation with + */
    if (strcmp(op, "+") == 0) {
        if (lr_is_string(left) || lr_is_string(right)) {
            const char *sl = lr_to_cstring(interp->ctx, left);
            const char *sr = lr_to_cstring(interp->ctx, right);
            size_t llen = strlen(sl);
            size_t rlen = strlen(sr);
            char *buf = (char *)malloc(llen + rlen + 1);
            if (buf) {
                memcpy(buf, sl, llen);
                memcpy(buf + llen, sr, rlen);
                buf[llen + rlen] = '\0';
                result = lr_new_string(interp->ctx, buf);
                free(buf);
            }
            lr_free_cstring(interp->ctx, sl);
            lr_free_cstring(interp->ctx, sr);
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return result;
        }
        /* Numeric addition */
        double da = to_number(interp->ctx, left);
        double db = to_number(interp->ctx, right);
        double sum = da + db;
        if (sum == (double)(int32_t)sum && !isnan(sum) && !isinf(sum)) {
            result = lr_new_int32(interp->ctx, (int32_t)sum);
        } else {
            result = lr_new_float64(interp->ctx, sum);
        }
        lr_free_value(interp->ctx, left);
        lr_free_value(interp->ctx, right);
        return result;
    }

    /* Numeric-only operations */
    double da = to_number(interp->ctx, left);
    double db = to_number(interp->ctx, right);

    if (strcmp(op, "-") == 0) {
        double v = da - db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "*") == 0) {
        double v = da * db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "/") == 0) {
        if (db == 0.0) {
            result = lr_new_float64(interp->ctx, da / db); /* Infinity or NaN */
        } else {
            double v = da / db;
            result = lr_new_float64(interp->ctx, v);
        }
    } else if (strcmp(op, "%") == 0) {
        if (db == 0.0) {
            result = lr_new_float64(interp->ctx, NAN);
        } else {
            double v = fmod(da, db);
            if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
                result = lr_new_int32(interp->ctx, (int32_t)v);
            else
                result = lr_new_float64(interp->ctx, v);
        }
    } else if (strcmp(op, "**") == 0) {
        double v = pow(da, db);
        result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "<") == 0) {
        result = lr_new_bool(interp->ctx, da < db);
    } else if (strcmp(op, ">") == 0) {
        result = lr_new_bool(interp->ctx, da > db);
    } else if (strcmp(op, "<=") == 0) {
        result = lr_new_bool(interp->ctx, da <= db);
    } else if (strcmp(op, ">=") == 0) {
        result = lr_new_bool(interp->ctx, da >= db);
    } else if (strcmp(op, "==") == 0) {
        result = lr_new_bool(interp->ctx, abstract_eq(interp->ctx, left, right));
    } else if (strcmp(op, "!=") == 0) {
        result = lr_new_bool(interp->ctx, !abstract_eq(interp->ctx, left, right));
    } else if (strcmp(op, "===") == 0) {
        result = lr_new_bool(interp->ctx, strict_eq(left, right));
    } else if (strcmp(op, "!==") == 0) {
        result = lr_new_bool(interp->ctx, !strict_eq(left, right));
    } else if (strcmp(op, "&") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da & (int32_t)db);
    } else if (strcmp(op, "|") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da | (int32_t)db);
    } else if (strcmp(op, "^") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da ^ (int32_t)db);
    } else if (strcmp(op, "<<") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da << (int32_t)db);
    } else if (strcmp(op, ">>") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da >> (int32_t)db);
    } else if (strcmp(op, ">>>") == 0) {
        result = lr_new_int32(interp->ctx, (uint32_t)(int32_t)da >> (int32_t)db);
    } else if (strcmp(op, "in") == 0) {
        /* property in object */
        if (!lr_is_object(right)) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "right-hand side of 'in' must be an object");
            interp->error_flag = 1;
            return LR_VALUE_UNDEFINED;
        }
        const char *prop = lr_to_cstring(interp->ctx, left);
        LRString *atom = lr_new_atom(interp->ctx, prop);
        result = lr_new_bool(interp->ctx, lr_has_property(interp->ctx, right, atom));
        lr_free_cstring(interp->ctx, prop);
    } else if (strcmp(op, "instanceof") == 0) {
        /* obj instanceof Func - check right.prototype in left's prototype chain */
        if (!lr_is_object(left)) {
            result = lr_new_bool(interp->ctx, 0);
        } else {
            LRValue proto = lr_get_property_str(interp->ctx, right, "prototype");
            if (!lr_is_object(proto)) {
                lr_free_value(interp->ctx, proto);
                result = lr_new_bool(interp->ctx, 0);
            } else {
                int found = 0;
                LRValue p = lr_get_prototype(interp->ctx, left);
                while (lr_is_object(p)) {
                    if (strict_eq(p, proto)) {
                        found = 1;
                        break;
                    }
                    LRValue next = lr_get_prototype(interp->ctx, p);
                    lr_free_value(interp->ctx, p);
                    p = next;
                }
                lr_free_value(interp->ctx, p);
                lr_free_value(interp->ctx, proto);
                result = lr_new_bool(interp->ctx, found);
            }
        }
    } else {
        /* Unknown operator, return undefined */
        result = LR_VALUE_UNDEFINED;
    }

    lr_free_value(interp->ctx, left);
    lr_free_value(interp->ctx, right);
    return result;
}

static LRValue eval_unary(Interpreter *interp, ASTNode *node)
{
    const char *op = node->u.unary.op;
    int prefix = node->u.unary.prefix;

    if (strcmp(op, "typeof") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        /* ES spec: typeof on an unresolvable identifier must NOT throw a
         * ReferenceError; it evaluates to "undefined". */
        if (interp->error_flag &&
            node->u.unary.arg && node->u.unary.arg->type == AST_IDENTIFIER) {
            interp->error_flag = 0;
            interp->error_message[0] = '\0';
            interp->exception_pending = 0;
            lr_free_value(interp->ctx, arg);
            return lr_new_string(interp->ctx, "undefined");
        }
        const char *type_str = "undefined";
        switch (arg.tag) {
        case LR_TYPE_UNDEFINED: type_str = "undefined"; break;
        case LR_TYPE_NULL:      type_str = "object"; break;
        case LR_TYPE_BOOL:      type_str = "boolean"; break;
        case LR_TYPE_INT32:
        case LR_TYPE_FLOAT64:   type_str = "number"; break;
        case LR_TYPE_STRING:    type_str = "string"; break;
        case LR_TYPE_OBJECT: {
            if (lr_is_function(interp->ctx, arg)) {
                type_str = "function";
            } else {
                type_str = "object";
            }
            break;
        }
        case LR_TYPE_SYMBOL:    type_str = "symbol"; break;
        default: break;
        }
        lr_free_value(interp->ctx, arg);
        return lr_new_string(interp->ctx, type_str);
    }

    if (strcmp(op, "void") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        lr_free_value(interp->ctx, arg);
        return LR_VALUE_UNDEFINED;
    }

    if (strcmp(op, "delete") == 0) {
        /* delete property */
        if (node->u.unary.arg->type == AST_MEMBER) {
            ASTNode *member = node->u.unary.arg;
            LRValue obj = interp_eval_node(interp, member->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); return LR_VALUE_UNDEFINED; }
            const char *prop = member->u.member.prop->u.ident.name;
            if (prop) {
                LRString *atom = lr_new_atom(interp->ctx, prop);
                lr_delete_property(interp->ctx, obj, atom, 0);
            }
            lr_free_value(interp->ctx, obj);
            return lr_new_bool(interp->ctx, 1);
        }
        if (node->u.unary.arg->type == AST_COMPUTED_MEMBER) {
            ASTNode *member = node->u.unary.arg;
            LRValue obj = interp_eval_node(interp, member->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); return LR_VALUE_UNDEFINED; }
            LRValue prop = interp_eval_node(interp, member->u.member.prop);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, prop); return LR_VALUE_UNDEFINED; }
            LRString *atom = lr_to_atom(interp->ctx, prop);
            lr_delete_property(interp->ctx, obj, atom, 0);
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
            return lr_new_bool(interp->ctx, 1);
        }
        /* delete of a simple identifier - in non-strict mode, returns true */
        return lr_new_bool(interp->ctx, 1);
    }

    if (strcmp(op, "!") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        int b = lr_to_bool(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        return lr_new_bool(interp->ctx, !b);
    }

    if (strcmp(op, "~") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        int32_t i = to_int32(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        return lr_new_int32(interp->ctx, ~i);
    }

    if (strcmp(op, "+") == 0 && prefix) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        double d = to_number(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        if (d == (double)(int32_t)d && !isnan(d) && !isinf(d))
            return lr_new_int32(interp->ctx, (int32_t)d);
        return lr_new_float64(interp->ctx, d);
    }

    if (strcmp(op, "-") == 0 && prefix) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        double d = to_number(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        d = -d;
        if (d == (double)(int32_t)d && !isnan(d) && !isinf(d))
            return lr_new_int32(interp->ctx, (int32_t)d);
        return lr_new_float64(interp->ctx, d);
    }

    /* Pre/post increment/decrement */
    if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        if (interp->error_flag) return arg;
        double old_val = to_number(interp->ctx, arg);
        double new_val = (op[0] == '+') ? old_val + 1.0 : old_val - 1.0;

        /* Try to assign back to the identifier */
        if (node->u.unary.arg->type == AST_IDENTIFIER) {
            const char *name = node->u.unary.arg->u.ident.name;
            LRValue nv;
            if (new_val == (double)(int32_t)new_val && !isnan(new_val) && !isinf(new_val))
                nv = lr_new_int32(interp->ctx, (int32_t)new_val);
            else
                nv = lr_new_float64(interp->ctx, new_val);
            scope_set_name(interp, name, nv);
            lr_free_value(interp->ctx, nv);
        }

        LRValue result;
        if (prefix) {
            /* Return new value */
            if (new_val == (double)(int32_t)new_val && !isnan(new_val) && !isinf(new_val))
                result = lr_new_int32(interp->ctx, (int32_t)new_val);
            else
                result = lr_new_float64(interp->ctx, new_val);
        } else {
            /* Return old value */
            result = arg; /* already have it */
            lr_free_value(interp->ctx, arg); /* no, we don't want to free it */
            /* Actually, we dup'd arg earlier, so we need to handle this differently */
            if (old_val == (double)(int32_t)old_val && !isnan(old_val) && !isinf(old_val))
                result = lr_new_int32(interp->ctx, (int32_t)old_val);
            else
                result = lr_new_float64(interp->ctx, old_val);
        }
        lr_free_value(interp->ctx, arg);
        return result;
    }

    return LR_VALUE_UNDEFINED;
}

static LRValue eval_assign(Interpreter *interp, ASTNode *node)
{
    const char *op = node->u.assign.op;
    ASTNode *target = node->u.assign.target;
    ASTNode *value_node = node->u.assign.value;

    /* Simple assignment: = */
    if (strcmp(op, "=") == 0) {
        LRValue val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return val;

        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, val)) {
                /* Not found in scope chain - declare in global scope */
                if (interp->error_flag) {
                    interp->error_flag = 0; /* Clear const error - just declare */
                    scope_declare_name(interp, name, val, 0);
                } else {
                    scope_declare_name(interp, name, val, 0);
                }
            }
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            const char *prop = target->u.member.prop->u.ident.name;
            lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, val));
            lr_free_value(interp->ctx, obj);
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, prop); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            LRString *atom = lr_to_atom(interp->ctx, prop);
            lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, val));
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        /* Destructuring assignment */
        if (target->type == AST_PATTERN || target->type == AST_ARRAY || target->type == AST_OBJECT) {
            eval_pattern(interp, target, val);
            if (interp->error_flag) { lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        lr_free_value(interp->ctx, val);
        return LR_VALUE_UNDEFINED;
    }

    /* Logical assignment: &&=, ||=, ??= */
    if (strcmp(op, "&&=") == 0) {
        LRValue left_val = interp_eval_node(interp, target);
        if (interp->error_flag) return left_val;
        int truthy = lr_to_bool(interp->ctx, left_val);
        if (!truthy) {
            /* Short-circuit: x &&= y → x when x is falsy */
            return left_val;
        }
        lr_free_value(interp->ctx, left_val);
        LRValue right_val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return right_val;
        LRValue result = lr_dup_value(interp->ctx, right_val);
        /* Assign back to target */
        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, result)) {
                if (!interp->error_flag)
                    scope_declare_name(interp, name, result, 0);
                interp->error_flag = 0;
            }
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (!interp->error_flag) {
                LRString *atom = lr_to_atom(interp->ctx, prop);
                lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
        }
        lr_free_value(interp->ctx, right_val);
        LRValue ret = lr_dup_value(interp->ctx, result);
        lr_free_value(interp->ctx, result);
        return ret;
    }

    if (strcmp(op, "||=") == 0) {
        LRValue left_val = interp_eval_node(interp, target);
        if (interp->error_flag) return left_val;
        int truthy = lr_to_bool(interp->ctx, left_val);
        if (truthy) {
            /* Short-circuit: x ||= y → x when x is truthy */
            return left_val;
        }
        lr_free_value(interp->ctx, left_val);
        LRValue right_val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return right_val;
        LRValue result = lr_dup_value(interp->ctx, right_val);
        /* Assign back to target */
        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, result)) {
                if (!interp->error_flag)
                    scope_declare_name(interp, name, result, 0);
                interp->error_flag = 0;
            }
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (!interp->error_flag) {
                LRString *atom = lr_to_atom(interp->ctx, prop);
                lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
        }
        lr_free_value(interp->ctx, right_val);
        LRValue ret = lr_dup_value(interp->ctx, result);
        lr_free_value(interp->ctx, result);
        return ret;
    }

    if (strcmp(op, "?" "?=") == 0) {
        LRValue left_val = interp_eval_node(interp, target);
        if (interp->error_flag) return left_val;
        if (!lr_is_null(left_val) && !lr_is_undefined(left_val)) {
            /* Short-circuit: x ??= y → x when x is not null/undefined */
            return left_val;
        }
        lr_free_value(interp->ctx, left_val);
        LRValue right_val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return right_val;
        LRValue result = lr_dup_value(interp->ctx, right_val);
        /* Assign back to target */
        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, result)) {
                if (!interp->error_flag)
                    scope_declare_name(interp, name, result, 0);
                interp->error_flag = 0;
            }
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (!interp->error_flag) {
                LRString *atom = lr_to_atom(interp->ctx, prop);
                lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
        }
        lr_free_value(interp->ctx, right_val);
        LRValue ret = lr_dup_value(interp->ctx, result);
        lr_free_value(interp->ctx, result);
        return ret;
    }

    /* Compound assignment: +=, -=, *=, /=, %=, **= */
    LRValue left_val = interp_eval_node(interp, target);
    if (interp->error_flag) return left_val;
    LRValue right_val = interp_eval_node(interp, value_node);
    if (interp->error_flag) { lr_free_value(interp->ctx, left_val); return right_val; }

    LRValue result = LR_VALUE_UNDEFINED;
    double da = to_number(interp->ctx, left_val);
    double db = to_number(interp->ctx, right_val);

    if (strcmp(op, "+=") == 0) {
        if (lr_is_string(left_val) || lr_is_string(right_val)) {
            const char *sl = lr_to_cstring(interp->ctx, left_val);
            const char *sr = lr_to_cstring(interp->ctx, right_val);
            size_t llen = strlen(sl);
            size_t rlen = strlen(sr);
            char *buf = (char *)malloc(llen + rlen + 1);
            if (buf) {
                memcpy(buf, sl, llen);
                memcpy(buf + llen, sr, rlen);
                buf[llen + rlen] = '\0';
                result = lr_new_string(interp->ctx, buf);
                free(buf);
            }
            lr_free_cstring(interp->ctx, sl);
            lr_free_cstring(interp->ctx, sr);
        } else {
            double v = da + db;
            if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
                result = lr_new_int32(interp->ctx, (int32_t)v);
            else
                result = lr_new_float64(interp->ctx, v);
        }
    } else if (strcmp(op, "-=") == 0) {
        double v = da - db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "*=") == 0) {
        double v = da * db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "/=") == 0) {
        result = lr_new_float64(interp->ctx, db == 0.0 ? da / db : da / db);
    } else if (strcmp(op, "%=") == 0) {
        double v = fmod(da, db);
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "**=") == 0) {
        result = lr_new_float64(interp->ctx, pow(da, db));
    } else {
        result = lr_dup_value(interp->ctx, left_val);
    }

    lr_free_value(interp->ctx, left_val);
    lr_free_value(interp->ctx, right_val);

    /* Assign back to target */
    if (target->type == AST_IDENTIFIER) {
        const char *name = target->u.ident.name;
        if (!scope_set_name(interp, name, result)) {
            if (!interp->error_flag)
                scope_declare_name(interp, name, result, 0);
            interp->error_flag = 0;
        }
    } else if (target->type == AST_MEMBER) {
        LRValue obj = interp_eval_node(interp, target->u.member.obj);
        if (!interp->error_flag) {
            const char *prop = target->u.member.prop->u.ident.name;
            lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
        }
        lr_free_value(interp->ctx, obj);
    } else if (target->type == AST_COMPUTED_MEMBER) {
        LRValue obj = interp_eval_node(interp, target->u.member.obj);
        LRValue prop = interp_eval_node(interp, target->u.member.prop);
        if (!interp->error_flag) {
            LRString *atom = lr_to_atom(interp->ctx, prop);
            lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
        }
        lr_free_value(interp->ctx, obj);
        lr_free_value(interp->ctx, prop);
    }

    LRValue ret = lr_dup_value(interp->ctx, result);
    lr_free_value(interp->ctx, result);
    return ret;
}

/* Build (or return cached) import.meta object for the current unit.
 * Exposes: url (file:// URL), filename (absolute path), dirname. */
static LRValue interp_get_import_meta(Interpreter *interp)
{
    LRContext *ctx = interp->ctx;

    if (interp->import_meta.tag == LR_TYPE_OBJECT)
        return lr_dup_value(ctx, interp->import_meta);

    LRValue meta = lr_new_object(ctx);
    const char *fn = interp->filename ? interp->filename : "";

    /* Resolve to an absolute path when possible */
    char abs[4096];
    abs[0] = '\0';
#ifdef _WIN32
    if (fn[0] == '\0' || fn[0] == '<' || !_fullpath(abs, fn, sizeof(abs))) {
        strncpy(abs, fn, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = '\0';
    }
#else
    if (fn[0] == '\0' || fn[0] == '<' || !realpath(fn, abs)) {
        strncpy(abs, fn, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = '\0';
    }
#endif

    /* file:// URL: normalize separators to '/' */
    char url[4352];
    {
        char norm[4096];
        size_t i;
        for (i = 0; abs[i] && i < sizeof(norm) - 1; i++)
            norm[i] = (abs[i] == '\\') ? '/' : abs[i];
        norm[i] = '\0';
        if (norm[0] == '/')
            snprintf(url, sizeof(url), "file://%s", norm);
        else if (norm[0])
            snprintf(url, sizeof(url), "file:///%s", norm);
        else
            snprintf(url, sizeof(url), "file:///");
    }
    lr_set_property_str(ctx, meta, "url", lr_new_string(ctx, url));
    lr_set_property_str(ctx, meta, "filename", lr_new_string(ctx, abs));

    /* dirname: strip last path component */
    {
        char dir[4096];
        strncpy(dir, abs, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *last = NULL;
        for (char *p = dir; *p; p++)
            if (*p == '/' || *p == '\\') last = p;
        if (last) *last = '\0';
        lr_set_property_str(ctx, meta, "dirname", lr_new_string(ctx, dir));
    }

    interp->import_meta = lr_dup_value(ctx, meta);
    return meta;
}

static LRValue eval_member(Interpreter *interp, ASTNode *node)
{
    int is_optional = node->u.member.is_optional;

    /* import.meta: the parser encodes it as member expr `import`.`meta` */
    if (node->u.member.obj && node->u.member.obj->type == AST_IDENTIFIER &&
        node->u.member.obj->u.ident.name &&
        strcmp(node->u.member.obj->u.ident.name, "import") == 0 &&
        node->u.member.prop && node->u.member.prop->type == AST_IDENTIFIER &&
        node->u.member.prop->u.ident.name &&
        strcmp(node->u.member.prop->u.ident.name, "meta") == 0) {
        return interp_get_import_meta(interp);
    }

    LRValue obj = interp_eval_node(interp, node->u.member.obj);
    if (interp->error_flag) return obj;

    if (is_optional && (lr_is_null(obj) || lr_is_undefined(obj))) {
        lr_free_value(interp->ctx, obj);
        return LR_VALUE_UNDEFINED;
    }

    const char *prop = NULL;
    if (node->u.member.prop) {
        if (node->u.member.prop->type == AST_IDENTIFIER) {
            prop = node->u.member.prop->u.ident.name;
        }
    }

    if (prop) {
        /* Try inline cache: if the property name is the same, use cached atom */
        int cache_idx = (int)((uintptr_t)prop % LR_IC_SIZE);
        LRInlineCache *cache = &interp->member_cache[cache_idx];

        if (cache->is_active && cache->prop_name == prop) {
            /* Cache hit: use cached atom directly */
            LRValue result = lr_get_property(interp->ctx, obj, cache->prop_atom);
            lr_free_value(interp->ctx, obj);
            cache->hit_count++;
            return result;
        }

        /* Cache miss: do normal lookup and update cache */
        LRValue result = lr_get_property_str(interp->ctx, obj, prop);
        lr_free_value(interp->ctx, obj);

        /* Update cache (round-robin replacement) */
        int ic_idx = interp->cache_index % LR_IC_SIZE;
        LRInlineCache *ic = &interp->member_cache[ic_idx];
        ic->prop_name = prop;
        /* Create a stable atom for the cache */
        ic->prop_atom = lr_new_atom(interp->ctx, prop);
        ic->is_active = 1;
        ic->hit_count = 0;
        interp->cache_index = (interp->cache_index + 1) % LR_IC_SIZE;

        return result;
    }

    lr_free_value(interp->ctx, obj);
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_computed_member(Interpreter *interp, ASTNode *node)
{
    int is_optional = node->u.member.is_optional;
    LRValue obj = interp_eval_node(interp, node->u.member.obj);
    if (interp->error_flag) return obj;

    if (is_optional && (lr_is_null(obj) || lr_is_undefined(obj))) {
        lr_free_value(interp->ctx, obj);
        return LR_VALUE_UNDEFINED;
    }

    LRValue prop = interp_eval_node(interp, node->u.member.prop);
    if (interp->error_flag) {
        lr_free_value(interp->ctx, obj);
        return prop;
    }

    LRString *atom = lr_to_atom(interp->ctx, prop);
    LRValue result = lr_get_property(interp->ctx, obj, atom);
    lr_free_value(interp->ctx, obj);
    lr_free_value(interp->ctx, prop);
    return result;
}

static LRValue eval_call(Interpreter *interp, ASTNode *node)
{
    ASTNode *callee_node = node->u.call.callee;
    int argc = node->u.call.argc;
    ASTNode **args = node->u.call.args;

    /* Evaluate arguments with spread support */
    LRValue *argv = NULL;
    int total_argc = 0;
    int argv_cap = argc > 0 ? argc : 0;
    if (argc > 0) {
        argv = (LRValue *)calloc(argv_cap, sizeof(LRValue));
        for (int i = 0; i < argc; i++) {
            if (args[i]->type == AST_SPREAD_ELEMENT) {
                LRValue spread_val = interp_eval_node(interp, args[i]->u.spread.arg);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    free(argv);
                    return LR_VALUE_UNDEFINED;
                }
                if (lr_is_array(interp->ctx, spread_val)) {
                    int32_t len = 0;
                    LRValue len_val = lr_get_property_str(interp->ctx, spread_val, "length");
                    lr_to_int32(interp->ctx, &len, len_val);
                    lr_free_value(interp->ctx, len_val);
                    /* Reallocate argv if needed */
                    if (total_argc + len > argv_cap) {
                        argv_cap = total_argc + len;
                        argv = (LRValue *)realloc(argv, argv_cap * sizeof(LRValue));
                    }
                    for (int32_t j = 0; j < len; j++) {
                        argv[total_argc] = lr_get_property_uint32(interp->ctx, spread_val, j);
                        total_argc++;
                    }
                }
                lr_free_value(interp->ctx, spread_val);
            } else {
                argv[total_argc] = interp_eval_node(interp, args[i]);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    free(argv);
                    return LR_VALUE_UNDEFINED;
                }
                total_argc++;
            }
        }
    }
    argc = total_argc; /* Update argc to reflect expanded arguments */

    /* Handle dynamic import() */
    if (callee_node->type == AST_IDENTIFIER &&
        strcmp(callee_node->u.ident.name, "import") == 0) {
        /* Dynamic import */
        if (argc < 1) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
            LRValue err = JS_ThrowTypeError(interp->ctx, "import() requires at least 1 argument");
            interp->error_flag = 1;
            interp->exception_pending = 1;
            interp->exception_value = lr_dup_value(interp->ctx, err);
            lr_free_value(interp->ctx, err);
            return LR_VALUE_UNDEFINED;
        }
        const char *spec = JS_ToCString(interp->ctx, argv[0]);
        if (!spec) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
            interp->error_flag = 1;
            return LR_VALUE_UNDEFINED;
        }
        /* Normalize the module specifier */
        char *normalized = NULL;
        if (interp->ctx->rt->module_normalize_func) {
            normalized = interp->ctx->rt->module_normalize_func(interp->ctx, NULL, spec, NULL);
        }
        const char *load_name = normalized ? normalized : spec;
        /* Load the module via the runtime's module loader */
        JSModuleDef *mod = NULL;
        if (interp->ctx->rt->module_loader_func) {
            mod = interp->ctx->rt->module_loader_func(interp->ctx, load_name, NULL);
        }
        JS_FreeCString(interp->ctx, spec);
        if (normalized) free(normalized);
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
        if (!mod) {
            interp->error_flag = 1;
            interp->exception_pending = 1;
            return LR_VALUE_UNDEFINED;
        }
        /* Return the module namespace wrapped in a resolved promise */
        LRValue ns_val;
        ns_val.tag = LR_TYPE_OBJECT;
        ns_val.u.ptr = mod->obj;
        if (mod->obj) mod->obj->ref_count++;
        LRValue promise = lr_new_promise(interp->ctx);
        lr_promise_resolve_internal(interp->ctx, promise, ns_val);
        lr_free_value(interp->ctx, ns_val);
        return promise;
    }

    /* Determine this binding and callee value */
    LRValue this_val = LR_VALUE_UNDEFINED;
    LRValue callee;

    /* super(...) call inside a derived class constructor */
    if (callee_node->type == AST_SUPER) {
        LRValue sup = LR_VALUE_UNDEFINED;
        if (!scope_lookup_internal(interp->current_scope, interp->ctx, "%superctor%", &sup)) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "'super' keyword unexpected here");
            interp->error_flag = 1;
            return LR_VALUE_UNDEFINED;
        }
        if (!scope_lookup_internal(interp->current_scope, interp->ctx, "this", &this_val)) {
            this_val = LR_VALUE_UNDEFINED;
        }
        LRValue result_sup = LR_VALUE_UNDEFINED;
        if (sup.tag == LR_TYPE_OBJECT) {
            LRObject *so = (LRObject *)sup.u.ptr;
            if (so->type == LR_OBJ_FUNCTION && so->extra) {
                interp->pending_closure = so->def_scope;
                result_sup = interp_invoke_function_ast(interp, (ASTNode *)so->extra,
                                                        this_val, argc, argv);
            } else {
                /* C-function base class (e.g. Error): call on this */
                lr_push_call_frame(interp->ctx, "super", NULL, 0);
                result_sup = lr_call(interp->ctx, sup, this_val, argc, argv);
                lr_pop_call_frame(interp->ctx);
            }
        }
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
        lr_free_value(interp->ctx, this_val);
        lr_free_value(interp->ctx, sup);
        return result_sup;
    }

    /* super.method(...) call: look up on parent prototype, this = current this */
    if ((callee_node->type == AST_MEMBER || callee_node->type == AST_COMPUTED_MEMBER) &&
        callee_node->u.member.obj && callee_node->u.member.obj->type == AST_SUPER) {
        LRValue sproto = LR_VALUE_UNDEFINED;
        scope_lookup_internal(interp->current_scope, interp->ctx, "%superproto%", &sproto);
        if (callee_node->type == AST_MEMBER &&
            callee_node->u.member.prop &&
            callee_node->u.member.prop->type == AST_IDENTIFIER) {
            callee = lr_get_property_str(interp->ctx, sproto,
                                         callee_node->u.member.prop->u.ident.name);
        } else {
            LRValue pk = interp_eval_node(interp, callee_node->u.member.prop);
            LRString *atom = lr_to_atom(interp->ctx, pk);
            callee = lr_get_property(interp->ctx, sproto, atom);
            lr_free_value(interp->ctx, pk);
        }
        lr_free_value(interp->ctx, sproto);
        if (!scope_lookup_internal(interp->current_scope, interp->ctx, "this", &this_val)) {
            this_val = LR_VALUE_UNDEFINED;
        }
    }
    /* Check if this is a method call (member expression as callee) */
    else if (callee_node->type == AST_MEMBER) {
        this_val = interp_eval_node(interp, callee_node->u.member.obj);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
            lr_free_value(interp->ctx, this_val);
            return LR_VALUE_UNDEFINED;
        }
        const char *prop = callee_node->u.member.prop->u.ident.name;
        callee = lr_get_property_str(interp->ctx, this_val, prop);
    } else if (callee_node->type == AST_COMPUTED_MEMBER) {
        this_val = interp_eval_node(interp, callee_node->u.member.obj);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
            lr_free_value(interp->ctx, this_val);
            return LR_VALUE_UNDEFINED;
        }
        LRValue prop = interp_eval_node(interp, callee_node->u.member.prop);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
            lr_free_value(interp->ctx, this_val);
            lr_free_value(interp->ctx, prop);
            return LR_VALUE_UNDEFINED;
        }
        LRString *atom = lr_to_atom(interp->ctx, prop);
        callee = lr_get_property(interp->ctx, this_val, atom);
        lr_free_value(interp->ctx, prop);
    } else {
        /* Normal function call - this = undefined (non-strict) or global */
        /* In non-strict mode, undefined this becomes global object */
        callee = interp_eval_node(interp, callee_node);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
            return LR_VALUE_UNDEFINED;
        }
        this_val = lr_get_global_object(interp->ctx);
    }

    if (interp->error_flag) {
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
        lr_free_value(interp->ctx, this_val);
        lr_free_value(interp->ctx, callee);
        return LR_VALUE_UNDEFINED;
    }

    /* Optional chaining short-circuit: if callee is null/undefined, return undefined */
    if (node->type == AST_OPTIONAL_CALL && (lr_is_null(callee) || lr_is_undefined(callee))) {
        if (argv) {
            for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]);
            free(argv);
        }
        lr_free_value(interp->ctx, this_val);
        lr_free_value(interp->ctx, callee);
        return LR_VALUE_UNDEFINED;
    }

    LRValue result;

    /* Check if callee is a native C function */
    if (lr_is_function(interp->ctx, callee)) {
        if (callee.tag == LR_TYPE_OBJECT) {
            LRObject *obj = (LRObject *)callee.u.ptr;
            if (obj->type == LR_OBJ_CFUNCTION) {
                /* Push a call frame for C function */
                LRCFunction *cf = (LRCFunction *)obj->extra;
                const char *cname = cf->name ? cf->name : "";
                lr_push_call_frame(interp->ctx, cname, NULL, 0);

                result = lr_call(interp->ctx, callee, this_val, argc, argv);

                lr_pop_call_frame(interp->ctx);

                if (lr_is_exception(result)) {
                    /* Capture exception */
                    interp->exception_pending = 1;
                    interp->exception_value = lr_dup_value(interp->ctx, result);
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "%s", lr_get_exception_str(interp->ctx));
                    interp->error_flag = 1;
                }
                goto call_done;
            }
        }
    }

    /* Check if callee is a interpreted function (AST_FUNC_EXPR or AST_ARROW) */
    if (callee_node->type == AST_FUNC_EXPR || callee_node->type == AST_ARROW ||
        callee_node->type == AST_FUNC_DECL) {
        /* Direct function expression call */
        result = interp_call_function(interp, callee_node, this_val, argc, argv);
        goto call_done;
    }

    /* Also handle the case where callee is a function expression that was stored */
    if (callee.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)callee.u.ptr;
        if (obj->type == LR_OBJ_BYTECODE_FUNC) {
            /* Bytecode function - for now, try calling via lr_call */
            lr_push_call_frame(interp->ctx, "", NULL, 0);
            result = lr_call(interp->ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(interp->ctx);
            goto call_done;
        }
        if (obj->type == LR_OBJ_CFUNCTION) {
            const char *cname = "";
            LRCFunction *cf = (LRCFunction *)obj->extra;
            if (cf) cname = cf->name ? cf->name : "";
            lr_push_call_frame(interp->ctx, cname, NULL, 0);
            result = lr_call(interp->ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(interp->ctx);
            goto call_done;
        }
        if (obj->type == LR_OBJ_FUNCTION) {
            /* Interpreted function object - try to get the AST from extra */
            if (obj->extra) {
                ASTNode *func_ast = (ASTNode *)obj->extra;
                interp->pending_closure = obj->def_scope;
                result = interp_invoke_function_ast(interp, func_ast, this_val, argc, argv);
                goto call_done;
            }
        }
    }

    /* Fallback: try lr_call */
    lr_push_call_frame(interp->ctx, "", NULL, 0);
    result = lr_call(interp->ctx, callee, this_val, argc, argv);
    lr_pop_call_frame(interp->ctx);
    if (lr_is_exception(result)) {
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(interp->ctx, result);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s", lr_get_exception_str(interp->ctx));
        interp->error_flag = 1;
    }

call_done:
    if (argv) {
        for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]);
        free(argv);
    }
    lr_free_value(interp->ctx, this_val);
    lr_free_value(interp->ctx, callee);
    return result;
}

static LRValue eval_new(Interpreter *interp, ASTNode *node)
{
    int argc = node->u.new_expr.argc;
    ASTNode **args = node->u.new_expr.args;
    ASTNode *callee_node = node->u.new_expr.callee;

    /* Evaluate arguments with spread support */
    LRValue *argv = NULL;
    int total_argc = 0;
    int argv_cap = argc > 0 ? argc : 0;
    if (argc > 0) {
        argv = (LRValue *)calloc(argv_cap, sizeof(LRValue));
        for (int i = 0; i < argc; i++) {
            if (args[i]->type == AST_SPREAD_ELEMENT) {
                LRValue spread_val = interp_eval_node(interp, args[i]->u.spread.arg);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    free(argv);
                    return LR_VALUE_UNDEFINED;
                }
                if (lr_is_array(interp->ctx, spread_val)) {
                    int32_t len = 0;
                    LRValue len_val = lr_get_property_str(interp->ctx, spread_val, "length");
                    lr_to_int32(interp->ctx, &len, len_val);
                    lr_free_value(interp->ctx, len_val);
                    if (total_argc + len > argv_cap) {
                        argv_cap = total_argc + len;
                        argv = (LRValue *)realloc(argv, argv_cap * sizeof(LRValue));
                    }
                    for (int32_t j = 0; j < len; j++) {
                        argv[total_argc] = lr_get_property_uint32(interp->ctx, spread_val, j);
                        total_argc++;
                    }
                }
                lr_free_value(interp->ctx, spread_val);
            } else {
                argv[total_argc] = interp_eval_node(interp, args[i]);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    free(argv);
                    return LR_VALUE_UNDEFINED;
                }
                total_argc++;
            }
        }
    }
    argc = total_argc; /* Update argc to reflect expanded arguments */

    LRValue callee = interp_eval_node(interp, callee_node);
    if (interp->error_flag) {
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); free(argv); }
        return LR_VALUE_UNDEFINED;
    }

    /* Push call frame for constructor */
    const char *ctor_name = "";
    if (callee.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)callee.u.ptr;
        if (obj->type == LR_OBJ_CFUNCTION) {
            LRCFunction *cf = (LRCFunction *)obj->extra;
            if (cf && cf->name) ctor_name = cf->name;
        }
    }
    lr_push_call_frame(interp->ctx, ctor_name, NULL, 0);
    LRValue result = lr_call_constructor(interp->ctx, callee, argc, argv);
    lr_pop_call_frame(interp->ctx);
    if (lr_is_exception(result)) {
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(interp->ctx, result);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s", lr_get_exception_str(interp->ctx));
        interp->error_flag = 1;
    }

    if (argv) {
        for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]);
        free(argv);
    }
    lr_free_value(interp->ctx, callee);
    return result;
}

static LRValue eval_conditional(Interpreter *interp, ASTNode *node)
{
    LRValue cond = interp_eval_node(interp, node->u.conditional.cond);
    if (interp->error_flag) return cond;

    int truthy = lr_to_bool(interp->ctx, cond);
    lr_free_value(interp->ctx, cond);

    if (truthy) {
        return interp_eval_node(interp, node->u.conditional.consequent);
    } else {
        return interp_eval_node(interp, node->u.conditional.alternate);
    }
}

static LRValue call_value_with_args(Interpreter *interp, ASTNode *callee_node,
                                    LRValue callee, LRValue this_val,
                                    int argc, LRValue *argv);

/* Spread `src` into array `arr` starting at *out_idx. Supports arrays,
 * strings, and iterables (Symbol.iterator, e.g. generator objects).
 * Returns 0 on success, -1 on error (error_flag set). */
static int spread_into_array(Interpreter *interp, LRValue arr, LRValue src, uint32_t *out_idx)
{
    LRContext *ctx = interp->ctx;
    if (lr_is_array(ctx, src)) {
        int32_t len = 0;
        LRValue len_val = lr_get_property_str(ctx, src, "length");
        lr_to_int32(ctx, &len, len_val);
        lr_free_value(ctx, len_val);
        for (int32_t j = 0; j < len; j++) {
            LRValue item = lr_get_property_uint32(ctx, src, (uint32_t)j);
            lr_set_property_uint32(ctx, arr, (*out_idx)++, item);
        }
        return 0;
    }
    if (lr_is_string(src)) {
        const char *s = lr_to_cstring(ctx, src);
        size_t slen = s ? strlen(s) : 0;
        char buf[2];
        for (size_t j = 0; j < slen; j++) {
            buf[0] = s[j]; buf[1] = '\0';
            lr_set_property_uint32(ctx, arr, (*out_idx)++, lr_new_string(ctx, buf));
        }
        lr_free_cstring(ctx, s);
        return 0;
    }
    if (lr_is_object(src)) {
        LRValue iter_fn = lr_get_property_str(ctx, src, "Symbol.iterator");
        if (lr_is_function(ctx, iter_fn)) {
            LRValue iter = call_value_with_args(interp, NULL, iter_fn, src, 0, NULL);
            lr_free_value(ctx, iter_fn);
            if (interp->error_flag) { lr_free_value(ctx, iter); return -1; }
            if (lr_is_object(iter)) {
                LRValue next_fn = lr_get_property_str(ctx, iter, "next");
                while (!interp->error_flag) {
                    LRValue nr = call_value_with_args(interp, NULL, next_fn, iter, 0, NULL);
                    if (interp->error_flag) { lr_free_value(ctx, nr); break; }
                    LRValue done = lr_get_property_str(ctx, nr, "done");
                    int is_done = lr_to_bool(ctx, done);
                    lr_free_value(ctx, done);
                    if (is_done) { lr_free_value(ctx, nr); break; }
                    LRValue value = lr_get_property_str(ctx, nr, "value");
                    lr_set_property_uint32(ctx, arr, (*out_idx)++, value);
                    lr_free_value(ctx, nr);
                }
                lr_free_value(ctx, next_fn);
            }
            lr_free_value(ctx, iter);
            return interp->error_flag ? -1 : 0;
        }
        lr_free_value(ctx, iter_fn);
    }
    snprintf(interp->error_message, sizeof(interp->error_message),
             "value is not iterable (cannot spread)");
    interp->exception_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 1;
    return -1;
}

static LRValue eval_array(Interpreter *interp, ASTNode *node)
{
    LRValue arr = lr_new_array(interp->ctx);
    int nelem = node->u.array.nelem;
    ASTNode **elements = node->u.array.elements;
    uint32_t out = 0;

    for (int i = 0; i < nelem; i++) {
        ASTNode *elem = elements[i];
        if (elem == NULL) {
            /* Array hole - skip (remains undefined) */
            out++;
            continue;
        }
        if (elem->type == AST_SPREAD_ELEMENT) {
            /* Spread element */
            LRValue spread_val = interp_eval_node(interp, elem->u.spread.arg);
            if (interp->error_flag) { lr_free_value(interp->ctx, arr); lr_free_value(interp->ctx, spread_val); return LR_VALUE_UNDEFINED; }
            if (spread_into_array(interp, arr, spread_val, &out) != 0) {
                lr_free_value(interp->ctx, spread_val);
                lr_free_value(interp->ctx, arr);
                return LR_VALUE_UNDEFINED;
            }
            lr_free_value(interp->ctx, spread_val);
        } else {
            LRValue val = interp_eval_node(interp, elem);
            if (interp->error_flag) { lr_free_value(interp->ctx, arr); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            /* lr_set_property_uint32 takes ownership of val, do NOT free it */
            lr_set_property_uint32(interp->ctx, arr, out++, val);
        }
    }

    /* Set length */
    lr_set_property_str(interp->ctx, arr, "length", lr_new_int32(interp->ctx, (int32_t)out));
    return arr;
}

static LRValue eval_object(Interpreter *interp, ASTNode *node)
{
    LRValue obj = lr_new_object(interp->ctx);
    int nprops = node->u.object.nprops;
    ASTNode **props = node->u.object.props;

    for (int i = 0; i < nprops; i++) {
        ASTNode *prop = props[i];
        if (prop->type == AST_SPREAD) {
            /* Spread property */
            LRValue spread_val = interp_eval_node(interp, prop->u.spread.arg);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, spread_val); return LR_VALUE_UNDEFINED; }
            if (lr_is_object(spread_val)) {
                /* Copy own properties */
                LRPropertyEnum *pe = NULL;
                uint32_t npe = 0;
                lr_get_own_property_names(interp->ctx, &pe, &npe, spread_val, 0);
                for (uint32_t j = 0; j < npe; j++) {
                    LRValue val = lr_get_property(interp->ctx, spread_val, pe[j].atom);
                    lr_set_property(interp->ctx, obj, pe[j].atom, val);
                    lr_free_value(interp->ctx, val);
                }
                lr_free_property_enum(interp->ctx, pe, npe);
            }
            lr_free_value(interp->ctx, spread_val);
        } else {
            ASTNode *key_node = prop->u.property.key;
            ASTNode *val_node = prop->u.property.val;
            int shorthand = prop->u.property.shorthand;

            /* Accessor property: { get x() {...} } / { set x(v) {...} } */
            int is_accessor = (val_node && val_node->type == AST_FUNC_EXPR &&
                               (val_node->u.func.is_getter || val_node->u.func.is_setter));

            LRValue key_val;
            if (key_node->type == AST_IDENTIFIER) {
                const char *kname = key_node->u.ident.name;
                if (is_accessor) {
                    LRValue fn = eval_func_expr(interp, val_node);
                    if (val_node->u.func.is_getter) {
                        lr_set_accessor_property_str(interp->ctx, obj, kname,
                                                     fn, LR_VALUE_UNDEFINED);
                    } else {
                        lr_set_accessor_property_str(interp->ctx, obj, kname,
                                                     LR_VALUE_UNDEFINED, fn);
                    }
                } else {
                    /* Regular or shorthand property: { x: v } / { x } */
                    (void)shorthand;
                    LRValue val = interp_eval_node(interp, val_node);
                    if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
                    /* lr_set_property_str takes ownership of val */
                    lr_set_property_str(interp->ctx, obj, kname, val);
                }
            } else {
                /* Computed or literal key */
                key_val = interp_eval_node(interp, key_node);
                if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, key_val); return LR_VALUE_UNDEFINED; }
                LRString *atom = lr_to_atom(interp->ctx, key_val);
                if (is_accessor) {
                    LRValue fn = eval_func_expr(interp, val_node);
                    if (val_node->u.func.is_getter) {
                        lr_set_accessor_property(interp->ctx, obj, atom,
                                                 fn, LR_VALUE_UNDEFINED);
                    } else {
                        lr_set_accessor_property(interp->ctx, obj, atom,
                                                 LR_VALUE_UNDEFINED, fn);
                    }
                } else {
                    LRValue val = interp_eval_node(interp, val_node);
                    if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, key_val); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
                    /* lr_set_property takes ownership of val */
                    lr_set_property(interp->ctx, obj, atom, val);
                }
                lr_free_value(interp->ctx, key_val);
            }
        }
    }

    return obj;
}

static LRValue eval_func_expr(Interpreter *interp, ASTNode *node)
{
    /* Create a function object with Function.prototype as its prototype */
    LRValue obj = lr_new_object_proto(interp->ctx, interp->ctx->function_proto);
    if (obj.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)obj.u.ptr;
        o->type = LR_OBJ_FUNCTION;
        /* Store the AST node as extra data so we can find it later */
        o->extra = (void *)node;
        /* Capture the defining scope for lexical closures */
        interp_capture_closure(interp, obj);
    }
    return obj;
}

static LRValue eval_arrow(Interpreter *interp, ASTNode *node)
{
    /* Arrow functions are similar to function expressions */
    LRValue obj = lr_new_object_proto(interp->ctx, interp->ctx->function_proto);
    if (obj.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)obj.u.ptr;
        o->type = LR_OBJ_FUNCTION;
        o->extra = (void *)node;
        interp_capture_closure(interp, obj);
    }
    return obj;
}

/* Unescape a template literal *cooked* text fragment into a freshly malloc'd
 * string (caller must free). Handles the common escape sequences. */
static char *template_unescape(const char *s)
{
    if (!s) { char *e = (char *)malloc(1); if (e) e[0] = '\0'; return e; }
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char n = s[i + 1];
            switch (n) {
            case 'n': out[j++] = '\n'; i++; break;
            case 't': out[j++] = '\t'; i++; break;
            case 'r': out[j++] = '\r'; i++; break;
            case 'b': out[j++] = '\b'; i++; break;
            case 'f': out[j++] = '\f'; i++; break;
            case 'v': out[j++] = '\v'; i++; break;
            case '0':
                if (i + 2 >= len || !isdigit((unsigned char)s[i + 2])) { out[j++] = '\0'; i++; }
                else out[j++] = c;
                break;
            case 'x':
                if (i + 3 < len) {
                    unsigned v = 0; int ok = 1;
                    for (int k = 0; k < 2; k++) {
                        char h = s[i + 2 + k];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) { ok = 0; break; }
                        v = (v << 4) | d;
                    }
                    if (ok) { out[j++] = (char)v; i += 3; break; }
                }
                out[j++] = c; break;
            case 'u':
                if (i + 2 < len && s[i + 2] == '{') {
                    unsigned v = 0; int ok = 1; size_t k = i + 3;
                    while (k < len && s[k] != '}') {
                        char h = s[k];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) { ok = 0; break; }
                        v = (v << 4) | d; k++;
                    }
                    if (ok && k < len) { out[j++] = (char)v; i = k; break; }
                    out[j++] = c; break;
                }
                if (i + 5 < len) {
                    unsigned v = 0; int ok = 1;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i + 2 + k];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) { ok = 0; break; }
                        v = (v << 4) | d;
                    }
                    if (ok) { out[j++] = (char)v; i += 5; break; }
                }
                out[j++] = c; break;
            case '\n': i++; break;                       /* line continuation */
            case '\r': i++; if (i + 1 < len && s[i + 1] == '\n') i++; break;
            case '\\': case '\'': case '"': case '`':
                out[j++] = n; i++; break;
            default: out[j++] = n; i++; break;          /* unknown escape: keep char */
            }
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

/* Invoke a callable value (function object or AST function) with already
 * evaluated arguments. Mirrors the tail of eval_call(). */
static LRValue call_value_with_args(Interpreter *interp, ASTNode *callee_node,
                                    LRValue callee, LRValue this_val,
                                    int argc, LRValue *argv)
{
    LRContext *ctx = interp->ctx;
    LRValue result;

    if (lr_is_function(ctx, callee) && callee.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)callee.u.ptr;
        if (obj->type == LR_OBJ_CFUNCTION) {
            LRCFunction *cf = (LRCFunction *)obj->extra;
            const char *cname = cf && cf->name ? cf->name : "";
            lr_push_call_frame(ctx, cname, NULL, 0);
            result = lr_call(ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(ctx);
            if (lr_is_exception(result)) {
                interp->exception_pending = 1;
                interp->exception_value = lr_dup_value(ctx, result);
                snprintf(interp->error_message, sizeof(interp->error_message),
                         "%s", lr_get_exception_str(ctx));
                interp->error_flag = 1;
            }
            return result;
        }
        if (obj->type == LR_OBJ_FUNCTION && obj->extra) {
            interp->pending_closure = obj->def_scope;
            return interp_invoke_function_ast(interp, (ASTNode *)obj->extra,
                                              this_val, argc, argv);
        }
        if (obj->type == LR_OBJ_BYTECODE_FUNC) {
            lr_push_call_frame(ctx, "", NULL, 0);
            result = lr_call(ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(ctx);
            return result;
        }
    }

    if (callee_node && (callee_node->type == AST_FUNC_EXPR ||
                        callee_node->type == AST_ARROW ||
                        callee_node->type == AST_FUNC_DECL)) {
        return interp_call_function(interp, callee_node, this_val, argc, argv);
    }

    lr_push_call_frame(ctx, "", NULL, 0);
    result = lr_call(ctx, callee, this_val, argc, argv);
    lr_pop_call_frame(ctx);
    if (lr_is_exception(result)) {
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(ctx, result);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s", lr_get_exception_str(ctx));
        interp->error_flag = 1;
    }
    return result;
}

static LRValue eval_template(Interpreter *interp, ASTNode *node)
{
    /* Template literal: `text ${expr} text` */
    int nparts = node->u.template_lit.nparts;
    int nexp   = node->u.template_lit.nexp;
    ASTNode **exprs = node->u.template_lit.exprs;

    /* Estimate total length using cooked (unescaped) fragments. */
    size_t total_len = 0;
    for (int i = 0; i < nparts; i++) {
        char *cooked = template_unescape(node->u.template_lit.parts[i]);
        total_len += strlen(cooked ? cooked : "");
        if (cooked) free(cooked);
    }

    LRValue *expr_vals = NULL;
    if (nexp > 0) {
        expr_vals = (LRValue *)calloc(nexp, sizeof(LRValue));
        for (int i = 0; i < nexp; i++) {
            expr_vals[i] = interp_eval_node(interp, exprs[i]);
            if (interp->error_flag) {
                for (int j = 0; j < i; j++) lr_free_value(interp->ctx, expr_vals[j]);
                free(expr_vals);
                return LR_VALUE_UNDEFINED;
            }
            const char *s = lr_to_cstring(interp->ctx, expr_vals[i]);
            total_len += strlen(s);
            lr_free_cstring(interp->ctx, s);
        }
    }

    char *buf = (char *)malloc(total_len + 1);
    if (!buf) {
        if (expr_vals) {
            for (int i = 0; i < nexp; i++) lr_free_value(interp->ctx, expr_vals[i]);
            free(expr_vals);
        }
        return LR_VALUE_UNDEFINED;
    }

    size_t pos = 0;
    for (int i = 0; i < nparts; i++) {
        char *cooked = template_unescape(node->u.template_lit.parts[i]);
        const char *p = cooked ? cooked : "";
        size_t plen = strlen(p);
        if (plen) { memcpy(buf + pos, p, plen); pos += plen; }
        if (cooked) free(cooked);
        if (i < nexp) {
            const char *s = lr_to_cstring(interp->ctx, expr_vals[i]);
            size_t slen = strlen(s);
            if (slen) { memcpy(buf + pos, s, slen); pos += slen; }
            lr_free_cstring(interp->ctx, s);
        }
    }
    buf[pos] = '\0';

    LRValue result = lr_new_string(interp->ctx, buf);
    free(buf);

    if (expr_vals) {
        for (int i = 0; i < nexp; i++) lr_free_value(interp->ctx, expr_vals[i]);
        free(expr_vals);
    }
    return result;
}

static LRValue eval_sequence(Interpreter *interp, ASTNode *node)
{
    LRValue result = LR_VALUE_UNDEFINED;
    int count = node->u.sequence.count;
    ASTNode **exprs = node->u.sequence.exprs;

    for (int i = 0; i < count; i++) {
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, exprs[i]);
        if (interp->error_flag) return result;
    }

    return result;
}

static LRValue eval_spread(Interpreter *interp, ASTNode *node)
{
    /* Spread element in expression context - evaluate the argument */
    return interp_eval_node(interp, node->u.spread.arg);
}

static LRValue eval_await(Interpreter *interp, ASTNode *node)
{
    /* Evaluate the argument */
    LRValue arg = interp_eval_node(interp, node->u.await_expr.arg);
    if (interp->error_flag) return arg;

    /* If the result is a Promise, settle it (draining microtasks if
     * needed), then unwrap: fulfilled -> value, rejected -> throw */
    if (lr_is_promise(interp->ctx, arg)) {
        LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(arg);
        if (pd && pd->state == LR_PROMISE_PENDING) {
            LRContext *jctx = NULL;
            int guard = 100000;
            while (pd->state == LR_PROMISE_PENDING && guard-- > 0) {
                if (lr_execute_pending_job(interp->ctx->rt, &jctx) <= 0) break;
            }
        }
        if (pd && pd->state == LR_PROMISE_REJECTED) {
            interp->exception_pending = 1;
            interp->exception_value = lr_dup_value(interp->ctx, pd->result);
            const char *s = lr_to_cstring(interp->ctx, pd->result);
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "%s", s ? s : "Promise rejected");
            lr_free_cstring(interp->ctx, s);
            interp->error_flag = 1;
            lr_free_value(interp->ctx, arg);
            return LR_VALUE_UNDEFINED;
        }
        LRValue result = pd ? lr_dup_value(interp->ctx, pd->result)
                            : LR_VALUE_UNDEFINED;
        lr_free_value(interp->ctx, arg);
        return result;
    }

    /* If it's a thenable (object with .then), we handle it as a promise */
    if (lr_is_object(arg)) {
        LRValue then_val = lr_get_property_str(interp->ctx, arg, "then");
        int has_then = lr_is_function(interp->ctx, then_val);
        lr_free_value(interp->ctx, then_val);
        if (has_then) {
            /* For now, just return the value directly.
             * In a full implementation, we'd need to suspend/resume the
             * interpreter, which requires async/await support at the
             * interpreter level. */
            return arg;
        }
    }

    /* Non-promise values are returned directly */
    return arg;
}

static LRValue eval_class_expr(Interpreter *interp, ASTNode *node)
{
    LRContext *ctx = interp->ctx;

    /* Evaluate 'extends' clause */
    LRValue parent = LR_VALUE_UNDEFINED;
    if (node->u.class_decl.extends) {
        parent = interp_eval_node(interp, node->u.class_decl.extends);
        if (interp->error_flag) {
            lr_free_value(ctx, parent);
            return LR_VALUE_UNDEFINED;
        }
    }

    /* Create the prototype object (inherits from parent.prototype) */
    LRValue proto;
    if (lr_is_object(parent)) {
        LRValue pproto = lr_get_property_str(ctx, parent, "prototype");
        if (lr_is_object(pproto)) {
            proto = lr_new_object_proto(ctx, pproto);
        } else {
            proto = lr_new_object(ctx);
        }
        lr_free_value(ctx, pproto);
    } else {
        proto = lr_new_object(ctx);
    }

    /* Create the constructor function object.
     * Its extra points at the class AST node so calls/new dispatch through
     * interp_call_class_function (field init + ctor body + implicit super).
     * Static members are inherited from the parent class object. */
    LRValue ctor;
    if (lr_is_object(parent)) {
        ctor = lr_new_object_proto(ctx, parent);
    } else {
        ctor = lr_new_object_proto(ctx, ctx->function_proto);
    }
    if (ctor.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)ctor.u.ptr;
        o->type = LR_OBJ_FUNCTION;
        o->extra = (void *)node;
        interp_capture_closure(interp, ctor);
    }

    const char *class_name = node->u.class_decl.name;
    if (class_name) {
        lr_set_property_str(ctx, ctor, "name", lr_new_string(ctx, class_name));
    }

    /* Wire prototype <-> constructor */
    lr_set_property_str(ctx, ctor, "prototype", lr_dup_value(ctx, proto));
    lr_set_property_str(ctx, proto, "constructor", lr_dup_value(ctx, ctor));

    /* Install methods, accessors, and static fields.
     * An inner scope binds the class name so static fields/blocks can
     * reference the class being defined (e.g. static { C.v = 42; }). */
    interp_push_scope(interp, 0);
    if (class_name) {
        scope_declare_name(interp, class_name, lr_dup_value(ctx, ctor), 1);
    }
    int nmethods = node->u.class_decl.nmethods;
    ASTNode **methods = node->u.class_decl.methods;
    for (int i = 0; i < nmethods; i++) {
        ASTNode *m = methods[i];
        if (!m) continue;

        if (m->type == AST_FUNC_EXPR) {
            const char *mname = m->u.func.name;

            /* The constructor body is invoked via the class node itself */
            if (!m->u.func.is_static && !m->u.func.is_getter && !m->u.func.is_setter &&
                mname && strcmp(mname, "constructor") == 0) {
                continue;
            }

            /* Static initialization block: run now with this = class */
            if (m->u.func.is_static && mname &&
                strcmp(mname, "__static_block__") == 0) {
                LRValue fn = eval_func_expr(interp, m);
                LRValue r = call_value_with_args(interp, m, fn, ctor, 0, NULL);
                lr_free_value(ctx, r);
                lr_free_value(ctx, fn);
                if (interp->error_flag) break;
                continue;
            }

            LRValue target = m->u.func.is_static ? ctor : proto;
            LRValue fn = eval_func_expr(interp, m);

            if (m->u.func.key_expr) {
                /* Computed method name: [expr]() {} */
                LRValue kv = interp_eval_node(interp, m->u.func.key_expr);
                if (interp->error_flag) {
                    lr_free_value(ctx, fn);
                    lr_free_value(ctx, kv);
                    continue;
                }
                LRString *atom = lr_to_atom(ctx, kv);
                if (m->u.func.is_getter) {
                    lr_set_accessor_property(ctx, target, atom, fn, LR_VALUE_UNDEFINED);
                } else if (m->u.func.is_setter) {
                    lr_set_accessor_property(ctx, target, atom, LR_VALUE_UNDEFINED, fn);
                } else {
                    lr_set_property(ctx, target, atom, fn);
                }
                lr_free_value(ctx, kv);
            } else {
                const char *key = mname ? mname : "";
                if (m->u.func.is_getter) {
                    lr_set_accessor_property_str(ctx, target, key, fn, LR_VALUE_UNDEFINED);
                } else if (m->u.func.is_setter) {
                    lr_set_accessor_property_str(ctx, target, key, LR_VALUE_UNDEFINED, fn);
                } else {
                    lr_set_property_str(ctx, target, key, fn);
                }
            }
        } else if (m->type == AST_PROPERTY && m->u.property.is_static) {
            /* Static class field: evaluated once at class definition */
            LRValue v = m->u.property.val
                ? interp_eval_node(interp, m->u.property.val)
                : LR_VALUE_UNDEFINED;
            if (interp->error_flag) {
                lr_free_value(ctx, v);
                continue;
            }
            if (m->u.property.key && m->u.property.key->type == AST_IDENTIFIER) {
                lr_set_property_str(ctx, ctor, m->u.property.key->u.ident.name, v);
            } else if (m->u.property.key) {
                LRValue kv = interp_eval_node(interp, m->u.property.key);
                LRString *atom = lr_to_atom(ctx, kv);
                lr_set_property(ctx, ctor, atom, v);
                lr_free_value(ctx, kv);
            } else {
                lr_free_value(ctx, v);
            }
        }
        /* Instance fields (AST_PROPERTY, !is_static) are initialized in
         * interp_call_class_function at construction time. */
    }
    interp_pop_scope(interp);

    lr_free_value(ctx, proto);
    lr_free_value(ctx, parent);
    return ctor;
}

/* ── Destructuring ─────────────────────────────────────────────────────── */

static LRValue eval_pattern(Interpreter *interp, ASTNode *node, LRValue value)
{
    /* Discriminate array vs object patterns: AST_ARRAY/AST_OBJECT literals
     * used as assignment targets, or AST_PATTERN with is_object flag. */
    int is_obj_pattern = (node->type == AST_OBJECT) ||
        (node->type == AST_PATTERN && node->u.pattern_object.is_object);

    /* Handle array destructuring: [a, b] = arr */
    if (!is_obj_pattern) {
        /* Array destructuring */
        int nelem = node->u.pattern_array.nelem;
        ASTNode **elements = node->u.pattern_array.elements;

        for (int i = 0; i < nelem; i++) {
            ASTNode *elem = elements[i];
            if (elem == NULL) continue; /* hole */

            if (elem->type == AST_REST || elem->type == AST_SPREAD_ELEMENT) {
                /* Rest element: ...rest */
                ASTNode *rest_target = elem->type == AST_REST ? elem->u.rest_elem.arg
                                                              : elem->u.spread.arg;
                if (rest_target && rest_target->type == AST_IDENTIFIER) {
                    const char *name = rest_target->u.ident.name;
                    /* Create a new array with remaining elements */
                    LRValue rest_arr = lr_new_array(interp->ctx);
                    int32_t len = 0;
                    LRValue len_val = lr_get_property_str(interp->ctx, value, "length");
                    lr_to_int32(interp->ctx, &len, len_val);
                    lr_free_value(interp->ctx, len_val);
                    int idx = 0;
                    for (int32_t j = i; j < len; j++) {
                        LRValue item = lr_get_property_uint32(interp->ctx, value, j);
                        lr_set_property_uint32(interp->ctx, rest_arr, idx++, item);
                        lr_free_value(interp->ctx, item);
                    }
                    lr_set_property_str(interp->ctx, rest_arr, "length", lr_new_int32(interp->ctx, idx));
                    scope_declare_name(interp, name, rest_arr, 0);
                    lr_free_value(interp->ctx, rest_arr);
                }
                break;
            }

            if (elem->type == AST_DEFAULT_VALUE) {
                ASTNode *left = elem->u.default_val.left;
                /* Get value or default */
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                if (lr_is_undefined(item_val)) {
                    lr_free_value(interp->ctx, item_val);
                    item_val = interp_eval_node(interp, elem->u.default_val.right);
                }
                if (left->type == AST_IDENTIFIER) {
                    scope_declare_name(interp, left->u.ident.name, item_val, 0);
                }
                lr_free_value(interp->ctx, item_val);
            } else if (elem->type == AST_IDENTIFIER) {
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                scope_declare_name(interp, elem->u.ident.name, item_val, 0);
                lr_free_value(interp->ctx, item_val);
            } else if (elem->type == AST_PATTERN || elem->type == AST_ARRAY ||
                       elem->type == AST_OBJECT) {
                /* Nested destructuring */
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                eval_pattern(interp, elem, item_val);
                lr_free_value(interp->ctx, item_val);
            } else if (elem->type == AST_ASSIGN &&
                       elem->u.assign.target &&
                       elem->u.assign.target->type == AST_IDENTIFIER) {
                /* Default from expression form: [a = 1] = ... */
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                if (lr_is_undefined(item_val)) {
                    lr_free_value(interp->ctx, item_val);
                    item_val = interp_eval_node(interp, elem->u.assign.value);
                }
                scope_declare_name(interp, elem->u.assign.target->u.ident.name, item_val, 0);
                lr_free_value(interp->ctx, item_val);
            }
        }
        return LR_VALUE_UNDEFINED;
    }

    /* Object destructuring: {a, b} = obj */
    {
        int nprops = node->u.pattern_object.nprops;
        ASTNode **props = node->u.pattern_object.props;

        for (int i = 0; i < nprops; i++) {
            ASTNode *prop = props[i];
            if (prop->type == AST_REST || prop->type == AST_SPREAD ||
                prop->type == AST_SPREAD_ELEMENT) {
                /* Rest in object pattern */
                ASTNode *rest_target = prop->type == AST_REST ? prop->u.rest_elem.arg
                                                              : prop->u.spread.arg;
                if (rest_target && rest_target->type == AST_IDENTIFIER) {
                    LRValue rest_obj = lr_new_object(interp->ctx);
                    /* Copy own properties, excluding keys consumed by the
                     * preceding pattern properties. */
                    LRPropertyEnum *pe = NULL;
                    uint32_t npe = 0;
                    lr_get_own_property_names(interp->ctx, &pe, &npe, value, 0);
                    for (uint32_t j = 0; j < npe; j++) {
                        const char *kname = lr_atom_to_cstring(interp->ctx, pe[j].atom);
                        int consumed = 0;
                        for (int k = 0; k < i && !consumed; k++) {
                            ASTNode *pp = props[k];
                            if (pp && pp->type == AST_PROPERTY && pp->u.property.key &&
                                pp->u.property.key->type == AST_IDENTIFIER &&
                                kname &&
                                strcmp(pp->u.property.key->u.ident.name, kname) == 0) {
                                consumed = 1;
                            }
                        }
                        if (!consumed) {
                            LRValue v = lr_get_property(interp->ctx, value, pe[j].atom);
                            lr_set_property(interp->ctx, rest_obj, pe[j].atom, v);
                            lr_free_value(interp->ctx, v);
                        }
                    }
                    lr_free_property_enum(interp->ctx, pe, npe);
                    scope_declare_name(interp, rest_target->u.ident.name, rest_obj, 0);
                    lr_free_value(interp->ctx, rest_obj);
                }
                break;
            }

            if (prop->type == AST_PROPERTY) {
                ASTNode *key_node = prop->u.property.key;
                ASTNode *val_node = prop->u.property.val;
                int shorthand = prop->u.property.shorthand;

                const char *key_name = NULL;
                if (key_node->type == AST_IDENTIFIER) {
                    key_name = key_node->u.ident.name;
                }

                if (key_name) {
                    LRValue prop_val = lr_get_property_str(interp->ctx, value, key_name);

                    if (val_node->type == AST_DEFAULT_VALUE) {
                        ASTNode *left = val_node->u.default_val.left;
                        if (lr_is_undefined(prop_val)) {
                            lr_free_value(interp->ctx, prop_val);
                            prop_val = interp_eval_node(interp, val_node->u.default_val.right);
                        }
                        if (left->type == AST_IDENTIFIER) {
                            scope_declare_name(interp, left->u.ident.name, prop_val, 0);
                        }
                        lr_free_value(interp->ctx, prop_val);
                    } else if (val_node->type == AST_IDENTIFIER) {
                        const char *target_name = val_node->u.ident.name;
                        if (shorthand) {
                            target_name = key_name;
                        }
                        scope_declare_name(interp, target_name, prop_val, 0);
                        lr_free_value(interp->ctx, prop_val);
                    } else if (val_node->type == AST_ASSIGN &&
                               val_node->u.assign.target &&
                               val_node->u.assign.target->type == AST_IDENTIFIER) {
                        /* Shorthand default from object literal: { a = 1 } */
                        if (lr_is_undefined(prop_val)) {
                            lr_free_value(interp->ctx, prop_val);
                            prop_val = interp_eval_node(interp, val_node->u.assign.value);
                        }
                        scope_declare_name(interp, val_node->u.assign.target->u.ident.name,
                                           prop_val, 0);
                        lr_free_value(interp->ctx, prop_val);
                    } else if (val_node->type == AST_PATTERN || val_node->type == AST_ARRAY ||
                               val_node->type == AST_OBJECT) {
                        eval_pattern(interp, val_node, prop_val);
                        lr_free_value(interp->ctx, prop_val);
                    } else {
                        lr_free_value(interp->ctx, prop_val);
                    }
                }
            }
        }
        return LR_VALUE_UNDEFINED;
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Generator Support (eager evaluation) ──────────────────────────────
 * Generator bodies run eagerly at call time; yields are buffered into a
 * JS array. The returned generator object steps through the buffer via
 * next()/return() and is iterable (Symbol.iterator returns itself).
 * A yield cap guards against unbounded/infinite generators. */

#define GEN_MAX_YIELDS 65536
#define GEN_ITEMS_PROP "__gen_items"
#define GEN_INDEX_PROP "__gen_i"
#define GEN_DONE_PROP  "__gen_done"
#define GEN_RET_PROP   "__gen_ret"

/* Build a { value, done } iterator result (takes ownership of value) */
static LRValue gen_make_result(LRContext *ctx, LRValue value, int done)
{
    LRValue res = lr_new_object(ctx);
    lr_set_property_str(ctx, res, "value", value);
    lr_set_property_str(ctx, res, "done", done ? LR_VALUE_TRUE : LR_VALUE_FALSE);
    return res;
}

static LRValue gen_next_cfunc(LRContext *ctx, LRValue this_val,
                              int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    LRValue done_v = lr_get_property_str(ctx, this_val, GEN_DONE_PROP);
    int done = lr_to_bool(ctx, done_v);
    lr_free_value(ctx, done_v);
    if (done) return gen_make_result(ctx, LR_VALUE_UNDEFINED, 1);

    LRValue items = lr_get_property_str(ctx, this_val, GEN_ITEMS_PROP);
    LRValue iv = lr_get_property_str(ctx, this_val, GEN_INDEX_PROP);
    int32_t i = 0; lr_to_int32(ctx, &i, iv);
    lr_free_value(ctx, iv);
    LRValue lv = lr_get_property_str(ctx, items, "length");
    int32_t len = 0; lr_to_int32(ctx, &len, lv);
    lr_free_value(ctx, lv);

    if (i >= len) {
        lr_free_value(ctx, items);
        lr_set_property_str(ctx, this_val, GEN_DONE_PROP, LR_VALUE_TRUE);
        LRValue ret = lr_get_property_str(ctx, this_val, GEN_RET_PROP);
        return gen_make_result(ctx, ret, 1);
    }
    LRValue item = lr_get_property_uint32(ctx, items, (uint32_t)i);
    lr_free_value(ctx, items);
    lr_set_property_str(ctx, this_val, GEN_INDEX_PROP, lr_new_int32(ctx, i + 1));
    return gen_make_result(ctx, item, 0);
}

static LRValue gen_return_cfunc(LRContext *ctx, LRValue this_val,
                                int argc, LRValue *argv)
{
    lr_set_property_str(ctx, this_val, GEN_DONE_PROP, LR_VALUE_TRUE);
    LRValue v = (argc > 0) ? lr_dup_value(ctx, argv[0]) : LR_VALUE_UNDEFINED;
    return gen_make_result(ctx, v, 1);
}

static LRValue gen_self_cfunc(LRContext *ctx, LRValue this_val,
                              int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    return lr_dup_value(ctx, this_val);
}

/* Build the generator object (takes ownership of items and ret_val) */
static LRValue gen_build_object(Interpreter *interp, LRValue items, LRValue ret_val)
{
    LRContext *ctx = interp->ctx;
    LRValue gen = lr_new_object(ctx);
    lr_set_property_str(ctx, gen, GEN_ITEMS_PROP, items);
    lr_set_property_str(ctx, gen, GEN_INDEX_PROP, lr_new_int32(ctx, 0));
    lr_set_property_str(ctx, gen, GEN_DONE_PROP, LR_VALUE_FALSE);
    lr_set_property_str(ctx, gen, GEN_RET_PROP, ret_val);
    lr_set_property_str(ctx, gen, "next",
        lr_new_cfunction(ctx, gen_next_cfunc, "next", 0));
    lr_set_property_str(ctx, gen, "return",
        lr_new_cfunction(ctx, gen_return_cfunc, "return", 1));
    lr_set_property_str(ctx, gen, "Symbol.iterator",
        lr_new_cfunction(ctx, gen_self_cfunc, "[Symbol.iterator]", 0));
    return gen;
}

/* Append one yielded value to the active generator buffer (dups v) */
static void gen_append(Interpreter *interp, LRValue v)
{
    if (interp->gen_count >= GEN_MAX_YIELDS) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "generator yield limit exceeded (%d); lazy/infinite "
                 "generators are not supported", GEN_MAX_YIELDS);
        interp->exception_value = LR_VALUE_UNDEFINED;
        interp->error_flag = 1;
        return;
    }
    lr_set_property_uint32(interp->ctx, interp->gen_items,
                           (uint32_t)interp->gen_count++,
                           lr_dup_value(interp->ctx, v));
}

/* yield* delegation: append every element of an iterable */
static void gen_delegate(Interpreter *interp, LRValue src)
{
    LRContext *ctx = interp->ctx;
    if (lr_is_string(src)) {
        const char *s = lr_to_cstring(ctx, src);
        size_t slen = s ? strlen(s) : 0;
        char buf[2] = {0, 0};
        for (size_t i = 0; i < slen && !interp->error_flag; i++) {
            buf[0] = s[i];
            LRValue item = lr_new_string(ctx, buf);
            gen_append(interp, item);
            lr_free_value(ctx, item);
        }
        lr_free_cstring(ctx, s);
        return;
    }
    if (lr_is_array(ctx, src)) {
        int32_t len = 0;
        LRValue len_val = lr_get_property_str(ctx, src, "length");
        lr_to_int32(ctx, &len, len_val);
        lr_free_value(ctx, len_val);
        for (int32_t i = 0; i < len && !interp->error_flag; i++) {
            LRValue item = lr_get_property_uint32(ctx, src, (uint32_t)i);
            gen_append(interp, item);
            lr_free_value(ctx, item);
        }
        return;
    }
    if (lr_is_object(src)) {
        LRValue iter_fn = lr_get_property_str(ctx, src, "Symbol.iterator");
        if (lr_is_function(ctx, iter_fn)) {
            LRValue iter = call_value_with_args(interp, NULL, iter_fn, src, 0, NULL);
            lr_free_value(ctx, iter_fn);
            if (interp->error_flag) { lr_free_value(ctx, iter); return; }
            if (lr_is_object(iter)) {
                LRValue next_fn = lr_get_property_str(ctx, iter, "next");
                while (!interp->error_flag) {
                    LRValue nr = call_value_with_args(interp, NULL, next_fn, iter, 0, NULL);
                    if (interp->error_flag) { lr_free_value(ctx, nr); break; }
                    LRValue done = lr_get_property_str(ctx, nr, "done");
                    int is_done = lr_to_bool(ctx, done);
                    lr_free_value(ctx, done);
                    if (is_done) { lr_free_value(ctx, nr); break; }
                    LRValue value = lr_get_property_str(ctx, nr, "value");
                    gen_append(interp, value);
                    lr_free_value(ctx, value);
                    lr_free_value(ctx, nr);
                }
                lr_free_value(ctx, next_fn);
            }
            lr_free_value(ctx, iter);
            return;
        }
        lr_free_value(ctx, iter_fn);
    }
    snprintf(interp->error_message, sizeof(interp->error_message),
             "yield* operand is not iterable");
    interp->exception_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 1;
}

/* ── Function Call Support ─────────────────────────────────────────────── */

static LRValue interp_call_function(Interpreter *interp, ASTNode *func_node,
                                     LRValue this_val, int argc, LRValue *argv)
{
    /* Consume the closure scope handed off by the caller (if any) */
    InterpScope *closure_scope = (InterpScope *)interp->pending_closure;
    interp->pending_closure = NULL;

    if (interp->depth >= MAX_CALL_DEPTH) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "Maximum call stack size exceeded");
        interp->error_flag = 1;
        return LR_VALUE_UNDEFINED;
    }

    interp->depth++;

    /* Determine function name and source location for call stack */
    const char *func_name = NULL;
    const char *filename = NULL;
    int line_number = 0;

    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        func_name = func_node->u.func.name;
        if (func_node->u.func.body) {
            line_number = func_node->u.func.body->token.line;
            (void)line_number; /* line info from token */
        }
    } else if (func_node->type == AST_ARROW) {
        func_name = NULL; /* arrow functions are anonymous */
        if (func_node->u.arrow.body) {
            line_number = func_node->u.arrow.body->token.line;
        }
    }

    /* Push a call frame onto the engine's call stack */
    lr_push_call_frame(interp->ctx, func_name ? func_name : "",
                       filename, func_node->token.line);

    /* Save interpreter state */
    int saved_break = interp->break_target;
    int saved_continue = interp->continue_target;
    int saved_return = interp->return_target;
    int saved_has_returned = interp->has_returned;
    LRValue saved_return_val = interp->return_value;
    int saved_error = interp->error_flag;
    char saved_err_msg[512];
    memcpy(saved_err_msg, interp->error_message, 512);

    interp->break_target = 0;
    interp->continue_target = 0;
    interp->break_label[0] = '\0';
    interp->continue_label[0] = '\0';
    const char *saved_pending_label = interp->pending_label;
    interp->pending_label = NULL;
    interp->return_target = 0;
    interp->has_returned = 0;
    interp->return_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 0;

    /* Create a new function scope. Its parent is the function's captured
     * (lexical) defining scope when available, otherwise the call-time
     * scope (legacy dynamic behavior for direct AST invocations). */
    InterpScope *saved_scope = interp->current_scope;
    InterpScope *func_scope = scope_new(
        closure_scope ? closure_scope : interp->current_scope, 1, 0);
    interp->current_scope = func_scope;

    /* Bind 'this' */
    scope_declare_name(interp, "this", this_val, 1); /* const-like */

    /* Bind super references for class methods (derived classes) */
    if ((func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) &&
        func_node->u.func.class_node &&
        func_node->u.func.class_node->u.class_decl.extends) {
        LRValue parent = interp_eval_node(interp,
            func_node->u.func.class_node->u.class_decl.extends);
        if (interp->error_flag) {
            /* Parent class not resolvable at call time: ignore, super unusable */
            interp->error_flag = 0;
            lr_free_value(interp->ctx, parent);
        } else if (lr_is_object(parent)) {
            scope_declare_name(interp, "%superctor%", parent, 1);
            LRValue sproto = lr_get_property_str(interp->ctx, parent, "prototype");
            scope_declare_name(interp, "%superproto%", sproto, 1);
            lr_free_value(interp->ctx, sproto);
            lr_free_value(interp->ctx, parent);
        } else {
            lr_free_value(interp->ctx, parent);
        }
    }

    /* Bind arguments */
    int nparams = 0;
    ASTNode **params = NULL;
    int is_arrow = 0;
    int is_async = 0;
    int is_generator = 0;

    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        nparams = func_node->u.func.nparams;
        params = func_node->u.func.params;
        is_async = func_node->u.func.is_async;
        is_generator = func_node->u.func.is_generator;
    } else if (func_node->type == AST_ARROW) {
        nparams = func_node->u.arrow.nparams;
        params = func_node->u.arrow.params;
        is_arrow = 1;
        is_async = func_node->u.arrow.is_async;
    }

    /* Generator: buffer yields into a fresh array while the body runs.
     * Nested calls save/restore so each generator gets its own buffer,
     * and yield inside a nested plain function stays an error. */
    int saved_gen_active = interp->gen_active;
    LRValue saved_gen_items = interp->gen_items;
    int saved_gen_count = interp->gen_count;
    interp->gen_active = is_generator;
    interp->gen_items = is_generator ? lr_new_array(interp->ctx) : LR_VALUE_UNDEFINED;
    interp->gen_count = 0;

    /* Bind 'arguments' object (not for arrow functions) */
    if (!is_arrow) {
        LRValue args_obj = lr_new_object(interp->ctx);
        for (int i = 0; i < argc; i++) {
            lr_set_property_uint32(interp->ctx, args_obj, i, lr_dup_value(interp->ctx, argv[i]));
        }
        lr_set_property_str(interp->ctx, args_obj, "length", lr_new_int32(interp->ctx, argc));
        /* Callee property */
        lr_set_property_str(interp->ctx, args_obj, "callee", LR_VALUE_UNDEFINED);
        scope_declare_name(interp, "arguments", args_obj, 0);
        lr_free_value(interp->ctx, args_obj);
    }

    /* Bind parameters */
    for (int i = 0; i < nparams; i++) {
        ASTNode *param = params[i];
        if (param->type == AST_IDENTIFIER) {
            const char *pname = param->u.ident.name;
            if (i < argc) {
                scope_declare_name(interp, pname, argv[i], 0);
            } else {
                scope_declare_name(interp, pname, LR_VALUE_UNDEFINED, 0);
            }
        } else if (param->type == AST_REST || param->type == AST_SPREAD_ELEMENT) {
            /* Rest parameter (AST_SPREAD_ELEMENT when the arrow params were
             * reconstructed from a parenthesized expression) */
            ASTNode *rest_target = param->type == AST_REST ? param->u.rest_elem.arg
                                                           : param->u.spread.arg;
            if (rest_target && rest_target->type == AST_IDENTIFIER) {
                LRValue rest_arr = lr_new_array(interp->ctx);
                int idx = 0;
                for (int j = i; j < argc; j++) {
                    lr_set_property_uint32(interp->ctx, rest_arr, idx++, lr_dup_value(interp->ctx, argv[j]));
                }
                lr_set_property_str(interp->ctx, rest_arr, "length", lr_new_int32(interp->ctx, idx));
                scope_declare_name(interp, rest_target->u.ident.name, rest_arr, 0);
                lr_free_value(interp->ctx, rest_arr);
            }
            break;
        } else if (param->type == AST_DEFAULT_VALUE) {
            ASTNode *left = param->u.default_val.left;
            ASTNode *right = param->u.default_val.right;
            if (left->type == AST_IDENTIFIER) {
                const char *pname = left->u.ident.name;
                if (i < argc && !lr_is_undefined(argv[i])) {
                    scope_declare_name(interp, pname, argv[i], 0);
                } else {
                    LRValue def_val = interp_eval_node(interp, right);
                    scope_declare_name(interp, pname, def_val, 0);
                    lr_free_value(interp->ctx, def_val);
                }
            }
        } else if (param->type == AST_PATTERN || param->type == AST_ARRAY ||
                   param->type == AST_OBJECT) {
            /* Destructuring parameter (array/object literal nodes appear when
             * arrow params were reconstructed from an expression) */
            eval_pattern(interp, param, i < argc ? argv[i] : LR_VALUE_UNDEFINED);
        } else if (param->type == AST_ASSIGN &&
                   param->u.assign.target &&
                   param->u.assign.target->type == AST_IDENTIFIER) {
            /* Default parameter reconstructed from expression: (a = 1) => */
            const char *pname = param->u.assign.target->u.ident.name;
            if (i < argc && !lr_is_undefined(argv[i])) {
                scope_declare_name(interp, pname, argv[i], 0);
            } else {
                LRValue def_val = interp_eval_node(interp, param->u.assign.value);
                scope_declare_name(interp, pname, def_val, 0);
                lr_free_value(interp->ctx, def_val);
            }
        }
    }

    /* Evaluate the body */
    LRValue result = LR_VALUE_UNDEFINED;
    ASTNode *body = NULL;

    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        body = func_node->u.func.body;
    } else if (func_node->type == AST_ARROW) {
        body = func_node->u.arrow.body;
    }

    if (body) {
        if (body->type == AST_BLOCK) {
            /* Function body is a block */
            result = interp_eval_node(interp, body);
            /* Don't free result - it's the return value */
        } else {
            /* Arrow function with expression body: () => expr */
            result = interp_eval_node(interp, body);
            /* Don't free - it's the return value */
        }

        /* Check for return statement */
        if (interp->has_returned) {
            if (result.tag != LR_TYPE_UNDEFINED) {
                /* Free the block result if we already have a return value */
                /* Actually, the return value is already set in interp->return_value */
                /* But we need to handle the case where the body evaluation returned something */
            }
            /* The return value is in interp->return_value */
            /* But we need to be careful about which value to use */
        }
    }

    /* Get the actual return value */
    if (interp->has_returned) {
        result = lr_dup_value(interp->ctx, interp->return_value);
    }

    /* Generator: wrap the buffered yields into a generator object */
    if (is_generator) {
        LRValue items = interp->gen_items;
        lr_set_property_str(interp->ctx, items, "length",
                            lr_new_int32(interp->ctx, interp->gen_count));
        if (interp->error_flag) {
            lr_free_value(interp->ctx, items);
            lr_free_value(interp->ctx, result);
            result = LR_VALUE_UNDEFINED;
        } else {
            /* result (the generator's return value) becomes the final
             * done:true value; ownership transfers to the object */
            result = gen_build_object(interp, items, result);
        }
    }
    interp->gen_active = saved_gen_active;
    interp->gen_items = saved_gen_items;
    interp->gen_count = saved_gen_count;

    /* Async: wrap the outcome in a Promise. Throws become rejections. */
    if (is_async && !is_generator) {
        LRValue p = lr_new_promise(interp->ctx);
        /* Give the bare promise its prototype so .then/.catch work */
        {
            LRValue g = lr_get_global_object(interp->ctx);
            LRValue ctor = lr_get_property_str(interp->ctx, g, "Promise");
            if (lr_is_object(ctor)) {
                LRValue proto = lr_get_property_str(interp->ctx, ctor, "prototype");
                if (lr_is_object(proto)) lr_set_prototype(interp->ctx, p, proto);
                lr_free_value(interp->ctx, proto);
            }
            lr_free_value(interp->ctx, ctor);
            lr_free_value(interp->ctx, g);
        }
        if (interp->error_flag) {
            LRValue reason;
            if (interp->exception_pending &&
                interp->exception_value.tag != LR_TYPE_EXCEPTION &&
                interp->exception_value.tag != LR_TYPE_UNDEFINED) {
                reason = lr_dup_value(interp->ctx, interp->exception_value);
            } else {
                reason = lr_new_string(interp->ctx, interp->error_message);
            }
            lr_promise_reject_internal(interp->ctx, p, reason);
            lr_free_value(interp->ctx, reason);
            /* The async function absorbs the throw into the rejection */
            interp->error_flag = 0;
            if (interp->exception_pending) {
                lr_free_value(interp->ctx, interp->exception_value);
                interp->exception_value = LR_VALUE_UNDEFINED;
                interp->exception_pending = 0;
            }
            interp->error_message[0] = '\0';
        } else {
            lr_promise_resolve_internal(interp->ctx, p, result);
        }
        lr_free_value(interp->ctx, result);
        result = p;
    }

    /* Pop any scopes left by early exits, then the function scope itself,
     * and restore the caller's scope (may differ from func_scope->parent
     * when a closure scope was used) */
    while (interp->current_scope && interp->current_scope != func_scope)
        interp_pop_scope(interp);
    interp->current_scope = saved_scope;
    scope_release(func_scope, interp->ctx);

    /* Restore interpreter state */
    interp->depth--;
    interp->break_target = saved_break;
    interp->continue_target = saved_continue;
    interp->pending_label = saved_pending_label;
    interp->return_target = saved_return;
    interp->has_returned = saved_has_returned;
    /* Don't restore return_value if we're inside a return (it propagates) */
    if (!saved_has_returned) {
        lr_free_value(interp->ctx, interp->return_value);
        interp->return_value = LR_VALUE_UNDEFINED;
    }
    interp->return_value = saved_return_val;
    /* Errors raised inside the function propagate to the caller;
     * otherwise restore the caller's error state */
    if (!interp->error_flag) {
        interp->error_flag = saved_error;
        if (!saved_error) {
            memcpy(interp->error_message, saved_err_msg, 512);
        }
    }

    /* Pop call frame */
    lr_pop_call_frame(interp->ctx);

    return result;
}

/* Call a class as a constructor body:
 * 1. implicit super(...) when derived and no explicit constructor
 * 2. instance field initialization
 * 3. explicit constructor body (if any) */
static LRValue interp_call_class_function(Interpreter *interp, ASTNode *class_node,
                                          LRValue this_val, int argc, LRValue *argv)
{
    /* Take the class's closure scope; re-injected for the ctor body below */
    InterpScope *cls_closure = (InterpScope *)interp->pending_closure;
    interp->pending_closure = NULL;

    LRContext *ctx = interp->ctx;
    int nmethods = class_node->u.class_decl.nmethods;
    ASTNode **methods = class_node->u.class_decl.methods;

    /* Find explicit constructor */
    ASTNode *ctor_ast = NULL;
    int has_fields = 0;
    for (int i = 0; i < nmethods; i++) {
        ASTNode *m = methods[i];
        if (!m) continue;
        if (m->type == AST_FUNC_EXPR && !m->u.func.is_static &&
            !m->u.func.is_getter && !m->u.func.is_setter &&
            m->u.func.name && strcmp(m->u.func.name, "constructor") == 0) {
            ctor_ast = m;
        } else if (m->type == AST_PROPERTY && !m->u.property.is_static) {
            has_fields = 1;
        }
    }

    /* Implicit super(...args) for derived classes without explicit ctor */
    if (!ctor_ast && class_node->u.class_decl.extends) {
        LRValue parent = interp_eval_node(interp, class_node->u.class_decl.extends);
        if (interp->error_flag) {
            lr_free_value(ctx, parent);
            return LR_VALUE_UNDEFINED;
        }
        if (lr_is_object(parent)) {
            LRObject *po = (LRObject *)parent.u.ptr;
            LRValue r = LR_VALUE_UNDEFINED;
            if (po->type == LR_OBJ_FUNCTION && po->extra) {
                interp->pending_closure = po->def_scope;
                r = interp_invoke_function_ast(interp, (ASTNode *)po->extra,
                                               this_val, argc, argv);
            } else if (po->type == LR_OBJ_CFUNCTION) {
                r = lr_call(ctx, parent, this_val, argc, argv);
            }
            lr_free_value(ctx, r);
        }
        lr_free_value(ctx, parent);
        if (interp->error_flag) return LR_VALUE_UNDEFINED;
    }

    /* Initialize instance fields (evaluated with 'this' bound) */
    if (has_fields) {
        interp_push_scope(interp, 1);
        scope_declare_name(interp, "this", this_val, 1);
        for (int i = 0; i < nmethods; i++) {
            ASTNode *m = methods[i];
            if (!m || m->type != AST_PROPERTY || m->u.property.is_static) continue;
            LRValue v = m->u.property.val
                ? interp_eval_node(interp, m->u.property.val)
                : LR_VALUE_UNDEFINED;
            if (interp->error_flag) {
                lr_free_value(ctx, v);
                break;
            }
            if (m->u.property.key && m->u.property.key->type == AST_IDENTIFIER) {
                lr_set_property_str(ctx, this_val,
                                    m->u.property.key->u.ident.name, v);
            } else if (m->u.property.key) {
                LRValue kv = interp_eval_node(interp, m->u.property.key);
                LRString *atom = lr_to_atom(ctx, kv);
                lr_set_property(ctx, this_val, atom, v);
                lr_free_value(ctx, kv);
            } else {
                lr_free_value(ctx, v);
            }
        }
        interp_pop_scope(interp);
        if (interp->error_flag) return LR_VALUE_UNDEFINED;
    }

    /* Run the explicit constructor body (with the class's lexical scope) */
    if (ctor_ast) {
        interp->pending_closure = cls_closure;
        return interp_call_function(interp, ctor_ast, this_val, argc, argv);
    }
    return LR_VALUE_UNDEFINED;
}

/* Dispatch any callable AST node: function/arrow or class */
static LRValue interp_invoke_function_ast(Interpreter *interp, ASTNode *ast,
                                          LRValue this_val, int argc, LRValue *argv)
{
    if (!ast) { interp->pending_closure = NULL; return LR_VALUE_UNDEFINED; }
    if (ast->type == AST_CLASS_DECL) {
        return interp_call_class_function(interp, ast, this_val, argc, argv);
    }
    if (ast->type == AST_FUNC_EXPR || ast->type == AST_FUNC_DECL ||
        ast->type == AST_ARROW) {
        return interp_call_function(interp, ast, this_val, argc, argv);
    }
    interp->pending_closure = NULL;
    return LR_VALUE_UNDEFINED;
}

/* ── Statement Evaluators ──────────────────────────────────────────────── */

static LRValue eval_block(Interpreter *interp, ASTNode *node)
{
    /* Blocks create a new scope for let/const */
    interp_push_scope(interp, 0);

    LRValue result = LR_VALUE_UNDEFINED;
    int count = node->u.list.count;
    ASTNode **items = node->u.list.items;

    for (int i = 0; i < count; i++) {
        if (interp->break_target || interp->continue_target ||
            interp->has_returned || interp->error_flag) {
            break;
        }
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, items[i]);
        if (interp->error_flag) {
            break;
        }
    }

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_if(Interpreter *interp, ASTNode *node)
{
    LRValue cond = interp_eval_node(interp, node->u.if_stmt.cond);
    if (interp->error_flag) return cond;

    int truthy = lr_to_bool(interp->ctx, cond);
    lr_free_value(interp->ctx, cond);

    if (truthy) {
        return interp_eval_node(interp, node->u.if_stmt.body);
    } else if (node->u.if_stmt.else_body) {
        return interp_eval_node(interp, node->u.if_stmt.else_body);
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Labeled break/continue helpers ────────────────────────────────────
 * A pending break/continue with an empty label targets the innermost
 * loop; with a label it targets the loop tagged with that label (via
 * interp->pending_label, set by the AST_LABEL handler). Non-matching
 * flags propagate outward through enclosing loops. */

static int break_is_mine(Interpreter *interp, const char *label)
{
    if (!interp->break_target) return 0;
    if (interp->break_label[0] == '\0') return 1;
    return label && strcmp(interp->break_label, label) == 0;
}

static int continue_is_mine(Interpreter *interp, const char *label)
{
    if (!interp->continue_target) return 0;
    if (interp->continue_label[0] == '\0') return 1;
    return label && strcmp(interp->continue_label, label) == 0;
}

static void consume_break(Interpreter *interp)
{
    interp->break_target = 0;
    interp->break_label[0] = '\0';
}

static void consume_continue(Interpreter *interp)
{
    interp->continue_target = 0;
    interp->continue_label[0] = '\0';
}

static LRValue eval_for(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    /* Create a scope for the loop variable */
    interp_push_scope(interp, 0);

    /* Evaluate init */
    if (node->u.for_stmt.init) {
        LRValue init_val = interp_eval_node(interp, node->u.for_stmt.init);
        lr_free_value(interp->ctx, init_val);
        if (interp->error_flag) { interp_pop_scope(interp); return LR_VALUE_UNDEFINED; }
    }

    LRValue result = LR_VALUE_UNDEFINED;

    while (!interp->break_target && !interp->has_returned && !interp->error_flag) {
        /* Test */
        if (node->u.for_stmt.test) {
            LRValue test = interp_eval_node(interp, node->u.for_stmt.test);
            int truthy = lr_to_bool(interp->ctx, test);
            lr_free_value(interp->ctx, test);
            if (!truthy) break;
        }

        /* Body */
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.for_stmt.body);
        if (interp->break_target) {
            if (break_is_mine(interp, my_label)) consume_break(interp);
            break;   /* consumed here, or propagates to an outer loop */
        }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;   /* labeled continue for an outer loop */
        }

        /* Update */
        if (node->u.for_stmt.update) {
            LRValue update = interp_eval_node(interp, node->u.for_stmt.update);
            lr_free_value(interp->ctx, update);
            if (interp->error_flag) break;
        }
    }

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_for_in(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    /* Evaluate the source collection */
    LRValue source = interp_eval_node(interp, node->u.for_in.source);
    if (interp->error_flag) return source;

    /* Get enumerable property names */
    LRPropertyEnum *props = NULL;
    uint32_t nprops = 0;
    lr_get_own_property_names(interp->ctx, &props, &nprops, source, 0);

    /* Create a scope for the loop variable */
    interp_push_scope(interp, 0);

    LRValue result = LR_VALUE_UNDEFINED;

    for (uint32_t i = 0; i < nprops; i++) {
        if (interp->break_target || interp->has_returned || interp->error_flag) break;

        const char *prop_name = lr_atom_to_cstring(interp->ctx, props[i].atom);

        /* Assign property name to the loop variable */
        LRValue prop_val = lr_new_string(interp->ctx, prop_name);
        lr_free_cstring(interp->ctx, prop_name);

        ASTNode *each = node->u.for_in.each;
        if (each) {
            if (each->type == AST_VAR_DECL) {
                /* var x in obj */
                if (each->u.var_decl.nvars > 0) {
                    ASTNode *declarator = each->u.var_decl.vars[0];
                    if (declarator && declarator->type == AST_VAR_DECLARATOR) {
                        ASTNode *var = declarator->u.declarator.var;
                        if (var && var->type == AST_IDENTIFIER) {
                            scope_declare_name(interp, var->u.ident.name, prop_val, 0);
                        }
                    }
                }
            } else if (each->type == AST_IDENTIFIER) {
                scope_set_name(interp, each->u.ident.name, prop_val);
            } else if (each->type == AST_ASSIGN) {
                /* Handle for (x in obj) - x is already assigned */
                /* This is for the case where for_in.each is the expression statement */
            }
        }

        lr_free_value(interp->ctx, prop_val);

        /* Execute body */
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.for_in.body);
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;   /* labeled continue for an outer loop */
        }
    }

    if (break_is_mine(interp, my_label)) consume_break(interp);

    lr_free_property_enum(interp->ctx, props, nprops);
    lr_free_value(interp->ctx, source);

    interp_pop_scope(interp);
    return result;
}

static void for_of_assign_var(Interpreter *interp, ASTNode *each, LRValue item)
{
    if (!each) return;
    if (each->type == AST_VAR_DECL) {
        if (each->u.var_decl.nvars > 0) {
            ASTNode *decl = each->u.var_decl.vars[0];
            if (decl && decl->type == AST_VAR_DECLARATOR) {
                ASTNode *var = decl->u.declarator.var;
                if (var && var->type == AST_IDENTIFIER)
                    scope_declare_name(interp, var->u.ident.name, item, 0);
                else if (var && (var->type == AST_PATTERN || var->type == AST_ARRAY ||
                                 var->type == AST_OBJECT))
                    eval_pattern(interp, var, item);
            }
        }
    } else if (each->type == AST_IDENTIFIER) {
        scope_set_name(interp, each->u.ident.name, item);
    } else if (each->type == AST_PATTERN || each->type == AST_ARRAY ||
               each->type == AST_OBJECT) {
        eval_pattern(interp, each, item);
    } else if (each->type == AST_ASSIGN) {
        ASTNode *target = each->u.assign.target;
        if (target->type == AST_IDENTIFIER) {
            scope_set_name(interp, target->u.ident.name, item);
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, item));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                LRValue prop = interp_eval_node(interp, target->u.member.prop);
                if (!interp->error_flag) {
                    LRString *atom = lr_to_atom(interp->ctx, prop);
                    lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, item));
                }
                lr_free_value(interp->ctx, prop);
            }
            lr_free_value(interp->ctx, obj);
        } else {
            eval_pattern(interp, target, item);
        }
    }
}

static LRValue eval_for_of(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    /* Evaluate the source iterable */
    LRValue source = interp_eval_node(interp, node->u.for_of.source);
    if (interp->error_flag) return source;

    /* Create a scope for the loop variable */
    interp_push_scope(interp, 0);

    LRValue result = LR_VALUE_UNDEFINED;
    ASTNode *each = node->u.for_of.each;

    if (lr_is_string(source)) {
        const char *s = lr_to_cstring(interp->ctx, source);
        size_t slen = s ? strlen(s) : 0;
        char buf[8];
        for (size_t i = 0; i < slen; i++) {
            if (interp->break_target || interp->has_returned || interp->error_flag) break;
            /* UTF-8 best-effort: emit one byte as a string (ASCII-safe). */
            buf[0] = s[i]; buf[1] = '\0';
            LRValue item = lr_new_string(interp->ctx, buf);
            for_of_assign_var(interp, each, item);
            lr_free_value(interp->ctx, item);

            if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
            result = interp_eval_node(interp, node->u.for_of.body);
            if (interp->continue_target) {
                if (continue_is_mine(interp, my_label)) consume_continue(interp);
                else break;
            }
        }
        lr_free_cstring(interp->ctx, s);
    } else if (lr_is_array(interp->ctx, source)) {
        int32_t len = 0;
        LRValue len_val = lr_get_property_str(interp->ctx, source, "length");
        lr_to_int32(interp->ctx, &len, len_val);
        lr_free_value(interp->ctx, len_val);

        for (int32_t i = 0; i < len; i++) {
            if (interp->break_target || interp->has_returned || interp->error_flag) break;

            LRValue item = lr_get_property_uint32(interp->ctx, source, i);
            for_of_assign_var(interp, each, item);
            lr_free_value(interp->ctx, item);

            if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
            result = interp_eval_node(interp, node->u.for_of.body);
            if (interp->continue_target) {
                if (continue_is_mine(interp, my_label)) consume_continue(interp);
                else break;
            }
        }
    } else if (lr_is_object(source)) {
        /* Iterable protocol: const iter = source[Symbol.iterator](); then iter.next() */
        LRValue iter_fn = lr_get_property_str(interp->ctx, source, "Symbol.iterator");
        if (lr_is_function(interp->ctx, iter_fn) && iter_fn.tag == LR_TYPE_OBJECT) {
            LRValue iter_argv[1] = { source };
            LRValue iter = call_value_with_args(interp, NULL, iter_fn, source, 1, iter_argv);
            lr_free_value(interp->ctx, iter_fn);
            if (interp->error_flag) {
                lr_free_value(interp->ctx, iter);
                lr_free_value(interp->ctx, source);
                interp_pop_scope(interp);
                return result;
            }
            if (lr_is_object(iter)) {
                LRValue next_fn = lr_get_property_str(interp->ctx, iter, "next");
                while (!interp->break_target && !interp->has_returned && !interp->error_flag) {
                    LRValue nr = call_value_with_args(interp, NULL, next_fn, iter, 0, NULL);
                    if (interp->error_flag) { lr_free_value(interp->ctx, nr); break; }
                    LRValue done = lr_get_property_str(interp->ctx, nr, "done");
                    int is_done = lr_to_bool(interp->ctx, done);
                    lr_free_value(interp->ctx, done);
                    if (is_done) { lr_free_value(interp->ctx, nr); break; }
                    LRValue value = lr_get_property_str(interp->ctx, nr, "value");
                    for_of_assign_var(interp, each, value);
                    lr_free_value(interp->ctx, value);
                    lr_free_value(interp->ctx, nr);

                    if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
                    result = interp_eval_node(interp, node->u.for_of.body);
                    if (interp->continue_target) {
                        if (continue_is_mine(interp, my_label)) consume_continue(interp);
                        else break;
                    }
                }
                lr_free_value(interp->ctx, next_fn);
            }
            lr_free_value(interp->ctx, iter);
        } else {
            lr_free_value(interp->ctx, iter_fn);
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "%s is not iterable", "value");
            interp->exception_value = LR_VALUE_UNDEFINED;
            interp->error_flag = 1;
        }
    } else {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s is not iterable", "value");
        interp->exception_value = LR_VALUE_UNDEFINED;
        interp->error_flag = 1;
    }

    if (break_is_mine(interp, my_label)) consume_break(interp);

    lr_free_value(interp->ctx, source);

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_while(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    LRValue result = LR_VALUE_UNDEFINED;

    while (!interp->break_target && !interp->has_returned && !interp->error_flag) {
        LRValue cond = interp_eval_node(interp, node->u.if_stmt.cond);
        int truthy = lr_to_bool(interp->ctx, cond);
        lr_free_value(interp->ctx, cond);

        if (!truthy) break;

        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.if_stmt.body);
        if (interp->break_target) {
            if (break_is_mine(interp, my_label)) consume_break(interp);
            break;
        }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;
        }
    }

    return result;
}

static LRValue eval_do_while(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    LRValue result = LR_VALUE_UNDEFINED;

    do {
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.if_stmt.body);
        if (interp->break_target) {
            if (break_is_mine(interp, my_label)) consume_break(interp);
            break;
        }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;
        }

        LRValue cond = interp_eval_node(interp, node->u.if_stmt.cond);
        int truthy = lr_to_bool(interp->ctx, cond);
        lr_free_value(interp->ctx, cond);
        if (!truthy) break;
    } while (!interp->break_target && !interp->has_returned && !interp->error_flag);

    return result;
}

static LRValue eval_switch(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    LRValue test = interp_eval_node(interp, node->u.switch_stmt.test);
    if (interp->error_flag) return test;

    int ncases = node->u.switch_stmt.ncases;
    ASTNode **cases = node->u.switch_stmt.cases;
    int matched = 0;
    LRValue result = LR_VALUE_UNDEFINED;

    for (int i = 0; i < ncases; i++) {
        ASTNode *case_node = cases[i];
        if (case_node->type == AST_DEFAULT) {
            matched = 1;
        } else if (case_node->type == AST_CASE) {
            if (!matched) {
                LRValue case_test = interp_eval_node(interp, case_node->u.if_stmt.cond);
                if (interp->error_flag) { lr_free_value(interp->ctx, test); return case_test; }
                matched = strict_eq(test, case_test);
                lr_free_value(interp->ctx, case_test);
            }
        }

        if (matched) {
            /* Execute case body */
            ASTNode *body = case_node->u.if_stmt.body;
            if (body && body->type == AST_BLOCK) {
                for (int j = 0; j < body->u.list.count; j++) {
                    if (interp->break_target || interp->has_returned || interp->error_flag) break;
                    if (result.tag != LR_TYPE_UNDEFINED) {
                        lr_free_value(interp->ctx, result);
                    }
                    result = interp_eval_node(interp, body->u.list.items[j]);
                }
            }
            if (interp->break_target) {
                if (break_is_mine(interp, my_label)) consume_break(interp);
                break;
            }
            if (interp->has_returned || interp->error_flag) break;
        }
    }

    lr_free_value(interp->ctx, test);
    return result;
}

static LRValue eval_break(Interpreter *interp, ASTNode *node)
{
    interp->break_label[0] = '\0';
    if (node && node->u.break_stmt.label &&
        node->u.break_stmt.label->type == AST_IDENTIFIER &&
        node->u.break_stmt.label->u.ident.name &&
        node->u.break_stmt.label->u.ident.name[0]) {
        snprintf(interp->break_label, sizeof(interp->break_label), "%s",
                 node->u.break_stmt.label->u.ident.name);
    }
    interp->break_target = 1;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_continue(Interpreter *interp, ASTNode *node)
{
    interp->continue_label[0] = '\0';
    if (node && node->u.continue_stmt.label &&
        node->u.continue_stmt.label->type == AST_IDENTIFIER &&
        node->u.continue_stmt.label->u.ident.name &&
        node->u.continue_stmt.label->u.ident.name[0]) {
        snprintf(interp->continue_label, sizeof(interp->continue_label), "%s",
                 node->u.continue_stmt.label->u.ident.name);
    }
    interp->continue_target = 1;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_return(Interpreter *interp, ASTNode *node)
{
    interp->has_returned = 1;
    if (node->u.return_stmt.arg) {
        interp->return_value = interp_eval_node(interp, node->u.return_stmt.arg);
    } else {
        interp->return_value = LR_VALUE_UNDEFINED;
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_throw(Interpreter *interp, ASTNode *node)
{
    LRValue arg = interp_eval_node(interp, node->u.throw_stmt.arg);
    if (interp->error_flag) return arg;

    /* Set exception state */
    interp->exception_pending = 1;
    interp->exception_value = lr_dup_value(interp->ctx, arg);
    const char *str = lr_to_cstring(interp->ctx, arg);
    snprintf(interp->error_message, sizeof(interp->error_message), "%s", str);
    lr_free_cstring(interp->ctx, str);
    interp->error_flag = 1;
    lr_free_value(interp->ctx, arg);
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_try(Interpreter *interp, ASTNode *node)
{
    LRValue result = LR_VALUE_UNDEFINED;

    /* Save exception state */
    int had_exception = interp->exception_pending;
    LRValue saved_exception = interp->exception_value;
    char saved_err_msg[512];
    memcpy(saved_err_msg, interp->error_message, 512);

    /* Clear exception state for the try block */
    interp->exception_pending = 0;
    interp->error_flag = 0;

    /* Execute try body */
    result = interp_eval_node(interp, node->u.try_stmt.body);

    int caught = 0;
    LRValue catch_result = LR_VALUE_UNDEFINED;

    /* Handle catch */
    if (interp->error_flag && node->u.try_stmt.catch_body) {
        caught = 1;
        interp->error_flag = 0;

        /* Push scope for catch variable */
        interp_push_scope(interp, 0);
        if (node->u.try_stmt.catch_var) {
            LRContext *ctx = interp->ctx;
            LRValue global = lr_get_global_object(ctx);
            LRValue exc_val = LR_VALUE_UNDEFINED;

            if (interp->exception_pending) {
                /* Engine exception (tag == LR_TYPE_EXCEPTION) or user-thrown value */
                if (JS_IsException(interp->exception_value)) {
                    /* Engine marker — create a proper Error object with the stored message */
                    lr_free_value(interp->ctx, interp->exception_value);
                    interp->exception_value = LR_VALUE_UNDEFINED;
                    /* Create an object with Error.prototype as its prototype */
                    JSValue err_obj = JS_NewObject(ctx);
                    JSValue err_ctor = JS_GetPropertyStr(ctx, global, "Error");
                    JSValue err_proto = JS_GetPropertyStr(ctx, err_ctor, "prototype");
                    if (JS_IsObject(err_proto)) {
                        LRObject *obj_ptr = (LRObject *)err_obj.u.ptr;
                        lr_free_value(ctx, obj_ptr->proto);
                        obj_ptr->proto = lr_dup_value(ctx, err_proto);
                    }
                    JS_FreeValue(ctx, err_proto);
                    JS_FreeValue(ctx, err_ctor);
                    JSValue msg = JS_NewString(ctx, interp->error_message);
                    exc_val = lr_error_constructor(ctx, err_obj, 1, &msg);
                    JS_FreeValue(ctx, msg);
                } else {
                    /* User-thrown value — use directly */
                    exc_val = lr_dup_value(interp->ctx, interp->exception_value);
                }
            } else if (interp->error_message[0]) {
                /* error_flag set but no pending exception — create an Error from the error message */
                JSValue err_obj = JS_NewObject(ctx);
                JSValue err_ctor = JS_GetPropertyStr(ctx, global, "Error");
                JSValue err_proto = JS_GetPropertyStr(ctx, err_ctor, "prototype");
                if (JS_IsObject(err_proto)) {
                    LRObject *obj_ptr = (LRObject *)err_obj.u.ptr;
                    lr_free_value(ctx, obj_ptr->proto);
                    obj_ptr->proto = lr_dup_value(ctx, err_proto);
                }
                JS_FreeValue(ctx, err_proto);
                JS_FreeValue(ctx, err_ctor);
                JSValue msg = JS_NewString(ctx, interp->error_message);
                exc_val = lr_error_constructor(ctx, err_obj, 1, &msg);
                JS_FreeValue(ctx, msg);
            }

            lr_free_value(ctx, global);
            scope_declare_name(interp, node->u.try_stmt.catch_var, exc_val, 1);
            lr_free_value(interp->ctx, exc_val);
        }

        interp->exception_pending = 0;
        catch_result = interp_eval_node(interp, node->u.try_stmt.catch_body);
        interp_pop_scope(interp);

        if (interp->error_flag) {
            /* Exception in catch block - propagate */
            lr_free_value(interp->ctx, result);
            result = catch_result;
            goto finally_check;
        }
    }

    /* If we caught, use catch result instead of try result */
    if (caught) {
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = catch_result;
    }

finally_check:
    /* Execute finally block (always) */
    if (node->u.try_stmt.finally_body) {
        LRValue finally_result = interp_eval_node(interp, node->u.try_stmt.finally_body);
        if (interp->error_flag) {
            /* Exception in finally - propagate it */
            if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
            result = finally_result;
        } else {
            if (!caught) {
                /* If try succeeded and no catch, restore original result */
                /* But if we had a pending exception that wasn't caught, restore it */
                if (had_exception && !node->u.try_stmt.catch_body) {
                    interp->exception_pending = 1;
                    interp->exception_value = lr_dup_value(interp->ctx, saved_exception);
                    interp->error_flag = 1;
                }
            }
            lr_free_value(interp->ctx, finally_result);
        }
    }

    /* If we didn't catch and there's an error, propagate it */
    if (!caught && interp->error_flag) {
        return result;
    }

    /* Clear exception if we caught it */
    if (caught && node->u.try_stmt.catch_body) {
        interp->exception_pending = 0;
        interp->error_flag = 0;
        if (interp->exception_value.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, interp->exception_value);
            interp->exception_value = LR_VALUE_UNDEFINED;
        }
    }

    return result;
}

static LRValue eval_var_decl(Interpreter *interp, ASTNode *node)
{
    /* Determine declaration kind from token type */
    int kind = 0; /* 0=var, 1=let, 2=const */
    switch (node->token.type) {
    case TOK_LET:   kind = 1; break;
    case TOK_CONST: kind = 2; break;
    default:        kind = 0; break;
    }

    int nvars = node->u.var_decl.nvars;
    ASTNode **vars = node->u.var_decl.vars;

    for (int i = 0; i < nvars; i++) {
        ASTNode *declarator = vars[i];
        if (declarator->type == AST_VAR_DECLARATOR) {
            ASTNode *var_node = declarator->u.declarator.var;
            ASTNode *init_node = declarator->u.declarator.init;

            if (var_node->type == AST_IDENTIFIER) {
                const char *name = var_node->u.ident.name;
                if (init_node) {
                    LRValue val = interp_eval_node(interp, init_node);
                    if (interp->error_flag) return val;
                    scope_declare_name(interp, name, val, kind);
                    lr_free_value(interp->ctx, val);
                } else {
                    scope_declare_name(interp, name, LR_VALUE_UNDEFINED, kind);
                }
            } else if (var_node->type == AST_PATTERN) {
                /* Destructuring declaration */
                if (init_node) {
                    LRValue val = interp_eval_node(interp, init_node);
                    if (interp->error_flag) return val;
                    eval_pattern(interp, var_node, val);
                    lr_free_value(interp->ctx, val);
                }
            }
        }
    }

    return LR_VALUE_UNDEFINED;
}

static LRValue eval_func_decl(Interpreter *interp, ASTNode *node)
{
    const char *name = node->u.func.name;
    if (name) {
        /* Create function object and declare it */
        LRValue func_obj = eval_func_expr(interp, node);
        /* Function declarations are hoisted - use var-like behavior */
        scope_declare_name(interp, name, func_obj, 0);
        lr_free_value(interp->ctx, func_obj);
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_class_decl(Interpreter *interp, ASTNode *node)
{
    LRValue class_obj = eval_class_expr(interp, node);
    const char *name = node->u.class_decl.name;
    if (name) {
        scope_declare_name(interp, name, class_obj, 1); /* let-like */
    }
    /* Return the class object so class *expressions*
     * (var X = class {...}) evaluate to the constructor. */
    return class_obj;
}

/* Collect bound identifier names from a binding pattern node (used by
 * `export var/let/const <pattern>` to learn which names to export).
 * Appends borrowed char* pointers (owned by the AST, not copied) into the
 * supplied growable array. */
static void collect_export_names(ASTNode *binding, char ***pnames, int *pn, int *pcap)
{
    if (!binding) return;
    switch (binding->type) {
    case AST_IDENTIFIER:
        if (binding->u.ident.name) {
            if (*pn >= *pcap) {
                *pcap = *pcap ? *pcap * 2 : 4;
                *pnames = (char **)realloc(*pnames, *pcap * sizeof(char *));
            }
            (*pnames)[(*pn)++] = binding->u.ident.name;
        }
        break;
    case AST_REST:
        collect_export_names(binding->u.rest_elem.arg, pnames, pn, pcap);
        break;
    case AST_DEFAULT_VALUE:
        collect_export_names(binding->u.default_val.left, pnames, pn, pcap);
        break;
    case AST_PATTERN:
        if (binding->u.pattern_array.is_object) {
            for (int i = 0; i < binding->u.pattern_object.nprops; i++) {
                ASTNode *p = binding->u.pattern_object.props[i];
                if (!p) continue;
                ASTNode *target = (p->type == AST_PROPERTY && p->u.property.val)
                                    ? p->u.property.val : p;
                collect_export_names(target, pnames, pn, pcap);
            }
        } else {
            for (int i = 0; i < binding->u.pattern_array.nelem; i++)
                collect_export_names(binding->u.pattern_array.elements[i], pnames, pn, pcap);
        }
        break;
    case AST_PROPERTY:
        collect_export_names(binding->u.property.val ? binding->u.property.val
                                                       : binding->u.property.key,
                             pnames, pn, pcap);
        break;
    default:
        break;
    }
}

/* Resolve a module specifier to its namespace object via the runtime's module
 * loader. Returns an LRValue wrapping the namespace (borrowed reference) and,
 * on success, sets *out_mod to the module definition. On failure it records an
 * exception on the interpreter and returns an undefined value with *out_mod
 * left NULL. */
static LRValue eval_resolve_module(Interpreter *interp, const char *spec,
                                   JSModuleDef **out_mod)
{
    *out_mod = NULL;
    if (!spec) return LR_VALUE_UNDEFINED;

    char *normalized = NULL;
    if (interp->ctx->rt->module_normalize_func)
        normalized = interp->ctx->rt->module_normalize_func(interp->ctx, NULL, spec, NULL);
    const char *load_name = normalized ? normalized : spec;

    JSModuleDef *mod = NULL;
    if (interp->ctx->rt->module_loader_func)
        mod = interp->ctx->rt->module_loader_func(interp->ctx, load_name, NULL);
    if (normalized) free(normalized);

    if (!mod || !mod->obj) {
        LRValue err = JS_ThrowReferenceError(interp->ctx, "Cannot load module '%s'", spec);
        interp->error_flag = 1;
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(interp->ctx, err);
        lr_free_value(interp->ctx, err);
        return LR_VALUE_UNDEFINED;
    }

    *out_mod = mod;
    LRValue ns;
    ns.tag = LR_TYPE_OBJECT;
    ns.u.ptr = mod->obj;
    return ns;
}

static LRValue eval_import(Interpreter *interp, ASTNode *node)
{
    ASTNode *src_node = node->u.import_decl.source;
    if (!src_node || src_node->type != AST_LITERAL) {
        /* import with no source, or a bare specifier */
        return LR_VALUE_UNDEFINED;
    }
    const char *spec = src_node->u.string.str;

    JSModuleDef *mod = NULL;
    LRValue ns = eval_resolve_module(interp, spec, &mod);
    if (!mod) return LR_VALUE_UNDEFINED;

    int nspec = node->u.import_decl.nspec;
    ASTNode **specifiers = node->u.import_decl.specifiers;
    for (int i = 0; i < nspec; i++) {
        ASTNode *spec_node = specifiers[i];
        if (!spec_node) continue;
        if (spec_node->type == AST_IMPORT_SPECIFIER) {
            const char *local_name =
                (spec_node->u.import_spec.local
                 && spec_node->u.import_spec.local->type == AST_IDENTIFIER)
                    ? spec_node->u.import_spec.local->u.ident.name : NULL;
            const char *export_name = spec_node->u.import_spec.is_default
                ? "default" : spec_node->u.import_spec.name;
            if (!local_name) local_name = export_name;
            if (!export_name) continue;
            /* Read the binding from the module namespace and bind it locally.
             * lr_get_property_str returns a caller-owned value; scope_declare_name
             * takes its own copy, so we free our copy afterwards. */
            LRValue val = lr_get_property_str(interp->ctx, ns, export_name);
            scope_declare_name(interp, local_name, val, 1);
            lr_free_value(interp->ctx, val);
        } else if (spec_node->type == AST_IMPORT_NAMESPACE) {
            ASTNode *local = spec_node->u.import_namespace.local;
            const char *local_name = (local && local->type == AST_IDENTIFIER)
                ? local->u.ident.name : NULL;
            if (local_name)
                scope_declare_name(interp, local_name, ns, 1);
        }
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_export(Interpreter *interp, ASTNode *node)
{
    LRObject *ns = interp->module_ns;
    int nspec = node->u.export_decl.nspec;
    ASTNode **specifiers = node->u.export_decl.specifiers;
    ASTNode *src_node = node->u.export_decl.source;

    /* Re-export from another module? */
    JSModuleDef *re_mod = NULL;
    LRValue re_ns = LR_VALUE_UNDEFINED;
    if (src_node && src_node->type == AST_LITERAL) {
        re_ns = eval_resolve_module(interp, src_node->u.string.str, &re_mod);
        if (!re_mod) return LR_VALUE_UNDEFINED; /* exception already recorded */
    }

    LRValue ns_val;
    ns_val.tag = (ns ? LR_TYPE_OBJECT : LR_TYPE_UNDEFINED);
    ns_val.u.ptr = ns;

    for (int i = 0; i < nspec; i++) {
        ASTNode *spec = specifiers[i];
        if (!spec) continue;

        if (spec->type == AST_EXPORT_DEFAULT) {
            /* export default <expr> */
            LRValue val = interp_eval_node(interp, spec->u.export_default.value);
            if (ns) lr_set_property_str(interp->ctx, ns_val, "default", val); /* takes ownership */
            else lr_free_value(interp->ctx, val);
        } else if (spec->type == AST_EXPORT_NAMED) {
            /* export { local } or export { local as alias } [from "mod"] */
            const char *local_name = spec->u.export_spec.name;
            const char *exported_name =
                (spec->u.export_spec.exported
                 && spec->u.export_spec.exported->type == AST_IDENTIFIER)
                    ? spec->u.export_spec.exported->u.ident.name : local_name;
            if (!exported_name) exported_name = local_name;
            if (!exported_name) continue;

            LRValue val;
            if (re_mod) {
                val = lr_get_property_str(interp->ctx, re_ns, local_name ? local_name : "");
            } else {
                val = LR_VALUE_UNDEFINED;
                scope_lookup_internal(interp->current_scope, interp->ctx, local_name, &val);
            }
            if (ns) lr_set_property_str(interp->ctx, ns_val, exported_name, val); /* takes ownership */
            else lr_free_value(interp->ctx, val);
        } else if (spec->type == AST_EXPORT_ALL) {
            /* export * from "module" */
            if (re_mod) {
                LRPropertyEnum *tab = NULL;
                uint32_t plen = 0;
                lr_get_own_property_names(interp->ctx, &tab, &plen, re_ns, JS_GPN_STRING_MASK);
                for (uint32_t k = 0; k < plen; k++) {
                    const char *pname = lr_atom_to_cstring(interp->ctx, tab[k].atom);
                    if (pname && strcmp(pname, "default") != 0) {
                        LRValue val = lr_get_property_str(interp->ctx, re_ns, pname);
                        if (ns) lr_set_property_str(interp->ctx, ns_val, pname, val); /* takes ownership */
                        else lr_free_value(interp->ctx, val);
                    }
                    lr_free_cstring(interp->ctx, pname);
                }
                lr_free_property_enum(interp->ctx, tab, plen);
            }
        } else if (spec->type == AST_VAR_DECL) {
            /* export var/let/const ... : evaluate (binds names), then export them */
            LRValue val = interp_eval_node(interp, spec);
            lr_free_value(interp->ctx, val);
            char **names = NULL;
            int n = 0, cap = 0;
            for (int v = 0; v < spec->u.var_decl.nvars; v++) {
                ASTNode *decl = spec->u.var_decl.vars[v];
                if (decl && decl->type == AST_VAR_DECLARATOR)
                    collect_export_names(decl->u.declarator.var, &names, &n, &cap);
            }
            for (int v = 0; v < n; v++) {
                LRValue bval = LR_VALUE_UNDEFINED;
                scope_lookup_internal(interp->current_scope, interp->ctx, names[v], &bval);
                if (ns) lr_set_property_str(interp->ctx, ns_val, names[v], bval); /* takes ownership */
                else lr_free_value(interp->ctx, bval);
            }
            free(names);
        } else if (spec->type == AST_FUNC_DECL) {
            /* export function name() {} — eval_func_decl only declares the
             * name and returns UNDEFINED, so read the function object back
             * from the scope to export it. */
            const char *fname = spec->u.func.name;
            LRValue decl = interp_eval_node(interp, spec);
            lr_free_value(interp->ctx, decl);
            if (fname) {
                LRValue bval = LR_VALUE_UNDEFINED;
                scope_lookup_internal(interp->current_scope, interp->ctx, fname, &bval);
                if (ns) lr_set_property_str(interp->ctx, ns_val, fname, bval); /* takes ownership */
                else lr_free_value(interp->ctx, bval);
            }
        } else if (spec->type == AST_CLASS_DECL) {
            /* export class Name {} */
            const char *cname = spec->u.class_decl.name;
            LRValue val = interp_eval_node(interp, spec);
            if (cname) {
                if (ns) lr_set_property_str(interp->ctx, ns_val, cname, val); /* takes ownership */
                else lr_free_value(interp->ctx, val);
            } else {
                lr_free_value(interp->ctx, val);
            }
        }
    }
    return LR_VALUE_UNDEFINED;
}

/* ── Main Evaluation Dispatch ──────────────────────────────────────────── */

static LRValue interp_eval_stmt(Interpreter *interp, ASTNode *node)
{
    if (!node) return LR_VALUE_UNDEFINED;

    switch (node->type) {
    case AST_BLOCK:       return eval_block(interp, node);
    case AST_IF:          return eval_if(interp, node);
    case AST_FOR:         return eval_for(interp, node);
    case AST_FOR_IN:      return eval_for_in(interp, node);
    case AST_FOR_OF:      return eval_for_of(interp, node);
    case AST_WHILE:       return eval_while(interp, node);
    case AST_DO_WHILE:    return eval_do_while(interp, node);
    case AST_SWITCH:      return eval_switch(interp, node);
    case AST_BREAK:       return eval_break(interp, node);
    case AST_CONTINUE:    return eval_continue(interp, node);
    case AST_RETURN:      return eval_return(interp, node);
    case AST_THROW:       return eval_throw(interp, node);
    case AST_TRY:         return eval_try(interp, node);
    case AST_EXPR_STMT: {
        if (node->u.expr_stmt.expr) {
            LRValue val = interp_eval_node(interp, node->u.expr_stmt.expr);
            return val;
        }
        return LR_VALUE_UNDEFINED;
    }
    case AST_VAR_DECL:    return eval_var_decl(interp, node);
    case AST_FUNC_DECL:   return eval_func_decl(interp, node);
    case AST_CLASS_DECL:  return eval_class_decl(interp, node);
    case AST_IMPORT:      return eval_import(interp, node);
    case AST_EXPORT:      return eval_export(interp, node);
    case AST_LABEL: {
        ASTNode *stmt = node->u.label_stmt.stmt;
        const char *lname = (node->u.label_stmt.label &&
                             node->u.label_stmt.label->type == AST_IDENTIFIER)
                            ? node->u.label_stmt.label->u.ident.name : NULL;
        /* Hand the label to a directly-following loop/switch */
        const char *saved_pending = interp->pending_label;
        int is_breakable = stmt &&
            (stmt->type == AST_FOR || stmt->type == AST_WHILE ||
             stmt->type == AST_DO_WHILE || stmt->type == AST_FOR_IN ||
             stmt->type == AST_FOR_OF || stmt->type == AST_SWITCH);
        if (is_breakable) interp->pending_label = lname;
        LRValue r = interp_eval_node(interp, stmt);
        if (is_breakable) interp->pending_label = saved_pending;
        /* Labeled non-loop statement (labeled block): 'break lbl' exits here.
         * Also catches a propagating labeled break for a nested label. */
        if (interp->break_target && lname && interp->break_label[0] &&
            strcmp(interp->break_label, lname) == 0) {
            interp->break_target = 0;
            interp->break_label[0] = '\0';
        }
        return r;
    }
    case AST_DEBUGGER:
        /* Debugger statement - no-op */
        return LR_VALUE_UNDEFINED;
    default:
        /* Fall through to expression evaluation */
        return interp_eval_node(interp, node);
    }
}

/* ── Direct Threading: Function Pointer Table ────────────────────────── */

/* Each AST node type maps to a handler function.
 * This replaces the big switch statement, reducing branch mispredictions. */
typedef LRValue (*EvalHandler)(Interpreter *interp, ASTNode *node);

static LRValue eval_statement_dispatch(Interpreter *interp, ASTNode *node);
static LRValue eval_this_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_super_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_tagged_template(Interpreter *interp, ASTNode *node);
static LRValue eval_yield_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_pattern_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_rest_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_default_val(Interpreter *interp, ASTNode *node);
static LRValue eval_property(Interpreter *interp, ASTNode *node);
static LRValue eval_program(Interpreter *interp, ASTNode *node);

static EvalHandler eval_handlers[AST_IMPORT_NAMESPACE + 1];

/* Initialize the dispatch table (called once at first use) */
static void init_eval_handlers(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    /* Statements all go through the statement dispatch */
    for (int i = 0; i <= AST_IMPORT_NAMESPACE; i++) {
        eval_handlers[i] = eval_statement_dispatch;
    }

    /* Expression handlers (override statement dispatch) */
    eval_handlers[AST_LITERAL]         = eval_literal;
    eval_handlers[AST_IDENTIFIER]      = eval_identifier;
    eval_handlers[AST_THIS]            = eval_this_expr;
    eval_handlers[AST_SUPER]           = eval_super_expr;
    eval_handlers[AST_BINARY]          = eval_binary;
    eval_handlers[AST_UNARY]           = eval_unary;
    eval_handlers[AST_CONDITIONAL]     = eval_conditional;
    eval_handlers[AST_ASSIGN]          = eval_assign;
    eval_handlers[AST_MEMBER]          = eval_member;
    eval_handlers[AST_COMPUTED_MEMBER] = eval_computed_member;
    eval_handlers[AST_OPTIONAL_MEMBER] = eval_member;
    eval_handlers[AST_CALL]            = eval_call;
    eval_handlers[AST_OPTIONAL_CALL]   = eval_call;
    eval_handlers[AST_NEW]             = eval_new;
    eval_handlers[AST_ARRAY]           = eval_array;
    eval_handlers[AST_OBJECT]          = eval_object;
    eval_handlers[AST_FUNC_EXPR]       = eval_func_expr;
    eval_handlers[AST_ARROW]           = eval_arrow;
    eval_handlers[AST_TEMPLATE]        = eval_template;
    eval_handlers[AST_TAGGED_TEMPLATE] = eval_tagged_template;
    eval_handlers[AST_SEQUENCE]        = eval_sequence;
    eval_handlers[AST_SPREAD_ELEMENT]  = eval_spread;
    eval_handlers[AST_AWAIT]           = eval_await;
    eval_handlers[AST_YIELD]           = eval_yield_expr;
    eval_handlers[AST_PATTERN]         = eval_pattern_expr;
    eval_handlers[AST_REST]            = eval_rest_expr;
    eval_handlers[AST_DEFAULT_VALUE]   = eval_default_val;
    eval_handlers[AST_PROPERTY]        = eval_property;
    eval_handlers[AST_PROGRAM]         = eval_program;
}

/* ── Expression/Statement Dispatch ─────────────────────────────────────── */

static LRValue eval_statement_dispatch(Interpreter *interp, ASTNode *node)
{
    return interp_eval_stmt(interp, node);
}

static LRValue eval_this_expr(Interpreter *interp, ASTNode *node)
{
    (void)node;
    LRValue this_val;
    if (scope_lookup_internal(interp->current_scope, interp->ctx, "this", &this_val)) {
        return this_val;
    }
    return lr_get_global_object(interp->ctx);
}

static LRValue eval_super_expr(Interpreter *interp, ASTNode *node)
{
    (void)node;
    /* 'super.prop' reads resolve against the parent prototype */
    LRValue sproto;
    if (scope_lookup_internal(interp->current_scope, interp->ctx, "%superproto%", &sproto)) {
        return sproto;
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_tagged_template(Interpreter *interp, ASTNode *node)
{
    ASTNode *tag_node = node->u.template_lit.tag;
    if (!tag_node) return LR_VALUE_UNDEFINED;
    LRContext *ctx = interp->ctx;

    LRValue tag = interp_eval_node(interp, tag_node);
    if (interp->error_flag) {
        lr_free_value(ctx, tag);
        return LR_VALUE_UNDEFINED;
    }

    int nparts = node->u.template_lit.nparts;
    int nexp   = node->u.template_lit.nexp;
    ASTNode **exprs = node->u.template_lit.exprs;

    /* Build the 'strings' array (cooked) and its '.raw' array (verbatim). */
    LRValue strings = lr_new_array(ctx);
    LRValue raw_arr = lr_new_array(ctx);
    for (int i = 0; i < nparts; i++) {
        const char *verbatim = node->u.template_lit.parts[i]
                                 ? node->u.template_lit.parts[i] : "";
        char *cooked = template_unescape(verbatim);
        lr_set_property_uint32(ctx, strings, i,
                               lr_new_string(ctx, cooked ? cooked : ""));
        lr_set_property_uint32(ctx, raw_arr, i, lr_new_string(ctx, verbatim));
        if (cooked) free(cooked);
    }
    lr_set_property_str(ctx, strings, "length", lr_new_int32(ctx, nparts));
    lr_set_property_str(ctx, raw_arr, "length", lr_new_int32(ctx, nparts));
    lr_set_property_str(ctx, strings, "raw", raw_arr); /* raw_arr ownership -> strings */

    /* Build arguments: [strings, ...exprValues] */
    int argc = nexp + 1;
    LRValue *argv = (LRValue *)calloc(argc, sizeof(LRValue));
    argv[0] = strings; /* strings is consumed via argv[0] below */
    for (int i = 0; i < nexp; i++) {
        argv[i + 1] = interp_eval_node(interp, exprs[i]);
        if (interp->error_flag) {
            for (int j = 0; j <= i; j++) lr_free_value(ctx, argv[j]);
            free(argv);
            lr_free_value(ctx, tag);
            return LR_VALUE_UNDEFINED;
        }
    }

    LRValue result = call_value_with_args(interp, tag_node, tag,
                                          LR_VALUE_UNDEFINED, argc, argv);

    for (int i = 0; i < argc; i++) lr_free_value(ctx, argv[i]);
    free(argv);
    lr_free_value(ctx, tag);
    return result;
}

static LRValue eval_yield_expr(Interpreter *interp, ASTNode *node)
{
    if (!interp->gen_active) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "yield is only valid inside a generator function");
        interp->exception_value = LR_VALUE_UNDEFINED;
        interp->error_flag = 1;
        return LR_VALUE_UNDEFINED;
    }
    LRValue arg = node->u.yield_expr.arg
        ? interp_eval_node(interp, node->u.yield_expr.arg)
        : LR_VALUE_UNDEFINED;
    if (interp->error_flag) return arg;

    if (node->u.yield_expr.is_delegate) {
        gen_delegate(interp, arg);
    } else {
        gen_append(interp, arg);
    }
    lr_free_value(interp->ctx, arg);
    /* Eager generators cannot receive values from next(v): yield -> undefined */
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_pattern_expr(Interpreter *interp, ASTNode *node)
{
    (void)interp; (void)node;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_rest_expr(Interpreter *interp, ASTNode *node)
{
    (void)interp; (void)node;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_default_val(Interpreter *interp, ASTNode *node)
{
    (void)interp; (void)node;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_property(Interpreter *interp, ASTNode *node)
{
    (void)interp; (void)node;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_program(Interpreter *interp, ASTNode *node)
{
    LRValue result = LR_VALUE_UNDEFINED;
    int count = node->u.list.count;
    ASTNode **items = node->u.list.items;
    for (int i = 0; i < count; i++) {
        if (interp->error_flag) break;
        if (interp->has_returned) break;
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, items[i]);
    }
    return result;
}

/* ── Main Eval Dispatch ────────────────────────────────────────────────── */

static LRValue interp_eval_node(Interpreter *interp, ASTNode *node)
{
    if (!node) return LR_VALUE_UNDEFINED;
    if (interp->error_flag) return LR_VALUE_UNDEFINED;

    /* Initialize the dispatch table on first use */
    init_eval_handlers();

    /* Direct dispatch via function pointer table */
    if ((size_t)node->type <= AST_IMPORT_NAMESPACE && eval_handlers[node->type]) {
        return eval_handlers[node->type](interp, node);
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Public API ────────────────────────────────────────────────────────── */

LRValue interp_eval(Interpreter *interp, ASTNode *node)
{
    return interp_eval_node(interp, node);
}

/* Callback for lr_call_direct: call a JS interpreter function from C builtins */
static LRValue interp_callback_call(LRContext *ctx, LRValue func,
                                     LRValue this_val, int argc, LRValue *argv)
{
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (!interp) return LR_VALUE_UNDEFINED;

    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (obj->type == LR_OBJ_FUNCTION && obj->extra) {
        ASTNode *func_ast = (ASTNode *)obj->extra;
        /* Call the function/class body with its captured closure scope */
        interp->pending_closure = obj->def_scope;
        return interp_invoke_function_ast(interp, func_ast, this_val, argc, argv);
    }
    return LR_VALUE_UNDEFINED;
}

void interp_reattach(Interpreter *interp, LRContext *ctx)
{
    ctx->call_js_function = interp_callback_call;
    ctx->opaque_interp = interp;
}

void interp_init(Interpreter *interp, LRContext *ctx, int is_module)
{
    memset(interp, 0, sizeof(*interp));
    interp->ctx = ctx;
    interp->is_module = is_module;
    interp->filename = NULL;
    interp->import_meta = LR_VALUE_UNDEFINED;

    /* Set up the callback so C builtins can call JS functions */
    if (!ctx->call_js_function) {
        ctx->call_js_function = interp_callback_call;
    }
    ctx->opaque_interp = interp;

    /* Install the closure-scope release hook so lr_free_object can drop
     * captured scopes when function objects die */
    lr_closure_scope_release = interp_closure_release_hook;

    /* Create global scope */
    InterpScope *global = scope_new(NULL, 1, 1);
    global->mirror_globals = !is_module;   /* Script mode mirrors top-level
                                             * var/function onto the global object */
    interp->global_scope = global;
    interp->current_scope = global;

    /* Bind global object properties */
    LRValue global_obj = lr_get_global_object(ctx);
    /* Make 'globalThis' and 'global' refer to the global object */
    lr_set_property_str(ctx, global_obj, "globalThis", lr_dup_value(ctx, global_obj));
    lr_free_value(ctx, global_obj);
}

void interp_free(Interpreter *interp)
{
    if (!interp) return;

    /* Release all scopes on the current chain (each drops its own ref;
     * scopes still captured by live closures survive until object free) */
    {
        InterpScope *s = interp->current_scope;
        while (s) {
            InterpScope *p = s->parent;
            scope_release(s, interp->ctx);
            s = p;
        }
    }
    interp->current_scope = NULL;
    interp->global_scope = NULL;

    /* Free exception value */
    if (interp->exception_value.tag != LR_TYPE_UNDEFINED) {
        lr_free_value(interp->ctx, interp->exception_value);
        interp->exception_value = LR_VALUE_UNDEFINED;
    }
    /* Free return value */
    if (interp->return_value.tag != LR_TYPE_UNDEFINED) {
        lr_free_value(interp->ctx, interp->return_value);
        interp->return_value = LR_VALUE_UNDEFINED;
    }

    /* Clear context callback pointers since the interpreter is stack-allocated */
    if (interp->ctx) {
        interp->ctx->opaque_interp = NULL;
        interp->ctx->call_js_function = NULL;
    }
}