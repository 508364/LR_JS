/*
 * L/R_JS - System Information API (system)
 *
 * Provides read-only system information: OS name, version, kernel,
 * CPU, GPU, RAM, hostname, uptime, and architecture.
 *
 * All data is sourced from standard Linux /proc and /sys filesystems.
 * No privileged operations are needed for reading system info.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_runtime.h"
#include "lr_platform.h"
#ifdef _WIN32
#include "lr_posix_win.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif

/* Linux provides sysinfo() via <sys/sysinfo.h>; macOS/BSD do not. */
#if LR_PLATFORM_LINUX
#include <sys/sysinfo.h>
#endif

/* ── Helper: read first line from a file, strip newline ───────────────── */

static char *read_first_line(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char buf[512];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    /* Strip trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    return strdup(buf);
}

/* ── Helper: skip lines matching a prefix, then return value ──────────── */

static char *read_field(const char *path, const char *prefix)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char buf[512];
    size_t plen = strlen(prefix);
    char *result = NULL;

    while (fgets(buf, sizeof(buf), fp)) {
        if (strncmp(buf, prefix, plen) == 0) {
            const char *val = buf + plen;
            /* Skip ": " and tabs after key */
            while (*val == ' ' || *val == '\t' || *val == ':') val++;
            size_t len = strlen(val);
            while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r'))
                len--;
            result = strndup(val, len);
            break;
        }
    }
    fclose(fp);
    return result;
}

/* ── system.name() ────────────────────────────────────────────────────── */

static char *get_os_name(void)
{
    /* Try /etc/os-release first */
    char *name = read_field("/etc/os-release", "NAME=");
    if (name) {
        /* Strip quotes */
        size_t len = strlen(name);
        if (len >= 2 && name[0] == '"' && name[len - 1] == '"') {
            name[len - 1] = '\0';
            memmove(name, name + 1, len - 1);
        }
        return name;
    }
    /* Fallback to uname */
    struct utsname uts;
    if (uname(&uts) == 0)
        return strdup(uts.sysname);
    return strdup("Unknown");
}

/* ── system.version() ─────────────────────────────────────────────────── */

static char *get_os_version(void)
{
    char *version = read_field("/etc/os-release", "VERSION=");
    if (version) {
        /* Strip quotes */
        size_t len = strlen(version);
        if (len >= 2 && version[0] == '"' && version[len - 1] == '"') {
            version[len - 1] = '\0';
            memmove(version, version + 1, len - 1);
        }
        return version;
    }
    /* Fallback */
    char *pretty = read_field("/etc/os-release", "PRETTY_NAME=");
    if (pretty) {
        size_t len = strlen(pretty);
        if (len >= 2 && pretty[0] == '"' && pretty[len - 1] == '"') {
            pretty[len - 1] = '\0';
            memmove(pretty, pretty + 1, len - 1);
        }
        return pretty;
    }
    return strdup("Unknown");
}

/* ── system.kernel() ──────────────────────────────────────────────────── */

static char *get_kernel_version(void)
{
    struct utsname uts;
    if (uname(&uts) == 0)
        return strdup(uts.release);
    return read_first_line("/proc/version");
}

/* ── system.arch() ────────────────────────────────────────────────────── */

static char *get_architecture(void)
{
    struct utsname uts;
    if (uname(&uts) == 0)
        return strdup(uts.machine);
    return strdup("Unknown");
}

/* ── system.cpu() ─────────────────────────────────────────────────────── */

static char *get_cpu_name(void)
{
    char *model = read_field("/proc/cpuinfo", "model name");
    if (model) return model;
    /* ARM fallback */
    char *impl = read_field("/proc/cpuinfo", "CPU implementer");
    char *part = read_field("/proc/cpuinfo", "CPU part");
    if (impl && part) {
        char buf[128];
        snprintf(buf, sizeof(buf), "ARM implementer %s part %s", impl, part);
        free(impl); free(part);
        return strdup(buf);
    }
    free(impl); free(part);
    return strdup("Unknown");
}

/* ── system.cpuCount() ────────────────────────────────────────────────── */

static int get_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

/* ── system.gpu() ─────────────────────────────────────────────────────── */

static char *get_gpu_info(void)
{
    /* Try /sys/class/drm/ for GPU device names */
    /* Look for renderD* or card* directories */
    for (int i = 0; i < 8; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/device", i);
        FILE *fp = fopen(path, "r");
        if (fp) {
            fclose(fp);
            /* Read the vendor and device names */
            char vendor_path[128], device_path[128];
            snprintf(vendor_path, sizeof(vendor_path),
                     "/sys/class/drm/card%d/device/vendor", i);
            snprintf(device_path, sizeof(device_path),
                     "/sys/class/drm/card%d/device/device", i);

            char *vendor = read_first_line(vendor_path);
            char *device = read_first_line(device_path);

            if (vendor && device) {
                char buf[256];
                snprintf(buf, sizeof(buf), "PCI %s:%s", vendor, device);
                free(vendor); free(device);
                /* Try to get a more descriptive name from modalias */
                char modalias_path[128];
                snprintf(modalias_path, sizeof(modalias_path),
                         "/sys/class/drm/card%d/device/modalias", i);
                char *modalias = read_first_line(modalias_path);
                if (modalias) {
                    char *result = strdup(buf);
                    free(modalias);
                    return result;
                }
                return strdup(buf);
            }
            free(vendor); free(device);
        }
    }
    return strdup("Unknown");
}

/* ── system.ram() ─────────────────────────────────────────────────────── */

static void get_ram_info(long long *total, long long *used, long long *free)
{
    *total = 0; *used = 0; *free = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;

    char buf[256];
    long long mem_total = 0, mem_free = 0, mem_available = 0, buffers = 0, cached = 0;
    int has_buffers = 0, has_cached = 0, has_available = 0;

    while (fgets(buf, sizeof(buf), fp)) {
        if (sscanf(buf, "MemTotal: %lld kB", &mem_total) == 1) continue;
        if (sscanf(buf, "MemFree: %lld kB", &mem_free) == 1) continue;
        if (sscanf(buf, "MemAvailable: %lld kB", &mem_available) == 1) {
            has_available = 1; continue;
        }
        if (sscanf(buf, "Buffers: %lld kB", &buffers) == 1) {
            has_buffers = 1; continue;
        }
        if (sscanf(buf, "Cached: %lld kB", &cached) == 1) {
            has_cached = 1; continue;
        }
    }
    fclose(fp);

    *total = mem_total * 1024;  /* kB → bytes */
    *free = mem_free * 1024;

    if (has_available) {
        *free = mem_available * 1024;
    } else if (has_buffers && has_cached) {
        *free = (mem_free + buffers + cached) * 1024;
    }

    *used = *total - *free;
    if (*used < 0) *used = 0;
}

/* ── system.uptime() ──────────────────────────────────────────────────── */

static double get_uptime(void)
{
    /* Try /proc/uptime first (Linux) */
    char *line = read_first_line("/proc/uptime");
    if (line) {
        double uptime;
        if (sscanf(line, "%lf", &uptime) == 1) {
            free(line);
            return uptime;
        }
        free(line);
    }
#if LR_PLATFORM_LINUX
    /* Fallback to sysinfo */
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return (double)si.uptime;
#endif
    return 0.0;
}

/* ── system.hostname() ────────────────────────────────────────────────── */

static char *get_hostname(void)
{
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        return strdup(buf);
    }
    return strdup("Unknown");
}

/* ══════════════════════════════════════════════════════════════════════════
 *  JS API Functions
 * ══════════════════════════════════════════════════════════════════════════ */

static JSValue sys_name(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char *name = get_os_name();
    JSValue ret = JS_NewString(ctx, name ? name : "Unknown");
    free(name);
    return ret;
}

static JSValue sys_version(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char *ver = get_os_version();
    JSValue ret = JS_NewString(ctx, ver ? ver : "Unknown");
    free(ver);
    return ret;
}

static JSValue sys_kernel(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char *kern = get_kernel_version();
    JSValue ret = JS_NewString(ctx, kern ? kern : "Unknown");
    free(kern);
    return ret;
}

static JSValue sys_arch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char *arch = get_architecture();
    JSValue ret = JS_NewString(ctx, arch ? arch : "Unknown");
    free(arch);
    return ret;
}

static JSValue sys_cpu(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char *cpu = get_cpu_name();
    JSValue ret = JS_NewString(ctx, cpu ? cpu : "Unknown");
    free(cpu);
    return ret;
}

static JSValue sys_cpuCount(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, get_cpu_count());
}

static JSValue sys_gpu(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char *gpu = get_gpu_info();
    JSValue ret = JS_NewString(ctx, gpu ? gpu : "Unknown");
    free(gpu);
    return ret;
}

static JSValue sys_ram(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    long long total, used, free_ram;
    get_ram_info(&total, &used, &free_ram);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "total", JS_NewFloat64(ctx, (double)total));
    JS_SetPropertyStr(ctx, obj, "used",  JS_NewFloat64(ctx, (double)used));
    JS_SetPropertyStr(ctx, obj, "free",  JS_NewFloat64(ctx, (double)free_ram));
    return obj;
}

static JSValue sys_uptime(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, get_uptime());
}

static JSValue sys_hostname(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char *host = get_hostname();
    JSValue ret = JS_NewString(ctx, host ? host : "Unknown");
    free(host);
    return ret;
}

static JSValue sys_info(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    char *name = get_os_name();
    char *ver = get_os_version();
    char *kern = get_kernel_version();
    char *arch = get_architecture();
    char *cpu = get_cpu_name();
    char *gpu = get_gpu_info();
    char *host = get_hostname();
    int cpu_count = get_cpu_count();
    double uptime = get_uptime();
    long long ram_total, ram_used, ram_free;
    get_ram_info(&ram_total, &ram_used, &ram_free);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name",     JS_NewString(ctx, name ? name : "Unknown"));
    JS_SetPropertyStr(ctx, obj, "version",  JS_NewString(ctx, ver ? ver : "Unknown"));
    JS_SetPropertyStr(ctx, obj, "kernel",   JS_NewString(ctx, kern ? kern : "Unknown"));
    JS_SetPropertyStr(ctx, obj, "arch",     JS_NewString(ctx, arch ? arch : "Unknown"));
    JS_SetPropertyStr(ctx, obj, "cpu",      JS_NewString(ctx, cpu ? cpu : "Unknown"));
    JS_SetPropertyStr(ctx, obj, "cpuCount", JS_NewInt32(ctx, cpu_count));
    JS_SetPropertyStr(ctx, obj, "gpu",      JS_NewString(ctx, gpu ? gpu : "Unknown"));
    JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, host ? host : "Unknown"));
    JS_SetPropertyStr(ctx, obj, "uptime",   JS_NewFloat64(ctx, uptime));

    JSValue ram_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ram_obj, "total", JS_NewFloat64(ctx, (double)ram_total));
    JS_SetPropertyStr(ctx, ram_obj, "used",  JS_NewFloat64(ctx, (double)ram_used));
    JS_SetPropertyStr(ctx, ram_obj, "free",  JS_NewFloat64(ctx, (double)ram_free));
    JS_SetPropertyStr(ctx, obj, "ram", ram_obj);

    free(name); free(ver); free(kern); free(arch);
    free(cpu); free(gpu); free(host);
    return obj;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_sysinfo_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue sys = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, sys, "name",     JS_NewCFunction(ctx, sys_name,     "name",     0));
    JS_SetPropertyStr(ctx, sys, "version",  JS_NewCFunction(ctx, sys_version,  "version",  0));
    JS_SetPropertyStr(ctx, sys, "kernel",   JS_NewCFunction(ctx, sys_kernel,   "kernel",   0));
    JS_SetPropertyStr(ctx, sys, "arch",     JS_NewCFunction(ctx, sys_arch,     "arch",     0));
    JS_SetPropertyStr(ctx, sys, "cpu",      JS_NewCFunction(ctx, sys_cpu,      "cpu",      0));
    JS_SetPropertyStr(ctx, sys, "cpuCount", JS_NewCFunction(ctx, sys_cpuCount, "cpuCount", 0));
    JS_SetPropertyStr(ctx, sys, "gpu",      JS_NewCFunction(ctx, sys_gpu,      "gpu",      0));
    JS_SetPropertyStr(ctx, sys, "ram",      JS_NewCFunction(ctx, sys_ram,      "ram",      0));
    JS_SetPropertyStr(ctx, sys, "uptime",   JS_NewCFunction(ctx, sys_uptime,   "uptime",   0));
    JS_SetPropertyStr(ctx, sys, "hostname", JS_NewCFunction(ctx, sys_hostname, "hostname", 0));
    JS_SetPropertyStr(ctx, sys, "info",     JS_NewCFunction(ctx, sys_info,     "info",     0));

    JS_SetPropertyStr(ctx, global, "system", sys);
    JS_FreeValue(ctx, global);

    lr_log(rt, LR_LOG_DEBUG, "SystemInfo API initialized");
}