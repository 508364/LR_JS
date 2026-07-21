/*
 * L/R_JS - Bytecode Cache (.lrfile)
 * Pure C, persistent disk cache for compiled JS bytecode.
 *
 * Cache format (.lrfile):
 *   ┌──────────────────────────────────────────────────┐
 *   │ Magic:    "LRBC"                          4 bytes│
 *   │ Version:  uint32_t                        4 bytes│
 *   │ Flags:    uint32_t                        4 bytes│
 *   │ SrcHash:  FNV-1a 64-bit                    8 bytes│
 *   │ SrcMtime: int64_t (Unix timestamp)        8 bytes│
 *   │ SrcSize:  uint64_t                        8 bytes│
 *   │ BcLen:    uint32_t                        4 bytes│
 *   │ Bytecode: variable                              │
 *   └──────────────────────────────────────────────────┘
 *
 * Usage:
 *   - Set cache_dir in config to enable
 *   - If cache_dir is NULL/empty, caching is disabled
 *   - Supports absolute and relative paths
 *   - Cache is keyed by FNV-1a 64-bit hash of source path + content
 *   - Stale cache (source modified) is automatically invalidated
 */

#ifndef LR_BYTECODE_H
#define LR_BYTECODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "lr_platform.h"
#include <sys/stat.h>

/* Forward declarations */
typedef struct LR_Runtime      LR_Runtime;
typedef struct LR_BytecodeCache LR_BytecodeCache;

/* ── Cache entry ───────────────────────────────────────────────────────── */

typedef struct LR_BytecodeEntry {
    uint64_t        source_hash;        /* FNV-1a 64-bit hash */
    char            source_path[1024];  /* Original source path */
    uint8_t        *bytecode;           /* Serialized bytecode */
    size_t           bytecode_len;
    int64_t          source_mtime;      /* Source file modification time */
    int64_t          cached_at;         /* When this entry was cached */
    int              hit_count;
    struct LR_BytecodeEntry *next;
} LR_BytecodeEntry;

/* ── Bytecode cache ────────────────────────────────────────────────────── */

#define LR_BYTECODE_MAGIC    0x4342524C  /* "LRBC" */
#define LR_BYTECODE_VERSION  2
#define LR_BYTECODE_FLAG_STRICT     (1 << 0)
#define LR_BYTECODE_FLAG_MODULE     (1 << 1)
#define LR_BYTECODE_FLAG_COMPRESSED (1 << 2)  /* LZ4 compressed bytecode */

struct LR_BytecodeCache {
    char            *cache_dir;         /* Cache directory path (NULL = disabled) */
    int              enabled;           /* 1 = cache enabled */
    int              compression;       /* 1 = use LZ4 compression */

    /* In-memory LRU cache (optional, for fast lookups) */
    LR_BytecodeEntry *entries;
    int              entry_count;
    int              max_entries;

    /* Statistics */
    int64_t          hit_count;
    int64_t          miss_count;
    int64_t          store_count;
    int64_t          invalid_count;
    int64_t          bytes_stored;
    int64_t          bytes_loaded;
    int64_t          bytes_saved;       /* Bytes saved by compression */

    /* Thread safety */
    pthread_mutex_t  mutex;

    /* Back-reference */
    LR_Runtime      *runtime;
};

/* ── API ────────────────────────────────────────────────────────────────── */

/* Initialize bytecode cache. cache_dir can be NULL to disable. */
void lr_bytecode_cache_init(LR_BytecodeCache *cache, LR_Runtime *rt,
                            const char *cache_dir);

/* Destroy bytecode cache. */
void lr_bytecode_cache_destroy(LR_BytecodeCache *cache);

/* Enable or disable the cache at runtime. */
void lr_bytecode_cache_set_enabled(LR_BytecodeCache *cache, int enabled);

/* Change cache directory. */
int  lr_bytecode_cache_set_dir(LR_BytecodeCache *cache, const char *dir);

/* Enable/disable LZ4 compression. */
void lr_bytecode_cache_set_compression(LR_BytecodeCache *cache, int enable);

/* ── Cache operations ──────────────────────────────────────────────────── */

/* Load bytecode from cache for a given source file.
 * Returns NULL if not cached or stale. Caller must free with free(). */
uint8_t *lr_bytecode_cache_load(LR_BytecodeCache *cache,
                                const char *source_path,
                                const uint8_t *source_data,
                                size_t source_len,
                                size_t *out_bytecode_len);

/* Store bytecode in cache for a given source file.
 * Returns 0 on success, -1 on error. */
int  lr_bytecode_cache_store(LR_BytecodeCache *cache,
                             const char *source_path,
                             const uint8_t *source_data,
                             size_t source_len,
                             const uint8_t *bytecode,
                             size_t bytecode_len,
                             int flags);

/* Invalidate cache entry for a source file. */
void lr_bytecode_cache_invalidate(LR_BytecodeCache *cache,
                                  const char *source_path);

/* Clear all cache entries (both memory and disk). */
void lr_bytecode_cache_clear_ctx(LR_BytecodeCache *cache);

/* ── Statistics ─────────────────────────────────────────────────────────── */

/* Print cache statistics. */
void lr_bytecode_cache_stats_ctx(LR_BytecodeCache *cache, FILE *fp);

/* ── Utilities ──────────────────────────────────────────────────────────── */

/* Compute FNV-1a 64-bit hash of data. */
uint64_t lr_fnv1a_64(const uint8_t *data, size_t len);

/* Compute cache file path for a source hash.
 * Returns path in out_buf (must be at least 4096 bytes). */
int  lr_bytecode_cache_path(LR_BytecodeCache *cache, uint64_t source_hash,
                            char *out_buf, size_t buf_size);

#endif /* LR_BYTECODE_H */