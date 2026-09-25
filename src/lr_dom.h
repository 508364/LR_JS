/*
 * L/R_JS - DOM Binding (Virtual DOM)
 * Pure C, lightweight virtual DOM for the LR_JS JavaScript engine.
 *
 * Provides: document, window, Element, HTMLElement, EventTarget,
 * Event, CustomEvent constructors and basic DOM manipulation.
 *
 * All DOM nodes are managed as C structs referenced by JS objects
 * via lr_set_opaque / lr_get_opaque.
 */
#ifndef LR_DOM_H
#define LR_DOM_H

#include "lr_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── DOM Node structure ───────────────────────────────────────────────────
 *
 * Each DOM element/text node is backed by one LR_DomNode struct,
 * pointed to by the JS object's opaque pointer.
 * Reference-counted: the JS object holds one reference; the parent
 * element holds one reference per child.  When ref_count reaches 0
 * the node is freed.
 */

typedef struct LR_DomNode {
    char             *tag_name;        /* "div", "span", etc. NULL for text nodes */
    char             *text_content;    /* Text content (for text nodes or innerHTML cache) */
    int               is_text_node;    /* 1 = text node, 0 = element node */
    int               ref_count;       /* reference count */

    /* Tree structure */
    struct LR_DomNode **children;      /* array of child pointers */
    int                child_count;
    int                child_capacity;
    struct LR_DomNode *parent;         /* parent node (NULL for root) */

    /* Attributes (parallel arrays key→value) */
    char             **attr_names;
    char             **attr_values;
    int                attr_count;
    int                attr_capacity;

    /* Style properties (parallel arrays) */
    char             **style_names;
    char             **style_values;
    int                style_count;
    int                style_capacity;
} LR_DomNode;

/* ── DOM API functions ─────────────────────────────────────────────────── */

/* Initialize DOM bindings on the given runtime.
 * Registers: document, window, Element, HTMLElement, Event, CustomEvent,
 * EventTarget constructors on the global object.
 * Called from lr_register_builtins() in lr_runtime.c.
 */
void lr_dom_init(LR_Runtime *rt);

/* ── DOM Node management ────────────────────────────────────────────────── */

/* Create a new DOM element node. Returns NULL on OOM. */
LR_DomNode *lr_dom_node_create(const char *tag_name);

/* Create a new DOM text node. Returns NULL on OOM. */
LR_DomNode *lr_dom_text_node_create(const char *text);

/* Retain (increment refcount) a DOM node. */
LR_DomNode *lr_dom_node_retain(LR_DomNode *node);

/* Release (decrement refcount) a DOM node. Frees when refcount hits 0. */
void lr_dom_node_release(LR_DomNode *node);

/* ── Tree manipulation ──────────────────────────────────────────────────── */

/* Append a child node. Returns 0 on success, -1 on error. */
int lr_dom_node_append_child(LR_DomNode *parent, LR_DomNode *child);

/* Remove a child node. Returns 0 on success, -1 if not found. */
int lr_dom_node_remove_child(LR_DomNode *parent, LR_DomNode *child);

/* ── Attribute manipulation ─────────────────────────────────────────────── */

/* Set an attribute. Creates or updates. */
void lr_dom_node_set_attribute(LR_DomNode *node, const char *name, const char *value);

/* Get an attribute. Returns NULL if not found. */
const char *lr_dom_node_get_attribute(LR_DomNode *node, const char *name);

/* Remove an attribute. */
void lr_dom_node_remove_attribute(LR_DomNode *node, const char *name);

/* ── Style manipulation ─────────────────────────────────────────────────── */

/* Set a style property. */
void lr_dom_node_set_style(LR_DomNode *node, const char *name, const char *value);

/* Get a style property. Returns "" if not set. */
const char *lr_dom_node_get_style(LR_DomNode *node, const char *name);

/* ── Query helpers ──────────────────────────────────────────────────────── */

/* Find the first element node with the given id (depth-first). */
LR_DomNode *lr_dom_node_get_element_by_id(LR_DomNode *root, const char *id);

/* Find the first element matching a simple tag selector (depth-first).
 * Only supports tag name selectors for now. */
LR_DomNode *lr_dom_node_query_selector(LR_DomNode *root, const char *selector);

/* Serialize a node and its children to an HTML string (malloc'd).
 * Caller must free() the returned string. */
char *lr_dom_node_serialize(LR_DomNode *node);

/* ── JS-side helpers (used by lr_dom.c internally) ──────────────────────── */

/* Create a JS Element object wrapping a new DOM node with the given tag. */
JSValue lr_dom_new_element(JSContext *ctx, const char *tag);

/* Create a JS Text node object wrapping a new DOM text node. */
JSValue lr_dom_new_text_node(JSContext *ctx, const char *text);

/* Append a child JS element to a parent JS element. */
JSValue lr_dom_append_child(JSContext *ctx, JSValue parent, JSValue child);

/* Set an attribute on a JS element. */
JSValue lr_dom_set_attribute(JSContext *ctx, JSValue element,
                              const char *name, const char *value);

/* Get an element by id from a JS Document object. */
JSValue lr_dom_get_element_by_id(JSContext *ctx, JSValue doc, const char *id);

/* Query selector from a JS root element. */
JSValue lr_dom_query_selector(JSContext *ctx, JSValue root, const char *selector);

#ifdef __cplusplus
}
#endif

#endif /* LR_DOM_H */