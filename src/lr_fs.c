/*
 * L/R_JS - File System API (fs)
 *
 * Provides basic file operations (read, write, list, stat, etc.) for
 * normal files. When an operation fails with a permission error (EACCES),
 * and a LR_FileWrapper is configured, it falls back to the wrapper.
 *
 * The wrapper is responsible for requesting OS-level permission (UAC on
 * Windows, polkit on Linux, etc.) and performing the actual I/O.
 *
 * If no wrapper is set, permission-denied operations throw a JS Error.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include "lr_runtime.h"

/* ── File wrapper API ──────────────────────────────────────────────────── */

void lr_file_set_wrapper(LR_Runtime *rt, LR_FileWrapper *wrapper)
{
    rt->file_wrapper = wrapper;
}

LR_FileWrapper *lr_file_get_wrapper(LR_Runtime *rt)
{
    return rt->file_wrapper;
}

void lr_file_result_free(LR_FileResult *result)
{
    free(result->data);
    free(result->error);
    if (result->entries) {
        for (int i = 0; i < result->entry_count; i++)
            free(result->entries[i]);
        free(result->entries);
    }
    memset(result, 0, sizeof(*result));
}

/* ── Helper: try wrapper fallback on permission error ──────────────────── */

static int try_wrapper_fallback(LR_Runtime *rt, const char *path,
                                 const char *operation, const void *data,
                                 size_t data_len, const char *extra,
                                 LR_FileResult *result)
{
    LR_FileWrapper *wrapper = rt->file_wrapper;
    if (!wrapper || !wrapper->execute)
        return -1;  /* no wrapper available */
    return wrapper->execute(wrapper->user_data, path, operation,
                            data, data_len, extra, result);
}

/* ── Helper: build JS error from errno or file result ──────────────────── */

static JSValue make_js_error(JSContext *ctx, const char *prefix,
                              const char *path, int errnum)
{
    char buf[512];
    const char *err_str = strerror(errnum);
    snprintf(buf, sizeof(buf), "%s '%s': %s", prefix, path, err_str);
    return JS_ThrowTypeError(ctx, "%s", buf);
}

/* ── fs.readFile(path) → string ────────────────────────────────────────── */

static JSValue fs_readFile(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readFile requires a path argument");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);
    char *data = NULL;
    long len = 0;

    /* Try direct read */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        int err = errno;
        if (err == EACCES || err == EPERM) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "read_file", NULL, 0, NULL, &result) == 0) {
                JSValue ret = JS_NewStringLen(ctx, result.data, result.data_len);
                lr_file_result_free(&result);
                JS_FreeCString(ctx, path);
                return ret;
            }
        }
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.readFile failed", path, err);
    }

    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    rewind(fp);

    data = malloc(len + 1);
    if (data) {
        size_t nread = fread(data, 1, len, fp);
        (void)nread;
        data[len] = '\0';
    }
    fclose(fp);

    JSValue ret = JS_NewStringLen(ctx, data ? data : "", len);
    free(data);
    JS_FreeCString(ctx, path);
    return ret;
}

/* ── fs.writeFile(path, data) → void ───────────────────────────────────── */

static JSValue fs_writeFile(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "fs.writeFile requires path and data arguments");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *data = JS_ToCString(ctx, argv[1]);
    if (!data) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "write_file", data, strlen(data), NULL, &result) == 0) {
                lr_file_result_free(&result);
                JS_FreeCString(ctx, data);
                JS_FreeCString(ctx, path);
                return JS_UNDEFINED;
            }
        }
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.writeFile failed", path, err);
    }

    fwrite(data, 1, strlen(data), fp);
    fclose(fp);

    JS_FreeCString(ctx, data);
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── fs.appendFile(path, data) → void ──────────────────────────────────── */

static JSValue fs_appendFile(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "fs.appendFile requires path and data arguments");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *data = JS_ToCString(ctx, argv[1]);
    if (!data) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    FILE *fp = fopen(path, "ab");
    if (!fp) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "write_file", data, strlen(data), NULL, &result) == 0) {
                lr_file_result_free(&result);
                JS_FreeCString(ctx, data);
                JS_FreeCString(ctx, path);
                return JS_UNDEFINED;
            }
        }
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.appendFile failed", path, err);
    }

    fwrite(data, 1, strlen(data), fp);
    fclose(fp);

    JS_FreeCString(ctx, data);
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── fs.readdir(path) → array ──────────────────────────────────────────── */

static JSValue fs_readdir(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readdir requires a path argument");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    DIR *dir = opendir(path);
    if (!dir) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "read_dir", NULL, 0, NULL, &result) == 0) {
                JSValue arr = JS_NewArray(ctx);
                for (int i = 0; i < result.entry_count; i++) {
                    JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, result.entries[i]));
                }
                lr_file_result_free(&result);
                JS_FreeCString(ctx, path);
                return arr;
            }
        }
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.readdir failed", path, err);
    }

    JSValue arr = JS_NewArray(ctx);
    int idx = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, entry->d_name));
    }
    closedir(dir);

    JS_FreeCString(ctx, path);
    return arr;
}

/* ── fs.mkdir(path) → void ─────────────────────────────────────────────── */

static JSValue fs_mkdir(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.mkdir requires a path argument");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    if (mkdir(path, 0755) != 0) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "create_dir", NULL, 0, NULL, &result) == 0) {
                lr_file_result_free(&result);
                JS_FreeCString(ctx, path);
                return JS_UNDEFINED;
            }
        }
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.mkdir failed", path, err);
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── fs.rmdir(path) → void ─────────────────────────────────────────────── */

static JSValue fs_rmdir(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.rmdir requires a path argument");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    if (rmdir(path) != 0) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "remove_dir", NULL, 0, NULL, &result) == 0) {
                lr_file_result_free(&result);
                JS_FreeCString(ctx, path);
                return JS_UNDEFINED;
            }
        }
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.rmdir failed", path, err);
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── fs.unlink(path) → void ────────────────────────────────────────────── */

static JSValue fs_unlink(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.unlink requires a path argument");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    if (unlink(path) != 0) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "delete_file", NULL, 0, NULL, &result) == 0) {
                lr_file_result_free(&result);
                JS_FreeCString(ctx, path);
                return JS_UNDEFINED;
            }
        }
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.unlink failed", path, err);
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── fs.rename(oldPath, newPath) → void ────────────────────────────────── */

static JSValue fs_rename(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "fs.rename requires oldPath and newPath arguments");

    const char *old_path = JS_ToCString(ctx, argv[0]);
    if (!old_path) return JS_EXCEPTION;
    const char *new_path = JS_ToCString(ctx, argv[1]);
    if (!new_path) { JS_FreeCString(ctx, old_path); return JS_EXCEPTION; }

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    if (rename(old_path, new_path) != 0) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, old_path, "rename", NULL, 0, new_path, &result) == 0) {
                lr_file_result_free(&result);
                JS_FreeCString(ctx, new_path);
                JS_FreeCString(ctx, old_path);
                return JS_UNDEFINED;
            }
        }
        JS_FreeCString(ctx, new_path);
        JS_FreeCString(ctx, old_path);
        return make_js_error(ctx, "fs.rename failed", old_path, err);
    }

    JS_FreeCString(ctx, new_path);
    JS_FreeCString(ctx, old_path);
    return JS_UNDEFINED;
}

/* ── fs.stat(path) → object ────────────────────────────────────────────── */

static JSValue fs_stat(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.stat requires a path argument");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    struct stat st;
    if (stat(path, &st) != 0) {
        int err = errno;
        if ((err == EACCES || err == EPERM) && rt->file_wrapper) {
            LR_FileResult result;
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, path, "stat", NULL, 0, NULL, &result) == 0) {
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, (int64_t)result.file_size));
                JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, result.is_dir));
                JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, result.is_file));
                lr_file_result_free(&result);
                JS_FreeCString(ctx, path);
                return obj;
            }
        }
        JS_FreeCString(ctx, path);
        return make_js_error(ctx, "fs.stat failed", path, err);
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, (int64_t)st.st_size));
    JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, S_ISDIR(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, S_ISREG(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "mtime", JS_NewInt64(ctx, (int64_t)st.st_mtime));

    JS_FreeCString(ctx, path);
    return obj;
}

/* ── fs.exists(path) → boolean ─────────────────────────────────────────── */

static JSValue fs_exists(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.exists requires a path argument");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    struct stat st;
    int ret = stat(path, &st);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ret == 0);
}

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_fs_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue fs = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, fs, "readFile",
                      JS_NewCFunction(ctx, fs_readFile, "readFile", 1));
    JS_SetPropertyStr(ctx, fs, "writeFile",
                      JS_NewCFunction(ctx, fs_writeFile, "writeFile", 2));
    JS_SetPropertyStr(ctx, fs, "appendFile",
                      JS_NewCFunction(ctx, fs_appendFile, "appendFile", 2));
    JS_SetPropertyStr(ctx, fs, "readdir",
                      JS_NewCFunction(ctx, fs_readdir, "readdir", 1));
    JS_SetPropertyStr(ctx, fs, "mkdir",
                      JS_NewCFunction(ctx, fs_mkdir, "mkdir", 1));
    JS_SetPropertyStr(ctx, fs, "rmdir",
                      JS_NewCFunction(ctx, fs_rmdir, "rmdir", 1));
    JS_SetPropertyStr(ctx, fs, "unlink",
                      JS_NewCFunction(ctx, fs_unlink, "unlink", 1));
    JS_SetPropertyStr(ctx, fs, "rename",
                      JS_NewCFunction(ctx, fs_rename, "rename", 2));
    JS_SetPropertyStr(ctx, fs, "stat",
                      JS_NewCFunction(ctx, fs_stat, "stat", 1));
    JS_SetPropertyStr(ctx, fs, "exists",
                      JS_NewCFunction(ctx, fs_exists, "exists", 1));

    JS_SetPropertyStr(ctx, global, "fs", fs);
    JS_FreeValue(ctx, global);

    lr_log(rt, LR_LOG_DEBUG, "File System API initialized (privilege-aware)");
}