/* p1404_firewall.c - dynamic, exact-port PF mutation for private stream111. */
#include "p1404_firewall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>

extern void altscreen_log(const char *fmt, ...);

#define PF_CAPTURE_MAX (128u * 1024u)
#define PF_TRANSACTION_MS 1500u
#define PF_REAP_MS 100u
static volatile unsigned g_pf_guard;
/* Protected by the same lock as rule mutations. Fixed size, no queue or heap
 * lifetime; a later open of the same port invalidates an older cleanup lease. */
static uint32_t g_pf_port_generations[65536];
static uint32_t g_pf_generation;
static int g_pending_child = -1; /* guarded; never accumulate timed-out children */
/* Diagnostics are transaction-local under g_pf_guard. */
static const char *g_pf_stage;
static const char *g_pf_program;
static const char *g_pf_command;
static int g_pf_status;
static char g_pf_error[384];
static uint64_t pf_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (unsigned long)ts.tv_nsec / 1000000u;
}
static int pf_expired(uint64_t deadline) {
    uint64_t now = pf_now_ms();
    if (!now || now >= deadline) { errno = ETIMEDOUT; return 1; }
    return 0;
}
static int pf_lock(uint64_t deadline) {
    for (;;) {
        if (pf_expired(deadline)) return 0;
        if (__sync_lock_test_and_set(&g_pf_guard, 1u) == 0u) return 1;
        usleep(5000);
    }
}
static void pf_unlock(void) { __sync_lock_release(&g_pf_guard); }

static int line_contains(const char *line, size_t n, const char *token) {
    size_t i, len = strlen(token);
    for (i = 0; i + len <= n; ++i)
        if (!memcmp(line + i, token, len)) return 1;
    return 0;
}
static int is_blocker(const char *line, size_t n) {
    return n >= 5u && !memcmp(line, "block", 5u) &&
           line_contains(line, n, "in quick on carplay0") &&
           line_contains(line, n, " all");
}

static int port_token_matches(const char *line, size_t n, const char *prefix, size_t len) {
    return n >= len && !memcmp(line, prefix, len) &&
           (n == len || line[len] == ' ' || line[len] == '\t' || line[len] == '\r');
}
static int is_our_rule(const char *line, size_t n, uint16_t port) {
    char a[160], b[160];
    int na = snprintf(a, sizeof(a),
        "pass in quick on carplay0 proto tcp from any to any port = %u", (unsigned)port);
    int nb = snprintf(b, sizeof(b),
        "pass in quick on carplay0 proto tcp from any to any port %u", (unsigned)port);
    if (na <= 0 || nb <= 0) return 0;
    return port_token_matches(line, n, a, (size_t)na) ||
           port_token_matches(line, n, b, (size_t)nb);
}

static int append_bytes(char *out, size_t cap, size_t *used,
                        const char *src, size_t n) {
    if (!out || !used || !src || *used + n + 1u > cap) return 0;
    memcpy(out + *used, src, n);
    *used += n;
    out[*used] = 0;
    return 1;
}

int p1404_alt111_firewall_rewrite(const char *rules, uint16_t port, int enable,
                                  char *out, size_t out_cap, int *changed_out) {
    const char *cur, *nl;
    size_t used = 0;
    int found = 0, blocker = 0, changed = 0;
    char rule[192];
    int rule_n;
    if (changed_out) *changed_out = 0;
    if (!rules || !port || !out || out_cap < 2u) return 0;
    out[0] = 0;
    for (cur = rules; *cur; cur = nl ? nl + 1 : cur + strlen(cur)) {
        size_t n;
        nl = strchr(cur, '\n');
        n = nl ? (size_t)(nl - cur) : strlen(cur);
        if (is_our_rule(cur, n, port)) found = 1;
        if (!nl) break;
    }
    if (enable && found) {
        if (strlen(rules) + 1u > out_cap) return 0;
        memcpy(out, rules, strlen(rules) + 1u);
        return 1;
    }
    rule_n = snprintf(rule, sizeof(rule),
        "pass in quick on carplay0 proto tcp from any to any port = %u keep state\n",
        (unsigned)port);
    if (rule_n <= 0 || (size_t)rule_n >= sizeof(rule)) return 0;
    cur = rules;
    while (*cur) {
        size_t n;
        nl = strchr(cur, '\n');
        n = nl ? (size_t)(nl - cur) : strlen(cur);
        if (!enable && is_our_rule(cur, n, port)) {
            changed = 1;
        } else {
            if (enable && !blocker && is_blocker(cur, n)) {
                if (!append_bytes(out, out_cap, &used, rule, (size_t)rule_n)) return 0;
                blocker = 1;
                changed = 1;
            }
            if (!append_bytes(out, out_cap, &used, cur, n)) return 0;
            if (nl && !append_bytes(out, out_cap, &used, "\n", 1u)) return 0;
        }
        if (!nl) break;
        cur = nl + 1;
    }
    if (enable && !blocker) return 0; /* fail closed if expected terminal block moved */
    if (changed_out) *changed_out = changed;
    return 1;
}

/* QNX spawnp maps only stdio into the helper. No fork in the multithreaded
 * receiver, no shell, and no blocking pipe read/pclose/system in teardown. */
static const char *pf_program(void) {
#if defined(QSHIM_SPAWN_H) || defined(__QNXNTO__) || defined(ALT111_PF_QNX_TEST)
    /* dio_manager's inherited PATH need not include the administrative tools.
     * The boot recorder already proves /armle/sbin/pfctl on the target. */
    static const char *const paths[] = {
        "/armle/sbin/pfctl", "/mnt/app/armle/sbin/pfctl",
        "/sbin/pfctl", "/usr/sbin/pfctl", "/proc/boot/pfctl"
    };
    unsigned i;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        if (access(paths[i], 1 /* QNX X_OK */) == 0) return paths[i];
    errno = ENOENT;
    return NULL;
#else
    return "pfctl";
#endif
}

static int pf_spawn(const char *program, char *const argv[], int output,
                    int null_fd, int error_fd) {
#if defined(QSHIM_SPAWN_H) || defined(__QNXNTO__) || defined(ALT111_PF_QNX_TEST)
    int map[3];
    map[0] = null_fd; map[1] = output; map[2] = error_fd;
    return spawnp(program, 3, map, NULL, argv, NULL);
#else
    /* Host tests exercise the same deadline/locking/rules code. */
    posix_spawn_file_actions_t actions;
    pid_t child;
    int rc;
    extern char **environ;
    rc = posix_spawn_file_actions_init(&actions);
    if (rc) { errno = rc; return -1; }
    rc = posix_spawn_file_actions_adddup2(&actions, null_fd, 0);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, output, 1);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, error_fd, 2);
    if (!rc) rc = posix_spawnp(&child, program, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) { errno = rc; return -1; }
    return (int)child;
#endif
}

static int run_pf(char *const argv[], int output, uint64_t deadline) {
    int child, null_fd, error_fd = -1, status, result, ok = 0, saved;
    char error_path[96];
    ssize_t bytes;
    unsigned i;
    uint64_t reap_deadline;
    if (g_pending_child > 0) {
        result = waitpid(g_pending_child, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno != ECHILD)) {
            errno = ETIMEDOUT; return 0;
        }
        g_pending_child = -1;
    }
    if (pf_expired(deadline)) return 0;
    g_pf_command = argv[1];
    g_pf_stage = "resolve_helper";
    g_pf_status = -1; g_pf_error[0] = 0;
    g_pf_program = pf_program();
    if (!g_pf_program) return 0;
    g_pf_stage = "open_helper_stdio";
    null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) return 0;
    snprintf(error_path, sizeof(error_path), "/tmp/carplay_alt111_pf_%ld.stderr",
             (long)getpid());
    error_fd = open(error_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (error_fd < 0) { saved = errno; close(null_fd); errno = saved; return 0; }
    unlink(error_path);
    g_pf_stage = "spawn_helper";
    child = pf_spawn(g_pf_program, argv, output >= 0 ? output : null_fd,
                     null_fd, error_fd);
    saved = errno;
    close(null_fd);
    errno = saved;
    if (child < 0) goto done;
    g_pf_stage = "wait_helper";
    for (;;) {
        result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            g_pf_status = status;
            if (status == 0) { ok = 1; goto done; }
            g_pf_stage = "helper_exit";
            errno = EIO; goto done;
        }
        if (result < 0 && errno == ECHILD) goto done;
        if (result < 0 && errno != EINTR) break;
        if (pf_expired(deadline)) break;
        usleep(5000);
    }
    saved = errno;
    kill(child, 9); /* SIGKILL, identical in QNX and host */
    reap_deadline = pf_now_ms() + PF_REAP_MS;
    for (;;) {
        result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) break;
        if (pf_expired(reap_deadline)) { g_pending_child = child; break; }
        usleep(5000);
    }
    errno = saved;
done:
    saved = errno;
    if (lseek(error_fd, 0, SEEK_SET) >= 0) {
        bytes = read(error_fd, g_pf_error, sizeof(g_pf_error) - 1u);
        if (bytes > 0) {
            g_pf_error[bytes] = 0;
            for (i = 0; i < (unsigned)bytes; ++i)
                if ((unsigned char)g_pf_error[i] < 32u) g_pf_error[i] = ' ';
        }
    }
    close(error_fd);
    errno = saved;
    return ok;
}

static char *capture_rules(uint16_t port, uint64_t deadline) {
    char path[96], *buf = NULL;
    char *argv[] = { "pfctl", "-sr", NULL };
    size_t used = 0;
    int fd, saved;
    ssize_t n;
    snprintf(path, sizeof(path), "/tmp/carplay_alt111_pf_%ld_%u.capture",
             (long)getpid(), (unsigned)port);
    g_pf_stage = "open_capture";
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return NULL;
    unlink(path); /* anonymous regular file; a stalled helper cannot hold EOF */
    if (!run_pf(argv, fd, deadline)) goto done;
    g_pf_stage = "read_capture";
    if (lseek(fd, 0, SEEK_SET) < 0) goto done;
    buf = (char *)malloc(PF_CAPTURE_MAX + 1u);
    if (!buf) goto done;
    do {
        n = read(fd, buf + used, PF_CAPTURE_MAX - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto bad;
        if (!n) break;
        used += (size_t)n;
    } while (used < PF_CAPTURE_MAX);
    if (!used || used == PF_CAPTURE_MAX || memchr(buf, 0, used)) goto bad;
    buf[used] = 0;
    goto done;
bad:
    free(buf); buf = NULL; errno = EIO;
done:
    saved = errno; close(fd); errno = saved;
    return buf;
}

static int apply_rule(uint16_t port, int enable, struct p1404_pf_lease *lease) {
    char *before = NULL, *after = NULL;
    char path[96];
    char *argv[] = { "pfctl", "-f", path, NULL };
    FILE *fp = NULL;
    size_t cap;
    int changed = 0, ok = 0, n, saved, write_ok;
    uint64_t start, deadline;
    if (!port || (!enable && lease && !lease->generation)) return 0;
    start = pf_now_ms();
    if (!start) { errno = EIO; return 0; }
    deadline = start + PF_TRANSACTION_MS;
    if (!pf_lock(deadline)) {
        altscreen_log("ERROR PHASE=ALT111_PF_TRANSACTION port=%u enable=%d stage=lock errno=%d", (unsigned)port, enable, errno);
        return 0;
    }
    g_pf_stage = "capture_rules"; g_pf_program = NULL;
    g_pf_command = "none";
    g_pf_status = -1; g_pf_error[0] = 0;
    if (lease && !enable && g_pf_port_generations[port] != lease->generation) {
        g_pf_stage = "stale_cleanup_skipped";
        ok = 1;
        goto done;
    }
    if (lease && enable && g_pf_generation == UINT32_MAX) {
        /* Never wrap an ownership identity onto a still-pending lease. */
        errno = EIO;
        goto done;
    }
    before = capture_rules(port, deadline);
    if (!before) goto done;
    cap = strlen(before) + 256u;
    after = (char *)malloc(cap);
    if (!after) goto done;
    g_pf_stage = "rewrite_rules";
    if (!p1404_alt111_firewall_rewrite(before, port, enable, after, cap, &changed)) {
        errno = EINVAL;
        goto done;
    }
    if (!changed) { ok = 1; goto done; }
    n = snprintf(path, sizeof(path), "/tmp/carplay_alt111_pf_%ld_%u.conf",
                 (long)getpid(), (unsigned)port);
    if (n <= 0 || (size_t)n >= sizeof(path)) goto done;
    g_pf_stage = "write_rules";
    fp = fopen(path, "wb");
    if (!fp) goto done;
    write_ok = fwrite(after, 1u, strlen(after), fp) == strlen(after);
    if (fclose(fp) != 0) write_ok = 0;
    if (!write_ok) {
        fp = NULL; unlink(path); goto done;
    }
    fp = NULL;
    if (run_pf(argv, -1, deadline)) ok = 1;
    saved = errno;
    unlink(path);
    errno = saved;
done:
    saved = errno;
    if (fp) fclose(fp);
    free(after); free(before);
    if (lease && enable && ok) {
        lease->port = port;
        lease->generation = ++g_pf_generation;
        g_pf_port_generations[port] = lease->generation;
    } else if (!enable && (!lease || g_pf_port_generations[port] == lease->generation)) {
        /* A consumed failed cleanup can leave a rule, but cannot own any future
         * connection. A new open will get a fresh identity even if rule exists. */
        g_pf_port_generations[port] = 0;
    }
    altscreen_log("%s PHASE=ALT111_PF_TRANSACTION port=%u enable=%d result=%s stage=%s command=%s errno=%d wait_status=%d helper=%s elapsed_ms=%llu detail=%.383s",
                  ok ? "INFO" : "ERROR", (unsigned)port, enable,
                  ok ? "OK" : "FAILED", g_pf_stage, g_pf_command, ok ? 0 : saved,
                  g_pf_status, g_pf_program ? g_pf_program : "unavailable",
                  (unsigned long long)(pf_now_ms() - start), g_pf_error);
    pf_unlock();
    errno = saved;
    return ok;
}

int p1404_alt111_firewall_open(uint16_t port) { return apply_rule(port, 1, NULL); }
int p1404_alt111_firewall_close(uint16_t port) { return apply_rule(port, 0, NULL); }
int p1404_alt111_firewall_open_owned(uint16_t port, struct p1404_pf_lease *lease) {
    if (!lease) return 0;
    memset(lease, 0, sizeof(*lease));
    return apply_rule(port, 1, lease);
}
int p1404_alt111_firewall_close_owned(const struct p1404_pf_lease *lease) {
    struct p1404_pf_lease copy;
    if (!lease) return 0;
    copy = *lease;
    return apply_rule(copy.port, 0, &copy);
}
