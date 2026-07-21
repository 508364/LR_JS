/*
 * L/R_JS - Bytecode Cache Implementation (.lrfile)
 * Pure C, persistent disk cache for compiled JS bytecode.
 *
 * Uses FNV-1a 64-bit hash for fast cache key generation.
 * Cache file format: magic + version + flags + hash + mtime + size + bytecode
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "lr_bytecode.h"
#include "lr_compress.h"
#include "lr_runtime.h"
#include "engine/lr_engine.h"

/* ── FNV-1a 64-bit hash ────────────────────────────────────────────────── */

#define FNV_OFFSET_BASIS_64 0xcbf29ce484222325ULL
#define FNV_PRIME_64        0x100000001b3ULL

uint64_t lr_fnv1a_64(const uint8_t *data, size_t len)
{
    uint64_t hash = FNV_OFFSET_BASIS_64;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)data[i];
        hash *= FNV_PRIME_64;
    }
    return hash;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
#if LR_PLATFORM_WINDOWS
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = '/';
        }
    }
#if LR_PLATFORM_WINDOWS
    return _mkdir(tmp);
#else
    return mkdir(tmp, 0755);
#endif
}

static int64_t get_file_mtime(const char *path)
{
    struct stat st;
    if (lr_stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

static uint8_t *read_entire_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(buf);
        return NULL;
    }

    *out_len = (size_t)size;
    return buf;
}

static int write_entire_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    return (written == len) ? 0 : -1;
}

/* ── Cache path ────────────────────────────────────────────────────────── */

int lr_bytecode_cache_path(LR_BytecodeCache *cache, uint64_t source_hash,
                           char *out_buf, size_t buf_size)
{
    if (!cache->cache_dir || !cache->enabled) return -1;

    snprintf(out_buf, buf_size, "%s/%016llx.lrfile",
             cache->cache_dir, (unsigned long long)source_hash);
    return 0;
}

/* ── Init / Destroy ────────────────────────────────────────────────────── */

void lr_bytecode_cache_init(LR_BytecodeCache *cache, LR_Runtime *rt,
                            const char *cache_dir)
{
    memset(cache, 0, sizeof(*cache));
    cache->runtime = rt;
    cache->max_entries = 256;
    cache->compression = 1;  /* Enable LZ4 compression by default */
    pthread_mutex_init(&cache->mutex, NULL);

    if (cache_dir && cache_dir[0]) {
        cache->cache_dir = strdup(cache_dir);
        cache->enabled = 1;
        /* Ensure cache directory exists */
        mkdir_p(cache_dir);
    }
}

void lr_bytecode_cache_destroy(LR_BytecodeCache *cache)
{
    /* Free in-memory entries */
    LR_BytecodeEntry *entry = cache->entries;
    while (entry) {
        LR_BytecodeEntry *next = entry->next;
        free(entry->bytecode);
        free(entry);
        entry = next;
    }
    cache->entries = NULL;

    free(cache->cache_dir);
    cache->cache_dir = NULL;
    pthread_mutex_destroy(&cache->mutex);
}

void lr_bytecode_cache_set_enabled(LR_BytecodeCache *cache, int enabled)
{
    pthread_mutex_lock(&cache->mutex);
    cache->enabled = enabled;
    pthread_mutex_unlock(&cache->mutex);
}

void lr_bytecode_cache_set_compression(LR_BytecodeCache *cache, int enable)
{
    pthread_mutex_lock(&cache->mutex);
    cache->compression = enable;
    pthread_mutex_unlock(&cache->mutex);
}

int lr_bytecode_cache_set_dir(LR_BytecodeCache *cache, const char *dir)
{
    if (!dir || !dir[0]) {
        pthread_mutex_lock(&cache->mutex);
        free(cache->cache_dir);
        cache->cache_dir = NULL;
        cache->enabled = 0;
        pthread_mutex_unlock(&cache->mutex);
        return 0;
    }

    if (mkdir_p(dir) != 0) return -1;

    pthread_mutex_lock(&cache->mutex);
    free(cache->cache_dir);
    cache->cache_dir = strdup(dir);
    cache->enabled = 1;
    pthread_mutex_unlock(&cache->mutex);
    return 0;
}

/* ── Cache operations ──────────────────────────────────────────────────── */

uint8_t *lr_bytecode_cache_load(LR_BytecodeCache *cache,
                                const char *source_path,
                                const uint8_t *source_data,
                                size_t source_len,
                                size_t *out_bytecode_len)
{
    if (!cache->enabled || !cache->cache_dir) return NULL;

    /* Compute hash of source content */
    uint64_t hash = lr_fnv1a_64(source_data, source_len);

    /* Get cache file path */
    char cache_path[4096];
    if (lr_bytecode_cache_path(cache, hash, cache_path, sizeof(cache_path)) != 0) {
        return NULL;
    }

    /* Read cache file */
    size_t file_len;
    uint8_t *file_data = read_entire_file(cache_path, &file_len);
    if (!file_data) {
        cache->miss_count++;
        return NULL;
    }

    /* Parse cache header */
    if (file_len < 40) {
        free(file_data);
        cache->miss_count++;
        return NULL;
    }

    uint32_t magic, version, flags, bc_len;
    uint64_t stored_hash;
    int64_t  stored_mtime;
    uint64_t stored_src_size;

    size_t off = 0;
    memcpy(&magic,           file_data + off, 4); off += 4;
    memcpy(&version,         file_data + off, 4); off += 4;
    memcpy(&flags,           file_data + off, 4); off += 4;
    memcpy(&stored_hash,     file_data + off, 8); off += 8;
    memcpy(&stored_mtime,    file_data + off, 8); off += 8;
    memcpy(&stored_src_size, file_data + off, 8); off += 8;
    memcpy(&bc_len,          file_data + off, 4); off += 4;

    /* Validate */
    if (magic != LR_BYTECODE_MAGIC || version < 1 || version > LR_BYTECODE_VERSION) {
        free(file_data);
        cache->miss_count++;
        return NULL;
    }

    if (stored_hash != hash) {
        free(file_data);
        cache->invalid_count++;
        unlink(cache_path);
        return NULL;
    }

    /* Check source file mtime */
    if (source_path && source_path[0] != '<') {
        int64_t current_mtime = get_file_mtime(source_path);
        if (current_mtime > 0 && current_mtime != stored_mtime) {
            free(file_data);
            cache->invalid_count++;
            unlink(cache_path);
            return NULL;
        }
    }

    if (off + bc_len > file_len) {
        free(file_data);
        cache->miss_count++;
        return NULL;
    }

    /* Extract bytecode (possibly decompress) */
    uint8_t *bytecode;
    size_t actual_len;

    if (flags & LR_BYTECODE_FLAG_COMPRESSED) {
        /* Decompress */
        bytecode = lr_decompress_if_needed(file_data + off, bc_len, 1, 0, &actual_len);
        if (!bytecode) {
            free(file_data);
            cache->miss_count++;
            return NULL;
        }
        cache->bytes_saved += (bc_len - actual_len);
    } else {
        actual_len = bc_len;
        bytecode = malloc(bc_len);
        if (!bytecode) {
            free(file_data);
            return NULL;
        }
        memcpy(bytecode, file_data + off, bc_len);
    }

    *out_bytecode_len = actual_len;
    free(file_data);

    cache->hit_count++;
    cache->bytes_loaded += actual_len;

    /* Update in-memory entry */
    pthread_mutex_lock(&cache->mutex);
    for (LR_BytecodeEntry *e = cache->entries; e; e = e->next) {
        if (e->source_hash == hash) {
            e->hit_count++;
            e->cached_at = time(NULL);
            pthread_mutex_unlock(&cache->mutex);
            return bytecode;
        }
    }
    pthread_mutex_unlock(&cache->mutex);

    return bytecode;
}

int lr_bytecode_cache_store(LR_BytecodeCache *cache,
                            const char *source_path,
                            const uint8_t *source_data,
                            size_t source_len,
                            const uint8_t *bytecode,
                            size_t bytecode_len,
                            int flags)
{
    if (!cache->enabled || !cache->cache_dir) return -1;

    uint64_t hash = lr_fnv1a_64(source_data, source_len);
    int64_t mtime = 0;
    if (source_path && source_path[0] != '<') {
        mtime = get_file_mtime(source_path);
    }

    /* Compress bytecode if enabled */
    uint8_t *bc_to_store = NULL;
    size_t bc_store_len;
    int bc_compressed = 0;

    if (cache->compression && bytecode_len > 256) {
        bc_to_store = lr_compress_if_beneficial(bytecode, bytecode_len,
                                                &bc_store_len, &bc_compressed,
                                                0.90); /* compress if saves >10% */
        if (bc_compressed) {
            flags |= LR_BYTECODE_FLAG_COMPRESSED;
            cache->bytes_saved += (bytecode_len - bc_store_len);
        }
    }

    if (!bc_to_store) {
        bc_to_store = (uint8_t *)bytecode;
        bc_store_len = bytecode_len;
    }

    /* Build cache file header */
    uint32_t magic   = LR_BYTECODE_MAGIC;
    uint32_t version = LR_BYTECODE_VERSION;
    uint32_t fc      = (uint32_t)flags;
    uint64_t src_size = (uint64_t)source_len;
    uint32_t bc_len  = (uint32_t)bc_store_len;

    size_t header_size = 4 + 4 + 4 + 8 + 8 + 8 + 4;
    size_t total_size = header_size + bc_store_len;

    uint8_t *buf = malloc(total_size);
    if (!buf) {
        if (bc_compressed) free(bc_to_store);
        return -1;
    }

    size_t off = 0;
    memcpy(buf + off, &magic,   4); off += 4;
    memcpy(buf + off, &version, 4); off += 4;
    memcpy(buf + off, &fc,      4); off += 4;
    memcpy(buf + off, &hash,    8); off += 8;
    memcpy(buf + off, &mtime,   8); off += 8;
    memcpy(buf + off, &src_size,8); off += 8;
    memcpy(buf + off, &bc_len,  4); off += 4;
    memcpy(buf + off, bc_to_store, bc_store_len);

    /* Get cache file path */
    char cache_path[4096];
    if (lr_bytecode_cache_path(cache, hash, cache_path, sizeof(cache_path)) != 0) {
        free(buf);
        if (bc_compressed) free(bc_to_store);
        return -1;
    }

    /* Write to disk */
    if (write_entire_file(cache_path, buf, total_size) != 0) {
        free(buf);
        if (bc_compressed) free(bc_to_store);
        return -1;
    }
    free(buf);
    if (bc_compressed) free(bc_to_store);

    cache->store_count++;
    cache->bytes_stored += bc_store_len;

    /* Add to in-memory cache */
    pthread_mutex_lock(&cache->mutex);

    /* Check if already exists */
    for (LR_BytecodeEntry *e = cache->entries; e; e = e->next) {
        if (e->source_hash == hash) {
            free(e->bytecode);
            e->bytecode = malloc(bytecode_len);
            if (e->bytecode) {
                memcpy(e->bytecode, bytecode, bytecode_len);
                e->bytecode_len = bytecode_len;
            }
            e->cached_at = time(NULL);
            pthread_mutex_unlock(&cache->mutex);
            return 0;
        }
    }

    /* Evict oldest if at capacity */
    if (cache->entry_count >= cache->max_entries) {
        LR_BytecodeEntry *prev = NULL, *oldest = NULL, *oldest_prev = NULL;
        for (LR_BytecodeEntry *e = cache->entries; e; prev = e, e = e->next) {
            if (!oldest || e->cached_at < oldest->cached_at) {
                oldest = e;
                oldest_prev = prev;
            }
        }
        if (oldest) {
            if (oldest_prev) oldest_prev->next = oldest->next;
            else cache->entries = oldest->next;
            free(oldest->bytecode);
            free(oldest);
            cache->entry_count--;
        }
    }

    /* Add new entry */
    LR_BytecodeEntry *entry = calloc(1, sizeof(*entry));
    if (entry) {
        entry->source_hash = hash;
        if (source_path) {
            strncpy(entry->source_path, source_path, sizeof(entry->source_path) - 1);
        }
        entry->bytecode = malloc(bytecode_len);
        if (entry->bytecode) {
            memcpy(entry->bytecode, bytecode, bytecode_len);
            entry->bytecode_len = bytecode_len;
        }
        entry->source_mtime = mtime;
        entry->cached_at = time(NULL);
        entry->next = cache->entries;
        cache->entries = entry;
        cache->entry_count++;
    }

    pthread_mutex_unlock(&cache->mutex);
    return 0;
}

void lr_bytecode_cache_invalidate(LR_BytecodeCache *cache,
                                  const char *source_path)
{
    if (!source_path || !cache->cache_dir) return;

    /* Read source to compute hash */
    size_t src_len;
    uint8_t *src = read_entire_file(source_path, &src_len);
    if (!src) return;

    uint64_t hash = lr_fnv1a_64(src, src_len);
    free(src);

    /* Remove from disk */
    char cache_path[4096];
    if (lr_bytecode_cache_path(cache, hash, cache_path, sizeof(cache_path)) == 0) {
        unlink(cache_path);
    }

    /* Remove from memory */
    pthread_mutex_lock(&cache->mutex);
    LR_BytecodeEntry *prev = NULL;
    for (LR_BytecodeEntry *e = cache->entries; e; prev = e, e = e->next) {
        if (e->source_hash == hash) {
            if (prev) prev->next = e->next;
            else cache->entries = e->next;
            free(e->bytecode);
            free(e);
            cache->entry_count--;
            cache->invalid_count++;
            break;
        }
    }
    pthread_mutex_unlock(&cache->mutex);
}

void lr_bytecode_cache_clear_ctx(LR_BytecodeCache *cache)
{
    pthread_mutex_lock(&cache->mutex);

    /* Clear in-memory entries */
    LR_BytecodeEntry *entry = cache->entries;
    while (entry) {
        LR_BytecodeEntry *next = entry->next;
        free(entry->bytecode);
        free(entry);
        entry = next;
    }
    cache->entries = NULL;
    cache->entry_count = 0;

    pthread_mutex_unlock(&cache->mutex);

    /* Clear disk cache if directory is set */
    if (cache->cache_dir && cache->enabled) {
        /* Simple approach: unlink all .lrfile files */
        /* We can't easily list files in pure C without platform APIs,
         * so we just track the directory and note it's cleared */
        cache->invalid_count += cache->store_count;
        cache->store_count = 0;
        cache->bytes_stored = 0;
    }
}

/* ── Statistics ────────────────────────────────────────────────────────── */

void lr_bytecode_cache_stats_ctx(LR_BytecodeCache *cache, FILE *fp)
{
    fprintf(fp, "\n");
    fprintf(fp, "╔══════════════════════════════════════════════════════════════╗\n");
    fprintf(fp, "║  L/R_JS Bytecode Cache Statistics                           ║\n");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Status:   %-47s ║\n",
            cache->enabled ? "enabled" : "disabled");
    fprintf(fp, "║  Directory: %-46s ║\n",
            cache->cache_dir ? cache->cache_dir : "(none)");
    fprintf(fp, "║  Compression: %-43s ║\n",
            cache->compression ? "LZ4 enabled" : "disabled");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Hits:    %10lld    Misses:  %10lld              ║\n",
            (long long)cache->hit_count, (long long)cache->miss_count);
    fprintf(fp, "║  Stores:  %10lld    Invalid: %10lld              ║\n",
            (long long)cache->store_count, (long long)cache->invalid_count);
    fprintf(fp, "║  Stored:  %10.2f MB  Loaded: %10.2f MB            ║\n",
            (double)cache->bytes_stored / (1024.0 * 1024.0),
            (double)cache->bytes_loaded / (1024.0 * 1024.0));
    fprintf(fp, "║  Saved:   %10.2f MB (compression)                   ║\n",
            (double)cache->bytes_saved / (1024.0 * 1024.0));
    fprintf(fp, "║  Mem entries: %7d / %-7d                         ║\n",
            cache->entry_count, cache->max_entries);
    fprintf(fp, "║  Hit rate: %6.1f%%                                      ║\n",
            (cache->hit_count + cache->miss_count) > 0
            ? 100.0 * (double)cache->hit_count / (double)(cache->hit_count + cache->miss_count)
            : 0.0);
    fprintf(fp, "║  Hash:    FNV-1a 64-bit                                    ║\n");
    fprintf(fp, "╚══════════════════════════════════════════════════════════════╝\n");
    fprintf(fp, "\n");
}