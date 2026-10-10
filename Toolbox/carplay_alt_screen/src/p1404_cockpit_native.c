/*
 * p1404_cockpit_native.c - private111 stock-decoder compatibility staging.
 *
 * Direct-display V2 keeps the V1-proven private111/stock OMX/displayable3/
 * Context80 chain and changes only decoded-pixel acquisition. The vendor
 * 0x0001000c OMX/Screen buffer is no longer treated as row-linear NV12.
 * After stock CScreenRender posts each private frame, Screen linearizes the
 * exact stock window into a normal pixmap; the result is repacked into the
 * existing /carplay111_decoded NV12 SHM. The real instrument sink remains the
 * separate MMI-derived displayable3 sidecar under Java-owned Context80.
 *
 * The historical displayable58/manage-window code below remains active only so
 * stock OMX can complete its normal buffer lifecycle while this fallback is
 * used.  It is decoder staging, not the direct-display source or success gate.
 * /carplay111_h264 is captured independently at ScreenStreamProcessData and is
 * the handoff boundary for the future standalone Qualcomm/QNX decoder backend.
 * Main110 remains exact stock passthrough.
 */
#include "p1404_cockpit_native.h"
#include "p1404_lock_wait.h"
#include "p1404_abi.h"
#include "p1404_airplay.h"
#include "altscreen_core.h"
#include "altscreen_paths.h"
#include "p1404_observe.h"
#include "p1404_control_fence.h"
#include "private111_direct_tap.h"
#include "p1404_stream_recovery.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/time.h>

/* Host fixtures and the legacy preload build may omit the K1004 direct
 * resolver. Keep it optional there; the exact direct overlay provides it. */
extern void *p1404_direct_stock_symbol_named(const char *name)
    __attribute__((weak));

#define NATIVE_SLOTS 8u
#define SCREEN_STREAM_VIDEO_IMPL_OFF 0x38u
#define OMX_VIDEO_RENDERER_OFF       0x0cu
#define NATIVE_MIN_POSTS             3u
#define NATIVE_POST_WINDOW_SECONDS   2u
#define NATIVE_STALL_SECONDS         3u
#define NATIVE_EVENT_RETRY_SECONDS   2u
#define NATIVE_EVENT_TIMEOUT_SECONDS 4u
#define P1404_P_WAIT                 0
#define NATIVE_CONFIG_REFUSED_STATUS (-58796)
#define P1404_RTLD_NOW                2
#define SCREEN_WINDOW_MANAGER_CONTEXT 1
#define SCREEN_DISPLAY_MANAGER_CONTEXT 8
#define SCREEN_PROPERTY_SIZE          40
#define SCREEN_PROPERTY_DISPLAY_COUNT 59
#define SCREEN_PROPERTY_DISPLAYS      60
#define SCREEN_PROPERTY_ID            87
#define SCREEN_MAX_DISPLAYS            8
#define DISPLAY_MANAGER_SECRET         "How are you gentlemen?"
#define DMDT_PATH                    "/eso/bin/apps/dmdt"

#define CSCREEN_CONFIG_SYMBOL "_ZN3dio13CScreenRender6configERKNS_16st_screen_configE"
#define CSCREEN_RENDER_SYMBOL "_ZN3dio13CScreenRender6renderEPh"

typedef int (*f_cscreen_config_t)(void *, const struct p1404_screen_config *);
typedef int (*f_cscreen_render_t)(void *, unsigned char *);
typedef int (*f_screen_stream_start_t)(void *);
typedef void *(*f_screen_copy_main_t)(int *);
typedef int (*f_screen_create_t)(void **, const void *);
typedef int (*f_screen_copy_delegates_t)(void *, void *);
typedef void (*f_screen_register_delegates_t)(void *, const void *);
typedef unsigned long native_pthread_t;
typedef int (*f_pthread_create_t)(native_pthread_t *, const void *,
                                  void *(*)(void *), void *);
typedef int (*f_pthread_detach_t)(native_pthread_t);
typedef long (*f_spawnl_t)(int, const char *, const char *, ...);
typedef void *screen_context_t;
typedef void *screen_display_t;
typedef void *screen_window_t;
typedef int (*f_screen_create_context_t)(screen_context_t *, int);
typedef int (*f_screen_destroy_context_t)(screen_context_t);
typedef int (*f_screen_get_context_iv_t)(screen_context_t, int, int *);
typedef int (*f_screen_get_context_pv_t)(screen_context_t, int, void **);
typedef int (*f_screen_get_display_iv_t)(screen_display_t, int, int *);
typedef int (*f_screen_create_window_group_t)(screen_window_t, const char *);
typedef int (*f_screen_create_window_buffers_t)(screen_window_t, int);
typedef int (*f_screen_manage_window_t)(screen_window_t, const char *);

enum native_route_action {
    NATIVE_ROUTE_NONE = 0,
    NATIVE_ROUTE_ACTIVATE = 1,
    NATIVE_ROUTE_RESTORE = 2
};

struct native_slot {
    struct alt111_stream_recovery recovery;
    int processing_live;
    void *receiver;
    void *stream;
    void *video_impl;
    void *renderer;
    screen_window_t window;
    uint64_t first_post_at;
    uint64_t last_post_at;
    uint32_t generation;
    uint32_t state_generation;
    uint32_t posts;
    uint32_t action_generation;
    uint32_t linearizer_aux_drops;
    unsigned long owner_thread;
    int monitor_started;
    int action;
    int action_pending;
    int preconfig_rewritten;
    int config_ok;
    int decoder_marked;
    int visible;
    int first_real_frame_posted;
    uint32_t config_width;
    uint32_t config_height;
    uint32_t config_format;
    uint32_t config_usage;
    uint64_t ui_event_at;
    uint64_t keyframe_event_at;
    uint64_t view_area_event_at;
    uint64_t view_area_ack_at;
    uint32_t view_area_inflight_seq;
    uint32_t view_area_acked_seq;
    uint32_t view_area_frame_generation;
    uint32_t view_area_frame_count;
    int ui_event_state;       /* 0=needed, 1=inflight, 2=accepted, 3=timed-out */
    int keyframe_event_state; /* 0=needed, 1=inflight, 2=accepted, 3=timed-out */
    int view_area_event_state;/* 0=needed, 1=inflight, 2=acknowledged */
    int view_area_target;     /* 0=FULL, 1=SMALL, -1=unknown */
    int view_area_applied;    /* last acknowledged index, -1=unknown */
    int view_area_frame_baseline_valid;
    int view_area_frame_pending;
};

struct native_thread_job {
    struct native_slot *slot;
    void *receiver;
    void *stream;
    uint32_t generation;
    uint32_t state_generation;
    int action;
};

static struct native_slot g_native[NATIVE_SLOTS];
static volatile unsigned g_native_guard;
static volatile unsigned g_route_guard;
static volatile unsigned g_recovery_guard;
/* Protected by g_route_guard. Identifies the generation that most recently
 * committed context76, so an old detached worker can never restore over a
 * newer private session. */
static uint32_t g_route_generation;
static f_cscreen_config_t g_real_config;
static f_cscreen_render_t g_real_render;
static f_screen_stream_start_t g_real_stream_start;
static f_screen_copy_main_t g_real_screen_copy_main;
static f_screen_create_t g_real_screen_create;
static f_screen_copy_delegates_t g_real_screen_copy_delegates;
static f_screen_register_delegates_t g_real_screen_register_delegates;

static void *native_direct_stock(const char *name) {
    return p1404_direct_stock_symbol_named ?
        p1404_direct_stock_symbol_named(name) : NULL;
}

static struct {
    void *receiver;
    void *stream;
    unsigned long owner_thread;
    int active;
    int invoked;
    int attached;
} g_start_claim;
static f_pthread_create_t g_pthread_create;
static f_pthread_detach_t g_pthread_detach;
static f_spawnl_t g_spawnl;
static void *g_screen_buffer_lib;
static f_screen_create_window_group_t g_screen_create_window_group;
static f_screen_create_window_buffers_t g_screen_create_window_buffers;
static f_screen_manage_window_t g_screen_manage_window;
static struct {
    void *renderer;
    screen_window_t window;
    unsigned long owner_thread;
    int active;
    int group_skipped;
    int manage_rc;
    int buffers_rc;
    int managed;
} g_managed_config;
static uint32_t g_generation;
static uint32_t g_target_width;
static uint32_t g_target_height;

static void native_lock(void) {
    while (__sync_lock_test_and_set(&g_native_guard, 1u) != 0u) p1404_lock_wait_yield();
}
static void native_unlock(void) { __sync_lock_release(&g_native_guard); }
static void route_lock(void) {
    while (__sync_lock_test_and_set(&g_route_guard, 1u) != 0u) p1404_lock_wait_yield();
}
static void route_unlock(void) { __sync_lock_release(&g_route_guard); }

static void *read_ptr_at(void *base, unsigned off) {
    void *value = NULL;
    if (base) memcpy(&value, (const unsigned char *)base + off, sizeof(value));
    return value;
}


/* Stock CScreenRender owns the QNX window and buffers. The measured
 * P1404/K1004 layout is CScreenRender+0x40 -> CWindowBuffers, whose +0 array
 * and +4 count are used by stock getWindowBufferHeader/render. The same Screen
 * handle is registered with DisplayManager immediately before buffer creation;
 * stock OMX allocation, posting and teardown remain unchanged. */
static int bind_screen_native_api(void) {
    void *lib;
    f_screen_create_window_group_t create_group;
    f_screen_create_window_buffers_t create_buffers;
    f_screen_manage_window_t manage_window;
    if (g_screen_create_window_group && g_screen_create_window_buffers &&
        g_screen_manage_window)
        return 1;
    lib = dlopen("libscreen.so.1", P1404_RTLD_NOW);
    if (!lib) return 0;
    create_group = (f_screen_create_window_group_t)
        dlsym(lib, "screen_create_window_group");
    create_buffers = (f_screen_create_window_buffers_t)
        dlsym(lib, "screen_create_window_buffers");
    manage_window = (f_screen_manage_window_t)dlsym(lib, "screen_manage_window");
    native_lock();
    if (!g_screen_buffer_lib) {
        g_screen_buffer_lib = lib;
        g_screen_create_window_group = create_group;
        g_screen_create_window_buffers = create_buffers;
        g_screen_manage_window = manage_window;
        lib = NULL;
    }
    native_unlock();
    if (lib) dlclose(lib);
    return g_screen_create_window_group && g_screen_create_window_buffers &&
           g_screen_manage_window;
}



static int native_route_requested(void) {
    /* Java/HMI owns Context80. Native route activation is disabled.
     * Stock displayable58 exists only as V1 decoder compatibility staging;
     * direct display consumes /carplay111_decoded, never Window58 readback. */
    return 0;
}

static struct native_slot *find_renderer_locked(void *renderer) {
    unsigned i;
    for (i = 0; i < NATIVE_SLOTS; ++i)
        if (g_native[i].renderer == renderer && g_native[i].stream)
            return &g_native[i];
    return NULL;
}

static struct native_slot *find_preconfig_locked(void *renderer,
                                                  unsigned long owner_thread) {
    unsigned i;
    void *candidate;
    for (i = 0; i < NATIVE_SLOTS; ++i) {
        if (!g_native[i].stream || g_native[i].renderer ||
            g_native[i].owner_thread != owner_thread || !g_native[i].video_impl)
            continue;
        candidate = read_ptr_at(g_native[i].video_impl, OMX_VIDEO_RENDERER_OFF);
        if (candidate == renderer) return &g_native[i];
    }
    return NULL;
}

static struct native_slot *find_stream_locked(void *receiver, void *stream) {
    unsigned i;
    for (i = 0; i < NATIVE_SLOTS; ++i)
        if (g_native[i].receiver == receiver && g_native[i].stream == stream)
            return &g_native[i];
    return NULL;
}

static struct native_slot *free_slot_locked(void) {
    unsigned i;
    for (i = 0; i < NATIVE_SLOTS; ++i)
        if (!g_native[i].stream) return &g_native[i];
    return NULL;
}

int p1404_cockpit_native_get_geometry(uint32_t *width, uint32_t *height) {
    uint32_t w, h;
    native_lock();
    w = g_target_width;
    h = g_target_height;
    native_unlock();
    if (!w || !h) return 0;
    if (width) *width = w;
    if (height) *height = h;
    return 1;
}

void p1404_cockpit_native_set_test_geometry(uint32_t width, uint32_t height) {
    if (!width || !height || width > 8192u || height > 8192u) return;
    native_lock();
    g_target_width = width;
    g_target_height = height;
    native_unlock();
    (void)altscreen_set_cluster_geometry(width, height);
}

int p1404_cockpit_native_refresh_geometry(void) {
    void *lib = NULL;
    screen_context_t context = NULL;
    screen_display_t displays[SCREEN_MAX_DISPLAYS];
    f_screen_create_context_t create_context = NULL;
    f_screen_destroy_context_t destroy_context = NULL;
    f_screen_get_context_iv_t get_context_iv = NULL;
    f_screen_get_context_pv_t get_context_pv = NULL;
    f_screen_get_display_iv_t get_display_iv = NULL;
    int count = 0, i, id, size[2], ok = 0;
    uint32_t width = 0, height = 0;

    memset(displays, 0, sizeof(displays));
    lib = dlopen("libscreen.so.1", P1404_RTLD_NOW);
    if (!lib) goto done;
    create_context = (f_screen_create_context_t)dlsym(lib, "screen_create_context");
    destroy_context = (f_screen_destroy_context_t)dlsym(lib, "screen_destroy_context");
    get_context_iv = (f_screen_get_context_iv_t)dlsym(lib, "screen_get_context_property_iv");
    get_context_pv = (f_screen_get_context_pv_t)dlsym(lib, "screen_get_context_property_pv");
    get_display_iv = (f_screen_get_display_iv_t)dlsym(lib, "screen_get_display_property_iv");
    if (!create_context || !destroy_context || !get_context_iv ||
        !get_context_pv || !get_display_iv) goto done;
    if (create_context(&context, SCREEN_WINDOW_MANAGER_CONTEXT) != 0 &&
        create_context(&context, SCREEN_DISPLAY_MANAGER_CONTEXT) != 0) goto done;
    if (get_context_iv(context, SCREEN_PROPERTY_DISPLAY_COUNT, &count) != 0 ||
        count <= 0 || count > SCREEN_MAX_DISPLAYS) goto done;
    if (get_context_pv(context, SCREEN_PROPERTY_DISPLAYS, (void **)displays) != 0)
        goto done;
    for (i = 0; i < count; ++i) {
        id = -1;
        size[0] = size[1] = 0;
        if (get_display_iv(displays[i], SCREEN_PROPERTY_ID, &id) != 0 ||
            get_display_iv(displays[i], SCREEN_PROPERTY_SIZE, size) != 0) continue;
        altscreen_log("PHASE=NATIVE_111_DISPLAY_CANDIDATE index=%d id=%d size=%dx%d",
                      i, id, size[0], size[1]);
        if (id == (int)ALT111_TARGET_DISPLAY_ID && size[0] > 0 && size[1] > 0 &&
            size[0] <= 8192 && size[1] <= 8192) {
            width = (uint32_t)size[0];
            height = (uint32_t)size[1];
            ok = 1;
            break;
        }
    }
done:
    if (context && destroy_context) (void)destroy_context(context);
    if (lib) dlclose(lib);
    if (!ok) {
        altscreen_log("ERROR PHASE=NATIVE_111_GEOMETRY_QUERY_FAILED target_display=1 count=%d fixed_fallback=0 private111_refused=1",
                      count);
        return 0;
    }
    native_lock();
    g_target_width = width;
    g_target_height = height;
    native_unlock();
    (void)altscreen_set_cluster_geometry(width, height);
    altscreen_log("PHASE=NATIVE_111_GEOMETRY_READY target_display=1 size=%ux%u source=SCREEN_PROPERTY_SIZE fixed_fallback=0",
                  width, height);
    return 1;
}

int p1404_cockpit_native_rewrite_config(const struct p1404_screen_config *input,
                                        struct p1404_screen_config *output) {
    uint32_t requested_width, requested_height;
    if (!input || !output || sizeof(*output) != 44u) return 0;
    if (!input->source_width || !input->source_height ||
        input->source_width > 8192u || input->source_height > 8192u ||
        !p1404_cockpit_native_get_geometry(&requested_width, &requested_height))
        return 0;
    memcpy(output, input, sizeof(*output));
    /* Respect the resolution negotiated from the runtime display request.
     * Window and source remain identical so Screen performs no scaling. */
    output->window_width = input->source_width;
    output->window_height = input->source_height;
    output->source_width = input->source_width;
    output->source_height = input->source_height;
    output->offset_x = 0;
    output->offset_y = 0;
    output->window_id = ALT111_DISPLAYABLE_ID;
    altscreen_log("PHASE=NATIVE_111_GEOMETRY_APPLIED requested=%ux%u negotiated=%ux%u fixed=0 screen_scaling=0",
                  requested_width, requested_height,
                  input->source_width, input->source_height);
    return 1;
}

int p1404_cockpit_native_bind_stock(void) {
    if (!g_real_config)
        g_real_config = (f_cscreen_config_t)p1404_stock_symbol_named(CSCREEN_CONFIG_SYMBOL);
    if (!g_real_render)
        g_real_render = (f_cscreen_render_t)p1404_stock_symbol_named(CSCREEN_RENDER_SYMBOL);
    if (!g_real_stream_start)
        g_real_stream_start = (f_screen_stream_start_t)
            p1404_stock_symbol_named("ScreenStreamStart");
    if (!g_real_screen_copy_main)
        g_real_screen_copy_main = (f_screen_copy_main_t)
            p1404_stock_symbol_named("ScreenCopyMain");
    if (!g_real_screen_create)
        g_real_screen_create = (f_screen_create_t)
            p1404_stock_symbol_named("ScreenCreate");
    if (!g_real_screen_copy_delegates)
        g_real_screen_copy_delegates = (f_screen_copy_delegates_t)
            p1404_stock_symbol_named("ScreenCopyDelegates");
    if (!g_real_screen_register_delegates)
        g_real_screen_register_delegates = (f_screen_register_delegates_t)
            p1404_stock_symbol_named("ScreenRegisterDelegates");
    if (!g_pthread_create)
        g_pthread_create = (f_pthread_create_t)dlsym(RTLD_DEFAULT, "pthread_create");
    if (!g_pthread_detach)
        g_pthread_detach = (f_pthread_detach_t)dlsym(RTLD_DEFAULT, "pthread_detach");
    if (!g_spawnl)
        g_spawnl = (f_spawnl_t)dlsym(RTLD_DEFAULT, "spawnl");
    /* Loader constructors must resolve symbols only. Creating a Screen manager
     * context here can fault or deadlock dio_manager before authorization and
     * before the logger exists. Geometry is queried lazily at Alt advertisement
     * and again before the private renderer attaches. */
    altscreen_log("PHASE=NATIVE_111_STOCK_BIND config=%d render=%d stream_start=%d screen_copy_main=%d screen_create=%d copy_delegates=%d register_delegates=%d pthread_create=%d pthread_detach=%d spawnl=%d geometry=%d",
                  g_real_config != NULL, g_real_render != NULL,
                  g_real_stream_start != NULL, g_real_screen_copy_main != NULL,
                  g_real_screen_create != NULL,
                  g_real_screen_copy_delegates != NULL,
                  g_real_screen_register_delegates != NULL,
                  g_pthread_create != NULL,
                  g_pthread_detach != NULL, g_spawnl != NULL,
                  p1404_cockpit_native_get_geometry(NULL, NULL));
    return g_real_config && g_real_render && g_real_stream_start &&
           g_real_screen_copy_main && g_real_screen_create &&
           g_real_screen_copy_delegates && g_real_screen_register_delegates &&
           bind_screen_native_api();
}

static int run_dmdt(const char *verb, const char *a, const char *b) {
    long rc;
    if (!g_spawnl || !verb || !a || !b) return -1;
    rc = g_spawnl(P1404_P_WAIT, DMDT_PATH, "dmdt", verb, a, b, (char *)0);
    return (int)rc;
}

static int run_activate_route(void) {
    int a, b, c;
    a = run_dmdt("dc", "76", "58");
    b = run_dmdt("sc", "1", "72");
    c = run_dmdt("sc", "1", "76");
    altscreen_log("PHASE=NATIVE_111_ROUTE_ACTIVATE_RESULT dc76_58=%d sc1_72=%d sc1_76=%d",
                  a, b, c);
    return a == 0 && b == 0 && c == 0;
}

static int run_restore_route(void) {
    int a, b, c;
    a = run_dmdt("dc", "76", "58");
    b = run_dmdt("sc", "1", "72");
    c = run_dmdt("sc", "1", "74");
    altscreen_log("PHASE=NATIVE_111_ROUTE_RESTORE_RESULT dc76_58=%d sc1_72=%d sc1_74=%d",
                  a, b, c);
    return a == 0 && b == 0 && c == 0;
}

static void *native_route_worker(void *arg) {
    struct native_thread_job *job = (struct native_thread_job *)arg;
    struct native_slot *slot = job ? job->slot : NULL;
    void *receiver = job ? job->receiver : NULL;
    void *stream = job ? job->stream : NULL;
    uint32_t generation = job ? job->generation : 0;
    uint32_t state_generation = job ? job->state_generation : 0;
    int action = job ? job->action : NATIVE_ROUTE_NONE;
    int live = 0;
    int executed = 0;
    int ok = 0;
    int route_still_requested = 0;
    int stale_activate = 0;
    int committed_restore = 0;

    /* dmdt changes global display-manager state. Serialize the entire
     * validate/execute/commit sequence so an old detached action cannot race a
     * new generation and restore context74 over its context76 activation. */
    route_lock();
    native_lock();
    live = slot && slot->stream == stream && slot->receiver == receiver &&
           slot->generation == generation &&
           slot->state_generation == state_generation &&
           slot->action_pending && slot->action == action;
    native_unlock();

    if (live && action == NATIVE_ROUTE_ACTIVATE) {
        if (native_route_requested()) {
            executed = 1;
            ok = run_activate_route();
        }
        route_still_requested = native_route_requested();
        if (executed && !ok) (void)run_restore_route();
    } else if (live && action == NATIVE_ROUTE_RESTORE) {
        executed = 1;
        ok = run_restore_route();
    }

    native_lock();
    live = slot && slot->stream == stream && slot->receiver == receiver &&
           slot->generation == generation &&
           slot->state_generation == state_generation;
    if (live) {
        slot->action_pending = 0;
        slot->action = NATIVE_ROUTE_NONE;
        if (action == NATIVE_ROUTE_ACTIVATE) {
            if (executed && ok && route_still_requested) {
                slot->visible = 1;
                g_route_generation = generation;
            } else if (executed && ok) {
                stale_activate = 1;
            }
        } else if (action == NATIVE_ROUTE_RESTORE && executed && ok) {
            slot->visible = 0;
            slot->first_real_frame_posted = 0;
            slot->posts = 0;
            slot->first_post_at = 0;
            if (g_route_generation == generation) g_route_generation = 0;
            committed_restore = 1;
        }
    } else if (action == NATIVE_ROUTE_ACTIVATE && executed && ok) {
        stale_activate = 1;
    }
    native_unlock();

    if (stale_activate) {
        (void)run_restore_route();
        if (g_route_generation == generation) g_route_generation = 0;
    }
    route_unlock();

    if (committed_restore)
        alt_state_mark_native(receiver, state_generation,
                              ALT_STATE_NATIVE_COCKPIT_HIDDEN,
                              "native111-route-restored-context74");
    if (action == NATIVE_ROUTE_ACTIVATE && executed && ok && !stale_activate) {
        uint32_t width = 0, height = 0;
        (void)p1404_cockpit_native_get_geometry(&width, &height);
        alt_state_mark_native(receiver, state_generation,
                              ALT_STATE_NATIVE_UI_ACTIVE,
                              "private111-context76-activation-after-config-and-showui");
        alt_state_mark_native(receiver, state_generation,
                              ALT_STATE_NATIVE_COCKPIT_VISIBLE,
                              "private111-stock-omx->displayable58-context76-dynamic-geometry");
        altscreen_log("PHASE=NATIVE_111_COCKPIT_ACTIVE receiver=%p stream=%p requested_geometry=%ux%u displayable=58 context=76 gate=first_real_type111_frame_posted",
                      receiver, stream, width, height);
    }
    free(job);
    return NULL;
}

static int request_route_action(struct native_slot *slot, int action) {
    struct native_thread_job *job;
    native_pthread_t thread = 0;
    int rc;
    if (!slot || !g_pthread_create || !g_pthread_detach || !g_spawnl) return 0;
    job = (struct native_thread_job *)calloc(1u, sizeof(*job));
    if (!job) return 0;
    native_lock();
    if (!slot->stream || slot->action_pending) {
        native_unlock();
        free(job);
        return 0;
    }
    slot->action_pending = 1;
    slot->action = action;
    slot->action_generation = slot->generation;
    job->slot = slot;
    job->receiver = slot->receiver;
    job->stream = slot->stream;
    job->generation = slot->generation;
    job->state_generation = slot->state_generation;
    job->action = action;
    native_unlock();
    rc = g_pthread_create(&thread, NULL, native_route_worker, job);
    if (rc != 0) {
        native_lock();
        if (slot->generation == job->generation &&
            slot->receiver == job->receiver && slot->stream == job->stream &&
            slot->action_generation == job->generation) {
            slot->action_pending = 0;
            slot->action = NATIVE_ROUTE_NONE;
        }
        native_unlock();
        free(job);
        altscreen_log("ERROR PHASE=NATIVE_111_ROUTE_THREAD_CREATE action=%d rc=%d", action, rc);
        return 0;
    }
    (void)g_pthread_detach(thread);
    return 1;
}

static int native_measured_view_area_canvas(uint32_t width,
                                             uint32_t height) {
    return width == 1440u &&
           (height == 542u || height == 540u || height == 455u);
}

static int native_read_view_area_target(void) {
    FILE *f;
    char line[192];
    char layout[128];
    int target = -1;
    int have_layout = 0;

    layout[0] = 0;
    f = fopen("/tmp/mmi-mirror-hmi.state", "r");
    if (!f) return -1;

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "view=FULL", 9u)) {
            target = 0;
        } else if (!strncmp(line, "view=SMALL", 10u)) {
            target = 1;
        } else if (!strncmp(line, "layout_name=", 12u)) {
            size_t n;
            strncpy(layout, line + 12u, sizeof(layout) - 1u);
            layout[sizeof(layout) - 1u] = 0;
            n = strlen(layout);
            while (n && (layout[n - 1u] == '\n' ||
                         layout[n - 1u] == '\r' ||
                         layout[n - 1u] == ' ' ||
                         layout[n - 1u] == '\t'))
                layout[--n] = 0;
            have_layout = 1;
        }
    }
    fclose(f);

    /*
     * /info predeclares two viewAreas on measured B9/Q7 canvases.
     * Q7 log evidence has the same FULL 370/700 and SMALL 490/460 bounds.
     * Never send index 1 merely because a stale/foreign HMI state says SMALL:
     * on an unknown layout the display may have advertised only one viewArea.
     */
    if (!have_layout || (!strstr(layout, "LayoutMIB2HighB9") &&
                         !strstr(layout, "LayoutMIB2HighQ7")))
        return -1;
    return target;
}

/*
 * V3 wheel control follows the proven Java-owned Context80 route, not the
 * retired native Context76/displayable58 route.  Java publishes this small
 * state atomically and only sets cluster_owned=1 after Context80 readback has
 * succeeded for an active CarPlay session.  Missing/partial/old state fails
 * closed so a wheel event can never escape to a stale or non-CarPlay session.
 */
#define CLUSTER_OWNERSHIP_STATE_FILE "/tmp/mmi-mirror-cluster-ownership.state"

static int native_read_cluster_owned_for_zoom(void) {
    FILE *f;
    char line[192];
    int version = 0;
    int carplay_session = 0;
    int cluster_owned = 0;
    int composite_applied = 0;
    int context = -1;

    f = fopen(CLUSTER_OWNERSHIP_STATE_FILE, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "version=1", 9u) &&
            (line[9] == '\n' || line[9] == '\r' || line[9] == 0))
            version = 1;
        else if (!strncmp(line, "carplay_session=1", 17u) &&
                 (line[17] == '\n' || line[17] == '\r' || line[17] == 0))
            carplay_session = 1;
        else if (!strncmp(line, "cluster_owned=1", 15u) &&
                 (line[15] == '\n' || line[15] == '\r' || line[15] == 0))
            cluster_owned = 1;
        else if (!strncmp(line, "composite_applied=1", 19u) &&
                 (line[19] == '\n' || line[19] == '\r' || line[19] == 0))
            composite_applied = 1;
        else if (!strncmp(line, "context=", 8u))
            (void)sscanf(line, "context=%d", &context);
    }
    fclose(f);
    return version == 1 && carplay_session && cluster_owned &&
           composite_applied && context == 80;
}

#define WHEEL_ZOOM_EVENT_FILE "/tmp/mmi-mirror-wheel-zoom.events"
#define WHEEL_ZOOM_BATCH_MAX 32u

/*
 * OEM target-follow wheel scheduling.
 *
 * Java still publishes one signed OEM_STEPS_V1 intent per magnification
 * callback. Native does not preserve those intents as a historical replay
 * queue. Instead it tracks the driver's latest desired relative zoom target
 * and the relative target already submitted to CarPlay. Direction reversals
 * therefore retarget immediately.
 *
 * Wheel timing intentionally uses its own modular 32-bit microsecond clock.
 * obs_now_us() is legacy seconds despite its name and remains untouched because
 * route/lifecycle timeouts depend on those second-based semantics.
 */
#define WHEEL_ZOOM_EVENT_MODEL "OEM_STEPS_V1"
#define WHEEL_ZOOM_SCHEDULER_MODEL "OEM_TARGET_FOLLOW_V1"
#define WHEEL_ZOOM_PACING_MODEL "BURST_ADAPTIVE_100_150_200_V2"
#define WHEEL_ZOOM_TARGET_LIMIT 12
#define WHEEL_ZOOM_MAX_EVENT_STEPS 16
#define WHEEL_ZOOM_MONITOR_TICK_US 50000u
#define ALT111_VIEW_AREA_POLL_US 100000u
#define WHEEL_ZOOM_ACTIVE_SHORT_PACE_US 100000u
#define WHEEL_ZOOM_ACTIVE_NORMAL_PACE_US 150000u
#define WHEEL_ZOOM_ACTIVE_LONG_PACE_US 200000u
#define WHEEL_ZOOM_QUIET_SMALL_BACKLOG_PACE_US 100000u
#define WHEEL_ZOOM_QUIET_BACKLOG_PACE_US 150000u
#define WHEEL_ZOOM_SEND_RETRY_US 150000u
#define WHEEL_ZOOM_FALLBACK_PACE_US 250000u
#define WHEEL_ZOOM_FRESH_FRAME_AGE_US 100000u
#define WHEEL_ZOOM_STALL_AGE_US 150000u
#define WHEEL_ZOOM_RECOVERY_FRAMES 3u
#define WHEEL_ZOOM_BURST_GAP_US 300000u
#define WHEEL_ZOOM_INPUT_ACTIVE_US 300000u
#define WHEEL_ZOOM_SHORT_BURST_MAX_STEPS 2u
#define WHEEL_ZOOM_NORMAL_BURST_MAX_STEPS 6u
#define WHEEL_ZOOM_SMALL_BACKLOG_MAX_STEPS 2u
#define WHEEL_ZOOM_STALL_ABORT_QUIET_US 350000u
#define WHEEL_ZOOM_STALL_ABORT_US 1200000u

static uint32_t wheel_now_us32(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0u;
    /* All wheel thresholds are <=1.2 s, so modular subtraction remains safe
     * across the 32-bit wrap while matching the decoded tap time base exactly. */
    return (uint32_t)tv.tv_sec * 1000000u + (uint32_t)tv.tv_usec;
}

/*
 * A decoded frame can be published after the monitor sampled wheel_now but
 * before p111_frame_tap_get_progress() returns its snapshot. In that narrow
 * race the publication timestamp is only slightly "in the future" relative to
 * the scheduler sample. Clamp only a future skew within the existing 150 ms
 * stall horizon to zero.
 *
 * A modular age above INT32_MAX is otherwise treated as definitely stale
 * instead of fresh. This matters after very long static-map intervals: an old
 * frame must still let the existing stall/abort path rebase pending zoom work.
 * Genuine short uint32 wrap intervals remain normal small positive ages.
 *
 * This helper deliberately does not change wheel pacing, target-follow,
 * rebase, retry, or any timing threshold.
 */
static uint32_t wheel_short_age_us32(uint32_t now, uint32_t before) {
    uint32_t age = (uint32_t)(now - before);
    uint32_t future_skew;
    if (age <= 0x7fffffffu) return age;
    future_skew = (uint32_t)(before - now);
    if (future_skew <= WHEEL_ZOOM_STALL_AGE_US) return 0u;
    return 0x7fffffffu;
}

static uint32_t native_wheel_zoom_dynamic_pace_us(
        unsigned burst_input_steps, int error_steps,
        uint32_t input_quiet_us, int reverse_response_pending,
        int next_direction, int last_input_direction,
        const char **mode) {
    unsigned backlog = (unsigned)(error_steps < 0 ? -error_steps : error_steps);

    if (reverse_response_pending &&
        (next_direction == 0 || next_direction == 1) &&
        next_direction == last_input_direction) {
        if (mode) *mode = "REVERSE_RESPONSE";
        return WHEEL_ZOOM_ACTIVE_SHORT_PACE_US;
    }

    if (input_quiet_us >= WHEEL_ZOOM_INPUT_ACTIVE_US) {
        if (backlog <= WHEEL_ZOOM_SMALL_BACKLOG_MAX_STEPS) {
            if (mode) *mode = "QUIET_SMALL_BACKLOG";
            return WHEEL_ZOOM_QUIET_SMALL_BACKLOG_PACE_US;
        }
        if (mode) *mode = "QUIET_BACKLOG";
        return WHEEL_ZOOM_QUIET_BACKLOG_PACE_US;
    }

    if (burst_input_steps <= WHEEL_ZOOM_SHORT_BURST_MAX_STEPS) {
        if (mode) *mode = "ACTIVE_SHORT";
        return WHEEL_ZOOM_ACTIVE_SHORT_PACE_US;
    }
    if (burst_input_steps <= WHEEL_ZOOM_NORMAL_BURST_MAX_STEPS) {
        if (mode) *mode = "ACTIVE_NORMAL";
        return WHEEL_ZOOM_ACTIVE_NORMAL_PACE_US;
    }
    if (mode) *mode = "ACTIVE_LONG";
    return WHEEL_ZOOM_ACTIVE_LONG_PACE_US;
}

static int native_wheel_zoom_target_accumulate(int target, int sent,
                                                int delta, int *saturated) {
    long error = (long)target - (long)sent;
    long next_error = error + (long)delta;
    if (saturated) *saturated = 0;
    if (next_error > WHEEL_ZOOM_TARGET_LIMIT) {
        next_error = WHEEL_ZOOM_TARGET_LIMIT;
        if (saturated) *saturated = 1;
    } else if (next_error < -WHEEL_ZOOM_TARGET_LIMIT) {
        next_error = -WHEEL_ZOOM_TARGET_LIMIT;
        if (saturated) *saturated = 1;
    }
    return sent + (int)next_error;
}

struct wheel_zoom_event {
    uint32_t epoch;
    uint32_t seq;
    int direction;
    int magnification;
    int delta;
    int step;
    int steps;
};

static unsigned native_read_wheel_zoom_events(
        uint32_t after_epoch, uint32_t after_seq,
        struct wheel_zoom_event *events, unsigned cap) {
    FILE *f;
    char line[256];
    unsigned count = 0;

    if (!events || !cap) return 0;
    f = fopen(WHEEL_ZOOM_EVENT_FILE, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        struct wheel_zoom_event e;
        unsigned epoch = 0, commit = 0;
        char model[32];
        size_t n = strlen(line);
        int fields;

        if (!n || line[n - 1] != '\n') continue;
        memset(&e, 0, sizeof(e));
        memset(model, 0, sizeof(model));
        fields = sscanf(
            line,
            "epoch=%u seq=%u direction=%d magnification=%d delta=%d step=%d steps=%d model=%31s commit=%u",
            &epoch, &e.seq, &e.direction, &e.magnification, &e.delta,
            &e.step, &e.steps, model, &commit);
        e.epoch = (uint32_t)epoch;
        if (fields != 9 || !e.epoch || !e.seq || commit != e.seq ||
            strcmp(model, WHEEL_ZOOM_EVENT_MODEL) != 0 ||
            (e.direction != 0 && e.direction != 1) ||
            e.delta == 0 || e.step != 0 || e.steps < 1 ||
            e.steps > WHEEL_ZOOM_MAX_EVENT_STEPS ||
            e.delta < -WHEEL_ZOOM_MAX_EVENT_STEPS ||
            e.delta > WHEEL_ZOOM_MAX_EVENT_STEPS ||
            e.steps != (e.delta < 0 ? -e.delta : e.delta) ||
            (e.delta < 0 ? e.direction != 0 : e.direction != 1))
            continue;
        if (e.epoch == after_epoch && e.seq <= after_seq) continue;
        events[count++] = e;
        if (count == cap) break;
    }
    fclose(f);
    return count;
}

static void native_wheel_zoom_tail(uint32_t *out_epoch, uint32_t *out_seq) {
    FILE *f;
    char line[256];
    uint32_t tail_epoch = 0;
    uint32_t tail_seq = 0;

    if (out_epoch) *out_epoch = 0;
    if (out_seq) *out_seq = 0;
    f = fopen(WHEEL_ZOOM_EVENT_FILE, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned epoch = 0, seq = 0, commit = 0;
        int direction = -1, magnification = 0, delta = 0, step = 0, steps = 0;
        char model[32];
        size_t n = strlen(line);
        if (!n || line[n - 1] != '\n') continue;
        memset(model, 0, sizeof(model));
        if (sscanf(
                line,
                "epoch=%u seq=%u direction=%d magnification=%d delta=%d step=%d steps=%d model=%31s commit=%u",
                &epoch, &seq, &direction, &magnification, &delta,
                &step, &steps, model, &commit) == 9 &&
            epoch && seq && seq == commit &&
            strcmp(model, WHEEL_ZOOM_EVENT_MODEL) == 0 &&
            (direction == 0 || direction == 1) &&
            delta != 0 && step == 0 && steps >= 1 &&
            steps <= WHEEL_ZOOM_MAX_EVENT_STEPS &&
            delta >= -WHEEL_ZOOM_MAX_EVENT_STEPS &&
            delta <= WHEEL_ZOOM_MAX_EVENT_STEPS &&
            steps == (delta < 0 ? -delta : delta) &&
            (delta < 0 ? direction == 0 : direction == 1)) {
            tail_epoch = (uint32_t)epoch;
            tail_seq = (uint32_t)seq;
        }
    }
    fclose(f);
    if (out_epoch) *out_epoch = tail_epoch;
    if (out_seq) *out_seq = tail_seq;
}

static void native_view_area_capture_frame_baseline(
        void *receiver, void *stream, uint32_t generation,
        uint32_t request_seq) {
    struct p111_frame_progress_snapshot progress;
    struct native_slot *slot;
    int progress_ok;

    memset(&progress, 0, sizeof(progress));
    progress_ok = p111_frame_tap_get_progress(stream, &progress) &&
                  progress.active;

    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot && slot->generation == generation &&
        slot->view_area_event_state == 1 &&
        slot->view_area_inflight_seq == request_seq) {
        slot->view_area_frame_baseline_valid = progress_ok;
        slot->view_area_frame_generation =
            progress_ok ? progress.generation : 0u;
        slot->view_area_frame_count =
            progress_ok ? progress.frame_count : 0u;
    }
    native_unlock();

    altscreen_log(
        "PHASE=ALT111_VIEWAREA_FRAME_BASELINE receiver=%p stream=%p "
        "generation=%u request_seq=%u telemetry=%s frame_generation=%u "
        "frame_count=%u",
        receiver, stream, generation, request_seq,
        progress_ok ? "decoded_progress" : "unavailable",
        progress_ok ? progress.generation : 0u,
        progress_ok ? progress.frame_count : 0u);
}

static void native_check_view_area_fresh_frame(
        void *receiver, void *stream, uint32_t generation) {
    struct p111_frame_progress_snapshot progress;
    struct native_slot *slot;
    uint32_t request_seq = 0;
    uint32_t baseline_generation = 0;
    uint32_t baseline_count = 0;
    uint64_t ack_at = 0;
    uint64_t now = obs_now_us();
    int view_area_index = -1;
    int baseline_valid = 0;
    int pending = 0;
    int progress_ok = 0;
    int completed = 0;
    int timed_out = 0;

    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot && slot->generation == generation &&
        slot->view_area_event_state == 2 &&
        slot->view_area_frame_pending &&
        slot->view_area_acked_seq != 0) {
        pending = 1;
        request_seq = slot->view_area_acked_seq;
        view_area_index = slot->view_area_applied;
        ack_at = slot->view_area_ack_at;
        baseline_valid = slot->view_area_frame_baseline_valid;
        baseline_generation = slot->view_area_frame_generation;
        baseline_count = slot->view_area_frame_count;
    }
    native_unlock();
    if (!pending) return;

    memset(&progress, 0, sizeof(progress));
    progress_ok = p111_frame_tap_get_progress(stream, &progress) &&
                  progress.active;

    if (!baseline_valid && progress_ok) {
        native_lock();
        slot = find_stream_locked(receiver, stream);
        if (slot && slot->generation == generation &&
            slot->view_area_event_state == 2 &&
            slot->view_area_frame_pending &&
            slot->view_area_acked_seq == request_seq &&
            !slot->view_area_frame_baseline_valid) {
            slot->view_area_frame_baseline_valid = 1;
            slot->view_area_frame_generation = progress.generation;
            slot->view_area_frame_count = progress.frame_count;
            baseline_generation = progress.generation;
            baseline_count = progress.frame_count;
            baseline_valid = 1;
        }
        native_unlock();
        if (baseline_valid) {
            altscreen_log(
                "PHASE=ALT111_VIEWAREA_FRAME_BASELINE_LATE receiver=%p "
                "stream=%p generation=%u request_seq=%u viewAreaIndex=%d "
                "frame_generation=%u frame_count=%u",
                receiver, stream, generation, request_seq, view_area_index,
                baseline_generation, baseline_count);
        }
        return;
    }

    if (progress_ok && baseline_valid &&
        (progress.generation != baseline_generation ||
         progress.frame_count != baseline_count)) {
        native_lock();
        slot = find_stream_locked(receiver, stream);
        if (slot && slot->generation == generation &&
            slot->view_area_event_state == 2 &&
            slot->view_area_frame_pending &&
            slot->view_area_acked_seq == request_seq) {
            slot->view_area_frame_pending = 0;
            completed = 1;
        }
        native_unlock();
        if (completed) {
            altscreen_log(
                "PHASE=ALT111_VIEWAREA_FRESH_FRAME receiver=%p stream=%p "
                "generation=%u request_seq=%u viewAreaIndex=%d "
                "baseline_generation=%u baseline_count=%u "
                "frame_generation=%u frame_count=%u "
                "meaning=fresh_type111_after_ack_not_semantic_relayout_proof",
                receiver, stream, generation, request_seq, view_area_index,
                baseline_generation, baseline_count,
                progress.generation, progress.frame_count);
        }
        return;
    }

    if (ack_at && now > ack_at + NATIVE_EVENT_TIMEOUT_SECONDS) {
        native_lock();
        slot = find_stream_locked(receiver, stream);
        if (slot && slot->generation == generation &&
            slot->view_area_event_state == 2 &&
            slot->view_area_frame_pending &&
            slot->view_area_acked_seq == request_seq) {
            slot->view_area_frame_pending = 0;
            timed_out = 1;
        }
        native_unlock();
        if (timed_out) {
            altscreen_log(
                "WARN PHASE=ALT111_VIEWAREA_NO_FRESH_FRAME receiver=%p "
                "stream=%p generation=%u request_seq=%u viewAreaIndex=%d "
                "ack_age_s=%llu telemetry=%s "
                "meaning=ack_without_observed_new_type111_frame",
                receiver, stream, generation, request_seq, view_area_index,
                (unsigned long long)(now - ack_at),
                progress_ok ? "decoded_progress" : "unavailable");
        }
    }
}

/* Only held around recovery submission and ProcessFrames enter/exit, never
 * while receiving video or while joining the worker. Avoid the control fence:
 * teardown holds that fence while joining this worker. */
static void recovery_lock(void) {
    while (__sync_lock_test_and_set(&g_recovery_guard, 1u)) usleep(1000);
}
static void recovery_unlock(void) { __sync_lock_release(&g_recovery_guard); }

void p1404_cockpit_native_processing(void *receiver, void *stream, int live) {
    struct native_slot *slot;
    recovery_lock();
    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot) slot->processing_live = live;
    native_unlock();
    recovery_unlock();
}

/* Recovery never changes stream ownership, TCP/CTR state, or Main110. */
void p1404_cockpit_native_recovery_result(void *receiver, void *stream,
        uint32_t generation, uint32_t event_seq, int status,
        int response_received) {
    struct native_slot *slot;
    int applied = 0;
    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot && slot->generation == generation)
        applied = alt111_recovery_result(&slot->recovery, event_seq);
    native_unlock();
    altscreen_log("PHASE=ALT111_RECOVERY_RESULT receiver=%p stream=%p generation=%u request_seq=%u status=%d response_received=%d applied=%d picture_recovered=UNPROVEN stale_callback_fenced=1",
        receiver, stream, generation, event_seq, status, response_received, applied);
}

static void native_check_stream_recovery(void *receiver, void *stream,
        uint32_t generation, uint32_t state_generation, uint32_t now,
        int cluster_owned) {
    struct p111_frame_progress_snapshot progress;
    struct native_slot *slot;
    unsigned action = 0, attempts = 0;
    uint32_t seq = 0, age = 0;
    int eligible, send_rc, live = 0;
    /* Read before native_lock: the frame producer uses its own tap lock. */
    if (!p111_frame_tap_get_progress(stream, &progress) || !progress.active)
        return;
    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot && slot->generation == generation &&
        slot->state_generation == state_generation) {
        eligible = slot->processing_live && cluster_owned && slot->ui_event_state == 2 &&
            slot->preconfig_rewritten && slot->config_ok &&
            slot->first_real_frame_posted && slot->keyframe_event_state != 1 &&
            slot->view_area_event_state != 1 &&
            alt_control_fence_is_current(state_generation);
        action = alt111_recovery_poll(&slot->recovery, now,
            progress.generation, progress.frame_count, eligible);
        seq = slot->recovery.command_seq;
        age = now - slot->recovery.last_progress_at;
        attempts = slot->recovery.attempts;
    }
    native_unlock();
    if (!action) return;
    altscreen_log("PHASE=ALT111_RECOVERY_PROGRESS receiver=%p stream=%p generation=%u request_seq=%u flags=%u attempts=%u decoded_frames=%u h264_packets=%u idle_ms=%u fresh_frame=%d budget_exhausted=%d",
        receiver, stream, generation, seq, action, attempts,
        progress.frame_count, progress.h264_packets, age / 1000u,
        !!(action & ALT111_RECOVERY_PROGRESS),
        !!(action & ALT111_RECOVERY_EXHAUSTED));
    if (!(action & ALT111_RECOVERY_REQUEST)) return;
    /* ProcessFrames exit waits for this guard before StopSession; no request
     * can run on a stopped private session. Native identity and cancellation
     * are checked again; a synchronous callback only takes native_lock. */
    recovery_lock();
    native_lock();
    slot = find_stream_locked(receiver, stream);
    live = slot && slot->generation == generation &&
        slot->state_generation == state_generation && slot->processing_live &&
        slot->recovery.inflight_seq == seq &&
        alt_control_fence_is_current(state_generation);
    native_unlock();
    send_rc = live ? alt_send_cluster_recovery(receiver, stream, generation, seq) : -1;
    recovery_unlock();
    altscreen_log("PHASE=ALT111_RECOVERY_SUBMIT receiver=%p stream=%p generation=%u request_seq=%u attempt=%u event=forceKeyFrame rc=%d live=%d preserve_main110=1 tcp_reconnect=0",
        receiver, stream, generation, seq, attempts, send_rc, live);
    if (send_rc != 0)
        p1404_cockpit_native_recovery_result(receiver, stream, generation,
            seq, send_rc, 0);
}

/*
 * This monitor is the same-session bridge between Audi NAV_VIEW_SIZE_CHOICE
 * and CarPlay's standard updateViewArea command.  Java publishes the OEM state;
 * no reconnect and no second /info transaction is required.
 */
static void *native_monitor_worker(void *arg) {
    struct native_thread_job *job = (struct native_thread_job *)arg;
    struct native_slot *slot = job ? job->slot : NULL;
    void *receiver = job ? job->receiver : NULL;
    void *stream = job ? job->stream : NULL;
    uint32_t generation = job ? job->generation : 0;
    uint32_t state_generation = job ? job->state_generation : 0;
    uint64_t now;
    int visible, pending, live, route_ready, event_kind, send_rc;
    int desired_view_area = -1, view_area_send_index, zoom_gate, cluster_owned;
    int wheel_generation_current;
    uint32_t view_area_command_seq = 0;
    uint32_t view_area_send_seq = 0;
    int zoom_target_steps = 0;
    int zoom_sent_steps = 0;
    int zoom_stall_latched = 0;
    int zoom_first_step_pending = 0;
    int zoom_send_frame_baseline_valid = 0;
    int zoom_have_send_time = 0;
    int zoom_have_input_time = 0;
    int zoom_have_failed_send_time = 0;
    int zoom_have_input_direction = 0;
    int zoom_last_input_direction = -1;
    int zoom_reverse_response_pending = 0;
    unsigned zoom_burst_input_steps = 0;
    struct wheel_zoom_event zoom_events[WHEEL_ZOOM_BATCH_MAX];
    struct p111_frame_progress_snapshot zoom_progress;
    unsigned zoom_count, zoom_i;
    uint32_t zoom_last_epoch = 0;
    uint32_t zoom_last_seq = 0;
    uint32_t zoom_command_seq = 0;
    uint32_t zoom_last_send_at = 0;
    uint32_t zoom_last_input_at = 0;
    uint32_t zoom_last_failed_send_at = 0;
    uint32_t zoom_stall_started_at = 0;
    uint32_t zoom_recovery_generation = 0;
    uint32_t zoom_recovery_frame_count = 0;
    uint32_t zoom_send_frame_generation = 0;
    uint32_t zoom_send_frame_count = 0;
    uint32_t wheel_now = 0;
    uint32_t view_area_last_poll_at = 0;

    native_wheel_zoom_tail(&zoom_last_epoch, &zoom_last_seq);
    altscreen_log(
        "PHASE=WHEEL_ZOOM_QUEUE_RESET receiver=%p stream=%p generation=%u "
        "baseline_epoch=%u baseline_seq=%u stale_events_before_attach=discarded "
        "event_model=%s scheduler=%s pacing=%s tick_ms=%u "
        "active_short_ms=%u active_normal_ms=%u active_long_ms=%u "
        "quiet_small_backlog_ms=%u quiet_backlog_ms=%u fallback_pace_ms=%u "
        "input_active_ms=%u short_burst_max=%u normal_burst_max=%u "
        "small_backlog_max=%u recovery_frames=%u fresh_age_ms=%u "
        "stall_age_ms=%u burst_gap_ms=%u stall_abort_quiet_ms=%u "
        "stall_abort_ms=%u target_limit=%d timebase=wheel_us32",
        receiver, stream, generation, zoom_last_epoch, zoom_last_seq,
        WHEEL_ZOOM_EVENT_MODEL, WHEEL_ZOOM_SCHEDULER_MODEL,
        WHEEL_ZOOM_PACING_MODEL,
        WHEEL_ZOOM_MONITOR_TICK_US / 1000u,
        WHEEL_ZOOM_ACTIVE_SHORT_PACE_US / 1000u,
        WHEEL_ZOOM_ACTIVE_NORMAL_PACE_US / 1000u,
        WHEEL_ZOOM_ACTIVE_LONG_PACE_US / 1000u,
        WHEEL_ZOOM_QUIET_SMALL_BACKLOG_PACE_US / 1000u,
        WHEEL_ZOOM_QUIET_BACKLOG_PACE_US / 1000u,
        WHEEL_ZOOM_FALLBACK_PACE_US / 1000u,
        WHEEL_ZOOM_INPUT_ACTIVE_US / 1000u,
        WHEEL_ZOOM_SHORT_BURST_MAX_STEPS,
        WHEEL_ZOOM_NORMAL_BURST_MAX_STEPS,
        WHEEL_ZOOM_SMALL_BACKLOG_MAX_STEPS,
        WHEEL_ZOOM_RECOVERY_FRAMES,
        WHEEL_ZOOM_FRESH_FRAME_AGE_US / 1000u,
        WHEEL_ZOOM_STALL_AGE_US / 1000u,
        WHEEL_ZOOM_BURST_GAP_US / 1000u,
        WHEEL_ZOOM_STALL_ABORT_QUIET_US / 1000u,
        WHEEL_ZOOM_STALL_ABORT_US / 1000u,
        WHEEL_ZOOM_TARGET_LIMIT);

    for (;;) {
        /*
         * Wheel target-follow uses a 50 ms scheduler quantum so the adaptive
         * 100/150/200 ms pacing states remain exactly representable. The existing lifecycle
         * clock remains obs_now_us() seconds; only wheel timing uses us32.
         */
        usleep(WHEEL_ZOOM_MONITOR_TICK_US);
        now = obs_now_us();
        wheel_now = wheel_now_us32();

        /*
         * Keep the proven live ViewArea/HMI-state polling cadence at 100 ms.
         * The 50 ms master tick exists only so wheel input and target-follow
         * scheduling are not quantized to the old 100/200 ms cadence.
         */
        if (!view_area_last_poll_at ||
            (uint32_t)(wheel_now - view_area_last_poll_at) >=
                ALT111_VIEW_AREA_POLL_US) {
            desired_view_area = native_read_view_area_target();
            view_area_last_poll_at = wheel_now;
        }

        cluster_owned = native_read_cluster_owned_for_zoom();
        zoom_count = native_read_wheel_zoom_events(
            zoom_last_epoch, zoom_last_seq,
            zoom_events, WHEEL_ZOOM_BATCH_MAX);
        event_kind = 0;
        view_area_send_index = -1;
        view_area_send_seq = 0;
        zoom_gate = 0;

        native_lock();
        live = slot && slot->stream == stream && slot->receiver == receiver &&
               slot->generation == generation &&
               slot->state_generation == state_generation;
        visible = live ? slot->visible : 0;
        pending = live ? slot->action_pending : 0;
        route_ready = live && slot->ui_event_state == 2 &&
                      slot->preconfig_rewritten && slot->config_ok &&
                      slot->first_real_frame_posted;

        if (live) {
            if (slot->ui_event_state == 1 &&
                now > slot->ui_event_at + NATIVE_EVENT_TIMEOUT_SECONDS) {
                slot->ui_event_state = 3;
                altscreen_log("ERROR PHASE=ALT111_EVENT_TIMEOUT receiver=%p stream=%p generation=%u event=showUI retry=0 fail_closed=1",
                              receiver, stream, generation);
            }
            if (slot->keyframe_event_state == 1 &&
                now > slot->keyframe_event_at + NATIVE_EVENT_TIMEOUT_SECONDS) {
                slot->keyframe_event_state = 3;
                altscreen_log("ERROR PHASE=ALT111_EVENT_TIMEOUT receiver=%p stream=%p generation=%u event=forceKeyFrame retry=0 fail_closed=1",
                              receiver, stream, generation);
            }

            /*
             * updateViewArea is idempotent by target index, but a timed-out
             * request may still complete late. Fence every attempt with its own
             * sequence so an old ACK can never satisfy a retry to the same
             * FULL/SMALL target.
             */
            if (slot->view_area_event_state == 1 &&
                now > slot->view_area_event_at + NATIVE_EVENT_TIMEOUT_SECONDS) {
                altscreen_log(
                    "WARN PHASE=ALT111_VIEWAREA_TIMEOUT receiver=%p stream=%p "
                    "generation=%u request_seq=%u target=%d retry=1 "
                    "stale_callback_fenced=1",
                    receiver, stream, generation,
                    slot->view_area_inflight_seq,
                    slot->view_area_target);
                slot->view_area_event_state = 0;
                slot->view_area_inflight_seq = 0;
                slot->view_area_frame_pending = 0;
                slot->view_area_frame_baseline_valid = 0;
            }

            if (desired_view_area == 0 || desired_view_area == 1) {
                if (slot->view_area_target != desired_view_area) {
                    altscreen_log("PHASE=ALT111_VIEWAREA_TARGET receiver=%p stream=%p generation=%u old=%d new=%d source=/tmp/mmi-mirror-hmi.state gate=LayoutMIB2HighB9OrQ7 config=%ux%u canvas_gate=%d",
                                  receiver, stream, generation,
                                  slot->view_area_target, desired_view_area,
                                  slot->config_width, slot->config_height,
                                  native_measured_view_area_canvas(
                                      slot->config_width,
                                      slot->config_height));
                    slot->view_area_target = desired_view_area;
                    slot->view_area_applied = -1;
                    slot->view_area_event_state = 0;
                    slot->view_area_inflight_seq = 0;
                    slot->view_area_acked_seq = 0;
                    slot->view_area_frame_pending = 0;
                    slot->view_area_frame_baseline_valid = 0;
                }
            }

            if (slot->ui_event_state == 0 &&
                (!slot->ui_event_at || now > slot->ui_event_at + NATIVE_EVENT_RETRY_SECONDS)) {
                slot->ui_event_state = 1;
                slot->ui_event_at = now;
                event_kind = ALT111_EVENT_SHOW_UI;
            } else if (slot->ui_event_state == 2 && slot->preconfig_rewritten &&
                       slot->keyframe_event_state == 0 &&
                       (!slot->keyframe_event_at ||
                        now > slot->keyframe_event_at + NATIVE_EVENT_RETRY_SECONDS)) {
                slot->keyframe_event_state = 1;
                slot->keyframe_event_at = now;
                event_kind = ALT111_EVENT_FORCE_KEYFRAME;
            } else if (slot->ui_event_state == 2 &&
                       route_ready &&
                       native_measured_view_area_canvas(
                           slot->config_width, slot->config_height) &&
                       (slot->view_area_target == 0 ||
                        slot->view_area_target == 1) &&
                       slot->view_area_applied != slot->view_area_target &&
                       slot->view_area_event_state != 1) {
                ++view_area_command_seq;
                if (!view_area_command_seq) ++view_area_command_seq;
                slot->view_area_event_state = 1;
                slot->view_area_event_at = now;
                slot->view_area_inflight_seq = view_area_command_seq;
                slot->view_area_frame_pending = 0;
                slot->view_area_frame_baseline_valid = 0;
                view_area_send_index = slot->view_area_target;
                view_area_send_seq = view_area_command_seq;
                event_kind = ALT111_EVENT_UPDATE_VIEW_AREA;
            }
        }
        native_unlock();

        if (!live) break;

        wheel_generation_current =
            alt_control_fence_is_current(state_generation);
        zoom_gate = route_ready && cluster_owned && wheel_generation_current;

        if (event_kind == ALT111_EVENT_UPDATE_VIEW_AREA) {
            native_view_area_capture_frame_baseline(
                receiver, stream, generation, view_area_send_seq);
            send_rc = alt_send_cluster_view_area(
                receiver, stream, generation,
                view_area_send_seq, view_area_send_index);
            if (send_rc != 0)
                p1404_cockpit_native_view_area_result(
                    receiver, stream, generation,
                    view_area_send_seq, view_area_send_index,
                    send_rc, 0);
        } else if (event_kind) {
            send_rc = alt_send_cluster_event(receiver, stream, generation, event_kind);
            if (send_rc != 0)
                p1404_cockpit_native_event_result(receiver, stream, generation,
                                                   event_kind, send_rc, 0);
        }

        native_check_view_area_fresh_frame(receiver, stream, generation);
        native_check_stream_recovery(receiver, stream, generation,
            state_generation, wheel_now, cluster_owned);

        if (!zoom_gate &&
            (zoom_target_steps != 0 || zoom_sent_steps != 0 ||
             zoom_have_send_time || zoom_have_input_time ||
             zoom_have_failed_send_time || zoom_stall_latched)) {
            if (zoom_target_steps != zoom_sent_steps) {
                altscreen_log(
                    "PHASE=WHEEL_ZOOM_TARGET_RESET receiver=%p stream=%p "
                    "generation=%u target=%d sent=%d error=%d "
                    "reason=gate_closed action=DROP_STALE_TARGET",
                    receiver, stream, generation, zoom_target_steps,
                    zoom_sent_steps, zoom_target_steps - zoom_sent_steps);
            }
            zoom_target_steps = 0;
            zoom_sent_steps = 0;
            zoom_last_input_at = 0;
            zoom_last_send_at = 0;
            zoom_have_input_time = 0;
            zoom_have_send_time = 0;
            zoom_have_failed_send_time = 0;
            zoom_last_failed_send_at = 0;
            zoom_have_input_direction = 0;
            zoom_last_input_direction = -1;
            zoom_reverse_response_pending = 0;
            zoom_burst_input_steps = 0;
            zoom_stall_started_at = 0;
            zoom_stall_latched = 0;
            zoom_first_step_pending = 0;
            zoom_send_frame_baseline_valid = 0;
            zoom_recovery_generation = 0;
            zoom_recovery_frame_count = 0;
            zoom_send_frame_generation = 0;
            zoom_send_frame_count = 0;
        }

        for (zoom_i = 0; zoom_i < zoom_count; ++zoom_i) {
            struct wheel_zoom_event *ze = &zoom_events[zoom_i];
            int target_before, error_before, error_after;
            int saturated, retarget, new_burst, input_direction_changed;

            if (ze->epoch != zoom_last_epoch) {
                altscreen_log(
                    "PHASE=WHEEL_ZOOM_EPOCH receiver=%p stream=%p generation=%u "
                    "old_epoch=%u new_epoch=%u sequence_reset=1 "
                    "target_reset=%d sent_reset=%d scheduler_reset=1",
                    receiver, stream, generation, zoom_last_epoch, ze->epoch,
                    zoom_target_steps, zoom_sent_steps);
                zoom_last_epoch = ze->epoch;
                zoom_last_seq = 0;
                zoom_target_steps = 0;
                zoom_sent_steps = 0;
                zoom_last_send_at = 0;
                zoom_last_input_at = 0;
                zoom_have_send_time = 0;
                zoom_have_input_time = 0;
                zoom_have_failed_send_time = 0;
                zoom_last_failed_send_at = 0;
                zoom_have_input_direction = 0;
                zoom_last_input_direction = -1;
                zoom_reverse_response_pending = 0;
                zoom_burst_input_steps = 0;
                zoom_stall_started_at = 0;
                zoom_stall_latched = 0;
                zoom_first_step_pending = 0;
                zoom_send_frame_baseline_valid = 0;
                zoom_recovery_generation = 0;
                zoom_recovery_frame_count = 0;
                zoom_send_frame_generation = 0;
                zoom_send_frame_count = 0;
                zoom_command_seq = 0;
            }
            if (ze->seq <= zoom_last_seq) continue;
            if (zoom_last_seq && ze->seq != zoom_last_seq + 1u)
                altscreen_log(
                    "WARN PHASE=WHEEL_ZOOM_QUEUE_GAP receiver=%p stream=%p "
                    "generation=%u epoch=%u expected_seq=%u actual_seq=%u",
                    receiver, stream, generation, ze->epoch,
                    zoom_last_seq + 1u, ze->seq);
            zoom_last_seq = ze->seq;

            altscreen_log(
                "PHASE=WHEEL_ZOOM_QUEUE receiver=%p stream=%p generation=%u "
                "epoch=%u seq=%u action=%s direction=%d magnification=%d "
                "delta=%d steps=%d event_model=%s result=%s reason=%s",
                receiver, stream, generation, ze->epoch, ze->seq,
                ze->direction == 0 ? "ZOOM_IN" : "ZOOM_OUT",
                ze->direction, ze->magnification, ze->delta, ze->steps,
                WHEEL_ZOOM_EVENT_MODEL,
                zoom_gate ? "queued" : "dropped",
                zoom_gate ? "java80_cluster_owned_route_ready_generation_current" :
                            "route_not_ready_or_java80_cluster_not_owned_or_generation_stale");
            if (!zoom_gate) continue;

            new_burst = !zoom_have_input_time ||
                (uint32_t)(wheel_now - zoom_last_input_at) >=
                    WHEEL_ZOOM_BURST_GAP_US;
            input_direction_changed = zoom_have_input_direction &&
                ze->direction != zoom_last_input_direction;
            if (new_burst || input_direction_changed) {
                zoom_burst_input_steps = 0;
                if (new_burst) zoom_reverse_response_pending = 0;
            }
            if (zoom_burst_input_steps <= 1000000u - (unsigned)ze->steps)
                zoom_burst_input_steps += (unsigned)ze->steps;
            else
                zoom_burst_input_steps = 1000000u;
            if (input_direction_changed)
                zoom_reverse_response_pending = 1;
            zoom_last_input_direction = ze->direction;
            zoom_have_input_direction = 1;

            target_before = zoom_target_steps;
            error_before = target_before - zoom_sent_steps;
            retarget =
                (error_before > 0 && ze->delta < 0) ||
                (error_before < 0 && ze->delta > 0);
            saturated = 0;
            zoom_target_steps = native_wheel_zoom_target_accumulate(
                zoom_target_steps, zoom_sent_steps,
                ze->delta, &saturated);
            error_after = zoom_target_steps - zoom_sent_steps;
            zoom_last_input_at = wheel_now;
            zoom_have_input_time = 1;
            if (new_burst) zoom_first_step_pending = 1;

            altscreen_log(
                "PHASE=WHEEL_ZOOM_TARGET receiver=%p stream=%p generation=%u "
                "source_seq=%u signed_steps=%d target_before=%d "
                "target_after=%d sent=%d error_before=%d error_after=%d "
                "retarget=%d new_burst=%d input_direction_changed=%d "
                "burst_input_steps=%u reverse_response_pending=%d "
                "saturated=%d target_limit=%d scheduler=%s",
                receiver, stream, generation, ze->seq, ze->delta,
                target_before, zoom_target_steps, zoom_sent_steps,
                error_before, error_after, retarget, new_burst,
                input_direction_changed, zoom_burst_input_steps,
                zoom_reverse_response_pending, saturated,
                WHEEL_ZOOM_TARGET_LIMIT, WHEEL_ZOOM_SCHEDULER_MODEL);
        }

        if (zoom_target_steps == zoom_sent_steps) {
            zoom_first_step_pending = 0;
            zoom_have_failed_send_time = 0;
            if (zoom_stall_latched) {
                altscreen_log(
                    "PHASE=WHEEL_ZOOM_STALL_CANCELLED receiver=%p stream=%p "
                    "generation=%u target=%d sent=%d reason=target_satisfied",
                    receiver, stream, generation,
                    zoom_target_steps, zoom_sent_steps);
                zoom_stall_latched = 0;
                zoom_stall_started_at = 0;
                zoom_recovery_generation = 0;
                zoom_recovery_frame_count = 0;
            }
            if (zoom_target_steps != 0) {
                altscreen_log(
                    "PHASE=WHEEL_ZOOM_TARGET_REBASE receiver=%p stream=%p "
                    "generation=%u settled_relative_level=%d "
                    "action=REBASE_TARGET_AND_SENT_TO_ZERO "
                    "outstanding_limit=%d",
                    receiver, stream, generation, zoom_target_steps,
                    WHEEL_ZOOM_TARGET_LIMIT);
                zoom_target_steps = 0;
                zoom_sent_steps = 0;
            }
        }

        /*
         * OEM_TARGET_FOLLOW_V1 + BURST_ADAPTIVE_100_150_200_V2:
         *   - first detent of a new burst may submit immediately;
         *   - while input is active: first 2 detents use 100 ms, detents 3..6
         *     use 150 ms, and detent 7+ uses 200 ms;
         *   - after 300 ms input quiet: backlog <=2 drains at 100 ms, larger
         *     backlog drains at 150 ms so the map does not trail the driver;
         *   - a real direction reversal gets a 100 ms response once the
         *     desired target has crossed into the new physical direction;
         *   - decoded-frame progress is liveness evidence only. It does not
         *     prove that the map camera animation completed;
         *   - no post-send progress for >=150 ms latches STALL;
         *   - only STALL recovery requires three fresh frames;
         *   - telemetry loss uses a conservative 250 ms timer;
         *   - a stall lasting >=1.2 s after >=350 ms input quiet rebases target
         *     to the submitted level, preventing a late replay after recovery.
         */
        if (zoom_gate && zoom_target_steps != zoom_sent_steps) {
            int send_ready = 0;
            int progress_ok = 0;
            int fallback_timer = 0;
            int first_send = !zoom_have_send_time;
            int error_steps = zoom_target_steps - zoom_sent_steps;
            int next_direction = error_steps < 0 ? 0 : 1;
            const char *pace_mode = "FIRST_SEND";
            uint32_t elapsed_us = first_send ? 0u :
                (uint32_t)(wheel_now - zoom_last_send_at);
            uint32_t input_quiet_us = zoom_have_input_time ?
                (uint32_t)(wheel_now - zoom_last_input_at) : 0u;
            uint32_t selected_pace_us = first_send ? 0u :
                native_wheel_zoom_dynamic_pace_us(
                    zoom_burst_input_steps, error_steps, input_quiet_us,
                    zoom_reverse_response_pending, next_direction,
                    zoom_last_input_direction, &pace_mode);
            uint32_t frame_age_us = 0;
            uint32_t fresh_since_send = 0;
            uint32_t recovery_fresh = 0;
            int baseline_current = 0;

            memset(&zoom_progress, 0, sizeof(zoom_progress));
            progress_ok = p111_frame_tap_get_progress(
                stream, &zoom_progress);
            if (progress_ok) {
                frame_age_us = wheel_short_age_us32(
                    wheel_now, zoom_progress.last_publish_us32);
                baseline_current =
                    zoom_send_frame_baseline_valid &&
                    zoom_send_frame_generation == zoom_progress.generation;
                if (baseline_current)
                    fresh_since_send =
                        zoom_progress.frame_count - zoom_send_frame_count;
            }

            if (!zoom_stall_latched && !first_send &&
                !zoom_first_step_pending &&
                elapsed_us >= WHEEL_ZOOM_STALL_AGE_US &&
                progress_ok && baseline_current &&
                fresh_since_send == 0u &&
                frame_age_us >= WHEEL_ZOOM_STALL_AGE_US) {
                zoom_stall_latched = 1;
                zoom_stall_started_at = wheel_now;
                zoom_recovery_generation = zoom_progress.generation;
                zoom_recovery_frame_count = zoom_progress.frame_count;
                altscreen_log(
                    "PHASE=WHEEL_ZOOM_FRAME_STALL receiver=%p stream=%p "
                    "generation=%u command_seq=%u target=%d sent=%d error=%d "
                    "elapsed_ms=%u frame_age_ms=%u fresh_since_send=%u "
                    "action=BLOCK_UNTIL_3_FRESH_FRAMES",
                    receiver, stream, generation, zoom_command_seq,
                    zoom_target_steps, zoom_sent_steps, error_steps,
                    elapsed_us / 1000u, frame_age_us / 1000u,
                    fresh_since_send);
            }

            if (zoom_stall_latched) {
                if (progress_ok) {
                    if (zoom_recovery_generation != zoom_progress.generation) {
                        zoom_recovery_generation = zoom_progress.generation;
                        zoom_recovery_frame_count = zoom_progress.frame_count;
                    }
                    recovery_fresh =
                        zoom_progress.frame_count - zoom_recovery_frame_count;
                    if (frame_age_us <= WHEEL_ZOOM_FRESH_FRAME_AGE_US &&
                        recovery_fresh >= WHEEL_ZOOM_RECOVERY_FRAMES) {
                        zoom_stall_latched = 0;
                        zoom_stall_started_at = 0;
                        altscreen_log(
                            "PHASE=WHEEL_ZOOM_FRAME_RECOVERED receiver=%p "
                            "stream=%p generation=%u command_seq=%u "
                            "target=%d sent=%d error=%d fresh_frames=%u "
                            "frame_age_ms=%u action=RESUME_TARGET_FOLLOW",
                            receiver, stream, generation, zoom_command_seq,
                            zoom_target_steps, zoom_sent_steps,
                            zoom_target_steps - zoom_sent_steps,
                            recovery_fresh, frame_age_us / 1000u);
                    }
                }

                if (zoom_stall_latched &&
                    (uint32_t)(wheel_now - zoom_stall_started_at) >=
                        WHEEL_ZOOM_STALL_ABORT_US &&
                    input_quiet_us >= WHEEL_ZOOM_STALL_ABORT_QUIET_US) {
                    altscreen_log(
                        "PHASE=WHEEL_ZOOM_STALL_ABORT receiver=%p stream=%p "
                        "generation=%u target_before=%d sent=%d error_before=%d "
                        "stall_ms=%u input_quiet_ms=%u "
                        "action=REBASE_TARGET_TO_SENT_NO_LATE_REPLAY",
                        receiver, stream, generation, zoom_target_steps,
                        zoom_sent_steps,
                        zoom_target_steps - zoom_sent_steps,
                        (uint32_t)(wheel_now - zoom_stall_started_at) / 1000u,
                        input_quiet_us / 1000u);
                    zoom_target_steps = zoom_sent_steps;
                    zoom_stall_latched = 0;
                    zoom_stall_started_at = 0;
                    zoom_first_step_pending = 0;
                    zoom_reverse_response_pending = 0;
                    zoom_recovery_generation = 0;
                    zoom_recovery_frame_count = 0;
                }
            }

            if (!zoom_stall_latched &&
                zoom_target_steps != zoom_sent_steps) {
                if (first_send) {
                    send_ready = 1;
                } else if (zoom_first_step_pending) {
                    send_ready = elapsed_us >= selected_pace_us;
                } else if (progress_ok && baseline_current) {
                    send_ready = elapsed_us >= selected_pace_us &&
                        fresh_since_send > 0u;
                } else if (elapsed_us >= WHEEL_ZOOM_FALLBACK_PACE_US) {
                    fallback_timer = 1;
                    pace_mode = "NO_TELEMETRY_FALLBACK";
                    selected_pace_us = WHEEL_ZOOM_FALLBACK_PACE_US;
                    send_ready = 1;
                }
            }

            if (send_ready && zoom_have_failed_send_time &&
                (uint32_t)(wheel_now - zoom_last_failed_send_at) <
                    WHEEL_ZOOM_SEND_RETRY_US)
                send_ready = 0;

            if (send_ready) {
                int sent_before = zoom_sent_steps;
                int attempted_sent;
                int direction =
                    zoom_target_steps < zoom_sent_steps ? 0 : 1;
                struct p111_frame_progress_snapshot after_send_progress;
                int after_send_progress_ok;

                attempted_sent = zoom_sent_steps +
                    (direction == 0 ? -1 : 1);

                ++zoom_command_seq;
                if (!zoom_command_seq) ++zoom_command_seq;

                send_rc = alt_send_cluster_zoom(
                    receiver, stream, generation,
                    zoom_command_seq, direction);

                memset(&after_send_progress, 0,
                       sizeof(after_send_progress));
                after_send_progress_ok = 0;
                if (send_rc == 0) {
                    zoom_last_send_at = wheel_now;
                    zoom_have_send_time = 1;
                    zoom_have_failed_send_time = 0;
                    zoom_sent_steps = attempted_sent;
                    zoom_first_step_pending = 0;
                    if (zoom_reverse_response_pending &&
                        zoom_have_input_direction &&
                        direction == zoom_last_input_direction)
                        zoom_reverse_response_pending = 0;

                    after_send_progress_ok = p111_frame_tap_get_progress(
                        stream, &after_send_progress);
                    if (after_send_progress_ok) {
                        zoom_send_frame_baseline_valid = 1;
                        zoom_send_frame_generation =
                            after_send_progress.generation;
                        zoom_send_frame_count =
                            after_send_progress.frame_count;
                    } else {
                        zoom_send_frame_baseline_valid = 0;
                        zoom_send_frame_generation = 0;
                        zoom_send_frame_count = 0;
                    }
                } else {
                    zoom_last_failed_send_at = wheel_now;
                    zoom_have_failed_send_time = 1;
                }

                altscreen_log(
                    "PHASE=WHEEL_ZOOM_PACED_SEND receiver=%p stream=%p "
                    "generation=%u command_seq=%u source_tail_seq=%u "
                    "action=%s direction=%d target=%d sent_before=%d "
                    "sent_after=%d error_after=%d scheduler=%s pacing=%s "
                    "first_send=%d first_step_pending=%d telemetry=%s "
                    "fallback_timer=%d elapsed_us=%u fresh_since_send=%u "
                    "frame_age_ms=%u pace_mode=%s selected_pace_ms=%u "
                    "input_quiet_ms=%u burst_input_steps=%u backlog_abs=%d "
                    "reverse_response_pending=%d response_gates_next=0 rc=%d",
                    receiver, stream, generation, zoom_command_seq,
                    zoom_last_seq,
                    direction == 0 ? "ZOOM_IN" : "ZOOM_OUT",
                    direction, zoom_target_steps, sent_before,
                    zoom_sent_steps,
                    zoom_target_steps - zoom_sent_steps,
                    WHEEL_ZOOM_SCHEDULER_MODEL,
                    WHEEL_ZOOM_PACING_MODEL,
                    first_send, zoom_first_step_pending,
                    progress_ok ? "decoded_progress" : "unavailable",
                    fallback_timer, elapsed_us, fresh_since_send,
                    frame_age_us / 1000u, pace_mode,
                    selected_pace_us / 1000u, input_quiet_us / 1000u,
                    zoom_burst_input_steps,
                    error_steps < 0 ? -error_steps : error_steps,
                    zoom_reverse_response_pending, send_rc);

                if (send_rc != 0) {
                    p1404_cockpit_native_zoom_result(
                        receiver, stream, generation,
                        zoom_command_seq, direction,
                        send_rc, 0);
                } else if (zoom_target_steps == zoom_sent_steps &&
                           zoom_target_steps != 0) {
                    altscreen_log(
                        "PHASE=WHEEL_ZOOM_TARGET_REBASE receiver=%p stream=%p "
                        "generation=%u settled_relative_level=%d "
                        "action=REBASE_TARGET_AND_SENT_TO_ZERO "
                        "reason=successful_catchup outstanding_limit=%d",
                        receiver, stream, generation, zoom_target_steps,
                        WHEEL_ZOOM_TARGET_LIMIT);
                    zoom_target_steps = 0;
                    zoom_sent_steps = 0;
                }
            }
        }

        if (route_ready && !visible && !pending && native_route_requested()) {
            altscreen_log("PHASE=NATIVE_111_ROUTE_READY receiver=%p stream=%p generation=%u basis=dynamic_config_plus_accepted_showui_plus_first_real_type111_post video_availability_gate=real_frame",
                          receiver, stream, generation);
            (void)request_route_action(slot, NATIVE_ROUTE_ACTIVATE);
        } else if (visible && !pending && !native_route_requested()) {
            altscreen_log("WARN PHASE=NATIVE_111_ROUTE_WATCHDOG reason=route_marker_removed restore=1");
            (void)request_route_action(slot, NATIVE_ROUTE_RESTORE);
        }
    }
    free(job);
    return NULL;
}

void p1404_cockpit_native_zoom_result(void *receiver, void *stream,
                                      uint32_t generation,
                                      uint32_t event_seq,
                                      int direction,
                                      int status,
                                      int response_received) {
    struct native_slot *slot;
    int generation_current = 0;

    native_lock();
    slot = find_stream_locked(receiver, stream);
    generation_current = slot && slot->generation == generation;
    native_unlock();

    altscreen_log(
        "PHASE=CLUSTER_ZOOM_RESPONSE receiver=%p stream=%p generation=%u "
        "seq=%u direction=%d action=%s status=%d response_received=%d "
        "generation_current=%d semantics=OBSERVE_ONLY_NO_SUCCESS_ASSUMPTION",
        receiver, stream, generation, event_seq, direction,
        direction == 0 ? "ZOOM_IN" : "ZOOM_OUT",
        status, response_received, generation_current);
}


void p1404_cockpit_native_event_result(void *receiver, void *stream,
                                        uint32_t generation, int event_kind,
                                        int status, int response_received) {
    struct native_slot *slot;
    uint32_t state_generation = 0;
    int accepted = status == 0 && response_received;
    int applied = 0;
    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot && slot->generation == generation) {
        state_generation = slot->state_generation;
        if (event_kind == ALT111_EVENT_SHOW_UI && slot->ui_event_state == 1) {
            slot->ui_event_state = accepted ? 2 : 0;
            applied = 1;
        } else if (event_kind == ALT111_EVENT_FORCE_KEYFRAME &&
                   slot->keyframe_event_state == 1) {
            slot->keyframe_event_state = accepted ? 2 : 0;
            applied = 1;
        }
    }
    native_unlock();
    if (accepted && applied && event_kind == ALT111_EVENT_SHOW_UI)
        alt_state_mark_native(receiver, state_generation,
                              ALT_STATE_NATIVE_UI_ACTIVE,
                              "showUI-alt-uuid-cluster-map-response-status0");
    altscreen_log("PHASE=ALT111_EVENT_RESULT receiver=%p stream=%p generation=%u event=%d status=%d response_received=%d accepted=%d applied=%d retry=%d",
                  receiver, stream, generation, event_kind, status,
                  response_received, accepted && applied, applied,
                  applied && !accepted);
}


void p1404_cockpit_native_view_area_result(void *receiver, void *stream,
                                            uint32_t generation,
                                            uint32_t event_seq,
                                            int view_area_index,
                                            int status,
                                            int response_received) {
    struct native_slot *slot;
    uint64_t result_now = obs_now_us();
    int accepted = status == 0 && response_received;
    int applied = 0;
    int stale = 0;

    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot && slot->generation == generation &&
        slot->view_area_target == view_area_index &&
        slot->view_area_event_state == 1 &&
        slot->view_area_inflight_seq == event_seq) {
        if (accepted) {
            slot->view_area_applied = view_area_index;
            slot->view_area_event_state = 2;
            slot->view_area_acked_seq = event_seq;
            slot->view_area_ack_at = result_now;
            slot->view_area_frame_pending = 1;
        } else {
            slot->view_area_event_state = 0;
            slot->view_area_inflight_seq = 0;
            slot->view_area_frame_pending = 0;
            slot->view_area_frame_baseline_valid = 0;
        }
        applied = 1;
    } else {
        stale = 1;
    }
    native_unlock();

    altscreen_log(
        "PHASE=ALT111_VIEWAREA_RESULT receiver=%p stream=%p generation=%u "
        "request_seq=%u viewAreaIndex=%d status=%d response_received=%d "
        "acknowledged=%d target_still_current=%d stale_callback=%d retry=%d "
        "visual_proof=pending_fresh_type111_frame",
        receiver, stream, generation, event_seq, view_area_index, status,
        response_received, accepted && applied, applied, stale,
        applied && !accepted);
}

int p1404_cockpit_native_prepare_start(void *receiver) {
    int ok = 0;
    if (!receiver || !p1404_mutate_armed || !p1404_cockpit_native_bind_stock())
        return 0;
    native_lock();
    if (!g_start_claim.active) {
        memset(&g_start_claim, 0, sizeof(g_start_claim));
        g_start_claim.receiver = receiver;
        g_start_claim.owner_thread = obs_thread_id();
        g_start_claim.active = 1;
        ok = 1;
    }
    native_unlock();
    altscreen_log("PHASE=NATIVE_111_START_CLAIM_PREPARE receiver=%p owner_thread=%lu ready=%d preconfig_attach=1",
                  receiver, obs_thread_id(), ok);
    return ok;
}

int p1404_cockpit_native_complete_start(void *receiver, void **stream) {
    int ok = 0;
    void *claimed_stream = NULL;
    unsigned long owner = obs_thread_id();
    if (stream) *stream = NULL;
    native_lock();
    if (g_start_claim.active && g_start_claim.receiver == receiver &&
        g_start_claim.owner_thread == owner) {
        claimed_stream = g_start_claim.stream;
        ok = g_start_claim.invoked && g_start_claim.attached && claimed_stream;
        memset(&g_start_claim, 0, sizeof(g_start_claim));
    }
    native_unlock();
    if (stream) *stream = claimed_stream;
    altscreen_log("PHASE=NATIVE_111_START_CLAIM_COMPLETE receiver=%p owner_thread=%lu stream=%p attached=%d preconfig_attach=%d",
                  receiver, owner, claimed_stream, ok, ok);
    return ok;
}

/* dio_manager imports ScreenCreate directly. Besides preserving its exact stock
 * behavior, this narrow front door guarantees that a redirect-install refusal
 * still starts the asynchronous worker and produces an explicit hook log rather
 * than another silent all-NO vehicle session. */
int ScreenCreate(void **out_screen, const void *properties) {
    f_screen_create_t stock = g_real_screen_create;
    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if (!stock)
        stock = (f_screen_create_t)native_direct_stock("ScreenCreate");
    if (!stock) {
        if (out_screen) *out_screen = NULL;
        return -1;
    }
    g_real_screen_create = stock;
    return stock(out_screen, properties);
}

/* Exact stock ABI: ScreenCopyMain returns the retained Screen object in r0 and
 * writes OSStatus through its sole argument. The previous int(void **) wrapper
 * accidentally returned -58796 as a Screen pointer when reached before dlsym
 * was ready; _ScreenCopyProperty then crashed in CFLRetain. */
void *ScreenCopyMain(int *out_err) {
    void *receiver = NULL;
    void *display_descriptor = NULL;
    void *stock_main = NULL;
    void *alt_screen = NULL;
    uint32_t delegates[5]; /* exact 20-byte Screen delegate block */
    unsigned long owner = obs_thread_id();
    int private_claim = 0;
    int rc = NATIVE_CONFIG_REFUSED_STATUS;
    int stock_err = NATIVE_CONFIG_REFUSED_STATUS;
    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if (!g_real_screen_copy_main)
        g_real_screen_copy_main = (f_screen_copy_main_t)
            native_direct_stock("ScreenCopyMain");
    if (!g_real_screen_copy_main) (void)p1404_cockpit_native_bind_stock();
    if (!g_real_screen_copy_main) {
        if (out_err) *out_err = NATIVE_CONFIG_REFUSED_STATUS;
        return NULL;
    }
    if (altscreen_runtime_is_ready && !altscreen_runtime_is_ready())
        return g_real_screen_copy_main(out_err);
    native_lock();
    if (g_start_claim.active && g_start_claim.owner_thread == owner) {
        receiver = g_start_claim.receiver;
        private_claim = 1;
    }
    native_unlock();
    if (!private_claim) return g_real_screen_copy_main(out_err);

    /* StartSession does not consume a display-info dictionary here. It calls
     * ScreenCopyDelegates on the returned object and therefore requires a real
     * stock Screen runtime instance (80 bytes on K1004), with delegates at
     * +0x3c. Returning the /info dictionary directly is an ABI violation. */
    if (!g_real_screen_create)
        g_real_screen_create = (f_screen_create_t)native_direct_stock("ScreenCreate");
    if (!g_real_screen_copy_delegates)
        g_real_screen_copy_delegates = (f_screen_copy_delegates_t)
            native_direct_stock("ScreenCopyDelegates");
    if (!g_real_screen_register_delegates)
        g_real_screen_register_delegates = (f_screen_register_delegates_t)
            native_direct_stock("ScreenRegisterDelegates");
    if (!g_real_screen_create || !g_real_screen_copy_delegates ||
        !g_real_screen_register_delegates) goto fail;

    display_descriptor = alt_build_cluster_display();
    if (!display_descriptor) goto fail;
    rc = g_real_screen_create(&alt_screen, display_descriptor);
    alt_airplay_release_object(display_descriptor);
    display_descriptor = NULL;
    if (rc != 0 || !alt_screen) goto fail;

    memset(delegates, 0, sizeof(delegates));
    stock_main = g_real_screen_copy_main(&stock_err);
    if (!stock_main || stock_err != 0) goto fail;
    rc = g_real_screen_copy_delegates(stock_main, delegates);
    alt_airplay_release_object(stock_main);
    stock_main = NULL;
    if (rc != 0) goto fail;
    g_real_screen_register_delegates(alt_screen, delegates);

    if (out_err) *out_err = 0;
    altscreen_log("PHASE=NATIVE_111_PRIVATE_SCREEN_COPY receiver=%p owner_thread=%lu screen=%p type=111 runtime_object=ScreenCreate delegates=stock_main geometry=runtime_display1 stock_main_mutated=0",
                  receiver, owner, alt_screen);
    return alt_screen;

fail:
    if (stock_main) alt_airplay_release_object(stock_main);
    if (display_descriptor) alt_airplay_release_object(display_descriptor);
    if (alt_screen) alt_airplay_release_object(alt_screen);
    if (out_err) *out_err = rc != 0 ? rc : NATIVE_CONFIG_REFUSED_STATUS;
    altscreen_log("ERROR PHASE=NATIVE_111_PRIVATE_SCREEN_COPY receiver=%p owner_thread=%lu result=REFUSED rc=%d stock_err=%d runtime_screen_required=1 stock_main_mutated=0",
                  receiver, owner, rc, stock_err);
    return NULL;
}

static int finalize_private_attach(void *receiver, void *stream) {
    struct native_slot *slot;
    struct native_thread_job *monitor_job;
    native_pthread_t monitor = 0;
    uint32_t generation = 0;
    void *video_impl = NULL, *renderer = NULL;
    int rc;
    monitor_job = (struct native_thread_job *)calloc(1u, sizeof(*monitor_job));
    if (!monitor_job) return 0;
    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot && slot->owner_thread == obs_thread_id() && slot->renderer &&
        slot->preconfig_rewritten && !slot->monitor_started) {
        slot->monitor_started = 1;
        generation = slot->generation;
        video_impl = slot->video_impl;
        renderer = slot->renderer;
        monitor_job->slot = slot;
        monitor_job->receiver = receiver;
        monitor_job->stream = stream;
        monitor_job->generation = generation;
        monitor_job->state_generation = slot->state_generation;
    } else slot = NULL;
    native_unlock();
    if (!slot) {
        free(monitor_job);
        return 0;
    }
    rc = g_pthread_create && g_pthread_detach ?
        g_pthread_create(&monitor, NULL, native_monitor_worker, monitor_job) : -1;
    if (rc != 0) {
        native_lock();
        slot = find_stream_locked(receiver, stream);
        if (slot && slot->generation == generation) slot->monitor_started = 0;
        native_unlock();
        free(monitor_job);
        altscreen_log("ERROR PHASE=NATIVE_111_ATTACH receiver=%p stream=%p result=REFUSED monitor_thread_rc=%d source59_fallthrough_blocked=1",
                      receiver, stream, rc);
        return 0;
    }
    (void)g_pthread_detach(monitor);
    altscreen_log("PHASE=NATIVE_111_ATTACH receiver=%p stream=%p video_impl=%p renderer=%p displayable=58 first_config_rewritten=1 main110_untouched=1 capture=0 rfb=0 scaling=0 fixed_geometry=0",
                  receiver, stream, video_impl, renderer);
    return 1;
}

/* ScreenStreamStart is called through libairplay's PLT after ScreenStreamCreate.
 * Stock initializes the video implementation and invokes its first config inside
 * this call. A staged, thread-qualified slot lets the config interposer bind the
 * newly-created renderer before that first config reaches displayable 59. */
int ScreenStreamStart(void *stream) {
    void *receiver = NULL;
    unsigned long owner = obs_thread_id();
    int private_claim = 0;
    int staged = 0;
    int attached = 0;
    int rc;
    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if (!g_real_stream_start)
        g_real_stream_start = (f_screen_stream_start_t)
            native_direct_stock("ScreenStreamStart");
    if (!g_real_stream_start) (void)p1404_cockpit_native_bind_stock();
    if (!g_real_stream_start) return NATIVE_CONFIG_REFUSED_STATUS;
    if (altscreen_runtime_is_ready && !altscreen_runtime_is_ready())
        return g_real_stream_start(stream);
    native_lock();
    if (g_start_claim.active && !g_start_claim.invoked &&
        g_start_claim.owner_thread == owner) {
        g_start_claim.invoked = 1;
        g_start_claim.stream = stream;
        receiver = g_start_claim.receiver;
        private_claim = 1;
    }
    native_unlock();
    if (!private_claim) return g_real_stream_start(stream);

    staged = p1404_cockpit_native_attach(receiver, stream);
    if (!staged) {
        altscreen_log("ERROR PHASE=NATIVE_111_PRECONFIG_ATTACH receiver=%p stream=%p result=REFUSED stock_start_called=0 main110_untouched=1",
                      receiver, stream);
        return NATIVE_CONFIG_REFUSED_STATUS;
    }
    altscreen_log("PHASE=NATIVE_111_PRECONFIG_ATTACH receiver=%p stream=%p result=STAGED stock_start_called=1 main110_untouched=1",
                  receiver, stream);
    rc = g_real_stream_start(stream);
    if (rc == 0) attached = finalize_private_attach(receiver, stream);
    if (rc != 0 || !attached) {
        p1404_cockpit_native_detach(receiver, stream);
        if (rc == 0) rc = NATIVE_CONFIG_REFUSED_STATUS;
    }
    native_lock();
    if (g_start_claim.active && g_start_claim.receiver == receiver &&
        g_start_claim.owner_thread == owner && g_start_claim.stream == stream)
        g_start_claim.attached = attached;
    native_unlock();
    altscreen_log("%s PHASE=NATIVE_111_PRECONFIG_COMPLETE receiver=%p stream=%p stock_rc=%d attached=%d first_config_rewritten=%d main110_untouched=1",
                  attached ? "" : "ERROR", receiver, stream, rc, attached, attached);
    return rc;
}

int p1404_cockpit_native_attach(void *receiver, void *stream) {
    struct native_slot *slot;
    struct altscreen_ctx state_snap;
    uint32_t width = 0, height = 0;
    void *video_impl, *renderer;
    uint32_t assigned_generation = 0;
    memset(&state_snap, 0, sizeof(state_snap));
    if (!receiver || !stream || !p1404_mutate_armed) return 0;
    if (!p1404_cockpit_native_bind_stock()) return 0;
    if (!alt_state_snapshot(receiver, 1, &state_snap) || !state_snap.generation) {
        altscreen_log("ERROR PHASE=NATIVE_111_ATTACH_STAGE receiver=%p stream=%p result=REFUSED state_generation_unavailable=1", receiver, stream);
        return 0;
    }
    if (!p1404_cockpit_native_get_geometry(&width, &height) &&
        !p1404_cockpit_native_refresh_geometry()) {
        altscreen_log("ERROR PHASE=NATIVE_111_ATTACH_GEOMETRY result=REFUSED reason=live_screen_geometry_unavailable bootstrap_not_used_for_renderer=1");
        return 0;
    }
    if (!p1404_cockpit_native_get_geometry(&width, &height)) {
        altscreen_log("ERROR PHASE=NATIVE_111_ATTACH_GEOMETRY result=REFUSED reason=geometry_not_published_after_refresh");
        return 0;
    }
    if (!alt_airplay_validate_runtime_geometry(width, height)) {
        altscreen_log("ERROR PHASE=NATIVE_111_ATTACH_GEOMETRY result=REFUSED reason=negotiated_runtime_geometry_mismatch source59_fallthrough_blocked=1");
        return 0;
    }
    video_impl = read_ptr_at(stream, SCREEN_STREAM_VIDEO_IMPL_OFF);
    renderer = read_ptr_at(video_impl, OMX_VIDEO_RENDERER_OFF);
    if (!video_impl) {
        altscreen_log("ERROR PHASE=NATIVE_111_ATTACH_STAGE receiver=%p stream=%p video_impl=%p result=REFUSED source59_fallthrough_blocked=1",
                      receiver, stream, video_impl);
        return 0;
    }

    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (!slot) slot = free_slot_locked();
    if (slot) {
        memset(slot, 0, sizeof(*slot));
        slot->receiver = receiver;
        slot->stream = stream;
        slot->video_impl = video_impl;
        slot->renderer = renderer;
        slot->owner_thread = obs_thread_id();
        slot->generation = ++g_generation;
        if (!slot->generation) slot->generation = ++g_generation;
        slot->state_generation = state_snap.generation;
        slot->view_area_target = -1;
        slot->view_area_applied = -1;
        assigned_generation = slot->generation;
    }
    native_unlock();
    if (!slot) {
        altscreen_log("ERROR PHASE=NATIVE_111_ATTACH_STAGE receiver=%p stream=%p result=REFUSED route_slots_full=1 source59_fallthrough_blocked=1",
                      receiver, stream);
        return 0;
    }
    altscreen_log("PHASE=NATIVE_111_ATTACH_STAGE receiver=%p stream=%p video_impl=%p renderer_before_start=%p generation=%u state_generation=%u requested_geometry=%ux%u first_config_pending=1",
                  receiver, stream, video_impl, renderer, assigned_generation,
                  state_snap.generation, width, height);
    return 1;
}

void p1404_cockpit_native_detach(void *receiver, void *stream) {
    struct native_slot *slot;
    uint32_t generation = 0;
    uint32_t state_generation = 0;
    int send_stop = 0;
    int restore = 0;
    int restored = 0;
    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot) {
        generation = slot->generation;
        state_generation = slot->state_generation;
        send_stop = slot->monitor_started;
        restore = slot->visible ||
                  (slot->action_pending && slot->action == NATIVE_ROUTE_ACTIVATE);
        slot->stream = NULL;
        slot->receiver = NULL;
        slot->renderer = NULL;
        slot->window = NULL;
        ++slot->generation;
    }
    native_unlock();
    if (generation) p111_direct_tap_stream_end(stream);
    if (generation && send_stop)
        (void)alt_send_cluster_event(receiver, stream, generation,
                                     ALT111_EVENT_STOP_UI);
    if (restore) {
        route_lock();
        /* A pending old activation either completed and restored itself before
         * we acquired this lock, or will observe the invalidated generation
         * after release and perform no display mutation. Only the generation
         * that actually owns context76 may restore it here. */
        if (g_route_generation == generation) {
            restored = run_restore_route();
            if (restored) g_route_generation = 0;
        }
        route_unlock();
    }
    if (restored)
        alt_state_mark_native(receiver, state_generation,
                              ALT_STATE_NATIVE_COCKPIT_HIDDEN,
                              "private111-detach-restored-context74");
    altscreen_log("PHASE=NATIVE_111_DETACH receiver=%p stream=%p restore=%d restored=%d",
                  receiver, stream, restore, restored);
}

#ifdef ALTSCREEN_NATIVE_HOST_TEST
void p1404_cockpit_native_test_route_reset(void) {
    native_lock();
    memset(g_native, 0, sizeof(g_native));
    memset(&g_managed_config, 0, sizeof(g_managed_config));
    g_generation = 0;
    native_unlock();
    route_lock();
    g_route_generation = 0;
    route_unlock();
}
int p1404_cockpit_native_test_route_seed(void *receiver, void *stream,
                                         uint32_t generation,
                                         uint32_t state_generation) {
    struct native_slot *slot;
    int ok = 0;
    native_lock();
    slot = free_slot_locked();
    if (slot && receiver && stream && generation && state_generation) {
        memset(slot, 0, sizeof(*slot));
        slot->receiver = receiver;
        slot->stream = stream;
        slot->generation = generation;
        slot->state_generation = state_generation;
        slot->view_area_target = -1;
        slot->view_area_applied = -1;
        ok = 1;
    }
    native_unlock();
    return ok;
}
int p1404_cockpit_native_test_route_request(void *receiver, void *stream,
                                            int action) {
    struct native_slot *slot;
    native_lock();
    slot = find_stream_locked(receiver, stream);
    native_unlock();
    return request_route_action(slot, action);
}
int p1404_cockpit_native_test_route_visible(void *receiver, void *stream) {
    struct native_slot *slot;
    int visible = 0;
    native_lock();
    slot = find_stream_locked(receiver, stream);
    if (slot) visible = slot->visible;
    native_unlock();
    return visible;
}
#endif

/* CScreenRender submits its fixed static group as a delayed Screen operation.
 * For the thread-qualified private renderer, suppress only that group request.
 * Main110 and every unrelated caller keep the stock group and behavior. */
int screen_create_window_group(screen_window_t window, const char *name) {
    int private_config = 0;
    if (!g_screen_create_window_group) (void)bind_screen_native_api();
    native_lock();
    if (g_managed_config.active &&
        g_managed_config.owner_thread == obs_thread_id()) {
        g_managed_config.window = window;
        g_managed_config.group_skipped = 1;
        private_config = 1;
    }
    native_unlock();
    if (private_config) return window && name ? 0 : -1;
    return g_screen_create_window_group ?
        g_screen_create_window_group(window, name) : -1;
}

/* At this call all stock window properties, including displayable 58, NV12,
 * usage and native geometry, are already present. Register that exact window
 * through the DisplayManager handshake used by MMI Cockpit Mirror 2.2, then
 * let stock allocate and own its decoder buffers. */
int screen_create_window_buffers(screen_window_t window, int count) {
    int private_config = 0;
    int group_skipped = 0;
    int manage_rc = -1;
    int buffers_rc;
    if (!g_screen_create_window_buffers || !g_screen_manage_window)
        (void)bind_screen_native_api();
    native_lock();
    if (g_managed_config.active &&
        g_managed_config.owner_thread == obs_thread_id() &&
        (!g_managed_config.window || g_managed_config.window == window)) {
        g_managed_config.window = window;
        group_skipped = g_managed_config.group_skipped;
        private_config = 1;
    }
    native_unlock();
    if (!private_config)
        return g_screen_create_window_buffers ?
            g_screen_create_window_buffers(window, count) : -1;
    if (group_skipped && g_screen_manage_window)
        manage_rc = g_screen_manage_window(window, DISPLAY_MANAGER_SECRET);
    if (manage_rc != 0) {
        native_lock();
        g_managed_config.manage_rc = manage_rc;
        native_unlock();
        return manage_rc;
    }
    buffers_rc = g_screen_create_window_buffers ?
        g_screen_create_window_buffers(window, count) : -1;
    native_lock();
    g_managed_config.manage_rc = manage_rc;
    g_managed_config.buffers_rc = buffers_rc;
    g_managed_config.managed = buffers_rc == 0;
    native_unlock();
    return buffers_rc;
}

/* Exported with the exact C++ names used by libairplay's PLT. */
int p1404_hook_cscreen_config(void *self, const struct p1404_screen_config *config)
    __asm__(CSCREEN_CONFIG_SYMBOL);
int p1404_hook_cscreen_config(void *self, const struct p1404_screen_config *config) {
    struct p1404_screen_config native_config;
    struct native_slot *slot;
    void *receiver = NULL;
    void *stream = NULL;
    uint32_t target_width = 0, target_height = 0;
    uint32_t generation = 0;
    uint32_t state_generation = 0;
    int owned_private = 0;
    int rewritten = 0;
    int geometry_match = 0;
    int managed_scope = 0;
    int managed_ok = 0;
    int group_skipped = 0;
    int manage_rc = -1;
    int buffers_rc = -1;
    screen_window_t managed_window = NULL;
    int rc;

    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if (!g_real_config)
        g_real_config = (f_cscreen_config_t)
            native_direct_stock(CSCREEN_CONFIG_SYMBOL);
    if (!g_real_config) (void)p1404_cockpit_native_bind_stock();
    if (!g_real_config) return -1;
    if (altscreen_runtime_is_ready && !altscreen_runtime_is_ready())
        return g_real_config(self, config);
    native_lock();
    slot = find_renderer_locked(self);
    if (!slot) {
        slot = find_preconfig_locked(self, obs_thread_id());
        if (slot) {
            slot->renderer = self;
            altscreen_log("PHASE=NATIVE_111_RENDERER_BOUND_PRECONFIG receiver=%p stream=%p renderer=%p owner_thread=%lu first_config=1",
                          slot->receiver, slot->stream, self, obs_thread_id());
        }
    }
    if (slot) {
        receiver = slot->receiver;
        stream = slot->stream;
        generation = slot->generation;
        state_generation = slot->state_generation;
        owned_private = 1;
    }
    native_unlock();
    if (owned_private)
        rewritten = p1404_cockpit_native_rewrite_config(config, &native_config);
    if (owned_private && !rewritten) {
        altscreen_log("ERROR PHASE=NATIVE_111_CONFIG_REFUSED receiver=%p stream=%p renderer=%p source=%ux%u runtime_geometry_unavailable_or_invalid=1 main110_untouched=1",
                      receiver, stream, self,
                      config ? config->source_width : 0u,
                      config ? config->source_height : 0u);
        return NATIVE_CONFIG_REFUSED_STATUS;
    }
    if (rewritten) {
        native_lock();
        if (!g_managed_config.active) {
            memset(&g_managed_config, 0, sizeof(g_managed_config));
            g_managed_config.renderer = self;
            g_managed_config.owner_thread = obs_thread_id();
            g_managed_config.active = 1;
            managed_scope = 1;
        }
        native_unlock();
        if (!managed_scope) {
            altscreen_log("ERROR PHASE=NATIVE_111_MANAGED_WINDOW_SCOPE receiver=%p stream=%p renderer=%p result=REFUSED concurrent_private_config=1 main110_untouched=1",
                          receiver, stream, self);
            return NATIVE_CONFIG_REFUSED_STATUS;
        }
    }
    rc = g_real_config(self, rewritten ? &native_config : config);
    if (managed_scope) {
        native_lock();
        group_skipped = g_managed_config.group_skipped;
        manage_rc = g_managed_config.manage_rc;
        buffers_rc = g_managed_config.buffers_rc;
        managed_ok = g_managed_config.managed;
        managed_window = g_managed_config.window;
        memset(&g_managed_config, 0, sizeof(g_managed_config));
        native_unlock();
        altscreen_log("PHASE=NATIVE_111_MANAGED_WINDOW receiver=%p stream=%p renderer=%p window=%p group_skipped=%d manage_rc=%d buffers_rc=%d managed=%d manager=displaymanager stock_buffer_owner=1 compat_decoder_staging=1 direct_sink_window58=0",
                      receiver, stream, self, managed_window,
                      group_skipped, manage_rc, buffers_rc, managed_ok);
        if (rc == 0 && !managed_ok) rc = NATIVE_CONFIG_REFUSED_STATUS;
    }
    if (rewritten) {
        (void)p1404_cockpit_native_get_geometry(&target_width, &target_height);
        geometry_match = native_config.source_width == target_width &&
                         native_config.source_height == target_height;
        altscreen_log("PHASE=NATIVE_111_CONFIG_RETURN receiver=%p stream=%p renderer=%p rc=%d input=%ux%u_source_%ux%u output=%ux%u_source_%ux%u target=%ux%u geometry_match=%d displayable=58 format=%u usage=0x%x scaling=0 fixed_geometry=0 dm_managed=%d",
                      receiver, stream, self, rc,
                      config ? config->window_width : 0u,
                      config ? config->window_height : 0u,
                      config ? config->source_width : 0u,
                      config ? config->source_height : 0u,
                      native_config.window_width, native_config.window_height,
                      native_config.source_width, native_config.source_height,
                      target_width, target_height, geometry_match,
                      native_config.format, native_config.usage, managed_ok);
        if (rc == 0) {
            native_lock();
            slot = find_renderer_locked(self);
            if (slot && slot->stream == stream &&
                slot->generation == generation) {
                slot->preconfig_rewritten = 1;
                slot->config_ok = geometry_match;
                slot->window = managed_window;
                slot->linearizer_aux_drops = 0u;
                slot->config_width = native_config.source_width;
                slot->config_height = native_config.source_height;
                slot->config_format = native_config.format;
                slot->config_usage = native_config.usage;
                slot->first_real_frame_posted = 0;
                if (slot->keyframe_event_state == 2)
                    slot->keyframe_event_state = 0;
            }
            native_unlock();
            if (geometry_match) {
                alt_state_mark_native(receiver, state_generation,
                    ALT_STATE_NATIVE_VIDEO_CONFIG,
                    "stock-omx-cscreen-config-dynamic-geometry-displayable58");
            } else {
                altscreen_log("PHASE=NATIVE_111_CONFIG_WAIT_NEGOTIATED_GEOMETRY receiver=%p stream=%p renderer=%p current=%ux%u target=%ux%u route_ready=0",
                              receiver, stream, self, native_config.source_width,
                              native_config.source_height, target_width, target_height);
            }
        }
    }
    return rc;
}

int p1404_hook_cscreen_render(void *self, unsigned char *buffer)
    __asm__(CSCREEN_RENDER_SYMBOL);
int p1404_hook_cscreen_render(void *self, unsigned char *buffer) {
    struct native_slot *slot;
    struct altscreen_ctx snap;
    void *receiver = NULL;
    void *stream = NULL;
    screen_window_t stock_window = NULL;
    uint64_t now;
    uint64_t first_post_at = 0;
    uint32_t posts = 0;
    uint32_t generation = 0;
    uint32_t state_generation = 0;
    uint32_t config_width = 0;
    uint32_t config_height = 0;
    uint32_t config_format = 0;
    uint32_t config_usage = 0;
    int config_ok = 0;
    int owned_private = 0;
    int first_real_post = 0;
    int should_mark = 0;
    int rc;

    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if (!g_real_render)
        g_real_render = (f_cscreen_render_t)
            native_direct_stock(CSCREEN_RENDER_SYMBOL);
    if (!g_real_render) (void)p1404_cockpit_native_bind_stock();
    if (!g_real_render) return -1;
    if (altscreen_runtime_is_ready && !altscreen_runtime_is_ready())
        return g_real_render(self, buffer);

    native_lock();
    slot = find_renderer_locked(self);
    if (slot) {
        owned_private = 1;
        receiver = slot->receiver;
        stream = slot->stream;
        generation = slot->generation;
        state_generation = slot->state_generation;
        config_ok = slot->config_ok;
        config_width = slot->config_width;
        config_height = slot->config_height;
        config_format = slot->config_format;
        config_usage = slot->config_usage;
        stock_window = slot->window;
    }
    native_unlock();

    /*
     * Direct-display V2 deliberately lets stock render first.  The decoded
     * pointer is Screen format 0x0001000c on the tested i.MX6 firmware and V1
     * proved that treating it as row-linear NV12 produces the moving garbled
     * picture.  After stock posts the exact buffer, ask Screen to linearize the
     * renderer's own window into a normal pixmap. The window handle is the
     * exact handle captured from screen_create_window_buffers() during config;
     * render does not infer another CScreenRender object offset.
     *
     * Main110 never enters this branch because ownership is bound to the
     * private stream.  Stock rendering remains the authoritative fail-open
     * path and is never suppressed by the V2 linearizer.
     */
    rc = g_real_render(self, buffer);
    if (rc != 0 || !owned_private || !config_ok) return rc;

    if (stream && buffer && config_width && config_height) {
        if (!p111_frame_tap_write_window(stream, stock_window,
                                         config_width, config_height,
                                         config_format, config_usage)) {
            uint32_t drop_count = 0u;
            /*
             * Never publish the original vendor OMX pointer after a Screen
             * linearization failure. Before the first good auxiliary frame we
             * simply remain not-ready; after success the linearizer freezes the
             * last good frame. Stock CarPlay rendering remains untouched.
             */
            native_lock();
            slot = find_renderer_locked(self);
            if (slot && slot->generation == generation &&
                slot->stream == stream)
                drop_count = ++slot->linearizer_aux_drops;
            native_unlock();
            if (drop_count == 1u || (drop_count % 60u) == 0u) {
                altscreen_log("WARN PHASE=FRAME_LINEARIZER_AUX_DROP stream=%p renderer=%p window=%p format=%u usage=0x%x stock_render_rc=%d count=%u raw_vendor_publish=0 rate_limited=1",
                              stream, self, stock_window,
                              config_format, config_usage, rc,
                              drop_count);
            }
        }
    }

    now = obs_now_us();
    native_lock();
    slot = find_renderer_locked(self);
    if (slot && slot->generation == generation && slot->stream == stream) {
        if (!slot->first_real_frame_posted) {
            slot->first_real_frame_posted = 1;
            first_real_post = 1;
        }
        if (!slot->posts ||
            (slot->last_post_at && now > slot->last_post_at + NATIVE_STALL_SECONDS)) {
            slot->posts = 0;
            slot->first_post_at = now;
            slot->decoder_marked = 0;
        }
        ++slot->posts;
        slot->last_post_at = now;
        posts = slot->posts;
        first_post_at = slot->first_post_at;
    }
    native_unlock();

    if (first_real_post)
        altscreen_log("PHASE=NATIVE_111_FIRST_REAL_FRAME receiver=%p stream=%p renderer=%p generation=%u result=POSTED geometry=%ux%u route_gate=eligible",
                      receiver, stream, self, generation, config_width, config_height);

    memset(&snap, 0, sizeof(snap));
    if (!alt_state_snapshot(receiver, 1, &snap) ||
        snap.generation != state_generation || snap.alt_screen_stream != stream)
        return rc;
    if (posts >= NATIVE_MIN_POSTS && snap.video_config_seen &&
        now <= first_post_at + NATIVE_POST_WINDOW_SECONDS) {
        native_lock();
        slot = find_renderer_locked(self);
        if (slot && slot->generation == generation && !slot->decoder_marked) {
            slot->decoder_marked = 1;
            should_mark = 1;
        }
        native_unlock();
    }
    if (should_mark) {
        alt_state_mark_native(receiver, state_generation,
            ALT_STATE_NATIVE_DECODER_READY,
            "private111-stock-omx-three-successful-posts-stock-avcc-config-observed");
        altscreen_log("PHASE=NATIVE_111_DECODER_READY receiver=%p stream=%p renderer=%p posts=%u sps=%u pps=%u idr=%u negotiated_geometry=%ux%u fixed_geometry=0 bitstream=stock_avcc annexb_observer_optional=1",
                      receiver, stream, self, posts, snap.nal_sps, snap.nal_pps,
                      snap.nal_idr, config_width, config_height);
    }
    return rc;
}
