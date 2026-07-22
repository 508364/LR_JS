/*
 * L/R_JS - Terminal API (term)
 *
 * Provides command execution via popen(3) for normal commands.
 * When execution fails with a permission error (EACCES/EPERM),
 * and a LR_TerminalWrapper is configured, it falls back to the wrapper.
 *
 * The wrapper is responsible for requesting OS-level permission (UAC on
 * Windows, polkit on Linux, etc.) and performing the actual execution.
 *
 * If no wrapper is set, permission-denied executions throw a JS Error.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include "lr_runtime.h"

/* ── Terminal wrapper API ──────────────────────────────────────────────── */

void lr_terminal_set_wrapper(LR_Runtime *rt, LR_TerminalWrapper *wrapper)
{
    rt->terminal_wrapper = wrapper;
}

LR_TerminalWrapper *lr_terminal_get_wrapper(LR_Runtime *rt)
{
    return rt->terminal_wrapper;
}

void lr_terminal_result_free(LR_TerminalResult *result)
{
    free(result->stdout_data);
    free(result->stderr_data);
    free(result->error);
    memset(result, 0, sizeof(*result));
}

/* ── Helper: try wrapper fallback on permission error ──────────────────── */

static int try_wrapper_fallback(LR_Runtime *rt, const char *command,
                                 const char *stdin_data, size_t stdin_len,
                                 LR_TerminalResult *result)
{
    LR_TerminalWrapper *wrapper = rt->terminal_wrapper;
    if (!wrapper || !wrapper->execute)
        return -1;  /* no wrapper available */
    return wrapper->execute(wrapper->user_data, command, "run",
                            stdin_data, stdin_len, result);
}

/* ── Helper: build JS error ────────────────────────────────────────────── */

static JSValue make_js_error(JSContext *ctx, const char *prefix,
                              const char *command, int errnum)
{
    char buf[512];
    const char *err_str = strerror(errnum);
    snprintf(buf, sizeof(buf), "%s '%s': %s", prefix, command, err_str);
    return JS_ThrowTypeError(ctx, "%s", buf);
}

/* ── Helper: build result object from LR_TerminalResult ────────────────── */

static JSValue result_to_js(JSContext *ctx, LR_TerminalResult *result)
{
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "exitCode",
                      JS_NewInt32(ctx, result->exit_code));
    JS_SetPropertyStr(ctx, obj, "stdout",
                      JS_NewStringLen(ctx,
                          result->stdout_data ? result->stdout_data : "",
                          result->stdout_len));
    JS_SetPropertyStr(ctx, obj, "stderr",
                      JS_NewStringLen(ctx,
                          result->stderr_data ? result->stderr_data : "",
                          result->stderr_len));
    return obj;
}

/* ── Helper: run command via popen, fill LR_TerminalResult ─────────────── */

static int run_popen(const char *command, LR_TerminalResult *result)
{
    /*
     * Use a shell wrapper to capture both stdout and stderr separately.
     * We run: sh -c 'command' >stdout_tmp 2>stderr_tmp; echo EXIT=$?
     * But that requires temp files. Instead, use a simpler approach:
     * run the command and capture combined output via popen, then
     * get the exit code from pclose.
     *
     * For separate stdout/stderr capture, we need a more complex approach
     * with pipes. Let's use mkstemp for temp files.
     */

    char stdout_template[] = "/tmp/lr_term_stdout_XXXXXX";
    char stderr_template[] = "/tmp/lr_term_stderr_XXXXXX";
    int stdout_fd = mkstemp(stdout_template);
    int stderr_fd = mkstemp(stderr_template);

    if (stdout_fd < 0 || stderr_fd < 0) {
        if (stdout_fd >= 0) { close(stdout_fd); unlink(stdout_template); }
        if (stderr_fd >= 0) { close(stderr_fd); unlink(stderr_template); }
        return -1;
    }
    close(stdout_fd);
    close(stderr_fd);

    /* Build the shell command: run command, capture stdout/stderr to temp files */
    char shell_cmd[16384];
    int n = snprintf(shell_cmd, sizeof(shell_cmd),
                     "(%s) >'%s' 2>'%s'; echo __LR_EXIT__=$?",
                     command, stdout_template, stderr_template);
    if (n < 0 || (size_t)n >= sizeof(shell_cmd)) {
        unlink(stdout_template);
        unlink(stderr_template);
        return -1;
    }

    /* Execute via popen */
    FILE *fp = popen(shell_cmd, "r");
    if (!fp) {
        int err = errno;
        unlink(stdout_template);
        unlink(stderr_template);
        if (err == EACCES || err == EPERM)
            return -2;  /* permission error, caller should try wrapper */
        return -1;
    }

    /* Read popen output (just the exit code line) */
    char buf[256];
    int exit_code = -1;
    while (fgets(buf, sizeof(buf), fp)) {
        int scanned;
        if (sscanf(buf, "__LR_EXIT__=%d", &scanned) == 1) {
            exit_code = scanned;
            break;
        }
    }
    if (exit_code < 0) exit_code = -1;
    pclose(fp);

    /* Read stdout from temp file */
    FILE *out_fp = fopen(stdout_template, "rb");
    if (out_fp) {
        fseek(out_fp, 0, SEEK_END);
        long len = ftell(out_fp);
        rewind(out_fp);
        if (len > 0) {
            result->stdout_data = malloc((size_t)len + 1);
            if (result->stdout_data) {
                size_t nread = fread(result->stdout_data, 1, (size_t)len, out_fp);
                (void)nread;
                result->stdout_data[(size_t)len] = '\0';
                result->stdout_len = (size_t)len;
            }
        }
        fclose(out_fp);
    }
    unlink(stdout_template);

    /* Read stderr from temp file */
    FILE *err_fp = fopen(stderr_template, "rb");
    if (err_fp) {
        fseek(err_fp, 0, SEEK_END);
        long len = ftell(err_fp);
        rewind(err_fp);
        if (len > 0) {
            result->stderr_data = malloc((size_t)len + 1);
            if (result->stderr_data) {
                size_t nread = fread(result->stderr_data, 1, (size_t)len, err_fp);
                (void)nread;
                result->stderr_data[(size_t)len] = '\0';
                result->stderr_len = (size_t)len;
            }
        }
        fclose(err_fp);
    }
    unlink(stderr_template);

    result->exit_code = exit_code;
    result->error_code = 0;
    return 0;
}

/* ── term.run(command) → { exitCode, stdout, stderr } ─────────────────── */

static JSValue term_run(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "term.run requires a command argument");

    const char *command = JS_ToCString(ctx, argv[0]);
    if (!command) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);
    LR_TerminalResult result;
    memset(&result, 0, sizeof(result));

    int ret = run_popen(command, &result);
    if (ret == -2) {
        /* Permission error: try wrapper fallback */
        memset(&result, 0, sizeof(result));
        if (try_wrapper_fallback(rt, command, NULL, 0, &result) == 0) {
            JSValue js_result = result_to_js(ctx, &result);
            lr_terminal_result_free(&result);
            JS_FreeCString(ctx, command);
            return js_result;
        }
        JS_FreeCString(ctx, command);
        return make_js_error(ctx, "term.run failed (permission denied)", command, EACCES);
    } else if (ret != 0) {
        JS_FreeCString(ctx, command);
        return make_js_error(ctx, "term.run failed", command, errno);
    }

    JSValue js_result = result_to_js(ctx, &result);
    lr_terminal_result_free(&result);
    JS_FreeCString(ctx, command);
    return js_result;
}

/* ── term.runBatch(commands[]) → [{ exitCode, stdout, stderr }, ...] ──── */

static JSValue term_runBatch(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "term.runBatch requires an array of commands");

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);
    JSValue arr = argv[0];

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);
    if (len < 0) len = 0;

    JSValue result_arr = JS_NewArray(ctx);

    for (int32_t i = 0; i < len; i++) {
        JSValue cmd_val = JS_GetPropertyUint32(ctx, arr, i);
        const char *command = JS_ToCString(ctx, cmd_val);

        if (!command) {
            /* Push error result */
            LR_TerminalResult err_result;
            memset(&err_result, 0, sizeof(err_result));
            err_result.exit_code = -1;
            err_result.error_code = EINVAL;
            JSValue err_obj = result_to_js(ctx, &err_result);
            JS_SetPropertyUint32(ctx, result_arr, i, err_obj);
            JS_FreeValue(ctx, cmd_val);
            continue;
        }

        LR_TerminalResult result;
        memset(&result, 0, sizeof(result));

        int ret = run_popen(command, &result);
        if (ret == -2) {
            /* Permission error: try wrapper fallback for this batch.
             * Per the batch re-authorization model, each batch of
             * privileged operations re-requests permission. */
            memset(&result, 0, sizeof(result));
            if (try_wrapper_fallback(rt, command, NULL, 0, &result) != 0) {
                result.exit_code = -1;
                result.error_code = EACCES;
            }
        } else if (ret != 0) {
            result.exit_code = -1;
            result.error_code = errno;
        }

        JSValue js_result = result_to_js(ctx, &result);
        lr_terminal_result_free(&result);
        JS_SetPropertyUint32(ctx, result_arr, i, js_result);
        JS_FreeCString(ctx, command);
        JS_FreeValue(ctx, cmd_val);
    }

    /* Set array length */
    JS_SetPropertyStr(ctx, result_arr, "length", JS_NewInt32(ctx, len));

    return result_arr;
}

/* ── Helper: free spawned callback values if they came from GetProperty ── */

static void free_spawn_callbacks(JSContext *ctx, int from_prop,
                                  JSValue on_stdout, JSValue on_stderr,
                                  JSValue on_exit)
{
    /* If callbacks came from JS_GetPropertyStr, they must be freed.
     * If they are argv references, the caller owns them. */
    if (from_prop) {
        JS_FreeValue(ctx, on_stdout);
        JS_FreeValue(ctx, on_stderr);
        JS_FreeValue(ctx, on_exit);
    }
}

/*
 * ── term.spawn(command, callbacks) → void
 *
 * Executes a command and provides real-time output via callbacks.
 *
 * callbacks can be:
 *   - A function: treated as onStdout (called for each stdout line)
 *   - An object: { onStdout(line), onStderr(line), onExit(code) }
 *
 * stdout is streamed in real-time via popen.
 * stderr is captured to a temp file and delivered after the process ends.
 * Supports the same permission-aware wrapper fallback as term.run().
 */

static JSValue term_spawn(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "term.spawn requires a command argument");

    const char *command = JS_ToCString(ctx, argv[0]);
    if (!command) return JS_EXCEPTION;

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    /* Parse callbacks */
    JSValue on_stdout = JS_UNDEFINED;
    JSValue on_stderr = JS_UNDEFINED;
    JSValue on_exit   = JS_UNDEFINED;
    int callbacks_from_prop = 0;

    if (argc >= 2) {
        if (JS_IsFunction(ctx, argv[1])) {
            /* Simple callback: treat as onStdout */
            on_stdout = argv[1];
        } else if (JS_IsObject(argv[1])) {
            on_stdout = JS_GetPropertyStr(ctx, argv[1], "onStdout");
            on_stderr = JS_GetPropertyStr(ctx, argv[1], "onStderr");
            on_exit   = JS_GetPropertyStr(ctx, argv[1], "onExit");
            callbacks_from_prop = 1;
        }
    }

    /* ── Create temp file for stderr ────────────────────────────────── */
    char stderr_template[] = "/tmp/lr_term_spawn_stderr_XXXXXX";
    int stderr_fd = mkstemp(stderr_template);
    if (stderr_fd < 0) {
        JSValue err = make_js_error(ctx, "term.spawn failed to create temp file", command, errno);
        free_spawn_callbacks(ctx, callbacks_from_prop, on_stdout, on_stderr, on_exit);
        JS_FreeCString(ctx, command);
        return err;
    }
    close(stderr_fd);

    /* ── Build shell command ────────────────────────────────────────── */
    char shell_cmd[16384];
    int n = snprintf(shell_cmd, sizeof(shell_cmd),
                     "(%s) 2>'%s'; echo __LR_SPAWN_EXIT__=$?",
                     command, stderr_template);
    if (n < 0 || (size_t)n >= sizeof(shell_cmd)) {
        unlink(stderr_template);
        JSValue err = make_js_error(ctx, "term.spawn command too long", command, E2BIG);
        free_spawn_callbacks(ctx, callbacks_from_prop, on_stdout, on_stderr, on_exit);
        JS_FreeCString(ctx, command);
        return err;
    }

    /* ── Execute via popen ──────────────────────────────────────────── */
    FILE *fp = popen(shell_cmd, "r");
    if (!fp) {
        int saved_errno = errno;
        unlink(stderr_template);
        if ((saved_errno == EACCES || saved_errno == EPERM) && rt->terminal_wrapper) {
            /* Fallback: wrapper doesn't support streaming, so run entire
             * command via wrapper then call callbacks with full output. */
            LR_TerminalResult wrapper_result;
            memset(&wrapper_result, 0, sizeof(wrapper_result));
            if (try_wrapper_fallback(rt, command, NULL, 0, &wrapper_result) == 0) {
                /* Call onStdout with full stdout */
                if (JS_IsFunction(ctx, on_stdout) && wrapper_result.stdout_data) {
                    JSValue line = JS_NewStringLen(ctx, wrapper_result.stdout_data,
                                                    wrapper_result.stdout_len);
                    JS_Call(ctx, on_stdout, JS_UNDEFINED, 1, &line);
                    JS_FreeValue(ctx, line);
                }
                /* Call onStderr with full stderr */
                if (JS_IsFunction(ctx, on_stderr) && wrapper_result.stderr_data) {
                    JSValue line = JS_NewStringLen(ctx, wrapper_result.stderr_data,
                                                    wrapper_result.stderr_len);
                    JS_Call(ctx, on_stderr, JS_UNDEFINED, 1, &line);
                    JS_FreeValue(ctx, line);
                }
                /* Call onExit */
                if (JS_IsFunction(ctx, on_exit)) {
                    JSValue code = JS_NewInt32(ctx, wrapper_result.exit_code);
                    JS_Call(ctx, on_exit, JS_UNDEFINED, 1, &code);
                    JS_FreeValue(ctx, code);
                }
                lr_terminal_result_free(&wrapper_result);
                free_spawn_callbacks(ctx, callbacks_from_prop, on_stdout, on_stderr, on_exit);
                JS_FreeCString(ctx, command);
                return JS_UNDEFINED;
            }
        }
        JSValue err = make_js_error(ctx, "term.spawn failed", command, saved_errno);
        free_spawn_callbacks(ctx, callbacks_from_prop, on_stdout, on_stderr, on_exit);
        JS_FreeCString(ctx, command);
        return err;
    }

    /* ── Read stdout line by line (real-time) ───────────────────────── */
    char line_buf[4096];
    int exit_code = -1;

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        /* Check for exit code marker */
        int scanned;
        if (sscanf(line_buf, "__LR_SPAWN_EXIT__=%d", &scanned) == 1) {
            exit_code = scanned;
            break;
        }
        /* Call onStdout callback */
        if (JS_IsFunction(ctx, on_stdout)) {
            JSValue line = JS_NewString(ctx, line_buf);
            JS_Call(ctx, on_stdout, JS_UNDEFINED, 1, &line);
            JS_FreeValue(ctx, line);
        }
    }
    if (exit_code < 0) exit_code = -1;
    pclose(fp);

    /* ── Read stderr from temp file ─────────────────────────────────── */
    FILE *err_fp = fopen(stderr_template, "rb");
    if (err_fp) {
        char err_buf[4096];
        while (fgets(err_buf, sizeof(err_buf), err_fp)) {
            if (JS_IsFunction(ctx, on_stderr)) {
                JSValue line = JS_NewString(ctx, err_buf);
                JS_Call(ctx, on_stderr, JS_UNDEFINED, 1, &line);
                JS_FreeValue(ctx, line);
            }
        }
        fclose(err_fp);
    }
    unlink(stderr_template);

    /* ── Call onExit callback ───────────────────────────────────────── */
    if (JS_IsFunction(ctx, on_exit)) {
        JSValue code = JS_NewInt32(ctx, exit_code);
        JS_Call(ctx, on_exit, JS_UNDEFINED, 1, &code);
        JS_FreeValue(ctx, code);
    }

    free_spawn_callbacks(ctx, callbacks_from_prop, on_stdout, on_stderr, on_exit);
    JS_FreeCString(ctx, command);
    return JS_UNDEFINED;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_terminal_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue term = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, term, "run",
                      JS_NewCFunction(ctx, term_run, "run", 1));
    JS_SetPropertyStr(ctx, term, "runBatch",
                      JS_NewCFunction(ctx, term_runBatch, "runBatch", 1));
    JS_SetPropertyStr(ctx, term, "spawn",
                      JS_NewCFunction(ctx, term_spawn, "spawn", 2));

    JS_SetPropertyStr(ctx, global, "term", term);
    JS_FreeValue(ctx, global);

    lr_log(rt, LR_LOG_DEBUG, "Terminal API initialized (privilege-aware)");
}