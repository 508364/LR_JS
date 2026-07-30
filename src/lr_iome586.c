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
    pthread_mutex_init(&c->mutex, NULL);
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
    pthread_mutex_destroy(&c->mutex);
}

int lr_iome586_set_dir(LR_Iome586Cache *c, const char *dir)
{
    pthread_mutex_lock(&c->mutex);
    free(c->cache_dir);
    c->cache_dir = NULL;
    c->enabled = 0;
    if (dir && dir[0]) {
        mkdir_p(dir);
        c->cache_dir = strdup(dir);
        c->enabled = 1;
    }
    pthread_mutex_unlock(&c->mutex);
    return 0;
}

/* ── Entry stream (the "files" inside the package) ─────────────────────── */

typedef struct {
    uint8_t *data;
    size_t   len, cap;
    int      error;
} Blob;

static void blob_put(Blob *b, const void *src, size_t n)
{
    if (b->error) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 512;
        while (nc < b->len + n) nc *= 2;
        uint8_t *nd = realloc(b->data, nc);
        if (!nd) { b->error = 1; return; }
        b->data = nd; b->cap = nc;
    }
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

/* Names that exist on the global object before any user script runs
 * (builtins). Snapshotting them would bloat the archive, so they are
 * filtered by a "not a builtin at init time" check via shadow list.
 * Cheap approach: skip names that resolve to objects/functions AND are
 * known builtin roots. User data objects are still recorded as tag 6. */
static int is_builtin_global(const char *name)
{
    static const char *skip[] = {
        "globalThis", "console", "Math", "JSON", "Object", "Array", "String",
        "Number", "Boolean", "Symbol", "BigInt", "Date", "RegExp", "Error",
        "TypeError", "RangeError", "SyntaxError", "ReferenceError",
        "EvalError", "URIError", "AggregateError", "Promise", "Proxy",
        "Reflect", "Map", "Set", "WeakMap", "WeakSet", "WeakRef",
        "FinalizationRegistry", "ArrayBuffer", "SharedArrayBuffer",
        "DataView", "Int8Array", "Uint8Array", "Uint8ClampedArray",
        "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
        "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array",
        "Function", "eval", "parseInt", "parseFloat", "isNaN", "isFinite",
        "encodeURI", "decodeURI", "encodeURIComponent", "decodeURIComponent",
        "escape", "unescape", "NaN", "Infinity", "undefined",
        "setTimeout", "clearTimeout", "setInterval", "clearInterval",
        "queueMicrotask", "fetch", "URL", "URLSearchParams", "TextEncoder",
        "TextDecoder", "atob", "btoa", "Event", "EventTarget", "CustomEvent",
        "AbortController", "AbortSignal", "performance", "crypto",
        "localStorage", "sessionStorage", "WebSocket", "Worker", "navigator",
        "structuredClone", "Intl", "fs", "terminal", "os",
        NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(name, skip[i]) == 0) return 1;
    return 0;
}

/* Snapshot the global variable bindings into an entry blob.
 * format: u32 count, then per binding: u16 name_len|name|value */
static void snapshot_globals(Blob *out, LRContext *ctx)
{
    LRPropertyEnum *tab = NULL;
    uint32_t len = 0, count = 0;
    Blob body = {0};

    if (lr_get_own_property_names(ctx, &tab, &len, ctx->global_obj,
                                  JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < len; i++) {
            if (!tab[i].atom) continue;
            const char *name = tab[i].atom->str;
            if (is_builtin_global(name)) continue;
            LRValue v = lr_get_property(ctx, ctx->global_obj, tab[i].atom);
            if (v.tag == LR_TYPE_EXCEPTION) continue;
            size_t nlen = strlen(name);
            blob_u16(&body, (uint16_t)nlen);
            blob_put(&body, name, nlen);
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
static void snapshot_nodes(Blob *out, LRContext *ctx, const ASTNode *program)
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
        c->bytes_saved += (int64_t)(payload_raw_len - stored_len);
    }
    /* The package password is the source hash. */
    xor_key(stored, stored_len, w->hash);
    flags |= LR_IOME586_FLAG_KEYED;

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
    if (rc == 0) c->bytes_stored += (int64_t)f.len;
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

    pthread_mutex_lock(&c->mutex);

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
    pthread_mutex_unlock(&c->mutex);

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
        pthread_mutex_lock(&c->mutex);
        remove(w->path_final);
        if (w->have_bak) move_file(w->path_bak, w->path_final);
        c->skip_count++;
        pthread_mutex_unlock(&c->mutex);
        free(w->ast);
        w->ast = NULL;
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
    snapshot_nodes(&nodes, ctx, program);
    entry_add(&payload, "nodes", nodes.data ? (void *)nodes.data : (void *)"",
              nodes.len);
    free(nodes.data);

    Blob globals = {0};
    snapshot_globals(&globals, ctx);
    entry_add(&payload, "globals",
              globals.data ? (void *)globals.data : (void *)"", globals.len);
    free(globals.data);

    entry_add_text(&payload, "state", state);

    int rc = -1;
    pthread_mutex_lock(&c->mutex);
    if (!payload.error)
        rc = write_container(c, w->path_final, LR_IOME586_STATUS_ARCHIVED,
                             w->flags, w, gain, payload.data, payload.len);
    if (rc == 0) {
        c->store_count++;
    } else {
        remove(w->path_final);
        if (w->have_bak) move_file(w->path_bak, w->path_final);
    }
    pthread_mutex_unlock(&c->mutex);

    free(payload.data);
    free(w->ast);
    w->ast = NULL;
    return rc == 0 ? 0 : -1;
}

void lr_iome586_abort(LR_Iome586Cache *c, LR_Iome586Writer *w)
{
    if (!w->active) return;
    w->active = 0;
    pthread_mutex_lock(&c->mutex);
    remove(w->path_final);
    if (w->have_bak) {
        move_file(w->path_bak, w->path_final);   /* automatic rollback */
        c->revert_count++;
    }
    pthread_mutex_unlock(&c->mutex);
    free(w->ast);
    w->ast = NULL;
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
    if (!fdata) { c->miss_count++; return -1; }

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
            }
        }
    }
    free(raw);
    free(fdata);

    if (!mf->ast) { lr_iome586_manifest_free(mf); c->miss_count++; return -1; }

    c->hit_count++;
    c->bytes_loaded += (int64_t)flen;
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
    c->invalid_count++;
    lr_iome586_manifest_free(mf);
    return -1;

corrupt:
    free(fdata);
    remove(path);
    c->invalid_count++;
    lr_iome586_manifest_free(mf);
    return -1;
}

void lr_iome586_manifest_free(LR_Iome586Manifest *mf)
{
    free(mf->desc);    free(mf->meta);   free(mf->path);
    free(mf->config);  free(mf->init);   free(mf->state);
    free(mf->ast);     free(mf->nodes);  free(mf->globals);
    memset(mf, 0, sizeof(*mf));
}

/* ── Restore ───────────────────────────────────────────────────────────── */

int lr_iome586_restore_globals(LRContext *ctx, const LR_Iome586Manifest *mf)
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

    pthread_mutex_lock(&c->mutex);
    int rc = -1;
    if (file_exists(bak)) {
        rc = move_file(bak, final_p);
        if (rc == 0) c->revert_count++;
    }
    pthread_mutex_unlock(&c->mutex);
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

    pthread_mutex_lock(&c->mutex);
    remove(p1); remove(p2); remove(p3);
    c->invalid_count++;
    pthread_mutex_unlock(&c->mutex);
}

void lr_iome586_clear(LR_Iome586Cache *c)
{
    pthread_mutex_lock(&c->mutex);
    c->store_count = 0;
    c->bytes_stored = 0;
    pthread_mutex_unlock(&c->mutex);
}

/* ── Statistics ────────────────────────────────────────────────────────── */

void lr_iome586_stats(LR_Iome586Cache *c, FILE *fp)
{
    fprintf(fp, "\n");
    fprintf(fp, "╔══════════════════════════════════════════════════════════════╗\n");
    fprintf(fp, "║  IOME586 Result Cache Statistics                             ║\n");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Status:      %-46s ║\n", c->enabled ? "enabled" : "disabled");
    fprintf(fp, "║  Directory:   %-46s ║\n", c->cache_dir ? c->cache_dir : "(none)");
    fprintf(fp, "║  Container:   .lrfile (LZ4 package, hash-keyed)              ║\n");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Hits:    %10lld    Misses:   %10lld             ║\n",
            (long long)c->hit_count, (long long)c->miss_count);
    fprintf(fp, "║  Stores:  %10lld    Skipped:  %10lld (<15%% gain) ║\n",
            (long long)c->store_count, (long long)c->skip_count);
    fprintf(fp, "║  Invalid: %10lld    Reverts:  %10lld             ║\n",
            (long long)c->invalid_count, (long long)c->revert_count);
    fprintf(fp, "║  Stored:  %10.2f KB  Loaded:   %10.2f KB           ║\n",
            (double)c->bytes_stored / 1024.0, (double)c->bytes_loaded / 1024.0);
    fprintf(fp, "║  Saved:   %10.2f KB (LZ4)                              ║\n",
            (double)c->bytes_saved / 1024.0);
    fprintf(fp, "║  Hit rate: %6.1f%%                                          ║\n",
            (c->hit_count + c->miss_count) > 0
              ? 100.0 * (double)c->hit_count /
                (double)(c->hit_count + c->miss_count)
              : 0.0);
    fprintf(fp, "╚══════════════════════════════════════════════════════════════╝\n");
    fprintf(fp, "\n");
}
