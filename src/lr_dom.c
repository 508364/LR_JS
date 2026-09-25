/*
 * L/R_JS - DOM Binding Implementation (Virtual DOM)
 * Pure C, lightweight virtual DOM for the LR_JS JavaScript engine.
 *
 * Provides Document, Element, HTMLElement, EventTarget constructors
 * and basic DOM manipulation APIs.
 *
 * Architecture:
 *   - Each DOM node is backed by a C struct (LR_DomNode) stored in the
 *     JS object's opaque pointer.
 *   - The JS object holds a reference to the DomNode; the parent holds
 *     a reference to each child.  When the JS object is GC'd, the
 *     opaque_free callback releases the DomNode reference.
 *   - Event handling is delegated to the existing EventTarget system
 *     in lr_event.c (__listeners array on the JS object).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_dom.h"

/* ── Forward declarations of JS-callable functions ──────────────────────── */

static JSValue lr_dom_element_constructor(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv);
static JSValue lr_dom_htmlelement_constructor(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);
static JSValue lr_dom_document_constructor(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);

/* Element prototype methods */
static JSValue lr_dom_js_append_child(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static JSValue lr_dom_js_remove_child(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static JSValue lr_dom_js_set_attribute(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv);
static JSValue lr_dom_js_get_attribute(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv);
static JSValue lr_dom_js_remove_attribute(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv);
static JSValue lr_dom_js_add_event_listener(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv);
static JSValue lr_dom_js_dispatch_event(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);

/* Document prototype methods */
static JSValue lr_dom_js_create_element(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
static JSValue lr_dom_js_create_text_node(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv);
static JSValue lr_dom_js_get_element_by_id(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static JSValue lr_dom_js_query_selector(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);

/* Element property getters/setters */
static JSValue lr_dom_get_tag_name(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_get_id(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_set_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue lr_dom_get_class_name(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_set_class_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue lr_dom_get_inner_html(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_set_inner_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue lr_dom_get_children(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_get_child_nodes(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_get_parent_node(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_get_style(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_get_first_child(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_get_last_child(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_get_text_content(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_set_text_content(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* Style object getter/setter */
static JSValue lr_dom_style_get(JSContext *ctx, JSValueConst this_val, JSValueConst prop);
static JSValue lr_dom_style_set(JSContext *ctx, JSValueConst this_val, JSValueConst prop, JSValueConst val);

/* Window/document property getters */
static JSValue lr_dom_window_get_document(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_document_get_document_element(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_document_get_body(JSContext *ctx, JSValueConst this_val);
static JSValue lr_dom_document_get_head(JSContext *ctx, JSValueConst this_val);

/* Forward declarations for helpers used before their definitions */
static LR_DomNode *lr_dom_find_child_by_tag(LR_DomNode *parent, const char *tag);
static JSValue lr_dom_wrap_node(JSContext *ctx, LR_DomNode *node);

/* ── DOM Node Management ────────────────────────────────────────────────── */

LR_DomNode *lr_dom_node_create(const char *tag_name)
{
    LR_DomNode *node = (LR_DomNode *)calloc(1, sizeof(LR_DomNode));
    if (!node) return NULL;
    node->tag_name = tag_name ? strdup(tag_name) : NULL;
    node->ref_count = 1;
    node->is_text_node = 0;
    node->child_capacity = 4;
    node->children = (LR_DomNode **)calloc((size_t)node->child_capacity, sizeof(LR_DomNode *));
    if (!node->children) {
        free(node->tag_name);
        free(node);
        return NULL;
    }
    node->attr_capacity = 4;
    node->attr_names = (char **)calloc((size_t)node->attr_capacity, sizeof(char *));
    node->attr_values = (char **)calloc((size_t)node->attr_capacity, sizeof(char *));
    if (!node->attr_names || !node->attr_values) {
        free(node->attr_names);
        free(node->attr_values);
        free(node->children);
        free(node->tag_name);
        free(node);
        return NULL;
    }
    node->style_capacity = 4;
    node->style_names = (char **)calloc((size_t)node->style_capacity, sizeof(char *));
    node->style_values = (char **)calloc((size_t)node->style_capacity, sizeof(char *));
    if (!node->style_names || !node->style_values) {
        free(node->style_names);
        free(node->style_values);
        free(node->attr_names);
        free(node->attr_values);
        free(node->children);
        free(node->tag_name);
        free(node);
        return NULL;
    }
    return node;
}

LR_DomNode *lr_dom_text_node_create(const char *text)
{
    LR_DomNode *node = (LR_DomNode *)calloc(1, sizeof(LR_DomNode));
    if (!node) return NULL;
    node->text_content = text ? strdup(text) : strdup("");
    node->ref_count = 1;
    node->is_text_node = 1;
    node->child_capacity = 0;
    node->children = NULL;
    node->attr_capacity = 0;
    node->attr_names = NULL;
    node->attr_values = NULL;
    node->style_capacity = 0;
    node->style_names = NULL;
    node->style_values = NULL;
    return node;
}

LR_DomNode *lr_dom_node_retain(LR_DomNode *node)
{
    if (!node) return NULL;
    node->ref_count++;
    return node;
}

void lr_dom_node_release(LR_DomNode *node)
{
    if (!node) return;
    if (node->ref_count <= 0) return;
    node->ref_count--;
    if (node->ref_count > 0) return;

    /* Free children recursively */
    for (int i = 0; i < node->child_count; i++) {
        lr_dom_node_release(node->children[i]);
    }

    /* Free attributes */
    for (int i = 0; i < node->attr_count; i++) {
        free(node->attr_names[i]);
        free(node->attr_values[i]);
    }

    /* Free style properties */
    for (int i = 0; i < node->style_count; i++) {
        free(node->style_names[i]);
        free(node->style_values[i]);
    }

    free(node->tag_name);
    free(node->text_content);
    free(node->children);
    free(node->attr_names);
    free(node->attr_values);
    free(node->style_names);
    free(node->style_values);
    free(node);
}

/* Opaque free callback for DOM nodes (called by engine when JS object is GC'd) */
static void lr_dom_opaque_free(void *opaque)
{
    LR_DomNode *node = (LR_DomNode *)opaque;
    if (node) {
        lr_dom_node_release(node);
    }
}

/* ── Tree Manipulation ──────────────────────────────────────────────────── */

int lr_dom_node_append_child(LR_DomNode *parent, LR_DomNode *child)
{
    if (!parent || !child || parent->is_text_node) return -1;

    /* Remove from old parent if any */
    if (child->parent) {
        lr_dom_node_remove_child(child->parent, child);
    }

    /* Grow array if needed */
    if (parent->child_count >= parent->child_capacity) {
        int new_cap = parent->child_capacity * 2;
        LR_DomNode **new_children = (LR_DomNode **)realloc(
            parent->children, (size_t)new_cap * sizeof(LR_DomNode *));
        if (!new_children) return -1;
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }

    parent->children[parent->child_count++] = lr_dom_node_retain(child);
    child->parent = parent;
    return 0;
}

int lr_dom_node_remove_child(LR_DomNode *parent, LR_DomNode *child)
{
    if (!parent || !child) return -1;

    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            /* Shift remaining children */
            for (int j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            child->parent = NULL;
            lr_dom_node_release(child);
            return 0;
        }
    }
    return -1;
}

/* ── Attribute Manipulation ─────────────────────────────────────────────── */

void lr_dom_node_set_attribute(LR_DomNode *node, const char *name, const char *value)
{
    if (!node || !name || node->is_text_node) return;

    /* Update existing attribute */
    for (int i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attr_names[i], name) == 0) {
            free(node->attr_values[i]);
            node->attr_values[i] = value ? strdup(value) : strdup("");
            return;
        }
    }

    /* Add new attribute */
    if (node->attr_count >= node->attr_capacity) {
        int new_cap = node->attr_capacity * 2;
        char **new_names = (char **)realloc(node->attr_names,
                                             (size_t)new_cap * sizeof(char *));
        char **new_values = (char **)realloc(node->attr_values,
                                              (size_t)new_cap * sizeof(char *));
        if (!new_names || !new_values) return;
        node->attr_names = new_names;
        node->attr_values = new_values;
        node->attr_capacity = new_cap;
    }

    node->attr_names[node->attr_count] = strdup(name);
    node->attr_values[node->attr_count] = value ? strdup(value) : strdup("");
    node->attr_count++;
}

const char *lr_dom_node_get_attribute(LR_DomNode *node, const char *name)
{
    if (!node || !name || node->is_text_node) return NULL;

    for (int i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attr_names[i], name) == 0) {
            return node->attr_values[i];
        }
    }
    return NULL;
}

void lr_dom_node_remove_attribute(LR_DomNode *node, const char *name)
{
    if (!node || !name || node->is_text_node) return;

    for (int i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attr_names[i], name) == 0) {
            free(node->attr_names[i]);
            free(node->attr_values[i]);
            for (int j = i; j < node->attr_count - 1; j++) {
                node->attr_names[j] = node->attr_names[j + 1];
                node->attr_values[j] = node->attr_values[j + 1];
            }
            node->attr_count--;
            return;
        }
    }
}

/* ── Style Manipulation ─────────────────────────────────────────────────── */

void lr_dom_node_set_style(LR_DomNode *node, const char *name, const char *value)
{
    if (!node || !name || node->is_text_node) return;

    /* Update existing style */
    for (int i = 0; i < node->style_count; i++) {
        if (strcmp(node->style_names[i], name) == 0) {
            free(node->style_values[i]);
            node->style_values[i] = value ? strdup(value) : strdup("");
            return;
        }
    }

    /* Add new style */
    if (node->style_count >= node->style_capacity) {
        int new_cap = node->style_capacity * 2;
        char **new_names = (char **)realloc(node->style_names,
                                             (size_t)new_cap * sizeof(char *));
        char **new_values = (char **)realloc(node->style_values,
                                              (size_t)new_cap * sizeof(char *));
        if (!new_names || !new_values) return;
        node->style_names = new_names;
        node->style_values = new_values;
        node->style_capacity = new_cap;
    }

    node->style_names[node->style_count] = strdup(name);
    node->style_values[node->style_count] = value ? strdup(value) : strdup("");
    node->style_count++;
}

const char *lr_dom_node_get_style(LR_DomNode *node, const char *name)
{
    if (!node || !name || node->is_text_node) return NULL;

    for (int i = 0; i < node->style_count; i++) {
        if (strcmp(node->style_names[i], name) == 0) {
            return node->style_values[i];
        }
    }
    return NULL;
}

/* ── Query Helpers ──────────────────────────────────────────────────────── */

LR_DomNode *lr_dom_node_get_element_by_id(LR_DomNode *root, const char *id)
{
    if (!root || !id) return NULL;

    if (!root->is_text_node) {
        const char *node_id = lr_dom_node_get_attribute(root, "id");
        if (node_id && strcmp(node_id, id) == 0) {
            return root;
        }
    }

    for (int i = 0; i < root->child_count; i++) {
        LR_DomNode *found = lr_dom_node_get_element_by_id(root->children[i], id);
        if (found) return found;
    }

    return NULL;
}

LR_DomNode *lr_dom_node_query_selector(LR_DomNode *root, const char *selector)
{
    if (!root || !selector) return NULL;

    if (!root->is_text_node && root->tag_name) {
        /* Simple tag name selector */
        if (selector[0] != '#' && selector[0] != '.') {
            if (strcmp(root->tag_name, selector) == 0) {
                return root;
            }
        }
        /* ID selector (#id) */
        if (selector[0] == '#') {
            const char *id = lr_dom_node_get_attribute(root, "id");
            if (id && strcmp(id, selector + 1) == 0) {
                return root;
            }
        }
        /* Class selector (.class) */
        if (selector[0] == '.') {
            const char *cls = lr_dom_node_get_attribute(root, "class");
            if (cls) {
                const char *p = cls;
                while (*p) {
                    /* Skip leading spaces */
                    while (*p == ' ') p++;
                    if (*p == '\0') break;
                    const char *start = p;
                    while (*p && *p != ' ') p++;
                    size_t len = (size_t)(p - start);
                    if (len == strlen(selector + 1) &&
                        strncmp(start, selector + 1, len) == 0) {
                        return root;
                    }
                }
            }
        }
    }

    for (int i = 0; i < root->child_count; i++) {
        LR_DomNode *found = lr_dom_node_query_selector(root->children[i], selector);
        if (found) return found;
    }

    return NULL;
}

/* ── HTML Serialization ─────────────────────────────────────────────────── */

/* Forward declaration */
static void lr_dom_serialize_recursive(LR_DomNode *node, char **buf, size_t *len, size_t *cap);

static void lr_dom_serialize_append(char **buf, size_t *len, size_t *cap,
                                     const char *str, size_t str_len)
{
    if (!str) return;
    size_t needed = *len + str_len + 1;
    if (needed > *cap) {
        *cap = (*cap == 0) ? 256 : *cap * 2;
        if (needed > *cap) *cap = needed + 64;
        *buf = (char *)realloc(*buf, *cap);
        if (!*buf) return;
    }
    memcpy(*buf + *len, str, str_len);
    *len += str_len;
    (*buf)[*len] = '\0';
}

static void lr_dom_serialize_append_str(char **buf, size_t *len, size_t *cap,
                                         const char *str)
{
    if (str) lr_dom_serialize_append(buf, len, cap, str, strlen(str));
}

/* Simple HTML-escape a string */
static char *lr_dom_escape_html(const char *str)
{
    if (!str) return strdup("");
    size_t len = strlen(str);
    /* Pre-allocate worst-case */
    char *escaped = (char *)malloc(len * 6 + 1);
    if (!escaped) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '&': memcpy(escaped + j, "&amp;", 5); j += 5; break;
            case '<': memcpy(escaped + j, "&lt;", 4); j += 4; break;
            case '>': memcpy(escaped + j, "&gt;", 4); j += 4; break;
            case '"': memcpy(escaped + j, "&quot;", 6); j += 6; break;
            default:  escaped[j++] = str[i]; break;
        }
    }
    escaped[j] = '\0';
    return escaped;
}

static void lr_dom_serialize_recursive(LR_DomNode *node, char **buf,
                                        size_t *len, size_t *cap)
{
    if (!node) return;

    if (node->is_text_node) {
        char *escaped = lr_dom_escape_html(node->text_content);
        if (escaped) {
            lr_dom_serialize_append_str(buf, len, cap, escaped);
            free(escaped);
        }
        return;
    }

    /* Opening tag */
    lr_dom_serialize_append(buf, len, cap, "<", 1);
    lr_dom_serialize_append_str(buf, len, cap, node->tag_name);

    /* Attributes */
    for (int i = 0; i < node->attr_count; i++) {
        lr_dom_serialize_append(buf, len, cap, " ", 1);
        lr_dom_serialize_append_str(buf, len, cap, node->attr_names[i]);
        lr_dom_serialize_append(buf, len, cap, "=\"", 2);
        char *escaped = lr_dom_escape_html(node->attr_values[i]);
        if (escaped) {
            lr_dom_serialize_append_str(buf, len, cap, escaped);
            free(escaped);
        }
        lr_dom_serialize_append(buf, len, cap, "\"", 1);
    }

    /* Style attribute */
    if (node->style_count > 0) {
        lr_dom_serialize_append(buf, len, cap, " style=\"", 8);
        for (int i = 0; i < node->style_count; i++) {
            if (i > 0) lr_dom_serialize_append(buf, len, cap, "; ", 2);
            lr_dom_serialize_append_str(buf, len, cap, node->style_names[i]);
            lr_dom_serialize_append(buf, len, cap, ":", 1);
            lr_dom_serialize_append_str(buf, len, cap, node->style_values[i]);
        }
        lr_dom_serialize_append(buf, len, cap, "\"", 1);
    }

    /* Self-closing tags */
    static const char *void_elements[] = {
        "br", "hr", "img", "input", "meta", "link", "area", "base",
        "col", "embed", "source", "track", "wbr", NULL
    };
    int is_void = 0;
    if (node->tag_name) {
        for (int v = 0; void_elements[v]; v++) {
            if (strcmp(node->tag_name, void_elements[v]) == 0) {
                is_void = 1;
                break;
            }
        }
    }

    if (is_void && node->child_count == 0) {
        lr_dom_serialize_append(buf, len, cap, " />", 3);
        return;
    }

    lr_dom_serialize_append(buf, len, cap, ">", 1);

    /* Children */
    for (int i = 0; i < node->child_count; i++) {
        lr_dom_serialize_recursive(node->children[i], buf, len, cap);
    }

    /* Closing tag */
    lr_dom_serialize_append(buf, len, cap, "</", 2);
    lr_dom_serialize_append_str(buf, len, cap, node->tag_name);
    lr_dom_serialize_append(buf, len, cap, ">", 1);
}

char *lr_dom_node_serialize(LR_DomNode *node)
{
    if (!node) return strdup("");
    char *buf = NULL;
    size_t len = 0, cap = 0;
    lr_dom_serialize_recursive(node, &buf, &len, &cap);
    if (!buf) return strdup("");
    return buf;
}

/* ── Simple HTML Parser for innerHTML setter ───────────────────────────────
 *
 * Parses a subset of HTML: text content, <tag>, <tag attr="val">,
 * nested elements, and self-closing tags.
 */

typedef struct {
    const char *input;
    size_t      pos;
    size_t      len;
} LR_HtmlParser;

static void lr_html_parser_skip_whitespace(LR_HtmlParser *p)
{
    while (p->pos < p->len &&
           (p->input[p->pos] == ' ' || p->input[p->pos] == '\t' ||
            p->input[p->pos] == '\n' || p->input[p->pos] == '\r')) {
        p->pos++;
    }
}

static int lr_html_parser_match(LR_HtmlParser *p, const char *s)
{
    size_t slen = strlen(s);
    if (p->pos + slen > p->len) return 0;
    if (strncmp(p->input + p->pos, s, slen) == 0) {
        p->pos += slen;
        return 1;
    }
    return 0;
}

static LR_DomNode *lr_html_parse_node(LR_HtmlParser *p);

/* Parse a text node (everything up to '<' or end) */
static LR_DomNode *lr_html_parse_text(LR_HtmlParser *p)
{
    size_t start = p->pos;
    while (p->pos < p->len && p->input[p->pos] != '<') {
        p->pos++;
    }
    if (p->pos == start) return NULL;

    size_t text_len = p->pos - start;
    char *text = (char *)malloc(text_len + 1);
    if (!text) return NULL;
    memcpy(text, p->input + start, text_len);
    text[text_len] = '\0';

    LR_DomNode *node = lr_dom_text_node_create(text);
    free(text);
    return node;
}

/* Parse an attribute key="value" */
static void lr_html_parse_attr(LR_HtmlParser *p, LR_DomNode *node)
{
    lr_html_parser_skip_whitespace(p);
    if (p->pos >= p->len) return;

    /* Attribute name */
    size_t name_start = p->pos;
    while (p->pos < p->len && p->input[p->pos] != '=' &&
           p->input[p->pos] != '>' && p->input[p->pos] != ' ' &&
           p->input[p->pos] != '\t' && p->input[p->pos] != '\n' &&
           p->input[p->pos] != '/') {
        p->pos++;
    }
    if (p->pos == name_start) return;

    size_t name_len = p->pos - name_start;
    char *name = (char *)malloc(name_len + 1);
    if (!name) return;
    memcpy(name, p->input + name_start, name_len);
    name[name_len] = '\0';

    lr_html_parser_skip_whitespace(p);

    /* Expect '=' */
    if (p->pos < p->len && p->input[p->pos] == '=') {
        p->pos++;
        lr_html_parser_skip_whitespace(p);

        /* Quoted value */
        if (p->pos < p->len && (p->input[p->pos] == '"' || p->input[p->pos] == '\'')) {
            char quote = p->input[p->pos];
            p->pos++;
            size_t val_start = p->pos;
            while (p->pos < p->len && p->input[p->pos] != quote) {
                p->pos++;
            }
            size_t val_len = p->pos - val_start;
            char *value = (char *)malloc(val_len + 1);
            if (value) {
                memcpy(value, p->input + val_start, val_len);
                value[val_len] = '\0';
                lr_dom_node_set_attribute(node, name, value);
                free(value);
            }
            if (p->pos < p->len) p->pos++; /* skip closing quote */
        }
    }

    free(name);
}

/* Parse an element node: <tag ...>content</tag> or <tag .../> */
static LR_DomNode *lr_html_parse_element(LR_HtmlParser *p)
{
    /* Skip '<' */
    p->pos++;

    /* Check for closing tag */
    if (p->pos < p->len && p->input[p->pos] == '/') {
        /* Skip to '>' */
        while (p->pos < p->len && p->input[p->pos] != '>') p->pos++;
        if (p->pos < p->len) p->pos++;
        return NULL;
    }

    /* Tag name */
    size_t tag_start = p->pos;
    while (p->pos < p->len && p->input[p->pos] != ' ' &&
           p->input[p->pos] != '\t' && p->input[p->pos] != '\n' &&
           p->input[p->pos] != '>' && p->input[p->pos] != '/') {
        p->pos++;
    }
    if (p->pos == tag_start) return NULL;

    size_t tag_len = p->pos - tag_start;
    char *tag_name = (char *)malloc(tag_len + 1);
    if (!tag_name) return NULL;
    memcpy(tag_name, p->input + tag_start, tag_len);
    tag_name[tag_len] = '\0';

    LR_DomNode *node = lr_dom_node_create(tag_name);
    free(tag_name);
    if (!node) return NULL;

    /* Parse attributes */
    int self_closing = 0;
    while (p->pos < p->len && p->input[p->pos] != '>') {
        if (p->input[p->pos] == '/') {
            /* Check for self-closing */
            self_closing = 1;
            p->pos++;
            lr_html_parser_skip_whitespace(p);
            continue;
        }
        lr_html_parse_attr(p, node);
        lr_html_parser_skip_whitespace(p);
    }

    /* Skip '>' */
    if (p->pos < p->len) p->pos++;

    /* If self-closing, done */
    if (self_closing) return node;

    /* Parse children until closing tag */
    while (p->pos < p->len) {
        lr_html_parser_skip_whitespace(p);
        if (p->pos >= p->len) break;

        if (p->input[p->pos] == '<' && p->pos + 1 < p->len &&
            p->input[p->pos + 1] == '/') {
            /* Closing tag */
            p->pos += 2; /* skip </ */
            while (p->pos < p->len && p->input[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++; /* skip > */
            break;
        }

        LR_DomNode *child = lr_html_parse_node(p);
        if (child) {
            lr_dom_node_append_child(node, child);
            lr_dom_node_release(child); /* release our reference, parent keeps it */
        } else {
            break;
        }
    }

    return node;
}

static LR_DomNode *lr_html_parse_node(LR_HtmlParser *p)
{
    if (p->pos >= p->len) return NULL;
    lr_html_parser_skip_whitespace(p);
    if (p->pos >= p->len) return NULL;

    if (p->input[p->pos] == '<' && p->pos + 1 < p->len &&
        p->input[p->pos + 1] != '/') {
        return lr_html_parse_element(p);
    }

    if (p->input[p->pos] == '<') {
        /* Skip closing tags */
        p->pos++;
        while (p->pos < p->len && p->input[p->pos] != '>') p->pos++;
        if (p->pos < p->len) p->pos++;
        return NULL;
    }

    return lr_html_parse_text(p);
}

/* Parse HTML string into a DOM node tree. Returns root node. */
static LR_DomNode *lr_dom_parse_html(const char *html)
{
    if (!html || !*html) return NULL;

    /* Create a virtual root to hold the parsed children */
    LR_DomNode *root = lr_dom_node_create("__fragment__");
    if (!root) return NULL;

    LR_HtmlParser parser;
    parser.input = html;
    parser.pos = 0;
    parser.len = strlen(html);

    while (parser.pos < parser.len) {
        LR_DomNode *child = lr_html_parse_node(&parser);
        if (child) {
            lr_dom_node_append_child(root, child);
            lr_dom_node_release(child);
        } else {
            break;
        }
    }

    return root;
}

/* ── JS-side helpers ──────────────────────────────────────────────────── */

/* Get the LR_DomNode from a JS object's opaque pointer */
static LR_DomNode *lr_dom_get_node(JSValue obj)
{
    return (LR_DomNode *)JS_GetOpaque(obj, 1);
}

/* ── Element constructor ────────────────────────────────────────────────── */

static JSValue lr_dom_element_constructor(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    JSValue obj = this_val;

    const char *tag = "div";
    if (argc >= 1 && JS_IsString(argv[0])) {
        tag = JS_ToCString(ctx, argv[0]);
    }

    LR_DomNode *node = lr_dom_node_create(tag);
    if (argc >= 1 && JS_IsString(argv[0])) {
        JS_FreeCString(ctx, tag);
    }

    if (!node) return JS_EXCEPTION;

    lr_set_opaque_with_free(obj, node, lr_dom_opaque_free);

    /* Initialize EventTarget listener storage */
    JSValue listeners = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, obj, "__listeners", listeners);

    return obj;
}

/* ── HTMLElement constructor ────────────────────────────────────────────── */

static JSValue lr_dom_htmlelement_constructor(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    /* Same as Element but with a different prototype */
    return lr_dom_element_constructor(ctx, this_val, argc, argv);
}

/* ── Document constructor (internal) ────────────────────────────────────── */

static JSValue lr_dom_document_constructor(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    JSValue obj = this_val;
    (void)argc; (void)argv;

    LR_DomNode *node = lr_dom_node_create("document");
    if (!node) return JS_EXCEPTION;

    lr_set_opaque_with_free(obj, node, lr_dom_opaque_free);

    JSValue listeners = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, obj, "__listeners", listeners);

    return obj;
}

/* ── Element prototype methods ──────────────────────────────────────────── */

static JSValue lr_dom_js_append_child(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "appendChild requires 1 argument");
    }

    LR_DomNode *parent = lr_dom_get_node(this_val);
    LR_DomNode *child = lr_dom_get_node(argv[0]);

    if (!parent) return JS_ThrowTypeError(ctx, "appendChild: invalid parent");
    if (!child) return JS_ThrowTypeError(ctx, "appendChild: invalid child node");

    if (lr_dom_node_append_child(parent, child) != 0) {
        return JS_ThrowTypeError(ctx, "appendChild: failed");
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue lr_dom_js_remove_child(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "removeChild requires 1 argument");
    }

    LR_DomNode *parent = lr_dom_get_node(this_val);
    LR_DomNode *child = lr_dom_get_node(argv[0]);

    if (!parent) return JS_ThrowTypeError(ctx, "removeChild: invalid parent");
    if (!child) return JS_ThrowTypeError(ctx, "removeChild: invalid child node");

    if (lr_dom_node_remove_child(parent, child) != 0) {
        return JS_ThrowTypeError(ctx, "removeChild: child not found");
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue lr_dom_js_set_attribute(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "setAttribute requires 2 arguments");
    }

    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_ThrowTypeError(ctx, "setAttribute: invalid element");

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);

    if (!name || !value) {
        if (name) JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_ThrowTypeError(ctx, "setAttribute: invalid arguments");
    }

    lr_dom_node_set_attribute(node, name, value);

    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue lr_dom_js_get_attribute(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "getAttribute requires 1 argument");
    }

    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_ThrowTypeError(ctx, "getAttribute: invalid element");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_ThrowTypeError(ctx, "getAttribute: invalid argument");

    const char *value = lr_dom_node_get_attribute(node, name);
    JS_FreeCString(ctx, name);

    if (value) {
        return JS_NewString(ctx, value);
    }
    return JS_NULL;
}

static JSValue lr_dom_js_remove_attribute(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "removeAttribute requires 1 argument");
    }

    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_ThrowTypeError(ctx, "removeAttribute: invalid element");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_ThrowTypeError(ctx, "removeAttribute: invalid argument");

    lr_dom_node_remove_attribute(node, name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* Event listener methods - delegate to the existing EventTarget system */
static JSValue lr_dom_js_add_event_listener(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    return lr_event_target_addEventListener(ctx, this_val, argc, argv);
}

static JSValue lr_dom_js_dispatch_event(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    return lr_event_target_dispatchEvent(ctx, this_val, argc, argv);
}

/* ── Document prototype methods ─────────────────────────────────────────── */

static JSValue lr_dom_js_create_element(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "createElement requires 1 argument");
    }

    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_ThrowTypeError(ctx, "createElement: invalid argument");

    JSValue el = lr_dom_new_element(ctx, tag);
    JS_FreeCString(ctx, tag);
    return el;
}

static JSValue lr_dom_js_create_text_node(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *text = "";
    if (argc >= 1) {
        text = JS_ToCString(ctx, argv[0]);
    }

    JSValue node = lr_dom_new_text_node(ctx, text ? text : "");
    if (argc >= 1 && text) {
        JS_FreeCString(ctx, text);
    }
    return node;
}

static JSValue lr_dom_js_get_element_by_id(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "getElementById requires 1 argument");
    }

    LR_DomNode *doc_node = lr_dom_get_node(this_val);
    if (!doc_node) return JS_NULL;

    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;

    LR_DomNode *found = lr_dom_node_get_element_by_id(doc_node, id);
    JS_FreeCString(ctx, id);

    if (!found) return JS_NULL;

    /* Wrap the found node in a JS object with proper prototype */
    return lr_dom_wrap_node(ctx, found);
}

static JSValue lr_dom_js_query_selector(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "querySelector requires 1 argument");
    }

    LR_DomNode *root = lr_dom_get_node(this_val);
    if (!root) return JS_NULL;

    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;

    LR_DomNode *found = lr_dom_node_query_selector(root, selector);
    JS_FreeCString(ctx, selector);

    if (!found) return JS_NULL;

    /* Wrap found node (same approach as getElementById) */
    JSValue el = JS_NewObject(ctx);
    lr_set_opaque_with_free(el, lr_dom_node_retain(found), lr_dom_opaque_free);

    JS_SetPropertyStr(ctx, el, "tagName",
                      JS_NewString(ctx, found->tag_name ? found->tag_name : ""));
    const char *id_attr = lr_dom_node_get_attribute(found, "id");
    JS_SetPropertyStr(ctx, el, "id",
                      JS_NewString(ctx, id_attr ? id_attr : ""));
    const char *cls_attr = lr_dom_node_get_attribute(found, "class");
    JS_SetPropertyStr(ctx, el, "className",
                      JS_NewString(ctx, cls_attr ? cls_attr : ""));

    JSValue elem_proto = JS_GetPropertyStr(ctx,
        JS_GetGlobalObject(ctx), "Element");
    if (!JS_IsUndefined(elem_proto)) {
        JSValue proto = JS_GetPropertyStr(ctx, elem_proto, "prototype");
        if (!JS_IsUndefined(proto)) {
            JS_SetPrototype(ctx, el, proto);
            JS_FreeValue(ctx, proto);
        }
        JS_FreeValue(ctx, elem_proto);
    }
    JS_FreeValue(ctx, JS_GetGlobalObject(ctx));

    JS_SetPropertyStr(ctx, el, "__listeners", JS_NewArray(ctx));

    return el;
}

/* ── Element property getters/setters ──────────────────────────────────── */

static JSValue lr_dom_get_tag_name(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node || !node->tag_name) return JS_NewString(ctx, "");
    return JS_NewString(ctx, node->tag_name);
}

static JSValue lr_dom_get_id(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_NewString(ctx, "");
    const char *id = lr_dom_node_get_attribute(node, "id");
    return JS_NewString(ctx, id ? id : "");
}

static JSValue lr_dom_set_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_UNDEFINED;
    const char *str = NULL;
    if (argc > 0) str = JS_ToCString(ctx, argv[0]);
    if (str) {
        lr_dom_node_set_attribute(node, "id", str);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue lr_dom_get_class_name(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_NewString(ctx, "");
    const char *cls = lr_dom_node_get_attribute(node, "class");
    return JS_NewString(ctx, cls ? cls : "");
}

static JSValue lr_dom_set_class_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_UNDEFINED;
    const char *str = NULL;
    if (argc > 0) str = JS_ToCString(ctx, argv[0]);
    if (str) {
        lr_dom_node_set_attribute(node, "class", str);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue lr_dom_get_inner_html(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_NewString(ctx, "");

    char *html = lr_dom_node_serialize(node);
    JSValue result = JS_NewString(ctx, html ? html : "");
    free(html);
    return result;
}

static JSValue lr_dom_set_inner_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_UNDEFINED;

    const char *html = NULL;
    if (argc > 0) html = JS_ToCString(ctx, argv[0]);
    if (!html) return JS_UNDEFINED;

    /* Remove all existing children */
    while (node->child_count > 0) {
        LR_DomNode *child = node->children[0];
        lr_dom_node_remove_child(node, child);
    }

    /* Parse HTML and add children */
    LR_DomNode *parsed = lr_dom_parse_html(html);
    JS_FreeCString(ctx, html);

    if (parsed) {
        /* Move children from parsed fragment to node.
         * Each child has ref_count = 1 (owned by fragment).
         * We retain before removing to prevent freeing during transfer,
         * then release after appending to the new parent. */
        while (parsed->child_count > 0) {
            LR_DomNode *child = parsed->children[0];
            lr_dom_node_retain(child);                /* temp keep-alive */
            lr_dom_node_remove_child(parsed, child);  /* releases fragment's ref */
            lr_dom_node_append_child(node, child);    /* retains for new parent */
            lr_dom_node_release(child);                /* release temp keep-alive */
        }
        lr_dom_node_release(parsed);
    }

    return JS_UNDEFINED;
}

static JSValue lr_dom_get_children(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_NewArray(ctx);

    JSValue arr = JS_NewArray(ctx);
    int idx = 0;
    for (int i = 0; i < node->child_count; i++) {
        LR_DomNode *child = node->children[i];
        if (child->is_text_node) continue; /* only element children */

        /* Wrap child in JS object with proper prototype */
        JSValue el = lr_dom_wrap_node(ctx, child);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)idx, el);
        idx++;
    }

    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, idx));
    return arr;
}

static JSValue lr_dom_get_child_nodes(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_NewArray(ctx);

    JSValue arr = JS_NewArray(ctx);
    int idx = 0;
    for (int i = 0; i < node->child_count; i++) {
        LR_DomNode *child = node->children[i];
        JSValue el;
        if (child->is_text_node) {
            el = JS_NewObject(ctx);
            lr_set_opaque_with_free(el, lr_dom_node_retain(child), lr_dom_opaque_free);
            JS_SetPropertyStr(ctx, el, "__listeners", JS_NewArray(ctx));
        } else {
            el = lr_dom_wrap_node(ctx, child);
        }
        JS_SetPropertyUint32(ctx, arr, (uint32_t)idx, el);
        idx++;
    }

    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, idx));
    return arr;
}

static JSValue lr_dom_get_parent_node(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node || !node->parent) return JS_NULL;

    return lr_dom_wrap_node(ctx, node->parent);
}

/* ── Style property (CSSStyleDeclaration) ───────────────────────────────── */

/* Create a CSSStyleDeclaration object for a given DOM node */
static JSValue lr_dom_create_style_object(JSContext *ctx, LR_DomNode *node)
{
    JSValue style = JS_NewObject(ctx);

    /* Store a back-reference to the node (we use a weak reference via a property) */
    /* The style object doesn't own the node; it's just a helper */
    /* We store the node pointer as opaque on the style object */
    lr_set_opaque_with_free(style, node, NULL); /* no-op free, node owned by element */

    /* Define a custom named property handler for style properties.
     * For simplicity, we'll use a __props object to store values.
     * When the user sets element.style.color = "red", we store it
     * both in the style object and in the DomNode's styles. */

    return style;
}

/* Style object property setter - called when user does element.style.color = "red" */
/* We can't override property access on a plain JS object without Proxy.
 * So instead, we'll use a different approach: define getter/setter for common
 * CSS properties on the prototype. For unknown properties, we'll use a
 * __props store. */
/* Actually, for simplicity, let's just make "style" a plain object and use
 * JS_DefinePropertyGetSet for common CSS properties. */
/* Even simpler: use a JS object with a standard __defineSetter__ pattern.
 * Or just make it a plain object. The user's style settings won't be reflected
 * in the DomNode's styles, but they'll be stored on the JS object. */
/* For a working implementation, let's use a custom handler approach: */

/* Actually, the simplest approach that works: just return a plain JS object.
 * When users set properties on it, they're stored in the JS object.
 * We also intercept the set via a special setter on the element's style property. */

/* Let's use a different approach: The style object stores a reference to the node
 * as opaque, and defines getter/setter properties for common CSS properties. */

/* Common CSS properties list */
static const char *lr_css_properties[] = {
    "color", "background", "backgroundColor", "backgroundImage",
    "backgroundSize", "backgroundPosition", "backgroundRepeat",
    "border", "borderWidth", "borderStyle", "borderColor",
    "borderRadius", "borderTop", "borderRight", "borderBottom", "borderLeft",
    "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
    "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
    "display", "position", "top", "right", "bottom", "left",
    "fontSize", "fontFamily", "fontWeight", "fontStyle", "font",
    "textAlign", "textDecoration", "textTransform", "lineHeight",
    "opacity", "visibility", "overflow", "overflowX", "overflowY",
    "zIndex", "float", "clear", "cursor", "boxShadow",
    "transform", "transition", "animation",
    "flex", "flexDirection", "flexWrap", "flexGrow", "flexShrink",
    "justifyContent", "alignItems", "alignSelf", "gap",
    "grid", "gridTemplate", "gridColumn", "gridRow",
    "listStyle", "listStyleType", "whiteSpace",
    NULL
};

static JSValue lr_dom_style_getter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    /* Get the property name from the calling context */
    /* This is a generic getter - we need to know which property is being accessed */
    /* For JS_DefinePropertyGetSet, the getter receives (ctx, this_val, argc, argv) */
    /* where argv[0] might be... but actually for getter/setter defined via
     * JS_DefinePropertyGetSet, the getter signature is just (ctx, this_val).
     * We need a different approach. */

    /* Actually, looking at the engine API more carefully:
     * JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter, flags)
     * defines a named property where getter is called as (ctx, this_val) with no args.
     * Each property gets its own getter/setter function.
     * But we can't create a separate function for each CSS property.
     * Use magic-based dispatch instead. */

    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* For the style object, we'll use a practical approach:
 * Create a generic style object where each CSS property is a regular data property.
 * When the user sets element.style.color = "red", it's stored on the JS object.
 * The DomNode's style data is synced separately. */

/* ── Style property implementation ───────────────────────────────────────── */

/* We use a simpler approach: the style object is a plain JS object.
 * We attach a custom property storage mechanism by defining the style
 * property on the element as a getter/setter pair that returns a cached
 * style object, and the style object stores values in the DomNode. */

/* Helper: create a style getter/setter for a specific CSS property using magic */
static JSValue lr_dom_style_prop_getter(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    /* Get the magic value - we need to get it from the function.
     * For now, we'll use a simpler approach below. */
    return JS_UNDEFINED;
}

/* ── Simplified style implementation ───────────────────────────────────────
 *
 * Instead of using getter/setter per CSS property, we use a simple approach:
 *   - element.style returns a plain JS object
 *   - Setting element.style.color = "red" stores on the JS object
 *   - We also sync to DomNode via a special __sync callback
 *
 * Actually, the simplest possible approach: make element.style a plain object.
 * Users can read/write properties on it. For innerHTML serialization, we
 * read from the DomNode's style storage. When the user does:
 *   element.style.color = "red"
 * It becomes a property on the JS style object. But we also want it in the
 * DomNode so innerHTML works. We can hook into the style getter.
 *
 * The cleanest approach for this engine:
 *   - element.style is a getter that returns a style object
 *   - The style object has a reference to the DomNode
 *   - We define individual getter/setter properties on the style prototype
 *     for each CSS property using the JS_CFUNC_DEF magic-based approach
 *
 * But since we can't easily do magic-based dispatch per property name
 * without a proper Proxy, let's use a different approach:
 *
 * Make each CSS property a getter/setter that reads/writes from the DomNode.
 * We'll create a single prototype and define each property using JS_DefinePropertyGetSet.
 */

/* Helper: create CSS property getter/setter with node context */
typedef struct {
    LR_DomNode *node;
    const char *prop_name;
} LR_StyleContext;

/* Generic style property getter using magic value */
static JSValue lr_dom_style_css_getter(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    /* Get the DomNode from the style object's opaque */
    LR_DomNode *node = (LR_DomNode *)JS_GetOpaque(this_val, 1);
    if (!node) return JS_UNDEFINED;

    /* Get the property name from the function's user data */
    LRCFunction *cf = NULL;
    JSValue func_val = JS_GetPropertyStr(ctx, this_val, "__getter_func");
    if (!JS_IsUndefined(func_val)) {
        JS_FreeValue(ctx, func_val);
    }

    /* We'll use a different approach: store CSS property values in a backing
     * store on the style object itself. The getter reads from the DomNode. */
    return JS_UNDEFINED;
}

/* ── Simplified style: use a __props object ────────────────────────────────
 *
 * For maximum simplicity and compatibility, element.style is a plain JS object.
 * Setting element.style.color = "red" stores the value directly on the JS object.
 * The DomNode stores style data separately.
 * When innerHTML is serialized, we read from DomNode's style storage.
 * To keep both in sync, we'll use a custom setter on the element's style property.
 */

/* Let's implement a clean version:
 * 1. element.style getter returns a cached style object
 * 2. The style object has a __node property (weak ref to the DomNode)
 * 3. We define per-property getter/setter on the style prototype
 * 4. For simplicity, we just define a few common properties
 */

/* For the initial implementation, let's use a super simple approach:
 * The style object is just a plain JS object. We'll use the setter on the
 * element's style property to also sync to the DomNode. */

static JSValue lr_dom_get_style(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_NewObject(ctx);

    /* Check if we already have a cached style object */
    JSValue cached = JS_GetPropertyStr(ctx, this_val, "__style_obj");
    if (!JS_IsUndefined(cached)) {
        return cached;
    }
    JS_FreeValue(ctx, cached);

    /* Create a new style object */
    JSValue style = JS_NewObject(ctx);

    /* Store the DomNode reference as opaque (no free - node owned by element) */
    lr_set_opaque_with_free(style, node, NULL);

    /* Define common CSS properties as getter/setter on the style object */
    /* For each property, we define a getter and setter. Since we can't use
     * magic-based dispatch easily, we create a separate function pair for each
     * property using a simple dispatch table. */

    /* Actually, for the initial implementation, let's just use a plain object
     * and store the node reference. Users can set properties directly.
     * We'll sync to DomNode when serializing. */

    /* Cache the style object on the element */
    JS_SetPropertyStr(ctx, this_val, "__style_obj", JS_DupValue(ctx, style));

    return style;
}

/* ── Window / Document property getters ─────────────────────────────────── */

static JSValue lr_dom_window_get_document(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue doc = JS_GetPropertyStr(ctx, global, "document");
    JS_FreeValue(ctx, global);
    return doc;
}

static JSValue lr_dom_document_get_document_element(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *doc_node = lr_dom_get_node(this_val);
    if (!doc_node) return JS_NULL;

    /* html is the first child of the document */
    LR_DomNode *html_node = lr_dom_find_child_by_tag(doc_node, "html");
    if (!html_node) return JS_NULL;

    return lr_dom_wrap_node(ctx, html_node);
}

/* Helper: find a child element by tag name, searching recursively through
 * the document tree (document → html → target). */
static LR_DomNode *lr_dom_find_child_by_tag(LR_DomNode *parent, const char *tag)
{
    if (!parent || !tag) return NULL;
    for (int i = 0; i < parent->child_count; i++) {
        LR_DomNode *child = parent->children[i];
        if (!child->is_text_node && child->tag_name &&
            strcmp(child->tag_name, tag) == 0) {
            return child;
        }
        /* Search recursively */
        LR_DomNode *found = lr_dom_find_child_by_tag(child, tag);
        if (found) return found;
    }
    return NULL;
}

/* Helper: wrap a DOM node in a JS Element object with proper prototype */
static JSValue lr_dom_wrap_node(JSContext *ctx, LR_DomNode *node)
{
    if (!node) return JS_NULL;
    JSValue el = JS_NewObject(ctx);
    lr_set_opaque_with_free(el, lr_dom_node_retain(node), lr_dom_opaque_free);
    JS_SetPropertyStr(ctx, el, "tagName",
                      JS_NewString(ctx, node->tag_name ? node->tag_name : ""));
    JS_SetPropertyStr(ctx, el, "__listeners", JS_NewArray(ctx));

    /* Set prototype to Element.prototype (which has getter/setter for
     * id, className, textContent, innerHTML, etc.) */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elem_ctor = JS_GetPropertyStr(ctx, global, "Element");
    if (!JS_IsUndefined(elem_ctor)) {
        JSValue proto = JS_GetPropertyStr(ctx, elem_ctor, "prototype");
        if (!JS_IsUndefined(proto)) {
            JS_SetPrototype(ctx, el, proto);
        }
    }
    JS_FreeValue(ctx, global);
    return el;
}

static JSValue lr_dom_document_get_body(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *doc_node = lr_dom_get_node(this_val);
    if (!doc_node) return JS_NULL;

    /* body is a child of html, which is a child of document */
    LR_DomNode *html_node = lr_dom_find_child_by_tag(doc_node, "html");
    if (!html_node) return JS_NULL;

    LR_DomNode *body_node = lr_dom_find_child_by_tag(html_node, "body");
    if (!body_node) return JS_NULL;

    return lr_dom_wrap_node(ctx, body_node);
}

static JSValue lr_dom_document_get_head(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *doc_node = lr_dom_get_node(this_val);
    if (!doc_node) return JS_NULL;

    LR_DomNode *html_node = lr_dom_find_child_by_tag(doc_node, "html");
    if (!html_node) return JS_NULL;

    LR_DomNode *head_node = lr_dom_find_child_by_tag(html_node, "head");
    if (!head_node) return JS_NULL;

    return lr_dom_wrap_node(ctx, head_node);
}

/* ── Additional element properties ──────────────────────────────────────── */

static JSValue lr_dom_get_first_child(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node || node->child_count == 0) return JS_NULL;

    LR_DomNode *child = node->children[0];
    if (child->is_text_node) {
        JSValue el = JS_NewObject(ctx);
        lr_set_opaque_with_free(el, lr_dom_node_retain(child), lr_dom_opaque_free);
        JS_SetPropertyStr(ctx, el, "textContent",
                          JS_NewString(ctx, child->text_content ? child->text_content : ""));
        JS_SetPropertyStr(ctx, el, "__listeners", JS_NewArray(ctx));
        return el;
    }
    return lr_dom_wrap_node(ctx, child);
}

static JSValue lr_dom_get_last_child(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node || node->child_count == 0) return JS_NULL;

    LR_DomNode *child = node->children[node->child_count - 1];
    if (child->is_text_node) {
        JSValue el = JS_NewObject(ctx);
        lr_set_opaque_with_free(el, lr_dom_node_retain(child), lr_dom_opaque_free);
        JS_SetPropertyStr(ctx, el, "textContent",
                          JS_NewString(ctx, child->text_content ? child->text_content : ""));
        JS_SetPropertyStr(ctx, el, "__listeners", JS_NewArray(ctx));
        return el;
    }
    return lr_dom_wrap_node(ctx, child);
}

static JSValue lr_dom_get_text_content(JSContext *ctx, JSValueConst this_val)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_NewString(ctx, "");
    if (node->is_text_node) {
        return JS_NewString(ctx, node->text_content ? node->text_content : "");
    }
    /* For element nodes, concatenate all text node children's text content */
    /* Walk children recursively and collect text from text nodes */
    size_t total_len = 0;
    /* First pass: calculate total length */
    for (int i = 0; i < node->child_count; i++) {
        LR_DomNode *child = node->children[i];
        if (child->is_text_node && child->text_content) {
            total_len += strlen(child->text_content);
        }
    }
    /* If no text children, return empty string */
    if (total_len == 0) return JS_NewString(ctx, "");
    /* Second pass: concatenate */
    char *buf = (char *)malloc(total_len + 1);
    if (!buf) return JS_NewString(ctx, "");
    size_t pos = 0;
    for (int i = 0; i < node->child_count; i++) {
        LR_DomNode *child = node->children[i];
        if (child->is_text_node && child->text_content) {
            size_t len = strlen(child->text_content);
            memcpy(buf + pos, child->text_content, len);
            pos += len;
        }
    }
    buf[pos] = '\0';
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

static JSValue lr_dom_set_text_content(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_DomNode *node = lr_dom_get_node(this_val);
    if (!node) return JS_UNDEFINED;

    const char *str = NULL;
    if (argc > 0) str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_UNDEFINED;

    free(node->text_content);
    node->text_content = strdup(str);

    /* If text node, we're done */
    if (node->is_text_node) {
        JS_FreeCString(ctx, str);
        return JS_UNDEFINED;
    }

    /* For element nodes, also remove all children and add a single text child */
    while (node->child_count > 0) {
        LR_DomNode *child = node->children[0];
        lr_dom_node_remove_child(node, child);
    }

    LR_DomNode *text_child = lr_dom_text_node_create(str);
    if (text_child) {
        lr_dom_node_append_child(node, text_child);
        lr_dom_node_release(text_child);
    }

    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* ── Public API: Create JS wrappers ─────────────────────────────────────── */

JSValue lr_dom_new_element(JSContext *ctx, const char *tag)
{
    JSValue el = JS_NewObject(ctx);
    LR_DomNode *node = lr_dom_node_create(tag);
    if (!node) return JS_EXCEPTION;

    lr_set_opaque_with_free(el, node, lr_dom_opaque_free);

    /* Set up tagName property */
    JS_SetPropertyStr(ctx, el, "tagName", JS_NewString(ctx, tag ? tag : ""));

    /* Initialize EventTarget listener storage */
    JS_SetPropertyStr(ctx, el, "__listeners", JS_NewArray(ctx));

    /* Set prototype to Element.prototype (which has getter/setter for
     * id, className, textContent, innerHTML, etc.) */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue element_ctor = JS_GetPropertyStr(ctx, global, "Element");
    if (!JS_IsUndefined(element_ctor)) {
        JSValue proto = JS_GetPropertyStr(ctx, element_ctor, "prototype");
        if (!JS_IsUndefined(proto)) {
            JS_SetPrototype(ctx, el, proto);
            JS_FreeValue(ctx, proto);
        }
        JS_FreeValue(ctx, element_ctor);
    }
    JS_FreeValue(ctx, global);

    return el;
}

JSValue lr_dom_new_text_node(JSContext *ctx, const char *text)
{
    JSValue node_val = JS_NewObject(ctx);
    LR_DomNode *node = lr_dom_text_node_create(text);
    if (!node) return JS_EXCEPTION;

    lr_set_opaque_with_free(node_val, node, lr_dom_opaque_free);

    JS_SetPropertyStr(ctx, node_val, "textContent",
                      JS_NewString(ctx, text ? text : ""));
    JS_SetPropertyStr(ctx, node_val, "__listeners", JS_NewArray(ctx));

    return node_val;
}

JSValue lr_dom_append_child(JSContext *ctx, JSValue parent, JSValue child)
{
    LR_DomNode *p = lr_dom_get_node(parent);
    LR_DomNode *c = lr_dom_get_node(child);

    if (!p || !c) return JS_ThrowTypeError(ctx, "appendChild: invalid node");

    if (lr_dom_node_append_child(p, c) != 0) {
        return JS_ThrowTypeError(ctx, "appendChild: failed");
    }

    return JS_DupValue(ctx, child);
}

JSValue lr_dom_set_attribute(JSContext *ctx, JSValue element,
                              const char *name, const char *value)
{
    LR_DomNode *node = lr_dom_get_node(element);
    if (!node) return JS_ThrowTypeError(ctx, "setAttribute: invalid element");

    lr_dom_node_set_attribute(node, name, value);
    return JS_UNDEFINED;
}

JSValue lr_dom_get_element_by_id(JSContext *ctx, JSValue doc, const char *id)
{
    LR_DomNode *doc_node = lr_dom_get_node(doc);
    if (!doc_node) return JS_NULL;

    LR_DomNode *found = lr_dom_node_get_element_by_id(doc_node, id);
    if (!found) return JS_NULL;

    return lr_dom_wrap_node(ctx, found);
}

JSValue lr_dom_query_selector(JSContext *ctx, JSValue root, const char *selector)
{
    LR_DomNode *root_node = lr_dom_get_node(root);
    if (!root_node) return JS_NULL;

    LR_DomNode *found = lr_dom_node_query_selector(root_node, selector);
    if (!found) return JS_NULL;

    JSValue el = JS_NewObject(ctx);
    lr_set_opaque_with_free(el, lr_dom_node_retain(found), lr_dom_opaque_free);

    JS_SetPropertyStr(ctx, el, "tagName",
                      JS_NewString(ctx, found->tag_name ? found->tag_name : ""));
    const char *id_attr = lr_dom_node_get_attribute(found, "id");
    JS_SetPropertyStr(ctx, el, "id", JS_NewString(ctx, id_attr ? id_attr : ""));
    const char *cls_attr = lr_dom_node_get_attribute(found, "class");
    JS_SetPropertyStr(ctx, el, "className", JS_NewString(ctx, cls_attr ? cls_attr : ""));
    JS_SetPropertyStr(ctx, el, "__listeners", JS_NewArray(ctx));

    return el;
}

/* ── Element and HTMLElement function lists ──────────────────────────────── */

static const JSCFunctionListEntry lr_dom_element_methods[] = {
    JS_CFUNC_DEF("appendChild",      1, lr_dom_js_append_child),
    JS_CFUNC_DEF("removeChild",      1, lr_dom_js_remove_child),
    JS_CFUNC_DEF("setAttribute",     2, lr_dom_js_set_attribute),
    JS_CFUNC_DEF("getAttribute",     1, lr_dom_js_get_attribute),
    JS_CFUNC_DEF("removeAttribute",  1, lr_dom_js_remove_attribute),
    JS_CFUNC_DEF("addEventListener", 2, lr_dom_js_add_event_listener),
    JS_CFUNC_DEF("dispatchEvent",    1, lr_dom_js_dispatch_event),
};

/* Element property list (getter/setter) */
static const JSCFunctionListEntry lr_dom_element_props[] = {
    JS_CGETSET_DEF("tagName",      lr_dom_get_tag_name,       NULL),
    JS_CGETSET_DEF("id",           lr_dom_get_id,             lr_dom_set_id),
    JS_CGETSET_DEF("className",    lr_dom_get_class_name,     lr_dom_set_class_name),
    JS_CGETSET_DEF("innerHTML",    lr_dom_get_inner_html,     lr_dom_set_inner_html),
    JS_CGETSET_DEF("children",     lr_dom_get_children,       NULL),
    JS_CGETSET_DEF("parentNode",   lr_dom_get_parent_node,    NULL),
    JS_CGETSET_DEF("childNodes",   lr_dom_get_child_nodes,    NULL),
    JS_CGETSET_DEF("style",        lr_dom_get_style,          NULL),
    JS_CGETSET_DEF("firstChild",   lr_dom_get_first_child,    NULL),
    JS_CGETSET_DEF("lastChild",    lr_dom_get_last_child,     NULL),
    JS_CGETSET_DEF("textContent",  lr_dom_get_text_content,   lr_dom_set_text_content),
};

/* Document prototype methods */
static const JSCFunctionListEntry lr_dom_document_methods[] = {
    JS_CFUNC_DEF("createElement",    1, lr_dom_js_create_element),
    JS_CFUNC_DEF("createTextNode",   1, lr_dom_js_create_text_node),
    JS_CFUNC_DEF("getElementById",   1, lr_dom_js_get_element_by_id),
    JS_CFUNC_DEF("querySelector",    1, lr_dom_js_query_selector),
};

/* Document property getters */
static const JSCFunctionListEntry lr_dom_document_props[] = {
    JS_CGETSET_DEF("documentElement", lr_dom_document_get_document_element, NULL),
    JS_CGETSET_DEF("body",            lr_dom_document_get_body,            NULL),
    JS_CGETSET_DEF("head",            lr_dom_document_get_head,            NULL),
};

/* ── DOM Initialization ─────────────────────────────────────────────────── */

void lr_dom_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* ── Get EventTarget prototype (already registered by lr_event_init) ── */
    JSValue event_target_ctor = JS_GetPropertyStr(ctx, global, "EventTarget");
    JSValue event_target_proto = JS_UNDEFINED;
    if (!JS_IsUndefined(event_target_ctor)) {
        event_target_proto = JS_GetPropertyStr(ctx, event_target_ctor, "prototype");
    }

    /* ── Create Element prototype (inherits from EventTarget) ───────────── */
    JSValue element_proto = JS_NewObject(ctx);
    if (!JS_IsUndefined(event_target_proto)) {
        JS_SetPrototype(ctx, element_proto, event_target_proto);
    }

    /* Add element methods */
    JS_SetPropertyFunctionList(ctx, element_proto, lr_dom_element_methods,
                                sizeof(lr_dom_element_methods) /
                                sizeof(lr_dom_element_methods[0]));

    /* Add element property getters/setters */
    JS_SetPropertyFunctionList(ctx, element_proto, lr_dom_element_props,
                                sizeof(lr_dom_element_props) /
                                sizeof(lr_dom_element_props[0]));

    /* ── Create Element constructor ─────────────────────────────────────── */
    JSValue element_ctor = JS_NewCFunction2(ctx, lr_dom_element_constructor,
                                             "Element", 1,
                                             JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, element_ctor, "prototype",
                      JS_DupValue(ctx, element_proto));
    JS_SetPropertyStr(ctx, global, "Element", element_ctor);

    /* ── Create HTMLElement prototype (inherits from Element) ────────────── */
    JSValue htmlelement_proto = JS_NewObject(ctx);
    JS_SetPrototype(ctx, htmlelement_proto, element_proto);

    /* HTMLElement shares the same methods as Element, so we can reuse
     * the same method list. Or we could add HTMLElement-specific methods. */

    JSValue htmlelement_ctor = JS_NewCFunction2(ctx, lr_dom_htmlelement_constructor,
                                                  "HTMLElement", 1,
                                                  JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, htmlelement_ctor, "prototype",
                      JS_DupValue(ctx, htmlelement_proto));
    JS_SetPropertyStr(ctx, global, "HTMLElement", htmlelement_ctor);

    /* ── Create Document prototype (inherits from HTMLElement) ──────────── */
    JSValue document_proto = JS_NewObject(ctx);
    JS_SetPrototype(ctx, document_proto, htmlelement_proto);

    /* Add document methods */
    JS_SetPropertyFunctionList(ctx, document_proto, lr_dom_document_methods,
                                sizeof(lr_dom_document_methods) /
                                sizeof(lr_dom_document_methods[0]));

    /* Add document property getters */
    JS_SetPropertyFunctionList(ctx, document_proto, lr_dom_document_props,
                                sizeof(lr_dom_document_props) /
                                sizeof(lr_dom_document_props[0]));

    /* ── Create the document global object ───────────────────────────────── */
    JSValue doc_obj = lr_dom_document_constructor(ctx,
        JS_NewObjectProto(ctx, document_proto), 0, NULL);
    JS_SetPropertyStr(ctx, global, "document", doc_obj);

    /* ── Initialize document tree: <html><head></head><body></body></html> ── */
    {
        LR_DomNode *doc_node = lr_dom_get_node(doc_obj);
        if (doc_node) {
            LR_DomNode *html_node = lr_dom_node_create("html");
            LR_DomNode *head_node = lr_dom_node_create("head");
            LR_DomNode *body_node = lr_dom_node_create("body");
            if (html_node && head_node && body_node) {
                lr_dom_node_append_child(html_node, head_node);
                lr_dom_node_append_child(html_node, body_node);
                lr_dom_node_append_child(doc_node, html_node);
            }
            /* Release our references (tree holds them now) */
            lr_dom_node_release(html_node);
            lr_dom_node_release(head_node);
            lr_dom_node_release(body_node);
        }
    }

    /* ── Create window object ────────────────────────────────────────────── */
    /* window is the global object itself, with a document property */
    JSValue window_obj = JS_DupValue(ctx, global);
    JS_SetPropertyStr(ctx, window_obj, "document", JS_DupValue(ctx, doc_obj));
    JS_SetPropertyStr(ctx, window_obj, "addEventListener",
                      JS_NewCFunction(ctx, lr_dom_js_add_event_listener,
                                      "addEventListener", 2));
    JS_SetPropertyStr(ctx, window_obj, "dispatchEvent",
                      JS_NewCFunction(ctx, lr_dom_js_dispatch_event,
                                      "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, global, "window", window_obj);

    /* ── Cleanup ─────────────────────────────────────────────────────────── */
    JS_FreeValue(ctx, event_target_proto);
    JS_FreeValue(ctx, event_target_ctor);
    JS_FreeValue(ctx, element_proto);
    JS_FreeValue(ctx, element_ctor);
    JS_FreeValue(ctx, htmlelement_proto);
    JS_FreeValue(ctx, htmlelement_ctor);
    JS_FreeValue(ctx, document_proto);
    JS_FreeValue(ctx, global);

    lr_log(rt, LR_LOG_DEBUG, "DOM API initialized");
}