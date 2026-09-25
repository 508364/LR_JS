/*
 * L/R_JS - IOME586 Result Cache Implementation
 *
 * See lr_iome586.h for the container layout. In short: every archive is an
 * LZ4 "compression package" (<namehash>.lrfile, named by script path so
 * edits refresh the same file; ".lrfile.lz4" also accepted on load)
 * whose payload is keyed with the source hash (the archive "password") and
 * carries named binary entries: meta / path / config / init / ast / nodes /
 * globals / state. Everything needed to restore the run is inside.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#include "lr_iome586.h"
#include "lr_compress.h"
#include "lr_runtime.h"
#include "engine/lr_engine.h"
#include "engine/lr_jit.h"

/* ── Hashing / checksums ───────────────────────────────────────────────── */

uint64_t lr_iome586_hash64(const uint8_t *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;           /* FNV-1a 64 */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint32_t fnv1a_32(const uint8_t *data, size_t len)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

uint32_t lr_iome586_crc32(const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t engine_version_id(void)
{
    return fnv1a_32((const uint8_t *)LR_JS_VERSION_STRING,
                    strlen(LR_JS_VERSION_STRING));
}

/* Payload keying: the archive password is the source hash. */
static void xor_key(uint8_t *data, size_t len, uint64_t key)
{
    uint8_t k[8];
    memcpy(k, &key, 8);
    for (size_t i = 0; i < len; i++)
        data[i] ^= k[i & 7];
}

/* ── CAS spinlock ─────────────────────────────────────────────────────────
 * User-space spin-wait using test-and-set.  No kernel involvement for the
 * short file-I/O critical sections in this module.  Backoff is a simple
 * CPU yield (rep nop / yield) — critical sections are tiny so we never
 * spin for long.                                                          */
static inline void iome_spin_lock(volatile int32_t *lock)
{
    while (LR_ATOMIC_TEST_AND_SET(lock)) {
        /* Spin with CPU yield hint to avoid wasting pipeline bandwidth */
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("rep; nop" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#elif defined(__arm__)
        __asm__ __volatile__("nop" ::: "memory");
#else
        /* Fallback: compiler barrier */
        __asm__ __volatile__("" ::: "memory");
#endif
    }
    lr_write_barrier();  /* ensure subsequent reads/writes happen after lock */
}

static inline void iome_spin_unlock(volatile int32_t *lock)
{
    lr_write_barrier();  /* ensure all writes complete before releasing */
    LR_ATOMIC_CLEAR(lock);
}

/* Convenience macros */
#define IOME_LOCK(c)   iome_spin_lock(&(c)->spinlock)
#define IOME_UNLOCK(c) iome_spin_unlock(&(c)->spinlock)

/* Atomic statistics helpers — no lock needed */
#define IOME_STAT_INC(c, field) \
    lr_atomic_fetch_add_64((volatile int64_t *)&(c)->field, 1)
#define IOME_STAT_ADD(c, field, val) \
    lr_atomic_fetch_add_64((volatile int64_t *)&(c)->field, (int64_t)(val))
#define IOME_STAT_SET(c, field, val) \
    lr_atomic_store_64((volatile int64_t *)&(c)->field, (int64_t)(val))
#define IOME_STAT_GET(c, field) \
    lr_atomic_load_64((volatile int64_t *)&(c)->field)

/* ── Small file helpers ────────────────────────────────────────────────── */

static int mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
#if LR_PLATFORM_WINDOWS
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = save;
        }
    }
#if LR_PLATFORM_WINDOWS
    return _mkdir(tmp);
#else
    return mkdir(tmp, 0755);
#endif
}

static int64_t file_mtime(const char *path)
{
    struct stat st;
    if (lr_stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

static int file_exists(const char *path)
{
    struct stat st;
    return lr_stat(path, &st) == 0;
}

static uint8_t *read_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) { free(buf); return NULL; }
    *out_len = (size_t)size;
    return buf;
}

static int write_all(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    return (wr == len) ? 0 : -1;
}

/* Replace dst with src (Windows rename() fails if dst exists). */
static int move_file(const char *src, const char *dst)
{
    remove(dst);
    return rename(src, dst);
}

/* ── Paths ─────────────────────────────────────────────────────────────── */

/* Primary path: <dir>/<namehash16>.lrfile ; the loader also accepts the
 * ".lrfile.lz4" spelling (the archive IS an LZ4 package either way). */
static void archive_path(const LR_Iome586Cache *c, uint64_t hash,
                         char *out, size_t out_size, int with_lz4_suffix)
{
    snprintf(out, out_size, "%s/%016llx.lrfile%s",
             c->cache_dir, (unsigned long long)hash,
             with_lz4_suffix ? ".lz4" : "");
}

/* Archive *name* key: stable per script so that editing the script updates
 * the same archive file in place (auto-refresh) instead of orphaning it.
 * Named sources use the path string; anonymous sources fall back to the
 * content hash. The content hash still lives inside the container header
 * (source_hash) for validation and as the payload XOR key. */
static uint64_t script_name_hash(const char *script_path,
                                 const uint8_t *src, size_t src_len)
{
    if (script_path && script_path[0] && script_path[0] != '<')
        return lr_iome586_hash64((const uint8_t *)script_path,
                                 strlen(script_path));
    return lr_iome586_hash64(src, src_len);
}

static void backup_path(const LR_Iome586Cache *c, uint64_t hash,
                        char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%016llx.lrfile.bak",
             c->cache_dir, (unsigned long long)hash);
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

void lr_iome586_init(LR_Iome586Cache *c, LR_Runtime *rt, const char *dir)
{
    memset(c, 0, sizeof(*c));
    c->runtime = rt;
    c->compression = 1;
    c->snapshot_strings = 1;   /* sensitive names are always excluded */
    c->restore_globals = 0;    /* warm re-run recomputes; restore is opt-in */
    c->spinlock = 0;
    if (dir && dir[0]) {
        c->cache_dir = strdup(dir);
        c->enabled = 1;
        mkdir_p(dir);        /* auto-provision the on-disk directory */
    }
}

void lr_iome586_destroy(LR_Iome586Cache *c)
{
    free(c->cache_dir);
    c->cache_dir = NULL;
    if (c->baseline_names) {
        for (uint32_t i = 0; i < c->baseline_count; i++)
            free(c->baseline_names[i]);
        free(c->baseline_names);
        c->baseline_names = NULL;
        c->baseline_count = 0;
    }
    /* spinlock is a plain int32, nothing to destroy */
}

/* Comparison function for sorted baseline_names (used by bsearch + qsort) */
static int baseline_name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ── Builtin baseline ("remap set") ────────────────────────────────────── */

int lr_iome586_capture_baseline(LR_Iome586Cache *c, LRContext *ctx)
{
    if (!c || !ctx) return -1;

    /* Reset a previous capture */
    if (c->baseline_names) {
        for (uint32_t i = 0; i < c->baseline_count; i++)
            free(c->baseline_names[i]);
        free(c->baseline_names);
        c->baseline_names = NULL;
        c->baseline_count = 0;
    }

    LRPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (lr_get_own_property_names(ctx, &tab, &len, ctx->global_obj,
                                  JS_GPN_STRING_MASK) != 0)
        return -1;

    c->baseline_names = (char **)calloc(len ? len : 1, sizeof(char *));
    if (!c->baseline_names) {
        lr_free_property_enum(ctx, tab, len);
        return -1;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        /* atom->str is a flexible array member, so its address is never NULL;
         * only the atom pointer itself needs checking. */
        if (!tab[i].atom) continue;
        c->baseline_names[n] = strdup(tab[i].atom->str);
        if (c->baseline_names[n]) n++;
    }
    c->baseline_count = n;
    lr_free_property_enum(ctx, tab, len);

    /* Sort for O(log n) bsearch lookup */
    if (n > 1) {
        qsort(c->baseline_names, n, sizeof(char *), baseline_name_cmp);
    }
    return (int)n;
}

/* Is this name part of the engine/BOM namespace? Prefers the dynamically
 * captured baseline; falls back to the static list when no baseline. */
static int is_builtin_global(const char *name);   /* fwd (static list) */

static int in_engine_namespace(const LR_Iome586Cache *c, const char *name)
{
    if (c && c->baseline_names && c->baseline_count) {
        return bsearch(&name, c->baseline_names, c->baseline_count,
                       sizeof(char *), baseline_name_cmp) != NULL;
    }
    return is_builtin_global(name);
}

int lr_iome586_set_dir(LR_Iome586Cache *c, const char *dir)
{
    IOME_LOCK(c);
    free(c->cache_dir);
    c->cache_dir = NULL;
    c->enabled = 0;
    if (dir && dir[0]) {
        mkdir_p(dir);
        c->cache_dir = strdup(dir);
        c->enabled = 1;
    }
    IOME_UNLOCK(c);
    return 0;
}

/* ── Entry stream (the "files" inside the package) ─────────────────────── */

typedef struct {
    uint8_t *data;
    size_t   len, cap;
    int      error;
} Blob;

static void blob_ensure(Blob *b, size_t needed)
{
    if (b->error || b->cap >= needed) return;
    size_t nc = b->cap ? b->cap : 2048;
    while (nc < needed) nc *= 2;
    uint8_t *nd = realloc(b->data, nc);
    if (!nd) { b->error = 1; return; }
    b->data = nd; b->cap = nc;
}

static void blob_put(Blob *b, const void *src, size_t n)
{
    if (b->error) return;
    blob_ensure(b, b->len + n);
    if (b->error) return;
    memcpy(b->data + b->len, src, n);
    b->len += n;
}
static void blob_u8(Blob *b, uint8_t v)   { blob_put(b, &v, 1); }
static void blob_u16(Blob *b, uint16_t v) { blob_put(b, &v, 2); }
static void blob_u32(Blob *b, uint32_t v) { blob_put(b, &v, 4); }
static void blob_i32(Blob *b, int32_t v)  { blob_put(b, &v, 4); }
static void blob_f64(Blob *b, double v)   { blob_put(b, &v, 8); }

/* entry:  u16 name_len | name | u32 data_len | data */
static void entry_add(Blob *b, const char *name, const void *data, size_t len)
{
    size_t nlen = strlen(name);
    blob_u16(b, (uint16_t)nlen);
    blob_put(b, name, nlen);
    blob_u32(b, (uint32_t)len);
    if (len) blob_put(b, data, len);
}

static void entry_add_text(Blob *b, const char *name, const char *text)
{
    entry_add(b, name, text ? text : "", text ? strlen(text) : 0);
}

/* ── Value snapshot helpers (globals / node results) ───────────────────── */

/* tags: 0 undefined, 1 null, 2 bool, 3 int32, 4 float64, 5 string,
 *       6 object/function (recorded, not restorable). */
static void blob_value(Blob *b, LRContext *ctx, LRValue v)
{
    switch (v.tag) {
    case LR_TYPE_UNDEFINED: blob_u8(b, 0); break;
    case LR_TYPE_NULL:      blob_u8(b, 1); break;
    case LR_TYPE_BOOL:      blob_u8(b, 2); blob_u8(b, v.u.bool_val ? 1 : 0); break;
    case LR_TYPE_INT32:     blob_u8(b, 3); blob_i32(b, v.u.int32); break;
    case LR_TYPE_FLOAT64:   blob_u8(b, 4); blob_f64(b, v.u.float64); break;
    case LR_TYPE_STRING: {
        const char *s = lr_to_cstring(ctx, v);
        size_t n = s ? strlen(s) : 0;
        blob_u8(b, 5);
        blob_u32(b, (uint32_t)n);
        if (n) blob_put(b, s, n);
        if (s) lr_free_cstring(ctx, s);
        break;
    }
    default:                blob_u8(b, 6); break;
    }
}

/* ── Sorted builtin global names for O(log n) lookup ──────────────────────
 * Must be kept in ASCII-sorted order for bsearch().  If you add a name,
 * insert it at the correct position so the array stays sorted.            */
static const char * const builtin_globals_sorted[] = {
    "AbortController", "AbortSignal", "AggregateError", "Array",
    "ArrayBuffer", "BigInt", "BigInt64Array", "BigUint64Array",
    "Boolean", "CustomEvent", "DataView", "Date", "Error",
    "EvalError", "Event", "EventTarget", "FinalizationRegistry",
    "Float32Array", "Float64Array", "Function", "Infinity",
    "Int16Array", "Int32Array", "Int8Array", "Intl",
    "JSON", "Map", "Math", "NaN", "Number", "Object",
    "Promise", "Proxy", "RangeError", "Reflect", "RegExp",
    "ReferenceError", "Set", "SharedArrayBuffer", "String",
    "Symbol", "SyntaxError", "TypeError", "URIError",
    "Uint16Array", "Uint32Array", "Uint8Array", "Uint8ClampedArray",
    "URL", "URLSearchParams", "WeakMap", "WeakRef", "WeakSet",
    "WebSocket", "Worker", "atob", "btoa", "clearInterval",
    "clearTimeout", "console", "crypto", "decodeURI",
    "decodeURIComponent", "encodeURI", "encodeURIComponent",
    "escape", "eval", "fetch", "fs", "globalThis",
    "isFinite", "isNaN", "localStorage", "navigator",
    "os", "parseFloat", "parseInt", "performance",
    "queueMicrotask", "sessionStorage", "setInterval",
    "setTimeout", "structuredClone", "terminal",
    "TextDecoder", "TextEncoder", "undefined", "unescape"
};
#define NUM_BUILTIN_GLOBALS \
    (sizeof(builtin_globals_sorted) / sizeof(builtin_globals_sorted[0]))

static int builtin_global_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* O(log n) binary search for builtin global lookup */
static int is_builtin_global(const char *name)
{
    return bsearch(&name, builtin_globals_sorted, NUM_BUILTIN_GLOBALS,
                   sizeof(const char *), builtin_global_cmp) != NULL;
}

/* Sensitive-looking global names must never have their VALUES persisted to
 * disk (they are still listed, as opaque tag 6). Case-insensitive substring
 * match. */
static int is_sensitive_global(const char *name)
{
    static const char *needles[] = {
        "token", "secret", "passw", "credential", "apikey", "api_key",
        "auth", "bearer", "cookie", "session", "private", NULL
    };
    char low[512];
    size_t i;
    for (i = 0; name[i] && i < sizeof(low) - 1; i++) {
        char ch = name[i];
        low[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    low[i] = '\0';
    for (int k = 0; needles[k]; k++)
        if (strstr(low, needles[k])) return 1;
    return 0;
}

/* Snapshot the global variable bindings into an entry blob.
 * format: u32 count, then per binding: u16 name_len|name|value
 * allow_strings=0 records ALL string values as opaque (tag 6).
 * Names in the engine/BOM baseline are skipped entirely. */
static void snapshot_globals(Blob *out, LRContext *ctx,
                             const LR_Iome586Cache *c, int allow_strings)
{
    LRPropertyEnum *tab = NULL;
    uint32_t len = 0, count = 0;
    Blob body = {0};

    if (lr_get_own_property_names(ctx, &tab, &len, ctx->global_obj,
                                  JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < len; i++) {
            if (!tab[i].atom) continue;
            const char *name = tab[i].atom->str;
            if (in_engine_namespace(c, name)) continue;
            LRValue v = lr_get_property(ctx, ctx->global_obj, tab[i].atom);
            if (v.tag == LR_TYPE_EXCEPTION) continue;
            size_t nlen = strlen(name);
            blob_u16(&body, (uint16_t)nlen);
            blob_put(&body, name, nlen);
            /* Never persist sensitive values; optionally no strings at all.
             * Such bindings are recorded as opaque (tag 6). */
            if (is_sensitive_global(name) ||
                (!allow_strings && v.tag == LR_TYPE_STRING))
                blob_u8(&body, 6);
            else
                blob_value(&body, ctx, v);
            lr_free_value(ctx, v);
            count++;
        }
        lr_free_property_enum(ctx, tab, len);
    }
    blob_u32(out, count);
    if (body.data) blob_put(out, body.data, body.len);
    free(body.data);
}

/* Per top-level node result snapshot (via the engine's opaque accessors).
 * format: u32 count, then per node:
 *   u16 ast_type | u32 line | u8 run_status(1=executed)
 *   u8 has_name | [u16 name_len|name|value(binding after run)] */
static void snapshot_nodes(Blob *out, LRContext *ctx, const ASTNode *program,
                           int allow_strings)
{
    int count = lr_engine_program_count(program);
    blob_u32(out, (uint32_t)count);
    for (int i = 0; i < count; i++) {
        uint16_t type = 0xFFFF;
        uint32_t line = 0;
        const char *name = NULL;
        lr_engine_program_node_info(program, i, &type, &line, &name);
        blob_u16(out, type);
        blob_u32(out, line);
        blob_u8(out, 1); /* run_status: executed */
        if (name && name[0]) {
            blob_u8(out, 1);
            size_t nlen = strlen(name);
            blob_u16(out, (uint16_t)nlen);
            blob_put(out, name, nlen);
            LRValue v = lr_get_property_str(ctx, ctx->global_obj, name);
            /* Same policy as snapshot_globals: never persist sensitive
             * values; optionally no strings at all (recorded as tag 6). */
            if (is_sensitive_global(name) ||
                (!allow_strings && v.tag == LR_TYPE_STRING))
                blob_u8(out, 6);
            else
                blob_value(out, ctx, v);
            lr_free_value(ctx, v);
        } else {
            blob_u8(out, 0);
        }
    }
}

/* ── Container writer ──────────────────────────────────────────────────── */

static int write_container(LR_Iome586Cache *c, const char *path,
                           uint32_t status, uint32_t flags,
                           const LR_Iome586Writer *w, double opt_ratio,
                           const uint8_t *payload_raw, size_t payload_raw_len)
{
    /* Compress + key the payload */
    uint32_t crc = lr_iome586_crc32(payload_raw, payload_raw_len);

    uint8_t *stored = NULL;
    size_t stored_len = 0;
    int compressed = 0;

    if (c->compression && payload_raw_len > 256) {
        stored = lr_compress_if_beneficial(payload_raw, payload_raw_len,
                                           &stored_len, &compressed, 0.90);
    }
    if (!stored) {
        stored = malloc(payload_raw_len ? payload_raw_len : 1);
        if (!stored) return -1;
        memcpy(stored, payload_raw, payload_raw_len);
        stored_len = payload_raw_len;
        compressed = 0;
    }
    if (compressed) {
        flags |= LR_IOME586_FLAG_COMPRESSED;
        IOME_STAT_ADD(c, bytes_saved, (int64_t)(payload_raw_len - stored_len));
    }
    /* No XOR keying: the old key (source_hash) lived in the same header,
     * making it obfuscation rather than encryption. Integrity is covered by
     * payload_crc32; legacy KEYED archives remain readable in the loader. */

    /* Plaintext archive comment: script name + creation time + version. */
    char desc[768];
    {
        char tbuf[64];
        time_t t = (time_t)w->created_at;
        struct tm *tmv = localtime(&t);
        if (tmv) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tmv);
        else snprintf(tbuf, sizeof(tbuf), "%lld", (long long)w->created_at);
        snprintf(desc, sizeof(desc),
                 "IOME586 archive\nscript=%s\ncreated=%s\nengine=L/R_JS v%s\n",
                 w->script, tbuf, LR_JS_VERSION_STRING);
    }
    uint32_t desc_len = (uint32_t)strlen(desc);

    Blob f = {0};
    blob_put(&f, LR_IOME586_NAME "\0", 8);              /* magic "IOME586\0" */
    blob_u32(&f, LR_IOME586_CONTAINER_VERSION);
    blob_u32(&f, engine_version_id());
    blob_u32(&f, status);
    blob_u32(&f, flags);
    blob_put(&f, &w->created_at, 8);
    blob_put(&f, &w->hash, 8);
    blob_put(&f, &w->mtime, 8);
    blob_put(&f, &w->src_size, 8);
    blob_u32(&f, (uint32_t)(opt_ratio * 1e6));
    blob_u32(&f, crc);
    blob_u32(&f, (uint32_t)stored_len);
    blob_u32(&f, (uint32_t)payload_raw_len);
    blob_u32(&f, desc_len);
    blob_put(&f, desc, desc_len);
    blob_put(&f, stored, stored_len);
    free(stored);

    int rc = -1;
    if (!f.error)
        rc = write_all(path, f.data, f.len);
    if (rc == 0) IOME_STAT_ADD(c, bytes_stored, (int64_t)f.len);
    free(f.data);
    return rc;
}

/* ── Phase 1: begin (write-while-running) ──────────────────────────────── */

int lr_iome586_begin(LR_Iome586Cache *c, const char *script_path,
                     const uint8_t *src, size_t src_len,
                     uint8_t *ast_bc, size_t ast_len,
                     uint32_t flags, int64_t parse_us,
                     LR_Iome586Writer *w)
{
    memset(w, 0, sizeof(*w));
    if (!c || !c->enabled || !c->cache_dir || !ast_bc) return -1;

    w->hash = lr_iome586_hash64(src, src_len);
    w->mtime = (script_path && script_path[0] != '<')
                 ? file_mtime(script_path) : 0;
    w->src_size = (uint64_t)src_len;
    w->created_at = (int64_t)time(NULL);
    w->flags = flags;
    w->parse_us = parse_us;
    w->ast = ast_bc;
    w->ast_len = ast_len;
    snprintf(w->script, sizeof(w->script), "%s",
             script_path ? script_path : "<anonymous>");

    /* The archive file is named by the *script* (path hash), so a changed
     * script auto-refreshes the same file. Content hash stays in w->hash. */
    uint64_t nh = script_name_hash(script_path, src, src_len);
    archive_path(c, nh, w->path_final, sizeof(w->path_final), 0);
    backup_path(c, nh, w->path_bak, sizeof(w->path_bak));

    IOME_LOCK(c);

    /* Keep the previous archive as .bak so the store can be rolled back.
     * Accept either the .lrfile or the legacy .lrfile.lz4 name. */
    char alt[4096];
    archive_path(c, nh, alt, sizeof(alt), 1);
    if (file_exists(w->path_final)) {
        if (move_file(w->path_final, w->path_bak) == 0) w->have_bak = 1;
    } else if (file_exists(alt)) {
        if (move_file(alt, w->path_bak) == 0) w->have_bak = 1;
    }

    /* Minimal WRITING container: meta + ast only. It exists on disk while
     * the script is still executing ("cache while running"). */
    Blob payload = {0};
    char meta[1024];
    snprintf(meta, sizeof(meta),
             "name=IOME586\nscript=%s\nhash=%016llx\nstatus=writing\n"
             "created=%lld\nversion=%s\ncontainer=%u\n",
             w->script, (unsigned long long)w->hash,
             (long long)w->created_at, LR_JS_VERSION_STRING,
             LR_IOME586_CONTAINER_VERSION);
    entry_add_text(&payload, "meta", meta);
    entry_add_text(&payload, "path",
                   "method=ast-treewalk-direct\nformat=LRA1\n"
                   "pipeline=lex>parse>serialize>exec\n");
    entry_add(&payload, "ast", w->ast, w->ast_len);

    int rc = -1;
    if (!payload.error)
        rc = write_container(c, w->path_final, LR_IOME586_STATUS_WRITING,
                             flags, w, 0.0, payload.data, payload.len);
    free(payload.data);
    IOME_UNLOCK(c);

    if (rc != 0) {
        /* Could not start the archive: roll the backup straight back. */
        if (w->have_bak) move_file(w->path_bak, w->path_final);
        free(w->ast);
        memset(w, 0, sizeof(*w));
        return -1;
    }
    w->active = 1;
    return 0;
}

/* ── Phase 2: commit ───────────────────────────────────────────────────── */

int lr_iome586_commit(LR_Iome586Cache *c, LR_Iome586Writer *w,
                      LRContext *ctx, const ASTNode *program,
                      int64_t exec_us)
{
    if (!w->active) return -1;
    w->active = 0;

    /* 15% rule: expected gain is the share of total run time that a warm
     * start eliminates (lex+parse+serialize). Below 15% -> do not cache. */
    double total = (double)(w->parse_us + exec_us);
    double gain = total > 0.0 ? (double)w->parse_us / total : 0.0;
    if (gain < LR_IOME586_MIN_GAIN) {
        IOME_LOCK(c);
        remove(w->path_final);
        if (w->have_bak) move_file(w->path_bak, w->path_final);
        IOME_UNLOCK(c);
        IOME_STAT_INC(c, skip_count);
        free(w->ast);
        w->ast = NULL;
        free(w->mir_data); w->mir_data = NULL; w->mir_len = 0;
        return 1;
    }

    LR_Runtime *rt = c->runtime;

    /* meta */
    char meta[1024];
    snprintf(meta, sizeof(meta),
             "name=IOME586\nscript=%s\nhash=%016llx\nstatus=archived\n"
             "created=%lld\nopt_ratio=%.6f\nversion=%s\ncontainer=%u\n",
             w->script, (unsigned long long)w->hash,
             (long long)w->created_at, gain, LR_JS_VERSION_STRING,
             LR_IOME586_CONTAINER_VERSION);

    /* config */
    char config[512];
    if (rt) {
        snprintf(config, sizeof(config),
                 "strict_mode=%d\nmemory_limit=%zu\ngc_mode=%d\n"
                 "log_level=%d\ntimeout_ms=%d\nmodule=%d\n",
                 rt->config.strict_mode, rt->config.memory_limit,
                 (int)rt->config.gc_mode, (int)rt->config.log_level,
                 rt->config.timeout_ms,
                 (w->flags & LR_IOME586_FLAG_MODULE) ? 1 : 0);
    } else {
        snprintf(config, sizeof(config), "unknown=1\n");
    }

    /* init: what the runtime set up before the script ran, and the result */
    char init_txt[512];
    {
        uint32_t nglobal = 0;
        LRPropertyEnum *tab = NULL;
        uint32_t len = 0;
        if (ctx && lr_get_own_property_names(ctx, &tab, &len, ctx->global_obj,
                                             JS_GPN_STRING_MASK) == 0) {
            nglobal = len;
            lr_free_property_enum(ctx, tab, len);
        }
        snprintf(init_txt, sizeof(init_txt),
                 "builtins=console,timers,fetch,url,encoding,event,performance,"
                 "crypto,storage,ws,worker,fs,terminal,sysinfo,promise,proxy,"
                 "reflect,map,set,core,extra\nglobal_props=%u\nresult=ok\n",
                 nglobal);
    }

    /* state: state machine + run state + timing */
    char state[512];
    snprintf(state, sizeof(state),
             "state_machine=idle\nrun_status=completed\nexception=0\n"
             "parse_us=%lld\nexec_us=%lld\ntotal_us=%lld\nopt_ratio=%.6f\n",
             (long long)w->parse_us, (long long)exec_us,
             (long long)(w->parse_us + exec_us), gain);

    /* Full payload */
    Blob payload = {0};
    entry_add_text(&payload, "meta", meta);
    entry_add_text(&payload, "path",
                   "method=ast-treewalk-direct\nformat=LRA1\n"
                   "pipeline=lex>parse>serialize>exec\n"
                   "warm=deserialize>exec\n");
    entry_add_text(&payload, "config", config);
    entry_add_text(&payload, "init", init_txt);
    entry_add(&payload, "ast", w->ast, w->ast_len);

    Blob nodes = {0};
    snapshot_nodes(&nodes, ctx, program, c->snapshot_strings);
    entry_add(&payload, "nodes", nodes.data ? (void *)nodes.data : (void *)"",
              nodes.len);
    free(nodes.data);

    Blob globals = {0};
    snapshot_globals(&globals, ctx, c, c->snapshot_strings);
    entry_add(&payload, "globals",
              globals.data ? (void *)globals.data : (void *)"", globals.len);
    free(globals.data);

    entry_add_text(&payload, "state", state);

    /* Bytecode: serialized BCProgram from the VM compiler */
    if (w->bc_data && w->bc_len > 0)
        entry_add(&payload, "bytecode", w->bc_data, w->bc_len);

    /* MIR: serialized MIR program for cross-platform codegen cache */
    if (w->mir_data && w->mir_len > 0)
        entry_add(&payload, "mir", w->mir_data, w->mir_len);

    int rc = -1;
    IOME_LOCK(c);
    if (!payload.error)
        rc = write_container(c, w->path_final, LR_IOME586_STATUS_ARCHIVED,
                             w->flags, w, gain, payload.data, payload.len);
    if (rc == 0) {
        IOME_UNLOCK(c);
        IOME_STAT_INC(c, store_count);
    } else {
        remove(w->path_final);
        if (w->have_bak) move_file(w->path_bak, w->path_final);
        IOME_UNLOCK(c);
    }

    free(payload.data);
    free(w->ast);  w->ast = NULL;
    free(w->bc_data); w->bc_data = NULL; w->bc_len = 0;
    return rc == 0 ? 0 : -1;
}

void lr_iome586_set_bytecode(LR_Iome586Writer *w, const uint8_t *data, size_t len)
{
    if (!w || !data || !len) return;
    free(w->bc_data);
    w->bc_data = (uint8_t *)malloc(len);
    if (w->bc_data) { memcpy(w->bc_data, data, len); w->bc_len = len; }
}

void lr_iome586_set_mir(LR_Iome586Writer *w, const uint8_t *data, size_t len)
{
    if (!w || !data || !len) return;
    free(w->mir_data);
    w->mir_data = (uint8_t *)malloc(len);
    if (w->mir_data) { memcpy(w->mir_data, data, len); w->mir_len = len; }
}

void lr_iome586_abort(LR_Iome586Cache *c, LR_Iome586Writer *w)
{
    if (!w->active) return;
    w->active = 0;
    IOME_LOCK(c);
    remove(w->path_final);
    int did_revert = 0;
    if (w->have_bak) {
        move_file(w->path_bak, w->path_final);   /* automatic rollback */
        did_revert = 1;
    }
    IOME_UNLOCK(c);
    if (did_revert) IOME_STAT_INC(c, revert_count);
    free(w->ast); w->ast = NULL;
    free(w->bc_data); w->bc_data = NULL; w->bc_len = 0;
    free(w->mir_data); w->mir_data = NULL; w->mir_len = 0;
}

/* ── Load ──────────────────────────────────────────────────────────────── */

static char *dup_text(const uint8_t *p, size_t n)
{
    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

int lr_iome586_load(LR_Iome586Cache *c, const char *script_path,
                    const uint8_t *src, size_t src_len,
                    LR_Iome586Manifest *mf)
{
    memset(mf, 0, sizeof(*mf));
    if (!c || !c->enabled || !c->cache_dir) return -1;

    uint64_t hash = lr_iome586_hash64(src, src_len);      /* content hash */
    uint64_t nh = script_name_hash(script_path, src, src_len);
    char path[4096];
    archive_path(c, nh, path, sizeof(path), 0);           /* .lrfile */
    size_t flen = 0;
    uint8_t *fdata = read_all(path, &flen);
    if (!fdata) {
        /* Legacy / user-renamed ".lrfile.lz4" spelling. */
        archive_path(c, nh, path, sizeof(path), 1);
        fdata = read_all(path, &flen);
    }
    if (!fdata) { IOME_STAT_INC(c, miss_count); return -1; }

    /* Header */
    size_t need = 8 + 4 * 4 + 8 * 4 + 4 * 4;
    if (flen < need || memcmp(fdata, LR_IOME586_NAME "\0", 8) != 0)
        goto corrupt;

    size_t off = 8;
    uint32_t desc_len, stored_len, raw_len;
    memcpy(&mf->container_version, fdata + off, 4); off += 4;
    memcpy(&mf->engine_version,    fdata + off, 4); off += 4;
    memcpy(&mf->status,            fdata + off, 4); off += 4;
    memcpy(&mf->flags,             fdata + off, 4); off += 4;
    memcpy(&mf->created_at,        fdata + off, 8); off += 8;
    memcpy(&mf->source_hash,       fdata + off, 8); off += 8;
    memcpy(&mf->source_mtime,      fdata + off, 8); off += 8;
    memcpy(&mf->source_size,       fdata + off, 8); off += 8;
    { uint32_t r; memcpy(&r, fdata + off, 4); off += 4; mf->opt_ratio = r / 1e6; }
    memcpy(&mf->checksum,          fdata + off, 4); off += 4;
    memcpy(&stored_len,            fdata + off, 4); off += 4;
    memcpy(&raw_len,               fdata + off, 4); off += 4;
    memcpy(&desc_len,              fdata + off, 4); off += 4;

    if (mf->container_version != LR_IOME586_CONTAINER_VERSION) goto stale;
    if (mf->engine_version != engine_version_id())              goto stale;
    if (mf->status != LR_IOME586_STATUS_ARCHIVED)               goto stale;
    if (mf->source_hash != hash)                                goto stale;
    if (mf->source_size != (uint64_t)src_len)                   goto stale;
    if (script_path && script_path[0] != '<' && mf->source_mtime != 0) {
        int64_t now_mtime = file_mtime(script_path);
        if (now_mtime > 0 && now_mtime != mf->source_mtime)     goto stale;
    }
    if (off + desc_len + stored_len > flen)                     goto corrupt;

    mf->desc = dup_text(fdata + off, desc_len);
    off += desc_len;

    /* Un-key + decompress the payload */
    uint8_t *stored = malloc(stored_len ? stored_len : 1);
    if (!stored) goto corrupt;
    memcpy(stored, fdata + off, stored_len);
    if (mf->flags & LR_IOME586_FLAG_KEYED)
        xor_key(stored, stored_len, hash);

    uint8_t *raw;
    size_t raw_actual;
    if (mf->flags & LR_IOME586_FLAG_COMPRESSED) {
        raw = lr_decompress_if_needed(stored, stored_len, 1, 0, &raw_actual);
        free(stored);
        if (!raw || raw_actual != raw_len) { free(raw); goto corrupt; }
    } else {
        raw = stored;
        raw_actual = stored_len;
    }
    if (lr_iome586_crc32(raw, raw_actual) != mf->checksum) {
        free(raw);
        goto corrupt;
    }

    /* Parse entries */
    {
        size_t p = 0;
        while (p + 6 <= raw_actual) {
            uint16_t nlen; memcpy(&nlen, raw + p, 2); p += 2;
            if (p + nlen + 4 > raw_actual) break;
            char name[64] = {0};
            memcpy(name, raw + p, nlen < 63 ? nlen : 63);
            p += nlen;
            uint32_t dlen; memcpy(&dlen, raw + p, 4); p += 4;
            if (p + dlen > raw_actual) break;
            const uint8_t *d = raw + p;
            p += dlen;

            if      (!strcmp(name, "meta"))    mf->meta   = dup_text(d, dlen);
            else if (!strcmp(name, "path"))    mf->path   = dup_text(d, dlen);
            else if (!strcmp(name, "config"))  mf->config = dup_text(d, dlen);
            else if (!strcmp(name, "init"))    mf->init   = dup_text(d, dlen);
            else if (!strcmp(name, "state"))   mf->state  = dup_text(d, dlen);
            else if (!strcmp(name, "ast")) {
                mf->ast = malloc(dlen ? dlen : 1);
                if (mf->ast) { memcpy(mf->ast, d, dlen); mf->ast_len = dlen; }
            } else if (!strcmp(name, "nodes")) {
                mf->nodes = malloc(dlen ? dlen : 1);
                if (mf->nodes) { memcpy(mf->nodes, d, dlen); mf->nodes_len = dlen; }
            } else if (!strcmp(name, "globals")) {
                mf->globals = malloc(dlen ? dlen : 1);
                if (mf->globals) { memcpy(mf->globals, d, dlen); mf->globals_len = dlen; }
            } else if (!strcmp(name, "bytecode")) {
                mf->bytecode = malloc(dlen ? dlen : 1);
                if (mf->bytecode) { memcpy(mf->bytecode, d, dlen); mf->bytecode_len = dlen; }
            } else if (!strcmp(name, "mir")) {
                mf->mir_data = malloc(dlen ? dlen : 1);
                if (mf->mir_data) { memcpy(mf->mir_data, d, dlen); mf->mir_len = dlen; }
            }
        }
    }
    free(raw);
    free(fdata);

    if (!mf->ast) { lr_iome586_manifest_free(mf); IOME_STAT_INC(c, miss_count); return -1; }

    IOME_STAT_INC(c, hit_count);
    IOME_STAT_ADD(c, bytes_loaded, (int64_t)flen);
    return 0;

stale:
    /* The script changed (or engine/container version did): keep the old
     * archive as ".bak" so lr_iome586_revert can still roll back, then
     * miss -> the cold path re-stores the SAME file name (auto-refresh). */
    free(fdata);
    {
        char bak[4096];
        backup_path(c, nh, bak, sizeof(bak));
        if (move_file(path, bak) != 0) remove(path);
    }
    IOME_STAT_INC(c, invalid_count);
    lr_iome586_manifest_free(mf);
    return -1;

corrupt:
    free(fdata);
    remove(path);
    IOME_STAT_INC(c, invalid_count);
    lr_iome586_manifest_free(mf);
    return -1;
}

void lr_iome586_manifest_free(LR_Iome586Manifest *mf)
{
    free(mf->desc);    free(mf->meta);   free(mf->path);
    free(mf->config);  free(mf->init);   free(mf->state);
    free(mf->ast);     free(mf->nodes);  free(mf->globals);
    free(mf->bytecode);
    free(mf->mir_data);
    memset(mf, 0, sizeof(*mf));
}

/* ── Restore ───────────────────────────────────────────────────────────── */

int lr_iome586_restore_globals(LR_Iome586Cache *c, LRContext *ctx,
                               const LR_Iome586Manifest *mf)
{
    if (!ctx || !mf->globals || mf->globals_len < 4) return -1;

    const uint8_t *p = mf->globals;
    size_t len = mf->globals_len, off = 0;
    uint32_t count;
    memcpy(&count, p, 4); off += 4;

    int restored = 0;
    for (uint32_t i = 0; i < count && off + 3 <= len; i++) {
        uint16_t nlen; memcpy(&nlen, p + off, 2); off += 2;
        if (off + nlen + 1 > len) break;
        char name[512] = {0};
        memcpy(name, p + off, nlen < 511 ? nlen : 511);
        off += nlen;
        uint8_t tag = p[off++];
        LRValue v = LR_VALUE_UNDEFINED;
        int restorable = 1;
        switch (tag) {
        case 0: v = LR_VALUE_UNDEFINED; break;
        case 1: v = LR_VALUE_NULL; break;
        case 2:
            if (off + 1 > len) { restorable = 0; break; }
            v = p[off++] ? LR_VALUE_TRUE : LR_VALUE_FALSE;
            break;
        case 3: {
            if (off + 4 > len) { restorable = 0; break; }
            int32_t iv; memcpy(&iv, p + off, 4); off += 4;
            v = lr_new_int32(ctx, iv);
            break;
        }
        case 4: {
            if (off + 8 > len) { restorable = 0; break; }
            double dv; memcpy(&dv, p + off, 8); off += 8;
            v = lr_new_float64(ctx, dv);
            break;
        }
        case 5: {
            if (off + 4 > len) { restorable = 0; break; }
            uint32_t slen; memcpy(&slen, p + off, 4); off += 4;
            if (off + slen > len) { restorable = 0; break; }
            char *s = dup_text(p + off, slen);
            off += slen;
            if (!s) { restorable = 0; break; }
            v = lr_new_string(ctx, s);
            free(s);
            break;
        }
        default: /* tag 6: object/function; recorded but not restorable */
            restorable = 0;
            break;
        }
        if (!restorable) continue;
        /* Remap protection: never let an archive (old version, another
         * engine build, or a tampered file) overwrite BOM / engine
         * namespace bindings like fetch, localStorage, console... */
        if (in_engine_namespace(c, name)) {
            lr_free_value(ctx, v);
            continue;
        }
        if (lr_set_property_str(ctx, ctx->global_obj, name, v) >= 0)
            restored++;
    }
    return restored;
}

/* ── Rollback / maintenance ────────────────────────────────────────────── */

static int hash_for_script(const char *script_path, uint64_t *out)
{
    /* Archives are named by the script path hash (stable across edits),
     * so maintenance ops can always find them even after the source
     * content changed. */
    if (!script_path || !script_path[0]) return -1;
    *out = lr_iome586_hash64((const uint8_t *)script_path,
                             strlen(script_path));
    return 0;
}

int lr_iome586_revert(LR_Iome586Cache *c, const char *script_path)
{
    if (!c->enabled || !c->cache_dir) return -1;
    uint64_t hash;
    if (hash_for_script(script_path, &hash) != 0) return -1;

    char bak[4096], final_p[4096];
    backup_path(c, hash, bak, sizeof(bak));
    archive_path(c, hash, final_p, sizeof(final_p), 0);

    IOME_LOCK(c);
    int rc = -1;
    int did_revert = 0;
    if (file_exists(bak)) {
        rc = move_file(bak, final_p) == 0 ? 0 : -1;
        if (rc == 0) did_revert = 1;
    }
    IOME_UNLOCK(c);
    if (did_revert) IOME_STAT_INC(c, revert_count);
    return rc;
}

void lr_iome586_invalidate(LR_Iome586Cache *c, const char *script_path)
{
    if (!c->enabled || !c->cache_dir) return;
    uint64_t hash;
    if (hash_for_script(script_path, &hash) != 0) return;

    char p1[4096], p2[4096], p3[4096];
    archive_path(c, hash, p1, sizeof(p1), 1);
    archive_path(c, hash, p2, sizeof(p2), 0);
    backup_path(c, hash, p3, sizeof(p3));

    IOME_LOCK(c);
    remove(p1); remove(p2); remove(p3);
    IOME_UNLOCK(c);
    IOME_STAT_INC(c, invalid_count);
}

void lr_iome586_clear(LR_Iome586Cache *c)
{
    IOME_STAT_SET(c, store_count, 0);
    IOME_STAT_SET(c, bytes_stored, 0);
}

/* ── Statistics ────────────────────────────────────────────────────────── */

void lr_iome586_stats(LR_Iome586Cache *c, FILE *fp)
{
    /* Snapshot all counters atomically */
    int64_t hits   = IOME_STAT_GET(c, hit_count);
    int64_t misses = IOME_STAT_GET(c, miss_count);
    int64_t stores = IOME_STAT_GET(c, store_count);
    int64_t skips  = IOME_STAT_GET(c, skip_count);
    int64_t inval  = IOME_STAT_GET(c, invalid_count);
    int64_t rev    = IOME_STAT_GET(c, revert_count);
    int64_t bs     = IOME_STAT_GET(c, bytes_stored);
    int64_t bl     = IOME_STAT_GET(c, bytes_loaded);
    int64_t bsv    = IOME_STAT_GET(c, bytes_saved);

    fprintf(fp, "\n");
    fprintf(fp, "╔══════════════════════════════════════════════════════════════╗\n");
    fprintf(fp, "║  IOME586 Result Cache Statistics                             ║\n");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Status:      %-46s ║\n", c->enabled ? "enabled" : "disabled");
    fprintf(fp, "║  Directory:   %-46s ║\n", c->cache_dir ? c->cache_dir : "(none)");
    fprintf(fp, "║  Container:   .lrfile (LZ4 package, hash-keyed)              ║\n");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Hits:    %10lld    Misses:   %10lld             ║\n",
            (long long)hits, (long long)misses);
    fprintf(fp, "║  Stores:  %10lld    Skipped:  %10lld (<15%% gain) ║\n",
            (long long)stores, (long long)skips);
    fprintf(fp, "║  Invalid: %10lld    Reverts:  %10lld             ║\n",
            (long long)inval, (long long)rev);
    fprintf(fp, "║  Stored:  %10.2f KB  Loaded:   %10.2f KB           ║\n",
            (double)bs / 1024.0, (double)bl / 1024.0);
    fprintf(fp, "║  Saved:   %10.2f KB (LZ4)                              ║\n",
            (double)bsv / 1024.0);
    fprintf(fp, "║  Hit rate: %6.1f%%                                          ║\n",
            (hits + misses) > 0
              ? 100.0 * (double)hits / (double)(hits + misses)
              : 0.0);
    fprintf(fp, "╚══════════════════════════════════════════════════════════════╝\n");
    fprintf(fp, "\n");
}

/* ── Distillation ──────────────────────────────────────────────────────── */
/* Note: distillation runs inline (no sandbox threads) to avoid
 * LR_THREAD_LOCAL cached-AST-node issues in the engine's parser. */

void lr_iome586_distill_config_default(LR_DistillConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_sandboxes = LR_DISTILL_DEFAULT_SANDBOXES;
    cfg->max_rounds    = LR_DISTILL_DEFAULT_ROUNDS;
    cfg->timeout_ms    = 0;
    cfg->verbose       = 0;
    cfg->enable_jit    = 1;
    cfg->use_parallel  = 1;
}

/* ── Count lines in a source buffer ─────────────────────────────────────── */

static int count_lines(const char *src, size_t len)
{
    int n = 1;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') n++;
    }
    return n;
}

/* ── Extract a line range from source (start_line=1-based, inclusive) ──── */

static char *extract_lines(const char *src, size_t src_len,
                           int start_line, int end_line)
{
    if (start_line < 1) start_line = 1;
    if (end_line < start_line) end_line = start_line;

    /* Find the start position */
    const char *begin = src;
    int line = 1;
    while (line < start_line && (size_t)(begin - src) < src_len) {
        if (*begin == '\n') line++;
        begin++;
    }

    /* Find the end position */
    const char *end = begin;
    while (line <= end_line && (size_t)(end - src) < src_len) {
        if (*end == '\n') line++;
        end++;
    }

    size_t out_len = (size_t)(end - begin);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    memcpy(out, begin, out_len);
    out[out_len] = '\0';
    return out;
}

/* ── Distillation analysis ──────────────────────────────────────────────── */

LR_DistillAnalysis *lr_iome586_distill_analyze(LRContext *ctx,
    const char *source, size_t source_len)
{
    (void)ctx;
    if (!source || !source_len) return NULL;

    LR_DistillAnalysis *analysis = (LR_DistillAnalysis *)
        calloc(1, sizeof(LR_DistillAnalysis));
    if (!analysis) return NULL;

    /* Simple line-based analysis: each line is a potential position.
     * Skip empty lines and comment-only lines. */
    int max_positions = count_lines(source, source_len);
    if (max_positions < 1) { free(analysis); return NULL; }

    analysis->positions = (LR_DistillPosition *)
        calloc((size_t)max_positions, sizeof(LR_DistillPosition));
    if (!analysis->positions) { free(analysis); return NULL; }

    int pos = 0;
    const char *p = source;
    const char *line_start = source;
    int line = 1;
    size_t remaining = source_len;

    while (remaining > 0 && pos < max_positions) {
        /* Find end of line */
        const char *nl = (const char *)memchr(p, '\n', remaining);
        size_t line_len = nl ? (size_t)(nl - p) : remaining;
        int is_blank = 1;
        for (size_t i = 0; i < line_len; i++) {
            char ch = line_start[i];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                is_blank = 0;
                break;
            }
        }
        if (!is_blank) {
            analysis->positions[pos].line = line;
            analysis->positions[pos].column = 1;
            analysis->positions[pos].source_text = (char *)malloc(line_len + 1);
            if (analysis->positions[pos].source_text) {
                memcpy(analysis->positions[pos].source_text, line_start, line_len);
                analysis->positions[pos].source_text[line_len] = '\0';
            }
            pos++;
        }
        line++;
        if (nl) {
            p = nl + 1;
            line_start = p;
            remaining = source_len - (size_t)(p - source);
        } else {
            break;
        }
    }

    analysis->num_positions = pos;
    return analysis;
}

void lr_iome586_distill_analysis_free(LR_DistillAnalysis *analysis)
{
    if (!analysis) return;
    for (int32_t i = 0; i < analysis->num_positions; i++) {
        free(analysis->positions[i].source_text);
    }
    free(analysis->positions);
    free(analysis);
}

/* ── Run distillation (inline, no sandbox threads) ──────────────────────── */

LR_DistillResult *lr_iome586_distill_run(LR_Runtime *rt,
    const char *source, size_t source_len,
    const LR_DistillConfig *config)
{
    if (!rt || !source || !source_len) return NULL;

    LR_DistillConfig cfg;
    if (config) {
        cfg = *config;
    } else {
        lr_iome586_distill_config_default(&cfg);
    }

    if (cfg.max_rounds < 1) cfg.max_rounds = 1;

    int total_lines = count_lines(source, source_len);

    LR_DistillResult *result = (LR_DistillResult *)
        calloc(1, sizeof(LR_DistillResult));
    if (!result) return NULL;

    result->num_rounds = cfg.max_rounds;
    result->rounds = (LR_DistillRound *)
        calloc((size_t)cfg.max_rounds, sizeof(LR_DistillRound));
    if (!result->rounds) {
        free(result);
        return NULL;
    }

    int64_t total_start = lr_get_time_us();

    /* Run each distillation round */
    for (int r = 0; r < cfg.max_rounds; r++) {
        int64_t round_start = lr_get_time_us();
        int num_chunks = cfg.max_sandboxes;
        if (num_chunks < 1) num_chunks = 1;
        if (num_chunks > total_lines) num_chunks = total_lines;
        if (num_chunks < 1) num_chunks = 1;

        int lines_per_chunk = (total_lines + num_chunks - 1) / num_chunks;
        if (lines_per_chunk < 1) lines_per_chunk = 1;

        if (cfg.verbose) {
            fprintf(stderr, "[IOME586] Distill round %d/%d: %d chunks, "
                    "~%d lines/chunk\n",
                    r + 1, cfg.max_rounds, num_chunks, lines_per_chunk);
        }

        /* Allocate per-round results */
        result->rounds[r].num_chunks = num_chunks;
        result->rounds[r].results = (char **)
            calloc((size_t)num_chunks, sizeof(char *));
        result->rounds[r].num_sandboxes = num_chunks;

        /* For each round, shift the chunk boundaries by round * offset */
        int round_offset = r * (lines_per_chunk / cfg.max_rounds + 1);

        for (int c = 0; c < num_chunks; c++) {
            int start_line = c * lines_per_chunk + 1 + round_offset;
            int end_line = (c + 1) * lines_per_chunk + round_offset;
            if (end_line > total_lines) end_line = total_lines;
            if (start_line > total_lines) {
                start_line = total_lines;
                end_line = total_lines;
            }

            char *chunk = extract_lines(source, source_len, start_line, end_line);
            if (!chunk) chunk = strdup("");

            /* Evaluate in the main runtime */
            int ret = lr_eval(rt, chunk, strlen(chunk), "(distill)");
            result->rounds[r].results[c] = (ret == 0) ? strdup("(ok)") : strdup("(error)");
            free(chunk);
        }

        int64_t round_end = lr_get_time_us();
        result->rounds[r].elapsed_ms = (double)(round_end - round_start) / 1000.0;

        /* Capture JIT stats if available */
        if (cfg.enable_jit && rt && rt->lr_rt && rt->lr_rt->jit_runtime) {
            LRJITRuntime *jit = (LRJITRuntime *)rt->lr_rt->jit_runtime;
            result->rounds[r].jit_compiles = jit->compile_count;
            result->rounds[r].jit_bailouts = jit->bailout_count;
            result->rounds[r].jit_executions = jit->exec_count;
            result->rounds[r].jit_code_size = jit->total_code_size;
        }

        if (cfg.verbose) {
            fprintf(stderr, "[IOME586] Round %d done: %.2f ms\n",
                    r + 1, result->rounds[r].elapsed_ms);
        }
    }

    int64_t total_end = lr_get_time_us();
    result->total_ms = (double)(total_end - total_start) / 1000.0;

    /* Aggregate JIT stats */
    if (cfg.enable_jit && rt && rt->lr_rt && rt->lr_rt->jit_runtime) {
        LRJITRuntime *jit = (LRJITRuntime *)rt->lr_rt->jit_runtime;
        result->jit_total_compiles = jit->compile_count;
        result->jit_total_bailouts = jit->bailout_count;
        result->jit_total_executions = jit->exec_count;
        result->jit_total_code_size = jit->total_code_size;
    }

    /* Build combined result from last round */
    if (cfg.max_rounds > 0 && result->rounds[cfg.max_rounds - 1].num_chunks > 0) {
        LR_DistillRound *last = &result->rounds[cfg.max_rounds - 1];
        size_t total = 0;
        for (int c = 0; c < last->num_chunks; c++) {
            if (last->results[c])
                total += strlen(last->results[c]) + 1;
        }
        result->combined = (char *)malloc(total + 1);
        if (result->combined) {
            result->combined[0] = '\0';
            for (int c = 0; c < last->num_chunks; c++) {
                if (last->results[c]) {
                    if (result->combined[0])
                        strcat(result->combined, "\n");
                    strcat(result->combined, last->results[c]);
                }
            }
            result->total_positions = last->num_chunks;
        }
    }

    return result;
}

void lr_iome586_distill_result_free(LR_DistillResult *result)
{
    if (!result) return;
    for (int32_t r = 0; r < result->num_rounds; r++) {
        for (int32_t c = 0; c < result->rounds[r].num_chunks; c++) {
            free(result->rounds[r].results[c]);
        }
        free(result->rounds[r].results);
    }
    free(result->rounds);
    free(result->combined);
    free(result);
}
