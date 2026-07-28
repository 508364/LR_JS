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

/* ── Scope Management ──────────────────────────────────────────────────── */

static InterpScope *scope_new(InterpScope *parent, int is_function, int is_global)
{
    InterpScope *s = (InterpScope *)calloc(1, sizeof(InterpScope));
    if (!s) return NULL;
    s->parent = parent;
    s->is_function_scope = is_function;
    s->is_global_scope = is_global;
    s->capacity = SCOPE_INIT_CAP;
    s->names = (char **)calloc(s->capacity, sizeof(char *));
    s->values = (LRValue *)calloc(s->capacity, sizeof(LRValue));
    s->is_const = (int *)calloc(s->capacity, sizeof(int));
    return s;
}

static void scope_free(InterpScope *scope, LRContext *ctx)
{
    if (!scope) return;
    for (int i = 0; i < scope->count; i++) {
        if (scope->names[i]) free(scope->names[i]);
        lr_free_value(ctx, scope->values[i]);
    }
    free(scope->names);
    free(scope->values);
    free(scope->is_const);
    free(scope);
}

static void interp_push_scope(Interpreter *interp, int is_function_scope)
{
    InterpScope *s = scope_new(interp->current_scope, is_function_scope, 0);
    if (interp->current_scope == NULL) {
        s->is_global_scope = 1;
    }
    interp->current_scope = s;
}

static void interp_pop_scope(Interpreter *interp)
{
    if (!interp->current_scope) return;
    InterpScope *old = interp->current_scope;
    interp->current_scope = old->parent;
    old->parent = NULL; /* prevent freeing parent chain */
    scope_free(old, interp->ctx);
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
 * Returns 1 if found (value set), 0 if not found. */
static int scope_lookup_internal(InterpScope *scope, const char *name, LRValue *value)
{
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

/* Declare a variable in the current scope (for let/const) or function scope (for var).
 * kind: 0=var, 1=let, 2=const */
static void scope_declare_name(Interpreter *interp, const char *name, LRValue value, int kind)
{
    InterpScope *scope;
    if (kind == 0) {
        /* var hoists to function scope */
        scope = find_function_scope(interp);
    } else {
        /* let/const are block-scoped */
        scope = interp->current_scope;
    }

    /* Check if already declared in this scope */
    for (int i = 0; i < scope->count; i++) {
        if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
            /* Redeclaration in same scope - update value */
            lr_free_value(interp->ctx, scope->values[i]);
            scope->values[i] = lr_dup_value(interp->ctx, value);
            return;
        }
    }

    /* Add new entry */
    if (scope->count >= scope->capacity) {
        scope->capacity *= 2;
        scope->names = (char **)realloc(scope->names, scope->capacity * sizeof(char *));
        scope->values = (LRValue *)realloc(scope->values, scope->capacity * sizeof(LRValue));
        scope->is_const = (int *)realloc(scope->is_const, scope->capacity * sizeof(int));
        /* Zero out new entries */
        for (int i = scope->count; i < scope->capacity; i++) {
            scope->names[i] = NULL;
            scope->values[i] = LR_VALUE_UNDEFINED;
            scope->is_const[i] = 0;
        }
    }
    scope->names[scope->count] = strdup(name);
    scope->values[scope->count] = lr_dup_value(interp->ctx, value);
    scope->is_const[scope->count] = (kind == 2) ? 1 : 0;
    scope->count++;
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
    if (scope_lookup_internal(interp->current_scope, name, &val)) {
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
        LRValue pv = lr_get_property(interp->ctx, right, atom);
        result = lr_new_bool(interp->ctx, !lr_is_undefined(pv));
        lr_free_value(interp->ctx, pv);
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

static LRValue eval_member(Interpreter *interp, ASTNode *node)
{
    int is_optional = node->u.member.is_optional;
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

    /* Check if this is a method call (member expression as callee) */
    if (callee_node->type == AST_MEMBER) {
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
                result = interp_call_function(interp, func_ast, this_val, argc, argv);
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

static LRValue eval_array(Interpreter *interp, ASTNode *node)
{
    LRValue arr = lr_new_array(interp->ctx);
    int nelem = node->u.array.nelem;
    ASTNode **elements = node->u.array.elements;

    for (int i = 0; i < nelem; i++) {
        ASTNode *elem = elements[i];
        if (elem == NULL) {
            /* Array hole - skip (remains undefined) */
            continue;
        }
        if (elem->type == AST_SPREAD_ELEMENT) {
            /* Spread element */
            LRValue spread_val = interp_eval_node(interp, elem->u.spread.arg);
            if (interp->error_flag) { lr_free_value(interp->ctx, arr); lr_free_value(interp->ctx, spread_val); return LR_VALUE_UNDEFINED; }
            if (lr_is_array(interp->ctx, spread_val)) {
                int32_t len = 0;
                LRValue len_val = lr_get_property_str(interp->ctx, spread_val, "length");
                lr_to_int32(interp->ctx, &len, len_val);
                lr_free_value(interp->ctx, len_val);
                for (int32_t j = 0; j < len; j++) {
                    LRValue item = lr_get_property_uint32(interp->ctx, spread_val, j);
                    /* lr_set_property_uint32 takes ownership of item */
                    lr_set_property_uint32(interp->ctx, arr, i, item);
                }
            }
            lr_free_value(interp->ctx, spread_val);
        } else {
            LRValue val = interp_eval_node(interp, elem);
            if (interp->error_flag) { lr_free_value(interp->ctx, arr); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            /* lr_set_property_uint32 takes ownership of val, do NOT free it */
            lr_set_property_uint32(interp->ctx, arr, i, val);
        }
    }

    /* Set length */
    lr_set_property_str(interp->ctx, arr, "length", lr_new_int32(interp->ctx, nelem));
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

            LRValue key_val;
            if (key_node->type == AST_IDENTIFIER) {
                const char *kname = key_node->u.ident.name;
                if (shorthand) {
                    /* { x } is shorthand for { x: x } */
                    LRValue val = interp_eval_node(interp, val_node);
                    if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
                    /* lr_set_property_str takes ownership of val */
                    lr_set_property_str(interp->ctx, obj, kname, val);
                } else {
                    LRValue val = interp_eval_node(interp, val_node);
                    if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
                    /* lr_set_property_str takes ownership of val */
                    lr_set_property_str(interp->ctx, obj, kname, val);
                }
            } else {
                /* Computed or literal key */
                key_val = interp_eval_node(interp, key_node);
                if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, key_val); return LR_VALUE_UNDEFINED; }
                LRValue val = interp_eval_node(interp, val_node);
                if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, key_val); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
                LRString *atom = lr_to_atom(interp->ctx, key_val);
                /* lr_set_property takes ownership of val */
                lr_set_property(interp->ctx, obj, atom, val);
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
    }
    return obj;
}

static LRValue eval_template(Interpreter *interp, ASTNode *node)
{
    /* Template literal: `text ${expr} text` */
    const char *raw = node->u.template_lit.raw;
    int nexp = node->u.template_lit.nexp;
    ASTNode **exprs = node->u.template_lit.exprs;

    if (!raw) return lr_new_string(interp->ctx, "");

    /* Concatenate parts */
    /* The raw string contains all the static text parts joined together.
     * We need to interleave the expressions. */
    size_t total_len = strlen(raw);

    /* Evaluate all expressions and sum their string lengths */
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

    /* Build the result string */
    char *buf = (char *)malloc(total_len + 1);
    if (!buf) {
        if (expr_vals) {
            for (int i = 0; i < nexp; i++) lr_free_value(interp->ctx, expr_vals[i]);
            free(expr_vals);
        }
        return LR_VALUE_UNDEFINED;
    }

    size_t pos = 0;
    const char *raw_ptr = raw;
    /* The raw string contains ${} markers where expressions go.
     * We need to parse the raw string, splitting at ${...} boundaries. */
    for (int i = 0; i < nexp; i++) {
        /* Find the next ${ in the raw string */
        const char *dollar_brace = strstr(raw_ptr, "${");
        if (dollar_brace) {
            size_t copy_len = dollar_brace - raw_ptr;
            memcpy(buf + pos, raw_ptr, copy_len);
            pos += copy_len;
            raw_ptr = dollar_brace + 2; /* skip ${ */
        }
        /* Insert expression value */
        const char *s = lr_to_cstring(interp->ctx, expr_vals[i]);
        size_t slen = strlen(s);
        memcpy(buf + pos, s, slen);
        pos += slen;
        lr_free_cstring(interp->ctx, s);
        /* Skip past the } in the raw string */
        const char *brace = strchr(raw_ptr, '}');
        if (brace) {
            raw_ptr = brace + 1;
        }
    }
    /* Copy remaining text */
    if (raw_ptr) {
        size_t remaining = strlen(raw_ptr);
        memcpy(buf + pos, raw_ptr, remaining);
        pos += remaining;
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

    /* If the result is a Promise, try to get its [[PromiseResult]] */
    if (lr_is_promise(interp->ctx, arg)) {
        LRValue result = lr_promise_get_result(arg);
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
    /* Create a class - simplified implementation */
    LRValue proto = lr_new_object(interp->ctx);
    LRValue ctor = lr_new_object(interp->ctx);
    if (ctor.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)ctor.u.ptr;
        o->type = LR_OBJ_FUNCTION;
    }

    /* Set prototype.constructor = ctor */
    const char *class_name = node->u.class_decl.name;
    if (class_name) {
        lr_set_property_str(interp->ctx, ctor, "name", lr_new_string(interp->ctx, class_name));
    }

    /* Set up prototype chain */
    LRValue extends = LR_VALUE_UNDEFINED;
    if (node->u.class_decl.extends) {
        extends = interp_eval_node(interp, node->u.class_decl.extends);
        if (interp->error_flag) {
            lr_free_value(interp->ctx, proto);
            lr_free_value(interp->ctx, ctor);
            lr_free_value(interp->ctx, extends);
            return LR_VALUE_UNDEFINED;
        }
    }

    /* Add methods */
    int nmethods = node->u.class_decl.nmethods;
    ASTNode **methods = node->u.class_decl.methods;
    for (int i = 0; i < nmethods; i++) {
        ASTNode *method = methods[i];
        if (method && method->type == AST_FUNC_EXPR) {
            const char *mname = method->u.func.name;
            /* Check if it's the constructor */
            if (mname && strcmp(mname, "constructor") == 0) {
                /* Assign to constructor */
                LRValue func_obj = eval_func_expr(interp, method);
                lr_set_property_str(interp->ctx, ctor, "prototype", lr_dup_value(interp->ctx, proto));
                /* Copy function properties */
                lr_free_value(interp->ctx, ctor);
                ctor = func_obj;
                lr_set_property_str(interp->ctx, proto, "constructor", lr_dup_value(interp->ctx, ctor));
            } else {
                LRValue func_obj = eval_func_expr(interp, method);
                lr_set_property_str(interp->ctx, proto, mname ? mname : "", func_obj);
                lr_free_value(interp->ctx, func_obj);
            }
        }
    }

    /* Set prototype */
    LRValue result = lr_new_object(interp->ctx);
    lr_set_property_str(interp->ctx, result, "prototype", lr_dup_value(interp->ctx, proto));
    if (class_name) {
        lr_set_property_str(interp->ctx, result, "name", lr_new_string(interp->ctx, class_name));
    }

    lr_free_value(interp->ctx, proto);
    lr_free_value(interp->ctx, extends);
    return result;
}

/* ── Destructuring ─────────────────────────────────────────────────────── */

static LRValue eval_pattern(Interpreter *interp, ASTNode *node, LRValue value)
{
    /* Handle array destructuring: [a, b] = arr */
    if (node->u.pattern_array.elements != NULL || node->type == AST_PATTERN) {
        /* Array destructuring */
        int nelem = node->u.pattern_array.nelem;
        ASTNode **elements = node->u.pattern_array.elements;

        for (int i = 0; i < nelem; i++) {
            ASTNode *elem = elements[i];
            if (elem == NULL) continue; /* hole */

            if (elem->type == AST_REST) {
                /* Rest element: ...rest */
                ASTNode *rest_target = elem->u.rest_elem.arg;
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
            } else if (elem->type == AST_PATTERN) {
                /* Nested destructuring */
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                eval_pattern(interp, elem, item_val);
                lr_free_value(interp->ctx, item_val);
            }
        }
        return LR_VALUE_UNDEFINED;
    }

    /* Object destructuring: {a, b} = obj */
    if (node->u.pattern_object.props != NULL) {
        int nprops = node->u.pattern_object.nprops;
        ASTNode **props = node->u.pattern_object.props;

        for (int i = 0; i < nprops; i++) {
            ASTNode *prop = props[i];
            if (prop->type == AST_REST) {
                /* Rest in object pattern */
                ASTNode *rest_target = prop->u.rest_elem.arg;
                if (rest_target && rest_target->type == AST_IDENTIFIER) {
                    LRValue rest_obj = lr_new_object(interp->ctx);
                    /* Copy all own properties */
                    LRPropertyEnum *pe = NULL;
                    uint32_t npe = 0;
                    lr_get_own_property_names(interp->ctx, &pe, &npe, value, 0);
                    for (uint32_t j = 0; j < npe; j++) {
                        LRValue v = lr_get_property(interp->ctx, value, pe[j].atom);
                        lr_set_property(interp->ctx, rest_obj, pe[j].atom, v);
                        lr_free_value(interp->ctx, v);
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
                    } else if (val_node->type == AST_PATTERN) {
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

/* ── Function Call Support ─────────────────────────────────────────────── */

static LRValue interp_call_function(Interpreter *interp, ASTNode *func_node,
                                     LRValue this_val, int argc, LRValue *argv)
{
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
    interp->return_target = 0;
    interp->has_returned = 0;
    interp->return_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 0;

    /* Create a new function scope */
    interp_push_scope(interp, 1);

    /* Bind 'this' */
    scope_declare_name(interp, "this", this_val, 1); /* const-like */

    /* Bind arguments */
    int nparams = 0;
    ASTNode **params = NULL;
    int is_arrow = 0;

    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        nparams = func_node->u.func.nparams;
        params = func_node->u.func.params;
    } else if (func_node->type == AST_ARROW) {
        nparams = func_node->u.arrow.nparams;
        params = func_node->u.arrow.params;
        is_arrow = 1;
    }

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
        } else if (param->type == AST_REST) {
            /* Rest parameter */
            ASTNode *rest_target = param->u.rest_elem.arg;
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
        } else if (param->type == AST_PATTERN) {
            /* Destructuring parameter */
            if (i < argc) {
                eval_pattern(interp, param, argv[i]);
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

    /* Pop scope */
    interp_pop_scope(interp);

    /* Restore interpreter state */
    interp->depth--;
    interp->break_target = saved_break;
    interp->continue_target = saved_continue;
    interp->return_target = saved_return;
    interp->has_returned = saved_has_returned;
    /* Don't restore return_value if we're inside a return (it propagates) */
    if (!saved_has_returned) {
        lr_free_value(interp->ctx, interp->return_value);
        interp->return_value = LR_VALUE_UNDEFINED;
    }
    interp->return_value = saved_return_val;
    interp->error_flag = saved_error;
    if (!saved_error) {
        memcpy(interp->error_message, saved_err_msg, 512);
    }

    /* Pop call frame */
    lr_pop_call_frame(interp->ctx);

    return result;
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

static LRValue eval_for(Interpreter *interp, ASTNode *node)
{
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
        if (interp->break_target) { interp->break_target = 0; break; }
        if (interp->has_returned || interp->error_flag) break;

        /* Update */
        if (node->u.for_stmt.update) {
            LRValue update = interp_eval_node(interp, node->u.for_stmt.update);
            lr_free_value(interp->ctx, update);
            if (interp->error_flag) break;
        }

        /* Handle continue */
        if (interp->continue_target) {
            interp->continue_target = 0;
        }
    }

    /* Clear continue if set */
    if (interp->continue_target) interp->continue_target = 0;
    /* Don't clear break - it's consumed by the loop */

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_for_in(Interpreter *interp, ASTNode *node)
{
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
            interp->continue_target = 0;
        }
    }

    if (interp->break_target) interp->break_target = 0;
    if (interp->continue_target) interp->continue_target = 0;

    lr_free_property_enum(interp->ctx, props, nprops);
    lr_free_value(interp->ctx, source);

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_for_of(Interpreter *interp, ASTNode *node)
{
    /* Evaluate the source iterable */
    LRValue source = interp_eval_node(interp, node->u.for_of.source);
    if (interp->error_flag) return source;

    /* Create a scope for the loop variable */
    interp_push_scope(interp, 0);

    LRValue result = LR_VALUE_UNDEFINED;

    if (lr_is_array(interp->ctx, source)) {
        int32_t len = 0;
        LRValue len_val = lr_get_property_str(interp->ctx, source, "length");
        lr_to_int32(interp->ctx, &len, len_val);
        lr_free_value(interp->ctx, len_val);

        for (int32_t i = 0; i < len; i++) {
            if (interp->break_target || interp->has_returned || interp->error_flag) break;

            LRValue item = lr_get_property_uint32(interp->ctx, source, i);

            ASTNode *each = node->u.for_of.each;
            if (each) {
                if (each->type == AST_VAR_DECL) {
                    if (each->u.var_decl.nvars > 0) {
                        ASTNode *declarator = each->u.var_decl.vars[0];
                        if (declarator && declarator->type == AST_VAR_DECLARATOR) {
                            ASTNode *var = declarator->u.declarator.var;
                            if (var && var->type == AST_IDENTIFIER) {
                                scope_declare_name(interp, var->u.ident.name, item, 0);
                            }
                        }
                    }
                } else if (each->type == AST_IDENTIFIER) {
                    scope_set_name(interp, each->u.ident.name, item);
                }
            }

            lr_free_value(interp->ctx, item);

            if (result.tag != LR_TYPE_UNDEFINED) {
                lr_free_value(interp->ctx, result);
            }
            result = interp_eval_node(interp, node->u.for_of.body);
            if (interp->continue_target) {
                interp->continue_target = 0;
            }
        }
    }

    if (interp->break_target) interp->break_target = 0;
    if (interp->continue_target) interp->continue_target = 0;

    lr_free_value(interp->ctx, source);

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_while(Interpreter *interp, ASTNode *node)
{
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
        if (interp->break_target) { interp->break_target = 0; break; }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) interp->continue_target = 0;
    }

    if (interp->continue_target) interp->continue_target = 0;
    return result;
}

static LRValue eval_do_while(Interpreter *interp, ASTNode *node)
{
    LRValue result = LR_VALUE_UNDEFINED;

    do {
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.if_stmt.body);
        if (interp->break_target) { interp->break_target = 0; break; }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) { interp->continue_target = 0; }

        LRValue cond = interp_eval_node(interp, node->u.if_stmt.cond);
        int truthy = lr_to_bool(interp->ctx, cond);
        lr_free_value(interp->ctx, cond);
        if (!truthy) break;
    } while (!interp->break_target && !interp->has_returned && !interp->error_flag);

    if (interp->continue_target) interp->continue_target = 0;
    return result;
}

static LRValue eval_switch(Interpreter *interp, ASTNode *node)
{
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
                interp->break_target = 0;
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
    (void)node;
    interp->break_target = 1;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_continue(Interpreter *interp, ASTNode *node)
{
    (void)node;
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
    lr_free_value(interp->ctx, class_obj);
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_import(Interpreter *interp, ASTNode *node)
{
    /* Import declaration - simplified stub */
    /* In a real implementation, this would load modules */
    int nspec = node->u.import_decl.nspec;
    ASTNode **specifiers = node->u.import_decl.specifiers;

    for (int i = 0; i < nspec; i++) {
        ASTNode *spec = specifiers[i];
        if (spec->type == AST_IMPORT_SPECIFIER) {
            const char *name = spec->u.import_spec.name;
            ASTNode *local = spec->u.import_spec.local;
            if (local && local->type == AST_IDENTIFIER) {
                const char *local_name = local->u.ident.name;
                /* Create a placeholder value */
                LRValue val = LR_VALUE_UNDEFINED;
                scope_declare_name(interp, local_name ? local_name : name, val, 1);
            }
        } else if (spec->type == AST_IMPORT_NAMESPACE) {
            ASTNode *local = spec->u.import_namespace.local;
            if (local && local->type == AST_IDENTIFIER) {
                LRValue ns = lr_new_object(interp->ctx);
                scope_declare_name(interp, local->u.ident.name, ns, 1);
                lr_free_value(interp->ctx, ns);
            }
        }
    }

    return LR_VALUE_UNDEFINED;
}

static LRValue eval_export(Interpreter *interp, ASTNode *node)
{
    /* Export declaration - simplified stub */
    /* In a real module system, this would register exports */
    int nspec = node->u.export_decl.nspec;
    ASTNode **specifiers = node->u.export_decl.specifiers;

    for (int i = 0; i < nspec; i++) {
        ASTNode *spec = specifiers[i];
        if (spec->type == AST_EXPORT_DEFAULT) {
            /* export default ... */
            if (spec->u.export_default.value) {
                LRValue val = interp_eval_node(interp, spec->u.export_default.value);
                lr_free_value(interp->ctx, val);
            }
        } else if (spec->type == AST_EXPORT_NAMED) {
            /* export { name } */
            const char *name = spec->u.export_spec.name;
            (void)name;
        } else if (spec->type == AST_VAR_DECL) {
            /* export var/let/const ... */
            LRValue val = interp_eval_node(interp, spec);
            lr_free_value(interp->ctx, val);
        } else if (spec->type == AST_FUNC_DECL) {
            /* export function ... */
            LRValue val = interp_eval_node(interp, spec);
            lr_free_value(interp->ctx, val);
        } else if (spec->type == AST_CLASS_DECL) {
            /* export class ... */
            LRValue val = interp_eval_node(interp, spec);
            lr_free_value(interp->ctx, val);
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
        /* Labeled statement - just execute the inner statement */
        return interp_eval_node(interp, node->u.label_stmt.stmt);
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
    if (scope_lookup_internal(interp->current_scope, "this", &this_val)) {
        return this_val;
    }
    return lr_get_global_object(interp->ctx);
}

static LRValue eval_super_expr(Interpreter *interp, ASTNode *node)
{
    (void)interp; (void)node;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_tagged_template(Interpreter *interp, ASTNode *node)
{
    (void)interp; (void)node;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_yield_expr(Interpreter *interp, ASTNode *node)
{
    (void)interp; (void)node;
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
        /* Call the function's body */
        if (func_ast->type == AST_FUNC_EXPR || func_ast->type == AST_FUNC_DECL) {
            return interp_call_function(interp, func_ast, this_val, argc, argv);
        }
        if (func_ast->type == AST_ARROW) {
            return interp_call_function(interp, func_ast, this_val, argc, argv);
        }
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

    /* Set up the callback so C builtins can call JS functions */
    if (!ctx->call_js_function) {
        ctx->call_js_function = interp_callback_call;
    }
    ctx->opaque_interp = interp;

    /* Create global scope */
    InterpScope *global = scope_new(NULL, 1, 1);
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

    /* Free all scopes */
    while (interp->current_scope) {
        InterpScope *s = interp->current_scope;
        interp->current_scope = s->parent;
        s->parent = NULL;
        scope_free(s, interp->ctx);
    }
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