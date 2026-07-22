/*
 * L/R_JS - Example Host Program
 *
 * Demonstrates how to embed L/R_JS as a library.
 *
 * Compile:
 *   gcc -Iinclude -Isrc -Isrc/engine -o host_app host_main.c -llr_js -lm -lpthread -ldl
 *
 * Or:
 *   make example
 *   ./build/example_host
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lr_js.h"

int main(void)
{
    LR_Config cfg;
    lr_config_default(&cfg);
    cfg.log_level = LR_LOG_INFO;

    LR_Runtime *rt = lr_runtime_new(&cfg);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }

    printf("=== Evaluating JS ===\n");
    fflush(stdout);

    const char *code =
        "console.log('Hello from L/R_JS!');\n"
        "var r = term.run(\"echo embedded\");\n"
        "console.log('exitCode: ' + r.exitCode);\n"
        "console.log('stdout: ' + r.stdout);\n";

    int ret = lr_eval(rt, code, strlen(code), "<main>");
    if (ret < 0) {
        fprintf(stderr, "Error: %s\n", lr_get_last_error(rt));
        lr_clear_last_error(rt);
    }

    if (lr_event_loop_pending(rt))
        lr_event_loop_run(rt);

    printf("=== Done ===\n");
    fflush(stdout);

    lr_runtime_free(rt);
    return 0;
}