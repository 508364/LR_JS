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
 * hash is stored in the header (source_hash) for validation.
 *
 * Security notes:
 *   - New archives are written UNKEYED: the historical XOR "keying" used
 *     source_hash as the key, which is stored in the same header, so it was
 *     obfuscation, not encryption. Integrity is covered by payload_crc32.
 *     The loader still accepts legacy KEYED archives for compatibility.
 *   - The globals snapshot never serializes values of sensitive-looking
 *     names (token/secret/password/...); they are recorded as opaque
 *     (tag 6, present-but-not-restorable). "snapshot_strings=0" extends
 *     this to ALL string globals (--iome586-no-strings).
 *   - The warm path re-executes the AST, so restoring the globals snapshot
 *     is redundant for correctness; it is now OPT-IN (restore_globals,
 *     --iome586-restore-globals) to avoid pre-seeding stale state that a
 *     script could observe via typeof-probing before its own assignments.
 *
 * Container layout (little-endian):
 *   ┌────────────────────────────────────────────────────────────────────┐
 *   │ magic        "IOME586\0"                                   8 bytes │
 *   │ container_version u32                                      4 bytes │
 *   │ engine_version    u32 (FNV-1a32 of LR_JS_VERSION_STRING)   4 bytes │
 *   │ status            u32 (1 = WRITING, 2 = ARCHIVED)          4 bytes │
 *   │ flags             u32 (strict/module/compressed/keyed)     4 bytes │
 *   │ created_at        i64 (unix time)                          8 bytes │
 *   │ source_hash       u64 (FNV-1a64 of script bytes; also the          │
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
    int              snapshot_strings; /* 1 = string globals may be archived
                                        * (sensitive names always excluded);
                                        * 0 = record all strings as opaque */
    int              restore_globals;  /* 1 = warm path rebinds the archived
                                        * globals snapshot before re-run
                                        * (default 0: re-run recomputes) */

    /* Builtin baseline ("remap set"): the global property names present
     * right after builtin registration, captured at runtime init. Both the
     * snapshot AND the restore path treat these names as engine namespace:
     * never archived, never overwritten by an archive. Falls back to the
     * static builtin list when empty (embedders that skip capture). */
    char           **baseline_names;
    uint32_t         baseline_count;

    /* Statistics — accessed via atomic ops, no lock required */
    volatile int64_t  hit_count;
    volatile int64_t  miss_count;
    volatile int64_t  store_count;
    volatile int64_t  skip_count;     /* archives discarded by the 15% rule */
    volatile int64_t  invalid_count;  /* stale / corrupt archives dropped */
    volatile int64_t  revert_count;
    volatile int64_t  bytes_stored;
    volatile int64_t  bytes_loaded;
    volatile int64_t  bytes_saved;    /* saved by LZ4 */

    /* ── CAS-based spinlock ──────────────────────────────────────────────
     * Replaces the former pthread_rwlock + pthread_mutex.  Statistics are
     * updated via atomic fetch-add and never touch the spinlock.  Only file
     * I/O critical sections (store / abort / invalidate) acquire it, and
     * they do so with a CAS spin that stays in user space — no kernel
     * context switch for the (very short) critical sections.                */
    volatile int32_t  spinlock;
    LR_Runtime       *runtime;
} LR_CACHE_ALIGNED LR_Iome586Cache;

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
    uint8_t  *bytecode; size_t bytecode_len;
    uint8_t  *mir_data;   size_t mir_len;      /* cross-platform MIR cache (optional) */
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
    uint8_t  *bc_data;        /* serialized bytecode (set before commit) */
    size_t    bc_len;
    uint8_t  *mir_data;       /* serialized MIR cache (set before commit) */
    size_t    mir_len;
} LR_Iome586Writer;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

void lr_iome586_init(LR_Iome586Cache *c, LR_Runtime *rt, const char *dir);
void lr_iome586_destroy(LR_Iome586Cache *c);
int  lr_iome586_set_dir(LR_Iome586Cache *c, const char *dir);

/* Capture the builtin baseline: record every property name currently on the
 * global object (call right after lr_register_builtins). These names are
 * excluded from snapshots and protected from restore, so BOM APIs
 * (fetch/localStorage/...) can never be shadowed by archived state.
 * Returns the number of names captured, or -1. */
int  lr_iome586_capture_baseline(LR_Iome586Cache *c, LRContext *ctx);

/* ── Load / restore ─────────────────────────────────────────────────────── */

/* Load and fully parse the archive for a script (content-hash keyed).
 * Returns 0 and fills *mf on success; -1 on miss/stale/corrupt. */
int  lr_iome586_load(LR_Iome586Cache *c, const char *script_path,
                     const uint8_t *src, size_t src_len,
                     LR_Iome586Manifest *mf);
void lr_iome586_manifest_free(LR_Iome586Manifest *mf);

/* Re-bind the archived global variable snapshot (primitive values) onto the
 * live global object. Names inside the builtin baseline (BOM APIs) are
 * NEVER overwritten, regardless of archive contents (remap protection).
 * Returns number of bindings restored, or -1. */
int  lr_iome586_restore_globals(LR_Iome586Cache *c, LRContext *ctx,
                                const LR_Iome586Manifest *mf);

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

/* Store bytecode for the current writer (call after compilation, before commit). */
void lr_iome586_set_bytecode(LR_Iome586Writer *w, const uint8_t *data, size_t len);

/* Store serialized MIR for the current writer (call after JIT compile, before commit). */
void lr_iome586_set_mir(LR_Iome586Writer *w, const uint8_t *data, size_t len);

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

/* ── Distillation (Parallel Sandbox Pipeline) ─────────────────────────────
 *
 * IOME586 distillation enables parallel execution of script chunks across
 * multiple sandboxed runtimes.  The pipeline:
 *
 *   1. Parse the script and count top-level statements.
 *   2. Divide the source into N work chunks (by line, roughly even).
 *   3. Spawn sandboxes; each evaluates one chunk in parallel.
 *   4. Collect per-chunk results into a shared result set.
 *   5. Optionally repeat for multiple rounds (default 3), each round
 *      splitting the work differently to explore more parallelism.
 *
 * The metaphor: "a group of people circling an engine, each installing
 * their part, then moving to the next section."
 * ──────────────────────────────────────────────────────────────────────── */

#define LR_DISTILL_DEFAULT_SANDBOXES  16
#define LR_DISTILL_DEFAULT_ROUNDS      3
#define LR_DISTILL_MAX_SANDBOXES      64

/* Per-position analysis result */
typedef struct LR_DistillPosition {
    int32_t  line;              /* source line (1-based) */
    int32_t  column;            /* source column */
    char    *source_text;        /* extracted source text (caller frees) */
} LR_DistillPosition;

/* Analysis: the set of parallelizable positions found in a script */
typedef struct LR_DistillAnalysis {
    int32_t             num_positions;
    LR_DistillPosition *positions;
} LR_DistillAnalysis;

/* One round of distillation */
typedef struct LR_DistillRound {
    int32_t  num_chunks;
    char   **results;           /* per-chunk result strings (caller frees each) */
    double   elapsed_ms;        /* round wall-clock time */
    int32_t  num_sandboxes;     /* sandboxes used this round */
    /* JIT stats for this round */
    int      jit_compiles;      /* number of JIT compilations */
    int      jit_bailouts;      /* number of bailouts */
    int      jit_executions;    /* number of JIT native executions */
    size_t   jit_code_size;     /* total native code bytes */
} LR_DistillRound;

/* Complete distillation result (aggregated across rounds) */
typedef struct LR_DistillResult {
    int32_t          num_rounds;
    LR_DistillRound *rounds;
    char            *combined;       /* all chunks concatenated (last round) */
    double           total_ms;
    int32_t          total_positions;
    /* Aggregate JIT stats */
    int              jit_total_compiles;
    int              jit_total_bailouts;
    int              jit_total_executions;
    size_t           jit_total_code_size;
} LR_DistillResult;

/* Distillation configuration */
typedef struct LR_DistillConfig {
    int32_t  max_sandboxes;     /* max parallel sandboxes (default 16) */
    int32_t  max_rounds;        /* distillation rounds (default 3) */
    int32_t  timeout_ms;        /* per-sandbox timeout (0 = no limit) */
    int      verbose;           /* print progress to stderr */
    int      enable_jit;        /* 1 = enable JIT with results caching (default 1) */
    int      use_parallel;      /* 1 = parallel sandbox execution (default 1) */
} LR_DistillConfig;

/* ── Distillation API ──────────────────────────────────────────────────── */

/* Analyse a script and return the parallelisable positions found.
 * Returns NULL on parse failure.  Caller must free with
 * lr_iome586_distill_analysis_free(). */
LR_DistillAnalysis *lr_iome586_distill_analyze(LRContext *ctx,
    const char *source, size_t source_len);

/* Free an analysis result. */
void lr_iome586_distill_analysis_free(LR_DistillAnalysis *analysis);

/* Run distillation on a script.
 *   rt      – the main runtime (used for sandbox creation, timeout, logging)
 *   source  – the JS source text
 *   config  – distillation parameters (NULL = use defaults)
 * Returns NULL on error.  Caller must free with
 * lr_iome586_distill_result_free(). */
LR_DistillResult *lr_iome586_distill_run(LR_Runtime *rt,
    const char *source, size_t source_len,
    const LR_DistillConfig *config);

/* Free a distillation result. */
void lr_iome586_distill_result_free(LR_DistillResult *result);

/* Fill a config with default values. */
void lr_iome586_distill_config_default(LR_DistillConfig *cfg);

#endif /* LR_IOME586_H */
