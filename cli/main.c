/*
 * L/R_JS - Command Line Interface
 * Pure C, ES2022-compatible JavaScript runtime
 *
 * Usage: lr_js [options] [script.js] [-- args...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "../src/lr_platform.h"
#include "../src/lr_runtime.h"

/* ── Global runtime for signal handling ────────────────────────────────── */

static LR_Runtime *g_rt = NULL;

static void sigint_handler(int sig)
{
    (void)sig;
    fprintf(stderr, "\nInterrupted. Shutting down...\n");
    if (g_rt) {
        lr_event_loop_stop(g_rt);
    }
}

/* ── Print usage ──────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    printf("L/R_JS v%s - Lightweight/Runtime JavaScript Engine\n", LR_JS_VERSION_STRING);
    printf("Usage: %s [options] [script.js] [-- args...]\n\n", prog);
    printf("Options:\n");
    printf("  -e, --eval <code>      Evaluate JavaScript code\n");
    printf("  -m, --module            Treat input as ES module\n");
    printf("  -s, --strict            Force strict mode\n");
    printf("  -i, --interactive       Start interactive REPL\n");
    printf("  -l, --log-level <n>     Set log level (0=none, 1=error, 2=warn, 3=info, 4=debug)\n");
    printf("  --memory-limit <mb>     Set memory limit in MB\n");
    printf("  --min-memory <bytes>    Set minimum required system memory (default: 1GB)\n");
    printf("  --no-memory-check       Skip system memory check at startup\n");
    printf("  --gc-stress             Enable GC stress mode\n");
    printf("  --gc-generational        Enable generational GC (default)\n");
    printf("  --gc-incremental         Enable incremental GC (default)\n");
    printf("  --gc-manual              Disable automatic GC\n");
    printf("  --gc-nursery-size <mb>   Set nursery size in MB\n");
    printf("  --gc-pause-target <ms>   Set target max GC pause in ms\n");
    printf("  --gc-stats               Print GC statistics on exit\n");
    printf("  --bytecode-cache <dir>   Enable .lrfile bytecode cache (set cache dir)\n");
    printf("  --bytecode-stats         Print bytecode cache statistics on exit\n");
    printf("  --sandbox-log <dir>      Enable per-sandbox log files (set log dir)\n");
    printf("  --dump-bytecode          Dump compiled bytecode\n");
    printf("  --strip-debug           Strip debug info\n");
    printf("  --timeout <ms>          Set execution timeout in ms\n");
    printf("  -h, --help               Show this help\n");
    printf("  -v, --version            Show version\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s script.js\n", prog);
    printf("  %s -e \"console.log('Hello, World!')\"\n", prog);
    printf("  %s -m app.mjs\n", prog);
    printf("  %s -i\n", prog);
    printf("\n");
    printf("Built-in Browser APIs:\n");
    printf("  console, setTimeout, setInterval, fetch, URL, URLSearchParams,\n");
    printf("  TextEncoder, TextDecoder, atob, btoa, Event, EventTarget,\n");
    printf("  CustomEvent, AbortController, performance, crypto, localStorage,\n");
    printf("  sessionStorage, queueMicrotask\n");
}

/* ── REPL ──────────────────────────────────────────────────────────────── */

static void repl_run(LR_Runtime *rt)
{
    char line[4096];
    int line_num = 1;

    printf("L/R_JS v%s REPL (type .exit to quit, .help for help)\n", LR_JS_VERSION_STRING);

    while (1) {
        printf("lr> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (len == 0) continue;

        /* Handle REPL commands */
        if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
            break;
        }
        if (strcmp(line, ".help") == 0) {
            printf("REPL Commands:\n");
            printf("  .exit, .quit    Exit REPL\n");
            printf("  .help           Show this help\n");
            printf("  .clear          Clear screen\n");
            printf("  .gc             Trigger garbage collection\n");
            printf("  .gc_stats       Show GC statistics\n");
            printf("  .bc_stats       Show bytecode cache statistics\n");
            printf("  .memory         Show memory usage\n");
            continue;
        }
        if (strcmp(line, ".clear") == 0) {
            printf("\033[2J\033[H");
            fflush(stdout);
            continue;
        }
        if (strcmp(line, ".gc") == 0) {
            lr_gc(rt);
            printf("GC triggered.\n");
            continue;
        }
        if (strcmp(line, ".gc_stats") == 0) {
            lr_gc_print_stats(rt, stdout);
            continue;
        }
        if (strcmp(line, ".bc_stats") == 0) {
            lr_bytecode_cache_stats(rt, stdout);
            continue;
        }
        if (strcmp(line, ".memory") == 0) {
            lr_dump_memory_usage(rt, stdout);
            continue;
        }

        /* Evaluate expression */
        char filename[32];
        snprintf(filename, sizeof(filename), "<repl:%d>", line_num++);

        int ret = lr_eval(rt, line, strlen(line), filename);
        if (ret < 0) {
            fprintf(stderr, "Error: %s\n", lr_get_last_error(rt));
            lr_clear_last_error(rt);
        }
    }
    printf("\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    lr_socket_init();

    /* Parse arguments */
    const char *eval_code = NULL;
    const char *script_file = NULL;
    int is_module = 0;
    int interactive = 0;
    int print_gc_stats = 0;
    int print_bytecode_stats = 0;
    const char *bytecode_cache_dir = NULL;
    const char *sandbox_log_dir = NULL;
    LR_Config cfg;

    lr_config_default(&cfg);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("L/R_JS v%s\n", LR_JS_VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eval") == 0) {
            if (i + 1 < argc) eval_code = argv[++i];
            else { fprintf(stderr, "Error: -e requires an argument\n"); return 1; }
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--module") == 0) {
            is_module = 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--strict") == 0) {
            cfg.strict_mode = 1;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
            interactive = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log-level") == 0) {
            if (i + 1 < argc) {
                cfg.log_level = (LR_LogLevel)atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--memory-limit") == 0) {
            if (i + 1 < argc) {
                cfg.memory_limit = (size_t)atoi(argv[++i]) * 1024 * 1024;
            }
        } else if (strcmp(argv[i], "--min-memory") == 0) {
            if (i + 1 < argc) {
                cfg.min_system_memory = (size_t)atoll(argv[++i]);
            }
        } else if (strcmp(argv[i], "--no-memory-check") == 0) {
            cfg.skip_memory_check = 1;
        } else if (strcmp(argv[i], "--gc-stress") == 0) {
            cfg.gc_mode = LR_GC_MODE_STRESS;
        } else if (strcmp(argv[i], "--gc-generational") == 0) {
            cfg.gc_mode = LR_GC_MODE_GENERATIONAL;
            cfg.gc_generational = 1;
            cfg.gc_incremental = 0;
        } else if (strcmp(argv[i], "--gc-incremental") == 0) {
            cfg.gc_mode = LR_GC_MODE_INCREMENTAL;
            cfg.gc_generational = 1;
            cfg.gc_incremental = 1;
        } else if (strcmp(argv[i], "--gc-manual") == 0) {
            cfg.gc_mode = LR_GC_MODE_MANUAL;
            cfg.gc_generational = 0;
            cfg.gc_incremental = 0;
        } else if (strcmp(argv[i], "--gc-nursery-size") == 0) {
            if (i + 1 < argc) {
                cfg.gc_nursery_size = (size_t)atoi(argv[++i]) * 1024 * 1024;
            }
        } else if (strcmp(argv[i], "--gc-pause-target") == 0) {
            if (i + 1 < argc) {
                cfg.gc_pause_target_ns = (int64_t)atoi(argv[++i]) * 1000000LL;
            }
        } else if (strcmp(argv[i], "--gc-stats") == 0) {
            print_gc_stats = 1;
        } else if (strcmp(argv[i], "--bytecode-cache") == 0) {
            if (i + 1 < argc) {
                bytecode_cache_dir = argv[++i];
            }
        } else if (strcmp(argv[i], "--bytecode-stats") == 0) {
            print_bytecode_stats = 1;
        } else if (strcmp(argv[i], "--sandbox-log") == 0) {
            if (i + 1 < argc) {
                sandbox_log_dir = argv[++i];
            }
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            cfg.dump_bytecode = 1;
        } else if (strcmp(argv[i], "--strip-debug") == 0) {
            cfg.strip_debug_info = 1;
        } else if (strcmp(argv[i], "--timeout") == 0) {
            if (i + 1 < argc) cfg.timeout_ms = atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            script_file = argv[i];
            break; /* rest are script args */
        }
    }

    /* Apply bytecode cache and sandbox log config */
    if (bytecode_cache_dir) {
        cfg.bytecode_cache_dir = (char *)bytecode_cache_dir;
    }
    if (sandbox_log_dir) {
        cfg.sandbox_log_dir = (char *)sandbox_log_dir;
    }

    /* Create runtime */
    g_rt = lr_runtime_new(&cfg);
    if (!g_rt) {
        fprintf(stderr, "Failed to create L/R_JS runtime\n");
        return 1;
    }

    /* Set signal handler */
    signal(SIGINT, sigint_handler);

    int exit_code = 0;

    if (eval_code) {
        /* Evaluate inline code */
        if (is_module) {
            exit_code = lr_eval_module(g_rt, eval_code, strlen(eval_code), "<eval>");
        } else {
            exit_code = lr_eval(g_rt, eval_code, strlen(eval_code), "<eval>");
        }
        if (exit_code < 0) {
            fprintf(stderr, "Error: %s\n", lr_get_last_error(g_rt));
        }
    } else if (script_file) {
        /* Evaluate script file */
        if (is_module) {
            size_t buf_len;
            uint8_t *buf = lr_load_file(g_rt, script_file, &buf_len);
            if (buf) {
                exit_code = lr_eval_module(g_rt, (const char *)buf, buf_len, script_file);
                free(buf);
            } else {
                fprintf(stderr, "Error: %s\n", lr_get_last_error(g_rt));
                exit_code = 1;
            }
        } else {
            exit_code = lr_eval_file(g_rt, script_file);
        }
        if (exit_code < 0) {
            fprintf(stderr, "Error: %s\n", lr_get_last_error(g_rt));
        }
    } else if (interactive) {
        repl_run(g_rt);
    } else {
        /* Default: interactive mode if stdin is a terminal */
        repl_run(g_rt);
    }

    /* Run event loop for pending async operations (timers, microtasks,
     * worker messages). Returns immediately if there is nothing to do. */
    lr_event_loop_run(g_rt);

    /* Print GC statistics if requested */
    if (print_gc_stats) {
        lr_gc_print_stats(g_rt, stdout);
    }

    /* Print bytecode cache statistics if requested */
    if (print_bytecode_stats) {
        lr_bytecode_cache_stats(g_rt, stdout);
    }

    lr_runtime_free(g_rt);
    g_rt = NULL;
    lr_socket_cleanup();
    return exit_code;
}