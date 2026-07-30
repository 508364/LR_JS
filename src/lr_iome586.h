/*
 * L/R_JS - IOME586 Result Cache
 *
 * IOME586 is the engine's persistent execution cache. It archives not only
 * the compiled AST but the *results* of interpreting a script, so a cached
 * run can skip lexing/parsing entirely and the archived state can be
 * inspected / restored offline.
 *
 * The cache file is, in essence, an LZ4 archive:
 *   <cache_dir>/<namehash16>.lrfile     (the loader also accepts the
 *                                        ".lrfile.lz4" spelling)
 * The file is named by the *script path* hash so that editing the script
 * refreshes the same archive in place (auto-update). The script *content*
 * hash is stored in the header (source_hash) for validation and is the
 * payload XOR key.
 *
 * Container layout (little-endian):
 *   ┌────────────────────────────────────────────────────────────────────┐
 *   │ magic        "IOME586\0"                                   8 bytes │
 *   │ container_version u32                                      4 bytes │
 *   │ engine_version    u32 (FNV-1a32 of LR_JS_VERSION_STRING)   4 bytes │
 *   │ status            u32 (1 = WRITING, 2 = ARCHIVED)          4 bytes │
 *   │ flags             u32 (strict/module/compressed/keyed)     4 bytes │
 *   │ created_at        i64 (unix time)                          8 bytes │
 *   │ source_hash       u64 (FNV-1a64 of script bytes; also the         │
 *   │                        archive "password" / XOR key)       8 bytes │
 *   │ source_mtime      i64                                      8 bytes │
 *   │ source_size       u64                                      8 bytes │
 *   │ opt_ratio_x1e6    u32 (estimated speedup fraction * 1e6)   4 bytes │
 *   │ payload_crc32     u32 (CRC-32 of the raw payload)          4 bytes │
 *   │ payload_stored    u32                                      4 bytes │
 *   │ payload_raw       u32                                      4 bytes │
 *   │ desc_len          u32                                      4 bytes │
 *   │ desc              UTF-8, PLAINTEXT "archive comment": a copy of    │
 *   │                   the script name + creation time + version        │
 *   │ payload           XOR-keyed(source_hash) [ LZ4( entries ) ]        │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 * The payload is a sequence of named binary entries ("files" inside the
 * archive), each:   u16 name_len | name | u32 data_len | data
 *
 *   meta     text     script name, hash, status, time, ratio, version, crc
 *   path     text     interpretation method ("ast-treewalk-direct", format)
 *   config   text     runtime configuration used for the run
 *   init     text     initialization summary (builtins, global prop count)
 *   ast      binary   serialized AST (LRA1 format, lr_ast_serialize)
 *   nodes    binary   per top-level-node results (type, line, binding value)
 *   globals  binary   global variable binding snapshot (restorable)
 *   state    text     interpreter/state-machine state + run status + timing
 *
 * Behaviours:
 *   - Write-while-running: the archive is created with status=WRITING right
 *     after parse (before execution) and finalized to ARCHIVED afterwards.
 *   - 15% rule: if the estimated saving (parse time / total time) is below
 *     0.15, the archive is discarded instead of committed.
 *   - Rollback: overwriting an archive keeps a ".bak"; lr_iome586_revert()
 *     restores it. Aborted runs auto-restore the previous archive.
 *   - The cache directory is created automatically.
 */

#ifndef LR_IOME586_H
#define LR_IOME586_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "lr_platform.h"

typedef struct LR_Runtime LR_Runtime;
typedef struct LRContext  LRContext;
typedef struct ASTNode    ASTNode;

/* ── Constants ──────────────────────────────────────────────────────────── */

#define LR_IOME586_NAME              "IOME586"
#define LR_IOME586_CONTAINER_VERSION 1u

#define LR_IOME586_STATUS_WRITING    1u
#define LR_IOME586_STATUS_ARCHIVED   2u

#define LR_IOME586_FLAG_STRICT       (1u << 0)
#define LR_IOME586_FLAG_MODULE       (1u << 1)
#define LR_IOME586_FLAG_COMPRESSED   (1u << 2)   /* payload is LZ4 */
#define LR_IOME586_FLAG_KEYED        (1u << 3)   /* payload XOR-keyed by hash */

/* Minimum estimated speedup required to keep an archive (requirement: <15%
 * improvement => do not cache). */
#define LR_IOME586_MIN_GAIN          0.15

/* ── Cache manager ──────────────────────────────────────────────────────── */

typedef struct LR_Iome586Cache {
    char            *cache_dir;      /* NULL = disabled */
    int              enabled;
    int              compression;    /* LZ4 payload compression (default on) */

    /* Statistics */
    int64_t          hit_count;
    int64_t          miss_count;
    int64_t          store_count;
    int64_t          skip_count;     /* archives discarded by the 15% rule */
    int64_t          invalid_count;  /* stale / corrupt archives dropped */
    int64_t          revert_count;
    int64_t          bytes_stored;
    int64_t          bytes_loaded;
    int64_t          bytes_saved;    /* saved by LZ4 */

    pthread_mutex_t  mutex;
    LR_Runtime      *runtime;
} LR_Iome586Cache;

/* ── Loaded manifest (fully restorable view of an archive) ──────────────── */

typedef struct LR_Iome586Manifest {
    /* Header fields */
    uint32_t  container_version;
    uint32_t  engine_version;
    uint32_t  status;
    uint32_t  flags;
    int64_t   created_at;
    uint64_t  source_hash;
    int64_t   source_mtime;
    uint64_t  source_size;
    double    opt_ratio;
    uint32_t  checksum;
    char     *desc;          /* plaintext archive comment */

    /* Entries (NULL / 0 when absent) */
    char     *meta;
    char     *path;          /* interpretation method */
    char     *config;
    char     *init;
    char     *state;
    uint8_t  *ast;      size_t ast_len;
    uint8_t  *nodes;    size_t nodes_len;
    uint8_t  *globals;  size_t globals_len;
} LR_Iome586Manifest;

/* ── Two-phase writer (write-while-running) ─────────────────────────────── */

typedef struct LR_Iome586Writer {
    int       active;
    uint64_t  hash;
    int64_t   mtime;
    uint64_t  src_size;
    int64_t   created_at;
    uint32_t  flags;
    int64_t   parse_us;
    char      script[512];
    char      path_final[4096];
    char      path_bak[4096];
    int       have_bak;
    uint8_t  *ast;           /* owned serialized AST */
    size_t    ast_len;
} LR_Iome586Writer;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

void lr_iome586_init(LR_Iome586Cache *c, LR_Runtime *rt, const char *dir);
void lr_iome586_destroy(LR_Iome586Cache *c);
int  lr_iome586_set_dir(LR_Iome586Cache *c, const char *dir);

/* ── Load / restore ─────────────────────────────────────────────────────── */

/* Load and fully parse the archive for a script (content-hash keyed).
 * Returns 0 and fills *mf on success; -1 on miss/stale/corrupt. */
int  lr_iome586_load(LR_Iome586Cache *c, const char *script_path,
                     const uint8_t *src, size_t src_len,
                     LR_Iome586Manifest *mf);
void lr_iome586_manifest_free(LR_Iome586Manifest *mf);

/* Re-bind the archived global variable snapshot (primitive values) onto the
 * live global object. Returns number of bindings restored, or -1. */
int  lr_iome586_restore_globals(LRContext *ctx, const LR_Iome586Manifest *mf);

/* ── Store (two-phase) ──────────────────────────────────────────────────── */

/* Phase 1 (right after parse, before execution): writes the archive with
 * status=WRITING. Takes ownership of ast_bc on success (returns 0). */
int  lr_iome586_begin(LR_Iome586Cache *c, const char *script_path,
                      const uint8_t *src, size_t src_len,
                      uint8_t *ast_bc, size_t ast_len,
                      uint32_t flags, int64_t parse_us,
                      LR_Iome586Writer *w);

/* Phase 2 (after successful execution): captures interpreter results
 * (globals, per-node results, run state) and finalizes to ARCHIVED.
 * Applies the 15% rule: returns 1 if the archive was discarded as not
 * beneficial, 0 if committed, -1 on error. */
int  lr_iome586_commit(LR_Iome586Cache *c, LR_Iome586Writer *w,
                       LRContext *ctx, const ASTNode *program,
                       int64_t exec_us);

/* Abort a begun archive: removes the WRITING file and restores the previous
 * archive from ".bak" if one existed (automatic rollback). */
void lr_iome586_abort(LR_Iome586Cache *c, LR_Iome586Writer *w);

/* ── Rollback / maintenance ─────────────────────────────────────────────── */

/* Restore the previous archive (".bak") for a script. 0 on success. */
int  lr_iome586_revert(LR_Iome586Cache *c, const char *script_path);

/* Drop the archive for a script (both current and ".bak"). */
void lr_iome586_invalidate(LR_Iome586Cache *c, const char *script_path);

void lr_iome586_stats(LR_Iome586Cache *c, FILE *fp);
void lr_iome586_clear(LR_Iome586Cache *c);

/* ── Utilities ──────────────────────────────────────────────────────────── */

uint64_t lr_iome586_hash64(const uint8_t *data, size_t len);
uint32_t lr_iome586_crc32(const uint8_t *data, size_t len);

#endif /* LR_IOME586_H */
