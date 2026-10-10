/*
 * altscreen_hook.c - constructor, state root, and bounded iAP2 bearer interposer.
 * Mutation stays at exact stock NetSocket and K1004 NmeFile I/O seams. The
 * latter additionally requires /dev/otg-cinemo ownership. A generation-qualified
 * pinned descriptor owns every accepted assembler byte so
 * close/reuse cannot redirect delayed I/O to a different open file.
 */
#include "p1404_abi.h"
#include "p1404_lock_wait.h"
#include "p1404_airplay.h"
#include "p1404_iap2.h"
#include "p1404_private111_backend.h"
#include "p1404_cockpit_native.h"
#include "altscreen_profile.h"
#include "altscreen_paths.h"
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#ifdef ALTSCREEN_HOOK_HOST_TEST
#include <errno.h>
#endif
#ifdef ALTSCREEN_DIRECT_PROXY
/* Direct-overlay builds import libc under its real names and export only the
 * private dependency names xpen64/xead/xrite/xlose. This gives dependency
 * constructors relocation-backed forwarding without relying on early dlsym. */
extern ssize_t send(int, const void *, size_t, int);
extern ssize_t recv(int, void *, size_t, int);
extern int open64(const char *, int, ...);
#ifndef ALTSCREEN_HOOK_HOST_TEST
extern int *__get_errno_ptr(void);
#endif
extern int pthread_join(pthread_t, void **);
#endif

#define IAP2_BEARER_SOURCE_MAX 3072u
#define IAP2_BEARER_OUTPUT_MAX IAP2_TX_MAX
#define BEARER_GUARD_SLOTS 16
#define BEARER_GUARD_SPINS 64
#define BEARER_TX_SLOTS 8
#define BEARER_TX_WRITE 1
#define BEARER_TX_SEND  2
#define BEARER_TX_RECV  3
#define BEARER_TX_READ  4

typedef int (*open64_fn)(const char *, int, ...);
typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef ssize_t (*send_fn)(int, const void *, size_t, int);
typedef ssize_t (*recv_fn)(int, void *, size_t, int);
typedef int (*close_fn)(int);
typedef int (*dup_fn)(int);
typedef int *(*errno_ptr_fn)(void);
typedef int (*thread_create_fn)(pthread_t *, const void *, void *(*)(void *), void *);
typedef int (*thread_join_fn)(pthread_t, void **);

#ifdef ALTSCREEN_DIRECT_PROXY
/* Exact K1004 libairplax relocation targets. Vehicle evidence proved that QNX
 * keeps these internal references self-bound even though the proxy has the same
 * exported names. The stock GOT is in its measured RW LOAD segment. We replace
 * only these relocation slots; all function bodies and unrelated entries stay
 * byte-for-byte stock. */
#define K1004_STOCK_GOT_SERVER_PROPERTY_PTR  0x000b8770u
#define K1004_STOCK_GOT_STREAM_START         0x000b7734u
#define K1004_STOCK_GOT_SESSION_SETUP        0x000b7744u
#define K1004_STOCK_GOT_SESSION_TEARDOWN     0x000b7960u
#define K1004_STOCK_GOT_SESSION_START        0x000b7c88u
#define K1004_STOCK_GOT_SESSION_PROPERTY     0x000b7d0cu
#define K1004_STOCK_GOT_SERVER_PROPERTY_CALL 0x000b7e30u
#define K1004_STOCK_GOT_CSCREEN_RENDER       0x000b7ea8u
#define K1004_STOCK_GOT_SCREEN_COPY_MAIN     0x000b7fccu
#define K1004_STOCK_GOT_STREAM_PROCESS       0x000b8000u
#define K1004_STOCK_GOT_SESSION_CONTROL      0x000b840cu
#define K1004_STOCK_GOT_COPY_DISPLAYS        0x000b84d8u
#define K1004_STOCK_GOT_STREAM_CREATE        0x000b85a4u
#define K1004_STOCK_GOT_CSCREEN_CONFIG       0x000b85f4u
#define K1004_STOCK_GOT_WINDOW_GROUP         0x000b7954u
#define K1004_STOCK_GOT_WINDOW_BUFFERS       0x000b84a8u
#define K1004_STOCK_IMAGE_SPAN               0x000b9778u

extern void *AirPlayReceiverServerPlatformCopyProperty(void *, unsigned, void *, void *, int *);
extern void *AirPlayReceiverSessionPlatformCopyProperty(void *, unsigned, void *, void *, int *);
extern int AirPlayReceiverSessionPlatformControl(void *, unsigned, void *, const void *, const void *, void **);
extern int AirPlayReceiverSessionSetup(void *, const void *, void **);
extern void AirPlayReceiverSessionTearDown(void *, const void *, int, unsigned char *);
extern int AirPlayReceiverSessionStart(void *, const void *);
extern void *AirPlayReceiverSessionScreen_CopyDisplaysInfo(void *, int *);
extern void *ScreenCopyMain(int *);
extern int ScreenStreamStart(void *);
extern int ScreenStreamCreate(void);
extern int ScreenStreamProcessData(void);
extern int screen_create_window_group(void *, const char *) __attribute__((weak));
extern int screen_create_window_buffers(void *, int) __attribute__((weak));
extern int p1404_hook_cscreen_config(void *, const struct p1404_screen_config *)
    __asm__("_ZN3dio13CScreenRender6configERKNS_16st_screen_configE");
extern int p1404_hook_cscreen_render(void *, unsigned char *)
    __asm__("_ZN3dio13CScreenRender6renderEPh");

struct direct_got_redirect {
    uintptr_t slot_offset;
    uintptr_t target;
};
static volatile unsigned g_direct_redirect_state; /* 0 unset, 1 installed, 2 refused */

int p1404_direct_install_internal_redirects(void) {
    uintptr_t stock_anchor = (uintptr_t)p1404_direct_stock_symbol_named("ScreenCopyMain");
    uintptr_t base;
    size_t i;
    struct direct_got_redirect redirects[] = {
        { K1004_STOCK_GOT_SERVER_PROPERTY_PTR,  (uintptr_t)AirPlayReceiverServerPlatformCopyProperty },
        { K1004_STOCK_GOT_STREAM_START,         (uintptr_t)ScreenStreamStart },
        { K1004_STOCK_GOT_SESSION_SETUP,        (uintptr_t)AirPlayReceiverSessionSetup },
        { K1004_STOCK_GOT_SESSION_TEARDOWN,     (uintptr_t)AirPlayReceiverSessionTearDown },
        { K1004_STOCK_GOT_SESSION_START,        (uintptr_t)AirPlayReceiverSessionStart },
        { K1004_STOCK_GOT_SESSION_PROPERTY,     (uintptr_t)AirPlayReceiverSessionPlatformCopyProperty },
        { K1004_STOCK_GOT_SERVER_PROPERTY_CALL, (uintptr_t)AirPlayReceiverServerPlatformCopyProperty },
        { K1004_STOCK_GOT_CSCREEN_RENDER,       (uintptr_t)p1404_hook_cscreen_render },
        { K1004_STOCK_GOT_SCREEN_COPY_MAIN,     (uintptr_t)ScreenCopyMain },
        { K1004_STOCK_GOT_STREAM_PROCESS,       (uintptr_t)ScreenStreamProcessData },
        { K1004_STOCK_GOT_SESSION_CONTROL,      (uintptr_t)AirPlayReceiverSessionPlatformControl },
        { K1004_STOCK_GOT_COPY_DISPLAYS,        (uintptr_t)AirPlayReceiverSessionScreen_CopyDisplaysInfo },
        { K1004_STOCK_GOT_STREAM_CREATE,        (uintptr_t)ScreenStreamCreate },
        { K1004_STOCK_GOT_CSCREEN_CONFIG,       (uintptr_t)p1404_hook_cscreen_config },
        { K1004_STOCK_GOT_WINDOW_GROUP,         (uintptr_t)screen_create_window_group },
        { K1004_STOCK_GOT_WINDOW_BUFFERS,       (uintptr_t)screen_create_window_buffers }
    };
    if (g_direct_redirect_state == 1u) return 1;
    if (g_direct_redirect_state == 2u || !stock_anchor ||
        !screen_create_window_group || !screen_create_window_buffers)
        return 0;
    base = stock_anchor - (uintptr_t)(OFF_SCREEN_COPY_MAIN + ALTSCREEN_STOCK_OFFSET_DELTA);

    /* Validate the complete table before the first mutation. Lazy PLT values and
     * eagerly resolved stock values both point inside this exact stock image. */
    for (i = 0; i < sizeof(redirects) / sizeof(redirects[0]); ++i) {
        uintptr_t current = *(volatile uintptr_t *)(base + redirects[i].slot_offset);
        if (current == redirects[i].target) continue;
        if (current < base || current >= base + K1004_STOCK_IMAGE_SPAN) {
            __sync_lock_test_and_set(&g_direct_redirect_state, 2u);
            return 0;
        }
    }
    for (i = 0; i < sizeof(redirects) / sizeof(redirects[0]); ++i)
        *(volatile uintptr_t *)(base + redirects[i].slot_offset) = redirects[i].target;
    __sync_synchronize();
    for (i = 0; i < sizeof(redirects) / sizeof(redirects[0]); ++i) {
        if (*(volatile uintptr_t *)(base + redirects[i].slot_offset) != redirects[i].target) {
            __sync_lock_test_and_set(&g_direct_redirect_state, 2u);
            return 0;
        }
    }
    __sync_lock_test_and_set(&g_direct_redirect_state, 1u);
    return 1;
}
int p1404_direct_internal_redirects_ready(void) {
    return __sync_fetch_and_add(&g_direct_redirect_state, 0u) == 1u;
}
#else
int p1404_direct_install_internal_redirects(void) { return 1; }
int p1404_direct_internal_redirects_ready(void) { return 1; }
#endif

#ifdef ALTSCREEN_HOOK_HOST_TEST
static int *host_errno_pointer(void);
#endif

#ifdef ALTSCREEN_DIRECT_PROXY
static open64_fn real_open64 = (open64_fn)open64;
static read_fn real_read = (read_fn)read;
static write_fn real_write = (write_fn)write;
static send_fn real_send = (send_fn)send;
static recv_fn real_recv = (recv_fn)recv;
static close_fn real_close = (close_fn)close;
static dup_fn real_dup = (dup_fn)dup;
#ifdef ALTSCREEN_HOOK_HOST_TEST
static errno_ptr_fn real_errno_ptr = host_errno_pointer;
#else
static errno_ptr_fn real_errno_ptr = (errno_ptr_fn)__get_errno_ptr;
#endif
static thread_create_fn real_thread_create = (thread_create_fn)pthread_create;
static thread_join_fn real_thread_join = (thread_join_fn)pthread_join;
static int g_forward_ready = 1;
#else
static open64_fn real_open64;
static read_fn real_read;
static write_fn real_write;
static send_fn real_send;
static recv_fn real_recv;
static close_fn real_close;
static dup_fn real_dup;
static errno_ptr_fn real_errno_ptr;
static thread_create_fn real_thread_create;
static thread_join_fn real_thread_join;
static int g_forward_ready;
#endif

struct bearer_tx_slot {
    int active;
    int busy;
    int io_refs;
    int closing;
    int retired;
    int drain_started;
    int drain_cancel;
    int drain_done;
    int joining;
    int fd;       /* immutable assembler/generation key */
    int match_fd; /* live public descriptor; -1 immediately after real close */
    int pin_fd;
    uint32_t generation;
    int kind;
    int flags;
    int tuple_valid;
    int tuple_kind;
    int tuple_flags;
    size_t length;
    size_t offset;
    pthread_t drain_thread;
    uint8_t data[IAP2_BEARER_OUTPUT_MAX];
};

struct bearer_raw_guard { int active; int fd; };
struct bearer_close_guard { int active; int fd; };
static volatile unsigned g_bearer_tx_lock;
static struct bearer_tx_slot g_bearer_tx[BEARER_TX_SLOTS];
static struct bearer_raw_guard g_bearer_raw[BEARER_GUARD_SLOTS];
static struct bearer_close_guard g_bearer_close[BEARER_GUARD_SLOTS];
static uint32_t g_next_generation = 1u;

struct bearer_guard_slot { unsigned long tid; int active; };
static volatile unsigned g_bearer_guard_lock;
static struct bearer_guard_slot g_bearer_guard[BEARER_GUARD_SLOTS];

static int bearer_guard_lock(void) {
    int i;
    for (i = 0; i < BEARER_GUARD_SPINS; ++i)
        if (__sync_lock_test_and_set(&g_bearer_guard_lock, 1u) == 0u) return 1;
    return 0;
}
static void bearer_guard_lock_wait(void) {
    while (__sync_lock_test_and_set(&g_bearer_guard_lock, 1u) != 0u) p1404_lock_wait_yield();
}
static void bearer_guard_unlock(void) { __sync_lock_release(&g_bearer_guard_lock); }
static void bearer_tx_lock(void) {
    while (__sync_lock_test_and_set(&g_bearer_tx_lock, 1u) != 0u) p1404_lock_wait_yield();
}
static void bearer_tx_unlock(void) { __sync_lock_release(&g_bearer_tx_lock); }
static void bearer_set_error(int value) {
    int *errorp = real_errno_ptr ? real_errno_ptr() : NULL;
    if (errorp) *errorp = value;
}

#ifndef ALTSCREEN_HOOK_HOST_TEST
static void bind_bearer(void) {
#ifdef ALTSCREEN_DIRECT_PROXY
    real_open64 = (open64_fn)open64;
    real_read = (read_fn)read;
    real_write = (write_fn)write;
    real_send = (send_fn)send;
    real_recv = (recv_fn)recv;
    real_close = (close_fn)close;
    real_dup = (dup_fn)dup;
    real_errno_ptr = (errno_ptr_fn)__get_errno_ptr;
    real_thread_create = (thread_create_fn)pthread_create;
    real_thread_join = (thread_join_fn)pthread_join;
#else
    real_open64 = (open64_fn)dlsym(RTLD_NEXT, "open64");
    real_read = (read_fn)dlsym(RTLD_NEXT, "read");
    real_write = (write_fn)dlsym(RTLD_NEXT, "write");
    real_send = (send_fn)dlsym(RTLD_NEXT, "send");
    real_recv = (recv_fn)dlsym(RTLD_NEXT, "recv");
    real_close = (close_fn)dlsym(RTLD_NEXT, "close");
    real_dup = (dup_fn)dlsym(RTLD_NEXT, "dup");
    real_errno_ptr = (errno_ptr_fn)dlsym(RTLD_DEFAULT, "__get_errno_ptr");
    real_thread_create = (thread_create_fn)dlsym(RTLD_DEFAULT, "pthread_create");
    real_thread_join = (thread_join_fn)dlsym(RTLD_DEFAULT, "pthread_join");
#endif
    g_forward_ready = real_read && real_write && real_send && real_recv &&
                      real_close && real_dup && real_errno_ptr &&
                      real_thread_create && real_thread_join;
#ifdef ALTSCREEN_DIRECT_PROXY
    g_forward_ready = g_forward_ready && real_open64;
#endif
}
#endif

static int bearer_enter(void) {
    unsigned long tid = pthread_self();
    int i, free_slot = -1;
    if (!bearer_guard_lock()) return 1;
    for (i = 0; i < BEARER_GUARD_SLOTS; ++i) {
        if (g_bearer_guard[i].active && g_bearer_guard[i].tid == tid) {
            bearer_guard_unlock();
            return 1;
        }
        if (!g_bearer_guard[i].active && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_bearer_guard[free_slot].tid = tid;
        g_bearer_guard[free_slot].active = 1;
    }
    bearer_guard_unlock();
    return free_slot < 0;
}
static void bearer_leave(void) {
    unsigned long tid = pthread_self();
    int i;
    bearer_guard_lock_wait();
    for (i = 0; i < BEARER_GUARD_SLOTS; ++i) {
        if (g_bearer_guard[i].active && g_bearer_guard[i].tid == tid) {
            memset(&g_bearer_guard[i], 0, sizeof(g_bearer_guard[i]));
            break;
        }
    }
    bearer_guard_unlock();
}

static struct bearer_tx_slot *bearer_find_open_locked(int fd) {
    int i;
    for (i = 0; i < BEARER_TX_SLOTS; ++i)
        if (g_bearer_tx[i].active && g_bearer_tx[i].match_fd == fd &&
            !g_bearer_tx[i].closing && !g_bearer_tx[i].retired)
            return &g_bearer_tx[i];
    return NULL;
}
static int bearer_raw_busy_locked(int fd) {
    int i;
    for (i = 0; i < BEARER_GUARD_SLOTS; ++i)
        if (g_bearer_raw[i].active && g_bearer_raw[i].fd == fd) return 1;
    return 0;
}
static int bearer_close_guard_find_locked(int fd) {
    int i;
    for (i = 0; i < BEARER_GUARD_SLOTS; ++i)
        if (g_bearer_close[i].active && g_bearer_close[i].fd == fd) return i;
    return -1;
}
static struct bearer_tx_slot *bearer_find_closing_locked(int fd) {
    int i;
    for (i = 0; i < BEARER_TX_SLOTS; ++i)
        if (g_bearer_tx[i].active && g_bearer_tx[i].match_fd == fd &&
            g_bearer_tx[i].closing && !g_bearer_tx[i].retired)
            return &g_bearer_tx[i];
    return NULL;
}
static uint32_t bearer_allocate_generation_locked(void) {
    uint32_t candidate;
    int i, collision;
    do {
        candidate = g_next_generation++;
        if (!candidate) candidate = g_next_generation++;
        collision = 0;
        for (i = 0; i < BEARER_TX_SLOTS; ++i)
            if (g_bearer_tx[i].active && g_bearer_tx[i].generation == candidate)
                collision = 1;
    } while (collision || !candidate);
    return candidate;
}

static void bearer_reap_done(void) {
    int i;
    if (!real_thread_join) return;
    for (;;) {
        pthread_t thread = (pthread_t)0;
        struct bearer_tx_slot *slot = NULL;
        bearer_tx_lock();
        for (i = 0; i < BEARER_TX_SLOTS; ++i) {
            if (g_bearer_tx[i].active && g_bearer_tx[i].drain_started &&
                g_bearer_tx[i].drain_done && !g_bearer_tx[i].joining) {
                slot = &g_bearer_tx[i];
                slot->joining = 1;
                thread = slot->drain_thread;
                break;
            }
        }
        bearer_tx_unlock();
        if (!slot) break;
        (void)real_thread_join(thread, NULL);
        bearer_tx_lock();
        if (slot->joining && slot->drain_done) {
            if (slot->retired) {
                memset(slot, 0, sizeof(*slot));
            } else {
                slot->drain_started = 0;
                slot->drain_cancel = 0;
                slot->drain_done = 0;
                slot->joining = 0;
            }
        }
        bearer_tx_unlock();
    }
}

/* Return 1 with a claimed slot, 0 for generation-safe pinned fail-open, and
 * -1 when lifecycle ownership makes the caller's numeric fd unsafe. */
static int bearer_claim(int fd, int candidate, int kind, int flags,
                        struct bearer_tx_slot **out, int *raw_token) {
    struct bearer_tx_slot *slot = NULL;
    int i, pin_fd = -1;
    *out = NULL;
    if (raw_token) *raw_token = -1;
    for (;;) {
        bearer_tx_lock();
        slot = bearer_find_open_locked(fd);
        if (!slot && (bearer_find_closing_locked(fd) ||
                      bearer_close_guard_find_locked(fd) >= 0)) {
            bearer_tx_unlock();
            bearer_set_error(9); /* EBADF: numeric fd is lifecycle-unsafe. */
            return -1;
        }
        if (slot && slot->busy) {
            bearer_tx_unlock();
            p1404_lock_wait_yield();
            continue;
        }
        if (!slot) {
            /* In the armed mutation build, unrelated process-wide writes stay
             * completely raw until an iAP2 sync candidate identifies the FD.
             * Observer-only tests retain the historical attribution behavior. */
            if (p1404_mutate_armed && alt_flag_iap2 && !candidate) {
                for (i = 0; i < BEARER_GUARD_SLOTS; ++i)
                    if (!g_bearer_raw[i].active) break;
                if (i == BEARER_GUARD_SLOTS) {
                    bearer_tx_unlock();
                    bearer_set_error(11);
                    return -1;
                }
                g_bearer_raw[i].active = 1;
                g_bearer_raw[i].fd = fd;
                bearer_tx_unlock();
                if (raw_token) *raw_token = i;
                return 2;
            }
            if (bearer_raw_busy_locked(fd)) {
                bearer_tx_unlock();
                continue;
            }
            for (i = 0; i < BEARER_TX_SLOTS; ++i)
                if (!g_bearer_tx[i].active) { slot = &g_bearer_tx[i]; break; }
            if (!slot) { bearer_tx_unlock(); return 0; }
            memset(slot, 0, sizeof(*slot));
            slot->active = 1;
            slot->busy = 1;
            slot->fd = fd;
            slot->match_fd = fd;
            slot->pin_fd = -1;
            slot->generation = bearer_allocate_generation_locked();
            slot->tuple_kind = kind;
            slot->tuple_flags = flags;
            bearer_tx_unlock();
            pin_fd = real_dup ? real_dup(fd) : -1;
            bearer_tx_lock();
            if (slot->closing || slot->retired) {
                slot->busy = 0;
                bearer_tx_unlock();
                if (pin_fd >= 0) (void)real_close(pin_fd);
                /* dup raced the lifecycle reservation. The numeric fd may
                 * already name an unrelated object, so raw fail-open is
                 * forbidden and the caller gets EBADF after pin cleanup. */
                bearer_set_error(9);
                return -1;
            }
            if (pin_fd < 0) {
                memset(slot, 0, sizeof(*slot));
                bearer_tx_unlock();
                /* Armed traffic must never fall back to the unpinned numeric
                 * descriptor after dup failure. Preserve dup's real errno. */
                return -1;
            }
            slot->pin_fd = pin_fd;
            bearer_tx_unlock();
            *out = slot;
            return 1;
        }
        slot->busy = 1;
        bearer_tx_unlock();
        *out = slot;
        return 1;
    }
}

static void bearer_release(struct bearer_tx_slot *slot) {
    bearer_tx_lock();
    if (slot) slot->busy = 0;
    bearer_tx_unlock();
}

static int bearer_set_payload(struct bearer_tx_slot *slot, const uint8_t *data,
                              size_t length, int kind, int flags) {
    if (!slot || !slot->busy || !data || !length ||
        length > sizeof(slot->data)) return 0;
    memcpy(slot->data, data, length);
    slot->length = length;
    slot->offset = 0u;
    slot->kind = kind;
    slot->flags = flags;
    return 1;
}

static ssize_t bearer_real_io(struct bearer_tx_slot *slot, int kind, int flags,
                              const uint8_t *buf, size_t n) {
    if (kind == BEARER_TX_SEND) return real_send(slot->pin_fd, buf, n, flags);
    return real_write(slot->pin_fd, buf, n);
}

static ssize_t bearer_flush(struct bearer_tx_slot *slot, int *complete,
                            size_t *wire_progress) {
    ssize_t r = 0;
    size_t progress = 0;
    if (complete) *complete = 0;
    if (wire_progress) *wire_progress = 0;
    while (slot && slot->offset < slot->length) {
        size_t remaining = slot->length - slot->offset;
        r = bearer_real_io(slot, slot->kind, slot->flags,
                           slot->data + slot->offset, remaining);
        if (r <= 0) break;
        if ((size_t)r > remaining) { r = -1; break; }
        (void)iap2_control_fd_note_outgoing_gen(slot->fd, slot->generation,
                                                slot->data + slot->offset,
                                                (size_t)r);
        slot->offset += (size_t)r;
        progress += (size_t)r;
    }
    if (wire_progress) *wire_progress = progress;
    if (slot && slot->offset == slot->length) {
        slot->length = slot->offset = 0u;
        if (complete) *complete = 1;
        return (ssize_t)progress;
    }
    return r;
}

static int bearer_flush_existing(struct bearer_tx_slot *slot, ssize_t *blocked) {
    int complete = 0;
    size_t progress = 0;
    ssize_t r;
    if (blocked) *blocked = -1;
    if (!slot->length) return 1;
    r = bearer_flush(slot, &complete, &progress);
    if (complete) return 1;
    if (blocked) *blocked = r <= 0 ? r : -1;
    return 0;
}

/* Flush assembler-only acknowledged bytes unchanged under their original
 * transport tuple.  Positive progress commits ownership; zero progress rolls
 * back without accepting bytes from the new call. */
static int bearer_flush_assembler(struct bearer_tx_slot *slot, ssize_t *blocked) {
    uint8_t raw[IAP2_ASSEMBLER_MAX];
    size_t raw_len = 0, progress = 0;
    int prepared, complete = 0;
    ssize_t r;
    if (blocked) *blocked = -1;
    prepared = iap2_control_fd_prepare_drain_gen(slot->fd, slot->generation,
                                                 raw, sizeof(raw), &raw_len);
    if (prepared == IAP2_MUTATE_PASS) return 1;
    if (prepared != IAP2_MUTATE_OUTPUT || !raw_len ||
        !bearer_set_payload(slot, raw, raw_len,
                            slot->tuple_kind, slot->tuple_flags)) {
        (void)iap2_control_fd_cancel_mutation_gen(slot->fd, slot->generation);
        return 0;
    }
    r = bearer_flush(slot, &complete, &progress);
    if (complete || progress) {
        (void)iap2_control_fd_commit_mutation_gen(slot->fd, slot->generation);
        if (complete) { slot->tuple_valid = 0; return 1; }
    } else {
        /* No wire byte owns this tentative output.  Discard only the local TX
         * copy; cancel restores the exact pre-call assembler transaction. */
        slot->length = slot->offset = 0u;
        (void)iap2_control_fd_cancel_mutation_gen(slot->fd, slot->generation);
    }
    if (blocked) *blocked = r <= 0 ? r : -1;
    return 0;
}

static ssize_t bearer_raw_tracked(struct bearer_tx_slot *slot, int kind,
                                  const void *buf, size_t n, int flags) {
    ssize_t r = bearer_real_io(slot, kind, flags, (const uint8_t *)buf, n);
    if (r > 0)
        (void)iap2_control_fd_note_outgoing_gen(slot->fd, slot->generation,
                                                (const uint8_t *)buf, (size_t)r);
    return r;
}
/* Acquire a pin without ever performing I/O on the caller's numeric fd.  The
 * PINNING guard is published before dup and is the only interval in which
 * close must return EAGAIN. Once dup succeeds the guard is removed: close may
 * proceed immediately because all potentially blocking I/O uses the pin. */
static int bearer_pin_untracked(int fd) {
    int i, token = -1, pin_fd, valid;
    if (!real_dup || !real_close) { bearer_set_error(11); return -1; }
    bearer_tx_lock();
    if (bearer_find_closing_locked(fd) ||
        bearer_close_guard_find_locked(fd) >= 0) {
        bearer_tx_unlock();
        bearer_set_error(9);
        return -1;
    }
    for (i = 0; i < BEARER_GUARD_SLOTS; ++i)
        if (!g_bearer_raw[i].active) { token = i; break; }
    if (token < 0) {
        bearer_tx_unlock();
        bearer_set_error(11);
        return -1;
    }
    g_bearer_raw[token].active = 1;
    g_bearer_raw[token].fd = fd;
    bearer_tx_unlock();

    pin_fd = real_dup(fd);
    bearer_tx_lock();
    valid = g_bearer_raw[token].active && g_bearer_raw[token].fd == fd &&
            !bearer_find_closing_locked(fd) &&
            bearer_close_guard_find_locked(fd) < 0;
    memset(&g_bearer_raw[token], 0, sizeof(g_bearer_raw[token]));
    bearer_tx_unlock();
    if (pin_fd < 0) return -1; /* preserve real dup errno */
    if (!valid) {
        (void)real_close(pin_fd);
        bearer_set_error(9);
        return -1;
    }
    return pin_fd;
}

static ssize_t bearer_raw_reserved(int token, int kind, int fd, void *buf,
                                   size_t n, int flags) {
    int pin_fd, valid, saved_error = 0;
    ssize_t r;
    pin_fd = real_dup ? real_dup(fd) : -1;
    bearer_tx_lock();
    valid = token >= 0 && token < BEARER_GUARD_SLOTS &&
            g_bearer_raw[token].active && g_bearer_raw[token].fd == fd &&
            !bearer_find_closing_locked(fd) &&
            bearer_close_guard_find_locked(fd) < 0;
    bearer_tx_unlock();
    if (pin_fd < 0 || !valid) {
        if (pin_fd >= 0) (void)real_close(pin_fd);
        bearer_tx_lock();
        if (token >= 0 && token < BEARER_GUARD_SLOTS)
            memset(&g_bearer_raw[token], 0, sizeof(g_bearer_raw[token]));
        bearer_tx_unlock();
        if (pin_fd >= 0) bearer_set_error(9);
        return -1;
    }
    if (kind == BEARER_TX_SEND)
        r = real_send(pin_fd, (const void *)buf, n, flags);
    else if (kind == BEARER_TX_RECV)
        r = real_recv(pin_fd, buf, n, flags);
    else if (kind == BEARER_TX_READ)
        r = real_read(pin_fd, buf, n);
    else
        r = real_write(pin_fd, (const void *)buf, n);
    if (real_errno_ptr) saved_error = *real_errno_ptr();
    (void)real_close(pin_fd);
    bearer_tx_lock();
    if (g_bearer_raw[token].active && g_bearer_raw[token].fd == fd)
        memset(&g_bearer_raw[token], 0, sizeof(g_bearer_raw[token]));
    bearer_tx_unlock();
    if (real_errno_ptr) *real_errno_ptr() = saved_error;
    return r;
}

static ssize_t bearer_raw_untracked(int kind, int fd, void *buf, size_t n,
                                    int flags) {
    int pin_fd, saved_error = 0;
    ssize_t r;
    pin_fd = bearer_pin_untracked(fd);
    if (pin_fd < 0) return -1;
    if (kind == BEARER_TX_SEND)
        r = real_send(pin_fd, (const void *)buf, n, flags);
    else if (kind == BEARER_TX_RECV)
        r = real_recv(pin_fd, buf, n, flags);
    else if (kind == BEARER_TX_READ)
        r = real_read(pin_fd, buf, n);
    else
        r = real_write(pin_fd, (const void *)buf, n);
    if (real_errno_ptr) saved_error = *real_errno_ptr();
    (void)real_close(pin_fd);
    if (real_errno_ptr) *real_errno_ptr() = saved_error;
    /* This path deliberately does not update unqualified iAP2 attribution: the
     * original numeric fd may close immediately after pin acquisition. */
    return r;
}

static __attribute__((noinline)) ssize_t bearer_forward(
    int kind, int fd, const void *buf, size_t n, int flags) {
    uint8_t frame[IAP2_BEARER_OUTPUT_MAX];
    const uint8_t *source = (const uint8_t *)buf;
    struct bearer_tx_slot *slot = NULL;
    size_t accepted = 0;
    ssize_t blocked = -1, r;
    int nested, claim, candidate, mutate, raw_token = -1;

    if ((kind == BEARER_TX_WRITE && !real_write) ||
        (kind == BEARER_TX_SEND && !real_send)) return -1;
    /* READY-before-stock-only must also mean no proxy locks or FD bookkeeping.
     * This is the path used by stock audio/video during startup and by every
     * dependency constructor. */
    if (!altscreen_runtime_is_ready() || !p1404_armed)
        return kind == BEARER_TX_SEND ? real_send(fd, buf, n, flags)
                                      : real_write(fd, buf, n);
    nested = bearer_enter();
    if (!g_forward_ready) {
        if (!nested) bearer_leave();
        bearer_set_error(11);
        return -1;
    }
    if (nested || fd < 0 || !buf || !n) {
        if (!nested) bearer_leave();
        if (fd < 0) {
            return kind == BEARER_TX_SEND ? real_send(fd, buf, n, flags)
                                          : real_write(fd, buf, n);
        }
        return bearer_raw_untracked(kind, fd, (void *)buf, n, flags);
    }
    bearer_reap_done();
    mutate = p1404_mutate_armed && alt_flag_iap2;
    candidate = mutate && iap2_mutation_buffer_candidate(source, n);
    claim = bearer_claim(fd, candidate, kind, flags, &slot, &raw_token);
    bearer_leave();
    if (claim < 0) return -1;
    if (claim == 2)
        return bearer_raw_reserved(raw_token, kind, fd, (void *)buf, n, flags);
    if (claim == 0)
        return bearer_raw_untracked(kind, fd, (void *)buf, n, flags);

    if (!bearer_flush_existing(slot, &blocked)) {
        bearer_release(slot);
        return blocked;
    }
    if (slot->tuple_valid &&
        (slot->tuple_kind != kind || slot->tuple_flags != flags)) {
        if (!bearer_flush_assembler(slot, &blocked)) {
            bearer_release(slot);
            return blocked;
        }
        slot->tuple_valid = 0;
    }
    if (!mutate || !iap2_control_fd_mutation_candidate_gen(fd, slot->generation,
                                                            source, n)) {
        r = bearer_raw_tracked(slot, kind, buf, n, flags);
        bearer_release(slot);
        return r;
    }

    while (accepted < n) {
        size_t chunk = n - accepted, wire_len = 0, progress = 0;
        int complete = 0, had_pending = 0, mutation;
        if (chunk > IAP2_BEARER_SOURCE_MAX) chunk = IAP2_BEARER_SOURCE_MAX;
        mutation = iap2_control_fd_prepare_mutation_gen(
            fd, slot->generation, source + accepted, chunk, frame, sizeof(frame),
            &wire_len, &had_pending);
        if (mutation == IAP2_MUTATE_ERROR) {
            bearer_release(slot);
            return accepted ? (ssize_t)accepted : -1;
        }
        if (mutation == IAP2_MUTATE_BUFFERED) {
            if (!slot->tuple_valid) {
                slot->tuple_valid = 1;
                slot->tuple_kind = kind;
                slot->tuple_flags = flags;
            }
            accepted += chunk;
            continue;
        }
        if (mutation != IAP2_MUTATE_OUTPUT || !wire_len) {
            r = bearer_raw_tracked(slot, kind, source + accepted, chunk, flags);
            if (r > 0) accepted += (size_t)r;
            if (r == (ssize_t)chunk) continue;
            bearer_release(slot);
            return accepted ? (ssize_t)accepted : r;
        }
        if (!slot->tuple_valid) {
            slot->tuple_valid = 1;
            slot->tuple_kind = kind;
            slot->tuple_flags = flags;
        }
        if (!bearer_set_payload(slot, frame, wire_len,
                                slot->tuple_kind, slot->tuple_flags)) {
            (void)iap2_control_fd_cancel_mutation_gen(fd, slot->generation);
            bearer_release(slot);
            return accepted ? (ssize_t)accepted : -1;
        }
        r = bearer_flush(slot, &complete, &progress);
        if (complete || progress) {
            (void)iap2_control_fd_commit_mutation_gen(fd, slot->generation);
            accepted += chunk;
            if (!complete) { bearer_release(slot); return (ssize_t)accepted; }
            slot->tuple_valid = 0;
            continue;
        }
        (void)had_pending;
        slot->length = slot->offset = 0u;
        (void)iap2_control_fd_cancel_mutation_gen(fd, slot->generation);
        bearer_release(slot);
        return accepted ? (ssize_t)accepted : r;
    }
    bearer_release(slot);
    return (ssize_t)accepted;
}

static void *bearer_drain_worker(void *opaque) {
    struct bearer_tx_slot *slot = (struct bearer_tx_slot *)opaque;
    ssize_t blocked = -1;
    for (;;) {
        bearer_tx_lock();
        /* pthread_create happens before libc close.  The worker must not touch
         * transport or attribution until close publishes a successful retire;
         * a failed libc close instead cancels this worker and leaves the slot
         * open/retryable. */
        if (slot->drain_cancel) {
            slot->drain_done = 1;
            bearer_tx_unlock();
            return NULL;
        }
        if (slot->retired && !slot->busy && slot->io_refs == 0) {
            slot->busy = 1;
            bearer_tx_unlock();
            break;
        }
        bearer_tx_unlock();
        (void)usleep(1000u);
    }
    if (!bearer_flush_existing(slot, &blocked))
        altscreen_log("ERROR IAP2_CLOSE_DRAIN generation=%u retained_wire=%u rc=%d",
                      slot->generation, (unsigned)(slot->length - slot->offset),
                      (int)blocked);
    else if (slot->tuple_valid && !bearer_flush_assembler(slot, &blocked))
        altscreen_log("ERROR IAP2_CLOSE_DRAIN generation=%u retained_source=1 rc=%d",
                      slot->generation, (int)blocked);
    if (slot->pin_fd >= 0) (void)real_close(slot->pin_fd);
    iap2_control_fd_close_gen(slot->fd, slot->generation);
    bearer_tx_lock();
    slot->busy = 0;
    slot->drain_done = 1;
    bearer_tx_unlock();
    return NULL;
}

#ifndef ALTSCREEN_DIRECT_PROXY
ssize_t write(int fd, const void *buf, size_t n) {
#ifndef ALTSCREEN_HOOK_HOST_TEST
    if (!real_write) bind_bearer();
#endif
    if (real_write && (!altscreen_runtime_is_ready() || !p1404_armed))
        return real_write(fd, buf, n);
    return bearer_forward(BEARER_TX_WRITE, fd, buf, n, 0);
}
ssize_t send(int fd, const void *buf, size_t n, int flags) {
#ifndef ALTSCREEN_HOOK_HOST_TEST
    if (!real_send) bind_bearer();
#endif
    if (real_send && (!altscreen_runtime_is_ready() || !p1404_armed))
        return real_send(fd, buf, n, flags);
    return bearer_forward(BEARER_TX_SEND, fd, buf, n, flags);
}
ssize_t recv(int fd, void *buf, size_t n, int flags) {
    struct bearer_tx_slot *slot = NULL;
    uint32_t generation = 0;
    int pin = -1, nested;
    ssize_t r;
#ifndef ALTSCREEN_HOOK_HOST_TEST
    if (!real_recv) bind_bearer();
#endif
    if (!real_recv) return -1;
    if (!altscreen_runtime_is_ready() || !p1404_armed)
        return real_recv(fd, buf, n, flags);
    nested = bearer_enter();
    if (!g_forward_ready) {
        if (!nested) bearer_leave();
        bearer_set_error(11);
        return -1;
    }
    if (nested || fd < 0 || !buf || !n) {
        if (!nested) bearer_leave();
        if (fd < 0) return real_recv(fd, buf, n, flags);
        return bearer_raw_untracked(BEARER_TX_RECV, fd, buf, n, flags);
    }
    bearer_reap_done();
    bearer_tx_lock();
    slot = bearer_find_open_locked(fd);
    if (slot && slot->pin_fd >= 0) {
        ++slot->io_refs;
        pin = slot->pin_fd;
        generation = slot->generation;
    } else {
        slot = NULL;
    }
    bearer_tx_unlock();
    bearer_leave();
    if (!slot) return bearer_raw_untracked(BEARER_TX_RECV, fd, buf, n, flags);

    r = real_recv(pin, buf, n, flags);
    if (r > 0)
        (void)iap2_control_fd_feed_incoming_gen(fd, generation, buf, (size_t)r);
    else if (r == 0)
        iap2_control_fd_peer_eof_gen(fd, generation);
    bearer_tx_lock();
    if (slot->generation == generation && slot->io_refs > 0) --slot->io_refs;
    bearer_tx_unlock();
    return r;
}
#endif /* !ALTSCREEN_DIRECT_PROXY */

#ifdef ALTSCREEN_DIRECT_PROXY
#define K1004_QNX_O_CREAT              0x100
#define K1004_NME_FILE_WRITE_SIZE       284u
#define K1004_NME_FILE_READ_SIZE        320u
#define K1004_NME_FILE_DELETE_SIZE       64u
#define K1004_NME_FILE_CREATE_POSIX_SIZE 400u
#define K1004_NME_OTG_FD_SLOTS             8
static uintptr_t g_nme_file_write_start;
static uintptr_t g_nme_file_read_start;
static uintptr_t g_nme_file_delete_start;
static uintptr_t g_nme_file_create_posix_start;
static int g_nme_otg_fds[K1004_NME_OTG_FD_SLOTS];
static uint32_t g_nme_otg_generations[K1004_NME_OTG_FD_SLOTS];
static uint32_t g_nme_otg_next_generation = 1u;

static __attribute__((unused)) int direct_function_head_matches(
                                        const void *fn,
                                        const unsigned char expected[16]) {
    return fn && !memcmp(fn, expected, 16u);
}

static __attribute__((unused)) int direct_bind_nme_io(void) {
#ifndef ALTSCREEN_HOOK_HOST_TEST
    static const unsigned char write_head[16] = {
        0xf0,0x41,0x2d,0xe9,0x30,0x2b,0x43,0xec,
        0x1c,0x40,0x9d,0xe5,0x18,0x50,0x9d,0xe5
    };
    static const unsigned char read_head[16] = {
        0xf0,0x47,0x2d,0xe9,0x24,0x40,0x9d,0xe5,
        0x20,0x60,0x9d,0xe5,0x01,0x70,0xa0,0xe1
    };
    static const unsigned char delete_head[16] = {
        0x10,0x40,0x2d,0xe9,0x00,0x40,0xa0,0xe1,
        0x04,0x00,0x90,0xe5,0x01,0x00,0x70,0xe3
    };
    static const unsigned char create_posix_head[16] = {
        0x05,0x30,0x02,0xe2,0x01,0x30,0x43,0xe2,
        0xf0,0x41,0x2d,0xe9,0x04,0x00,0x53,0xe3
    };
    void *write_fn_addr = dlsym(RTLD_DEFAULT, "_ZN7NmeFile5WriteEPKvyjPj");
    void *read_fn_addr = dlsym(RTLD_DEFAULT, "_ZN7NmeFile4ReadEPvyjPj");
    void *delete_fn_addr = dlsym(RTLD_DEFAULT, "_ZN7NmeFile6DeleteEv");
    void *create_posix_fn_addr = dlsym(RTLD_DEFAULT, "_ZN7NmeFile11CreatePosixEPKcj");
    if (!direct_function_head_matches(write_fn_addr, write_head) ||
        !direct_function_head_matches(read_fn_addr, read_head) ||
        !direct_function_head_matches(delete_fn_addr, delete_head) ||
        !direct_function_head_matches(create_posix_fn_addr, create_posix_head))
        return 0;
    g_nme_file_write_start = (uintptr_t)write_fn_addr;
    g_nme_file_read_start = (uintptr_t)read_fn_addr;
    g_nme_file_delete_start = (uintptr_t)delete_fn_addr;
    g_nme_file_create_posix_start = (uintptr_t)create_posix_fn_addr;
    __sync_synchronize();
#endif
    return g_nme_file_write_start && g_nme_file_read_start &&
           g_nme_file_delete_start && g_nme_file_create_posix_start;
}

static int direct_caller_in_range(uintptr_t start, size_t size, void *caller) {
    uintptr_t pc = (uintptr_t)caller;
    return start && size && pc >= start && pc < start + size;
}

static int direct_caller_in_stock(const char *name, size_t size, void *caller) {
    uintptr_t start = (uintptr_t)p1404_direct_stock_symbol_named(name);
    return direct_caller_in_range(start, size, caller);
}

static int direct_nme_otg_fd_contains(int fd) {
    int i, found = 0;
    if (fd < 0) return 0;
    bearer_tx_lock();
    for (i = 0; i < K1004_NME_OTG_FD_SLOTS; ++i)
        if (g_nme_otg_fds[i] == fd + 1) found = 1;
    bearer_tx_unlock();
    return found;
}
static uint32_t direct_nme_otg_fd_generation(int fd) {
    int i; uint32_t generation = 0;
    bearer_tx_lock();
    for (i = 0; i < K1004_NME_OTG_FD_SLOTS; ++i)
        if (g_nme_otg_fds[i] == fd + 1) generation = g_nme_otg_generations[i];
    bearer_tx_unlock();
    return generation;
}
static uint32_t direct_nme_otg_fd_set(int fd, int present) {
    int i, free_slot = -1; uint32_t generation = 0;
    if (fd < 0) return 0;
    bearer_tx_lock();
    for (i = 0; i < K1004_NME_OTG_FD_SLOTS; ++i) {
        if (g_nme_otg_fds[i] == fd + 1) {
            g_nme_otg_fds[i] = 0; g_nme_otg_generations[i] = 0;
            if (free_slot < 0) free_slot = i;
        } else if (!g_nme_otg_fds[i] && free_slot < 0) free_slot = i;
    }
    if (present && free_slot >= 0) {
        generation = g_nme_otg_next_generation++;
        if (!generation) generation = g_nme_otg_next_generation++;
        g_nme_otg_fds[free_slot] = fd + 1;
        g_nme_otg_generations[free_slot] = generation;
    }
    bearer_tx_unlock();
    return generation;
}
static void direct_nme_otg_fd_clear_generation(int fd, uint32_t generation) {
    int i;
    if (fd < 0 || !generation) return;
    bearer_tx_lock();
    for (i = 0; i < K1004_NME_OTG_FD_SLOTS; ++i)
        if (g_nme_otg_fds[i] == fd + 1 &&
            g_nme_otg_generations[i] == generation) {
            g_nme_otg_fds[i] = 0; g_nme_otg_generations[i] = 0;
        }
    bearer_tx_unlock();
}

/* The direct overlay redirects libairplax's write/close imports by name, and
 * the K1004 Nme overlay redirects only NmeBase read/write/close imports. These
 * imports include stock audio, logging and unrelated sockets. Only an FD
 * already owned by the iAP2 assembler may enter close lifecycle machinery.
 * This check intentionally preserves raw stock semantics for every other FD. */
static int bearer_fd_is_managed(int fd) {
    int managed;
    if (fd < 0) return 0;
    bearer_tx_lock();
    managed = bearer_find_open_locked(fd) != NULL ||
              bearer_find_closing_locked(fd) != NULL ||
              bearer_raw_busy_locked(fd) ||
              bearer_close_guard_find_locked(fd) >= 0;
    bearer_tx_unlock();
    return managed;
}
#else
static __attribute__((unused)) int direct_bind_nme_io(void) { return 1; }
#endif

static __attribute__((noinline)) int bearer_close_forward(int fd) {
    struct bearer_tx_slot *slot;
    int nested, close_rc, create_rc, guard_index = -1, i;
#ifndef ALTSCREEN_HOOK_HOST_TEST
    if (!real_close) bind_bearer();
#endif
    if (!real_close) return -1;
    if (!altscreen_runtime_is_ready() || !p1404_armed || fd < 0)
        return real_close(fd);
    nested = bearer_enter();
    if (!g_forward_ready) {
        if (!nested) bearer_leave();
        bearer_set_error(11);
        return -1;
    }
    if (!nested) bearer_reap_done();
    bearer_tx_lock();
    /* A PINNING operation still needs the original descriptor for dup. Never
     * wait behind it and never close underneath it; the caller gets a genuine
     * retryable failure. A completed pin removes this guard before blocking. */
    if (bearer_raw_busy_locked(fd)) {
        bearer_tx_unlock();
        if (!nested) bearer_leave();
        bearer_set_error(11);
        return -1;
    }
    slot = bearer_find_open_locked(fd);
    if (!slot) {
        if (bearer_find_closing_locked(fd) ||
            bearer_close_guard_find_locked(fd) >= 0) {
            bearer_tx_unlock();
            if (!nested) bearer_leave();
            bearer_set_error(11);
            return -1; /* follower: never close a potentially reused fd */
        }
        for (i = 0; i < BEARER_GUARD_SLOTS; ++i)
            if (!g_bearer_close[i].active) {
                guard_index = i;
                g_bearer_close[i].active = 1;
                g_bearer_close[i].fd = fd;
                break;
            }
        if (guard_index < 0) {
            bearer_tx_unlock();
            if (!nested) bearer_leave();
            bearer_set_error(11);
            return -1;
        }
        bearer_tx_unlock();
        if (!nested) bearer_leave();
        close_rc = real_close(fd);
        if (close_rc == 0) iap2_control_fd_close(fd);
        if (guard_index >= 0) {
            bearer_tx_lock();
            if (g_bearer_close[guard_index].active &&
                g_bearer_close[guard_index].fd == fd)
                memset(&g_bearer_close[guard_index], 0,
                       sizeof(g_bearer_close[guard_index]));
            bearer_tx_unlock();
        }
        return close_rc;
    }
    if (slot->drain_started) {
        bearer_tx_unlock();
        if (!nested) bearer_leave();
        bearer_set_error(11);
        return -1;
    }
    slot->closing = 1;
    bearer_tx_unlock();
    if (!nested) bearer_leave();

    /* Reserve the drain owner before closing the real descriptor.  The worker
     * waits for retired/cancel publication, so it cannot drain early.  A
     * create failure leaves the descriptor and generation fully retryable. */
    create_rc = real_thread_create ?
        real_thread_create(&slot->drain_thread, NULL, bearer_drain_worker, slot) : 11;
    if (create_rc != 0) {
        bearer_tx_lock();
        slot->closing = 0;
        bearer_tx_unlock();
        altscreen_log("ERROR IAP2_CLOSE_DRAIN_WORKER generation=%u create_rc=%d real_close_calls=0",
                      slot->generation, create_rc);
        bearer_set_error(create_rc > 0 ? create_rc : 11);
        return -1;
    }
    bearer_tx_lock();
    slot->drain_started = 1;
    bearer_tx_unlock();

    /* Exactly one real close, with no wait for busy libc I/O. */
    close_rc = real_close(fd);
    bearer_tx_lock();
    if (close_rc == 0) {
        /* The numeric descriptor may be reused as soon as close returns. Stop
         * matching it to this retired generation immediately; the worker keeps
         * slot->fd only as its immutable assembler key and drains via pin_fd. */
        slot->match_fd = -1;
        slot->retired = 1;
    } else {
        slot->closing = 0;
        slot->drain_cancel = 1;
    }
    bearer_tx_unlock();
    /* Once libc succeeds, no hook-side drain outcome may override it. */
    return close_rc;
}

#ifndef ALTSCREEN_DIRECT_PROXY
int close(int fd) {
    if (real_close && (!altscreen_runtime_is_ready() || !p1404_armed))
        return real_close(fd);
    return bearer_close_forward(fd);
}
#endif

#ifdef ALTSCREEN_DIRECT_PROXY
/* K1004 shares dynstr tails, so the four equal-length renames also rewrite the
 * adjacent stdio names: fread/fwrite/fclose become fxead/fxrite/fxlose. Preserve
 * their stock stdio ABIs exactly; they never enter AltScreen. `read` had to
 * become the four-character `xead` because a longer name would overwrite the
 * first byte of the next dynstr entry and blank that symbol's name. */
size_t fxead(void *ptr, size_t size, size_t count, FILE *stream) {
    if (!stream || (size && count && !ptr) || (size && count > (size_t)-1 / size)) { bearer_set_error(22); return 0; }
    return fread(ptr, size, count, stream);
}
size_t fxrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    if (!stream || (size && count && !ptr) || (size && count > (size_t)-1 / size)) { bearer_set_error(22); return 0; }
    return fwrite(ptr, size, count, stream);
}
int fxlose(FILE *stream) {
    if (!stream) { bearer_set_error(22); return EOF; }
    return fclose(stream);
}

static ssize_t bearer_read_forward(int fd, void *buf, size_t n) {
    struct bearer_tx_slot *slot = NULL;
    uint32_t generation = 0;
    int pin = -1, nested;
    ssize_t r;
    if (!real_read) return -1;
    if (!altscreen_runtime_is_ready() || !p1404_armed || fd < 0 || !buf || !n)
        return real_read(fd, buf, n);
    nested = bearer_enter();
    if (nested || !g_forward_ready) {
        if (!nested) bearer_leave();
        return real_read(fd, buf, n);
    }
    bearer_reap_done();
    bearer_tx_lock();
    slot = bearer_find_open_locked(fd);
    if (slot && slot->pin_fd >= 0) {
        ++slot->io_refs;
        pin = slot->pin_fd;
        generation = slot->generation;
    } else {
        slot = NULL;
    }
    bearer_tx_unlock();
    bearer_leave();
    if (!slot) return real_read(fd, buf, n);
    r = real_read(pin, buf, n);
    if (r > 0)
        (void)iap2_control_fd_feed_incoming_gen(fd, generation, buf, (size_t)r);
    else if (r == 0)
        iap2_control_fd_peer_eof_gen(fd, generation);
    bearer_tx_lock();
    if (slot->generation == generation && slot->io_refs > 0) --slot->io_refs;
    bearer_tx_unlock();
    return r;
}

int xpen64(const char *path, int flags, ...) {
    if (!path) { bearer_set_error(22); return -1; }
    if (!real_open64) { bearer_set_error(11); return -1; }
    void *caller = __builtin_return_address(0);
    int fd;
    if (flags & K1004_QNX_O_CREAT) {
        va_list ap;
        int mode;
        va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
        fd = real_open64(path, flags, mode);
    } else {
        fd = real_open64(path, flags);
    }
    if (fd >= 0 && altscreen_runtime_is_ready() && p1404_armed &&
        direct_caller_in_range(g_nme_file_create_posix_start,
                               K1004_NME_FILE_CREATE_POSIX_SIZE, caller))
        (void)direct_nme_otg_fd_set(fd,
            path && !strcmp(path, "/dev/otg-cinemo"));
    return fd;
}

ssize_t xrite(int fd, const void *buf, size_t n) {
    if (!real_write) { bearer_set_error(11); return -1; }
    void *caller;
    int stock_net_write, nme_file_write;
    /* This branch precedes every resolver, lock, FD lookup and parser action. */
    if (!altscreen_runtime_is_ready() || !p1404_armed ||
        !p1404_mutate_armed || !alt_flag_iap2)
        return real_write(fd, buf, n);
    caller = __builtin_return_address(0);
    stock_net_write = direct_caller_in_stock(
        "NetSocket_WriteInternal", SIZE_NET_SOCKET_WRITE_INTERNAL, caller);
    nme_file_write = direct_caller_in_range(
        g_nme_file_write_start, K1004_NME_FILE_WRITE_SIZE, caller);
    if (!stock_net_write &&
        (!nme_file_write || !direct_nme_otg_fd_contains(fd)))
        return real_write(fd, buf, n);
    /* A fresh FD is claimed only by a syntactically valid iAP2 sync candidate.
     * Once claimed, later fragmented writes remain on the managed path. Audio,
     * logs and all other stock writes never take proxy locks or get duplicated. */
    if (!bearer_fd_is_managed(fd) &&
        (!buf || !n || !iap2_mutation_buffer_candidate(buf, n)))
        return real_write(fd, buf, n);
    return bearer_forward(BEARER_TX_WRITE, fd, buf, n, 0);
}
ssize_t xead(int fd, void *buf, size_t n) {
    if (!real_read) { bearer_set_error(11); return -1; }
    void *caller = __builtin_return_address(0);
    if (!altscreen_runtime_is_ready() || !p1404_armed ||
        !direct_caller_in_range(g_nme_file_read_start,
                                K1004_NME_FILE_READ_SIZE, caller) ||
        !direct_nme_otg_fd_contains(fd) || !bearer_fd_is_managed(fd))
        return real_read(fd, buf, n);
    return bearer_read_forward(fd, buf, n);
}
int xlose(int fd) {
    if (!real_close) { bearer_set_error(11); return -1; }
    void *caller;
    int targeted_close, nme_delete, rc;
    uint32_t nme_generation;
    if (!altscreen_runtime_is_ready() || !p1404_armed || fd < 0)
        return real_close(fd);
    caller = __builtin_return_address(0);
    nme_delete = direct_caller_in_range(g_nme_file_delete_start,
                                        K1004_NME_FILE_DELETE_SIZE, caller);
    targeted_close =
        direct_caller_in_stock("NetSocket_Delete", SIZE_NET_SOCKET_DELETE, caller) ||
        direct_caller_in_stock("NetSocket_Disconnect", SIZE_NET_SOCKET_DISCONNECT, caller) ||
        nme_delete;
    if (!targeted_close) return real_close(fd);
    nme_generation = nme_delete ? direct_nme_otg_fd_generation(fd) : 0;
    rc = bearer_fd_is_managed(fd) ? bearer_close_forward(fd) : real_close(fd);
    /* Keep ownership through the real close so concurrent writes cannot escape
     * lifecycle guards; clear only after libc has retired the descriptor. */
    if (nme_delete && rc == 0)
        direct_nme_otg_fd_clear_generation(fd, nme_generation);
    return rc;
}
#endif

#ifdef ALTSCREEN_HOOK_HOST_TEST
static int host_dup_offset(int fd) {
    if (fd < 0) { errno = EBADF; return -1; }
    return fd + 100;
}
static int *host_errno_pointer(void) { return &errno; }
void altscreen_hook_test_bind(write_fn w, send_fn s, recv_fn r, close_fn c) {
#ifndef ALTSCREEN_DIRECT_PROXY
    real_open64 = NULL;
#endif
    real_read = (read_fn)read;
    real_write = w; real_send = s; real_recv = r; real_close = c;
    real_dup = host_dup_offset;
    real_errno_ptr = host_errno_pointer;
    real_thread_create = (thread_create_fn)pthread_create;
    real_thread_join = (thread_join_fn)pthread_join;
    g_forward_ready = w && s && r && c;
    memset(g_bearer_guard, 0, sizeof(g_bearer_guard));
    memset(g_bearer_tx, 0, sizeof(g_bearer_tx));
    memset(g_bearer_raw, 0, sizeof(g_bearer_raw));
    memset(g_bearer_close, 0, sizeof(g_bearer_close));
    g_bearer_guard_lock = g_bearer_tx_lock = 0;
    g_next_generation = 1u;
    iap2_control_fd_reset();
}
void altscreen_hook_test_bind_lifecycle(dup_fn d, thread_create_fn create) {
    real_dup = d ? d : host_dup_offset;
    real_thread_create = create ? create : (thread_create_fn)pthread_create;
}
#ifdef ALTSCREEN_DIRECT_PROXY
/* Test-only fault injection; never exported by a vehicle build. */
void altscreen_hook_test_null_forwarder(int which) {
    switch (which) {
    case 0: real_open64 = NULL; break;
    case 1: real_write = NULL; break;
    case 2: real_read = NULL; break;
    case 3: real_close = NULL; break;
    }
}
void altscreen_hook_test_bind_nme(open64_fn o, read_fn r, void *write_start,
                                  void *read_start, void *delete_start,
                                  void *create_posix_start) {
    real_open64 = o ? o : (open64_fn)open64;
    real_read = r ? r : (read_fn)read;
    g_nme_file_write_start = (uintptr_t)write_start;
    g_nme_file_read_start = (uintptr_t)read_start;
    g_nme_file_delete_start = (uintptr_t)delete_start;
    g_nme_file_create_posix_start = (uintptr_t)create_posix_start;
    memset(g_nme_otg_fds, 0, sizeof(g_nme_otg_fds));
    memset(g_nme_otg_generations, 0, sizeof(g_nme_otg_generations));
    g_nme_otg_next_generation = 1u;
    g_forward_ready = real_open64 && real_read && real_write && real_send &&
                      real_recv && real_close && real_dup && real_errno_ptr &&
                      real_thread_create && real_thread_join;
}
#endif
int altscreen_hook_test_reserve_tx(int fd) {
    struct bearer_tx_slot *slot = NULL;
    int r = bearer_claim(fd, 1, BEARER_TX_WRITE, 0, &slot, NULL);
    if (r != 1 || !slot) return 0;
    slot->tuple_valid = 1;
    slot->tuple_kind = BEARER_TX_WRITE;
    slot->tuple_flags = 0;
    bearer_release(slot);
    return 1;
}
void altscreen_hook_test_fill_raw_guards(int fd) {
    int i;
    bearer_tx_lock();
    for (i = 0; i < BEARER_GUARD_SLOTS; ++i) {
        g_bearer_raw[i].active = 1;
        g_bearer_raw[i].fd = fd + i;
    }
    bearer_tx_unlock();
}
void altscreen_hook_test_clear_raw_guards(void) {
    bearer_tx_lock();
    memset(g_bearer_raw, 0, sizeof(g_bearer_raw));
    bearer_tx_unlock();
}
ssize_t altscreen_hook_test_nested_write(int fd, const void *buf, size_t n) {
    ssize_t r;
    (void)bearer_enter();
    r = write(fd, buf, n);
    bearer_leave();
    return r;
}
ssize_t altscreen_hook_test_nested_send(int fd, const void *buf, size_t n, int flags) {
    ssize_t r;
    (void)bearer_enter();
    r = send(fd, buf, n, flags);
    bearer_leave();
    return r;
}
ssize_t altscreen_hook_test_nested_recv(int fd, void *buf, size_t n, int flags) {
    ssize_t r;
    (void)bearer_enter();
    r = recv(fd, buf, n, flags);
    bearer_leave();
    return r;
}
ssize_t altscreen_hook_test_contended_write(int fd, const void *buf, size_t n) {
    ssize_t r;
    __sync_lock_test_and_set(&g_bearer_guard_lock, 1u);
    r = write(fd, buf, n);
    __sync_lock_release(&g_bearer_guard_lock);
    return r;
}
void altscreen_hook_test_reap(void) { bearer_reap_done(); }
int altscreen_hook_test_pending_drains(void) {
    int i, pending = 0;
    bearer_reap_done();
    bearer_tx_lock();
    for (i = 0; i < BEARER_TX_SLOTS; ++i)
        if (g_bearer_tx[i].active && g_bearer_tx[i].drain_started) ++pending;
    bearer_tx_unlock();
    return pending;
}
void altscreen_runtime_ensure_initialized(void) { }
int altscreen_runtime_is_ready(void) { return 1; }
int altscreen_runtime_wait_ready(unsigned timeout_ms) { (void)timeout_ms; return 1; }
#else
static volatile unsigned g_runtime_init_state; /* 0 idle, 1 worker, 2 negotiation-ready, 3 inert */
static volatile unsigned g_runtime_display_state; /* 0 pending, 1 verified, 2 deferred */
#define ALTSCREEN_NEGOTIATION_WAIT_SLICE_US 5000u

int altscreen_runtime_wait_ready(unsigned timeout_ms) {
    unsigned waited_ms = 0u;
    while (__sync_fetch_and_add(&g_runtime_init_state, 0u) == 1u &&
           waited_ms < timeout_ms) {
        usleep(ALTSCREEN_NEGOTIATION_WAIT_SLICE_US);
        waited_ms += 5u;
    }
    return altscreen_runtime_is_ready();
}

/* Stream 111 firewall setup spawns /bin/sh and pfctl. Neither project hook
 * may propagate into those helper exec() descendants. Parent mappings are
 * already established; only the inherited environment is cleaned here. */
#define ALTSCREEN_SELF_PRELOAD "/mnt/app/root/carplay-altscreen/lib/libcarplay_altscreen.so"
#define ALTSCREEN_SELF_PRELOAD_LEGACY "/mnt/app/root/hooks/libcarplay_altscreen.so"
#define ALTSCREEN_RGI_PRELOAD "/mnt/app/root/carplay-altscreen/lib/libcarplay_rgi_meta.so"
#define ALTSCREEN_RGI_PRELOAD_LEGACY "/mnt/app/root/hooks/libcarplay_hook.so"

static int altscreen_preload_token_is_project_hook(const char *token, size_t length) {
    const size_t current_len = sizeof(ALTSCREEN_SELF_PRELOAD) - 1u;
    const size_t legacy_len = sizeof(ALTSCREEN_SELF_PRELOAD_LEGACY) - 1u;
    const size_t rgi_len = sizeof(ALTSCREEN_RGI_PRELOAD) - 1u;
    const size_t rgi_legacy_len = sizeof(ALTSCREEN_RGI_PRELOAD_LEGACY) - 1u;
    return (length == current_len && !memcmp(token, ALTSCREEN_SELF_PRELOAD, current_len)) ||
           (length == legacy_len && !memcmp(token, ALTSCREEN_SELF_PRELOAD_LEGACY, legacy_len)) ||
           (length == rgi_len && !memcmp(token, ALTSCREEN_RGI_PRELOAD, rgi_len)) ||
           (length == rgi_legacy_len &&
            !memcmp(token, ALTSCREEN_RGI_PRELOAD_LEGACY, rgi_legacy_len));
}
static void altscreen_strip_project_hooks_from_child_preload(void) {
    const char *value = getenv("LD_PRELOAD");
    const char *cursor;
    char *clean, *out;
    size_t value_len;
    int kept = 0;
    if (!value || !*value) return;
    value_len = strlen(value);
    clean = (char *)malloc(value_len + 1u);
    if (!clean) {
        if (altscreen_preload_token_is_project_hook(value, value_len)) (void)unsetenv("LD_PRELOAD");
        return;
    }
    cursor = value; out = clean;
    while (*cursor) {
        const char *end = strchr(cursor, ':');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length && !altscreen_preload_token_is_project_hook(cursor, length)) {
            if (kept) *out++ = ':';
            memcpy(out, cursor, length); out += length; kept = 1;
        }
        if (!end) break;
        cursor = end + 1;
    }
    *out = 0;
    if (kept) (void)setenv("LD_PRELOAD", clean, 1);
    else (void)unsetenv("LD_PRELOAD");
    free(clean);
}

#define ALTSCREEN_GEOMETRY_WAIT_STEPS    30u
#define ALTSCREEN_GEOMETRY_WAIT_US  1000000u

int altscreen_runtime_is_ready(void) {
    return __sync_fetch_and_add(&g_runtime_init_state, 0u) == 2u;
}

static void *altscreen_runtime_init_worker(void *unused) {
    int backend_ready;
    int native_stock_ready;
    int nme_io_ready;
    int native_geometry_ready = 0;
    int process_allowed;
    int state_root_ready;
    unsigned geometry_step;
    const char *pname;
    (void)unused;

    /*
     * V3.4 removes removable-SD state from the CarPlay runtime authority.
     * Keep reporting whether an SD state root happens to be visible for
     * diagnostics, but never wait for it and never let it arm/disarm the
     * current CarPlay negotiation.
     */
    state_root_ready = altscreen_state_root_is_authoritative();

    iap2_control_fd_reset();
    altscreen_init();
    if (!altscreen_log_start_async()) {
        p1404_armed = 0;
        p1404_mutate_armed = 0;
        __sync_lock_test_and_set(&g_runtime_init_state, 3u);
        return NULL;
    }
    altscreen_paths_report();
    altscreen_log("PHASE=RUNTIME_AUTHORITY policy=INSTALLED_PRELOAD sd_authoritative=%d sd_gate=DISABLED waited_ms=0 startup_thread_blocked=0",
                  state_root_ready ? 1 : 0);
    (void)p1404_bind_stock_exports();
    native_stock_ready = p1404_cockpit_native_bind_stock();
    nme_io_ready = direct_bind_nme_io();
    p1404_airplay_bind();
    altscreen_log("PHASE=STOCK_INTERNAL_REDIRECT result=%s slots=16 scope=AUG22_PROFILE_GOT code_bytes_changed=0",
                  p1404_direct_internal_redirects_ready() ? "PASS" : "REFUSED");
    if (!p1404_direct_internal_redirects_ready()) {
        p1404_armed = 0;
        p1404_mutate_armed = 0;
        __sync_lock_test_and_set(&g_runtime_init_state, 3u);
        return NULL;
    }
    pname = p1404_process_name();
    process_allowed = process_is_allowed(pname);
    altscreen_log("PHASE=RUNTIME_PROCESS_IDENTITY result=%s process=%s policy=INSTALLED_PRELOAD identity_override=DISABLED worker_started=1 stock_until_ready=1",
                  process_allowed ? "PASS" : "REFUSED", pname);
    if (!process_allowed || !p1404_probe_stack()) {
        p1404_armed = 0;
        p1404_mutate_armed = 0;
        __sync_lock_test_and_set(&g_runtime_init_state, 3u);
        return NULL;
    }
    p1404_airplay_bind();
    if (!g_forward_ready || !native_stock_ready || !nme_io_ready) {
        altscreen_log("PHASE=RUNTIME_FORWARDING_GATE result=REFUSED libc_forward=%d native_stock=%d nme_exact_io=%d stock_fail_open=YES",
                      g_forward_ready, native_stock_ready, nme_io_ready);
        p1404_armed = 0;
        p1404_mutate_armed = 0;
        __sync_lock_test_and_set(&g_runtime_init_state, 3u);
        return NULL;
    }
    altscreen_log("PHASE=RUNTIME_FORWARDING_GATE result=PASS libc_forward=1 native_stock=1 nme_exact_io=1");
    altscreen_log("PHASE=LOG_WRITER_READY mode=bounded_async queue_slots=256 line_bytes=768 max_bytes=16777216 no_truncate=1 deferred_worker=1 startup_thread_blocked=0");
    /* V3.4 fixes the legacy iAP2 profile to observe-only in-process. No SD
     * profile file participates in normal CarPlay negotiation. */
    altscreen_profile_set(&altscreen_prof_observe);
    altscreen_log("PROFILE policy=V34_FIXED_OBSERVE removable_state_dependency=NONE");
    p1404_airplay_bind();
    p1404_airplay_load_flags();
    backend_ready = p1404_private111_backend_install();
    altscreen_log("PHASE=PRIVATE111_BACKEND_INSTALL_RESULT ready=%d identity=%d create111_armed=%d",
                  backend_ready, p1404_identity_ok, alt_flag_create111);
    if (!backend_ready) {
        p1404_armed = 0;
        p1404_mutate_armed = 0;
        __sync_synchronize();
        __sync_lock_test_and_set(&g_runtime_init_state, 3u);
        return NULL;
    }

    /*
     * V3.5 separates protocol readiness from physical-display readiness.
     * Once the exact stock ABI, CF bindings and private111 backend are ready,
     * the first AirPlay capability transaction may safely advertise AltScreen.
     * Display-1 geometry remains an asynchronous confirmation step and must not
     * make an early /info or descriptor-free SETUP fall back to stock forever.
     */
    __sync_synchronize();
    __sync_lock_test_and_set(&g_runtime_init_state, 2u);
    altscreen_log("PHASE=NEGOTIATION_READY result=PASS policy=V35_EARLY_PROTOCOL_READY geometry_ready=0 geometry_gate_async=1 first_capability_can_wait_bounded=1");

    /* Confirm live Screen display-1 geometry in the background.  Private111
     * attach performs its own just-in-time refresh, so a slow Screen service
     * no longer disables the whole CarPlay session. */
    for (geometry_step = 0; geometry_step < ALTSCREEN_GEOMETRY_WAIT_STEPS;
         ++geometry_step) {
        if (p1404_cockpit_native_refresh_geometry()) {
            native_geometry_ready = 1;
            break;
        }
        usleep(ALTSCREEN_GEOMETRY_WAIT_US);
    }
    g_runtime_display_state = native_geometry_ready ? 1u : 2u;
    altscreen_log("PHASE=RUNTIME_GEOMETRY_GATE result=%s attempts=%u startup_thread_blocked=0 fixed_fallback=0 negotiation_ready=1 private111_attach_will_retry=1",
                  native_geometry_ready ? "PASS" : "DEFERRED",
                  native_geometry_ready ? geometry_step + 1u : geometry_step);
    if (!native_geometry_ready) {
        altscreen_log("WARN PHASE=DISPLAY_GEOMETRY_DEFERRED reason=screen_service_not_ready_within_background_window negotiation_kept=1 renderer_attach_requires_live_refresh=1");
    }
    altscreen_log("RUNTIME ready process=%s identity=%d armed=%d mutate=%d bearer=%d profile=%s private111_backend=%d negotiation_ready=1 display_geometry_ready=%d",
                  pname, p1404_identity_ok, p1404_armed, p1404_mutate_armed,
                  g_forward_ready, altscreen_profile_name(), backend_ready,
                  native_geometry_ready ? 1 : 0);
    altscreen_log("RUNTIME bearer open64=%p read=%p write=%p send=%p recv=%p close=%p dup=%p nme_create=%p nme_write=%p nme_read=%p nme_delete=%p",
                  (void *)real_open64, (void *)real_read, (void *)real_write, (void *)real_send,
                  (void *)real_recv, (void *)real_close, (void *)real_dup,
#ifdef ALTSCREEN_DIRECT_PROXY
                  (void *)g_nme_file_create_posix_start,
                  (void *)g_nme_file_write_start, (void *)g_nme_file_read_start,
                  (void *)g_nme_file_delete_start
#else
                  NULL, NULL, NULL, NULL
#endif
                  );
    return NULL;
}

/* Never perform runtime initialization on dio_manager's startup thread. The
 * constructor or first high-level hook launches one worker and returns
 * immediately; all profile, logging, ABI and backend work stays on that worker. */
void altscreen_runtime_ensure_initialized(void) {
    pthread_t thread;
    if (g_runtime_init_state != 0u) return;
    if (!__sync_bool_compare_and_swap(&g_runtime_init_state, 0u, 1u)) return;
    if (!real_thread_create ||
        real_thread_create(&thread, NULL, altscreen_runtime_init_worker, NULL) != 0) {
        p1404_armed = 0;
        p1404_mutate_armed = 0;
        __sync_lock_test_and_set(&g_runtime_init_state, 3u);
    }
}

__attribute__((constructor)) static void altscreen_ctor(void) {
    const char *pname;

    /* Remove only this hook from the inherited preload before dio_manager can
     * spawn shell/pfctl helpers. Preserve every unrelated preload token. */
    altscreen_strip_project_hooks_from_child_preload();

    /* Defense in depth: even if a helper is launched with an explicit preload,
     * do not bind libc forwarding, patch GOT slots or start the runtime worker
     * outside the measured CarPlay host processes. FORCE_START is authorization,
     * never a process-identity override. */
    pname = p1404_process_name();
    if (!process_is_allowed(pname)) {
        p1404_armed = 0;
        p1404_mutate_armed = 0;
        __sync_lock_test_and_set(&g_runtime_init_state, 3u);
        return;
    }

    bind_bearer();
    (void)p1404_direct_install_internal_redirects();
    altscreen_runtime_ensure_initialized();
}
#endif
