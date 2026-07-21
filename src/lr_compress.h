/*
 * L/R_JS - LZ4 Compression (Pure C, minimal implementation)
 *
 * Fast LZ4-compatible block compression for .lrfile bytecode cache.
 * Format: sequence of token + literals + match
 *   Token byte: upper 4 bits = literal length (0-15, 15+ means extra bytes)
 *               lower 4 bits = match length - 4 (0-15, 15+ means extra bytes)
 *
 * Compression ratio for JS bytecode: ~30-60% size reduction
 * Decompression speed: ~2-4 GB/s (single core)
 */

#ifndef LR_COMPRESS_H
#define LR_COMPRESS_H

#include <stdint.h>
#include <stddef.h>

/* ── LZ4 Block Compression ─────────────────────────────────────────────── */

/* Maximum compressed size for a given input size (worst case). */
#define LR_LZ4_COMPRESS_BOUND(n) ((size_t)(n) + (size_t)(n) / 255 + 16)

/* Compress data using LZ4 block format.
 * Returns compressed size, or 0 on error.
 * dst must be at least LR_LZ4_COMPRESS_BOUND(src_len) bytes. */
size_t lr_lz4_compress(const uint8_t *src, uint8_t *dst, size_t src_len);

/* Decompress LZ4 block data.
 * Returns decompressed size, or 0 on error.
 * dst_len must be the exact original size. */
size_t lr_lz4_decompress(const uint8_t *src, uint8_t *dst, size_t src_len,
                         size_t dst_len);

/* ── Utility ────────────────────────────────────────────────────────────── */

/* Compress if beneficial. Returns compressed data (malloc'd) and sets *out_len.
 * If compression doesn't save enough, returns NULL and copies original.
 * Caller must free() the returned buffer. */
uint8_t *lr_compress_if_beneficial(const uint8_t *data, size_t data_len,
                                   size_t *out_len, int *was_compressed,
                                   double min_ratio);

/* Decompress if was_compressed. Returns decompressed data (malloc'd).
 * Caller must free() the returned buffer. */
uint8_t *lr_decompress_if_needed(const uint8_t *data, size_t data_len,
                                 int was_compressed, size_t orig_len,
                                 size_t *out_len);

#endif /* LR_COMPRESS_H */