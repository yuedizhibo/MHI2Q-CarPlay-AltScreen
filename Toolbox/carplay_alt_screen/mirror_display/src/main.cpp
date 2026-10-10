#include "private111_direct_source.h"
#include "cluster_video_display.h"
#include "startup_logo.h"
#include "state_snapshot.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static StateSnapshotPublisher g_state_publisher;
static const unsigned kNoFramePollUs = 5000u;
/* Diagnostic thresholds only: presentation/polling behavior is unchanged. */
static const unsigned kDecodedStallReportUs = 500000u;
static const unsigned kDecodedStallHeartbeatPolls = 1000u;
static const char kBuildId[] =
    "carplay-private111-direct-display-v3.1-oem-map-1to1-clip";

static const char *volatile_path(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return (v && *v) ? v : fallback;
}

static const char *ready_path() {
    return volatile_path("ALT111_MIRROR_READY_FILE",
                         "/tmp/altscreen_mirror.ready");
}

static const char *base_ready_path() {
    return volatile_path("ALT111_MIRROR_BASE_READY_FILE",
                         "/tmp/mmi-altscreen-basevideo.ready");
}

static const char *gate_token_path() {
    return volatile_path("ALT111_MIRROR_GATE_TOKEN_FILE",
                         "/tmp/altscreen_mirror.phone111.gate");
}

static const char *hook_log_path() {
    return volatile_path("ALT111_MIRROR_HOOK_LOG",
                         "/tmp/altscreen_hook.log");
}

static const char *displayable_state_path() {
    return volatile_path("ALT111_DISPLAYABLE_STATE_FILE",
                         "/tmp/mmi-altscreen-displayable3.state");
}

static unsigned long long now_us() {
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) return 0;
    return (unsigned long long)(unsigned long)tv.tv_sec * 1000000ULL +
           (unsigned long long)(unsigned long)tv.tv_usec;
}

/* A clock correction during boot must not stretch a 4.57 second animation. */
static unsigned long long monotonic_us() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000000ULL +
           (unsigned long long)ts.tv_nsec / 1000ULL;
}

static void wait_for_logo_time(unsigned long long due_us) {
    while (!g_stop) {
        const unsigned long long now = monotonic_us();
        if (!now || now >= due_us) return;
        unsigned long long remaining = due_us - now;
        if (remaining > 10000ULL) remaining = 10000ULL;
        usleep((unsigned)remaining);
    }
}

static void sanitize_state_value(char *value) {
    if (!value) return;
    for (; *value; ++value) {
        if (*value == '\n' || *value == '\r' || *value == '=')
            *value = ' ';
    }
}

/*
 * COLD_START_DISPLAYABLE3_OBSERVER_V1
 *
 * A tiny /tmp-only snapshot refreshed at 2 Hz.  This is intentionally
 * independent from the removed SD ring/per-frame telemetry system.
 * It reads only the already-created displayable3 native window and never
 * creates a Screen context, enumerates windows, or writes Screen properties.
 */
static void publish_displayable_state(const ClusterVideoDisplay &display,
                                      const Private111DirectSource *source,
                                      const char *phase) {
    Mhi2qWindowState state;
    memset(&state, 0, sizeof(state));
    (void)display.sample_window_state(&state);

    char manager[sizeof(state.manager)];
    strncpy(manager, state.manager, sizeof(manager) - 1u);
    manager[sizeof(manager) - 1u] = 0;
    sanitize_state_value(manager);

    const unsigned long long ts_us = now_us();
    const unsigned long long ts_ms = ts_us / 1000ULL;
    const uint32_t generation = source ? source->generation() : 0u;
    const uint32_t h264_packets = source ? source->h264_packets() : 0u;
    const uint32_t decoded_frames = source ? source->decoded_frames() : 0u;
    const uint32_t sequence = source ? source->sequence() : 0u;

    const char *path = displayable_state_path();
    char snapshot[2048];
    const int snapshot_n = snprintf(snapshot, sizeof(snapshot),
                    "schema=1\n"
                    "observer=DISPLAYABLE3_OWNERSHIP_V1\n"
                    "display_observer_revision=V32_READABLE_STATE_V1\n"
                    "mode=OBSERVE_ONLY\n"
                    "timestamp_ms=%llu\n"
                    "phase=%s\n"
                    "backend_ready=%d\n"
                    "native_window_present=%d\n"
                    "native_window=0x%lx\n"
                    "kd_window=%d\n"
                    "displayable=%d\n"
                    "visible_valid=%d\n"
                    "visible=%d\n"
                    "manager_valid=%d\n"
                    "manager=%s\n"
                    "first_present=%d\n"
                    "presented_frames=%lu\n"
                    "generation=%u\n"
                    "sequence=%u\n"
                    "h264_packets=%u\n"
                    "decoded_frames=%u\n",
                    ts_ms,
                    phase ? phase : "periodic",
                    state.backend_ready ? 1 : 0,
                    state.native_window_present ? 1 : 0,
                    state.native_window_value,
                    state.kd_window,
                    state.displayable_id,
                    state.visible_valid ? 1 : 0,
                    state.visible,
                    state.manager_valid ? 1 : 0,
                    manager,
                    display.first_frame_presented() ? 1 : 0,
                    display.frame_count(),
                    (unsigned)generation,
                    (unsigned)sequence,
                    (unsigned)h264_packets,
                    (unsigned)decoded_frames);
    const bool snapshot_valid = snapshot_n > 0 &&
                                (size_t)snapshot_n < sizeof(snapshot);
    (void)g_state_publisher.publish(path, snapshot_valid ? snapshot : 0,
                            snapshot_valid ? (size_t)snapshot_n : 0u,
                            monotonic_us() / 1000ULL);

    static int have_previous = 0;
    static int last_backend_ready = -1;
    static int last_native_present = -1;
    static int last_visible_valid = -1;
    static int last_visible = -999;
    static int last_manager_valid = -1;
    static unsigned long last_native_window = 0;
    static int last_kd_window = -999;
    static char last_manager[96] = "";
    static unsigned long long last_heartbeat_us = 0;

    const int changed =
        !have_previous ||
        last_backend_ready != (state.backend_ready ? 1 : 0) ||
        last_native_present != (state.native_window_present ? 1 : 0) ||
        last_visible_valid != (state.visible_valid ? 1 : 0) ||
        last_visible != state.visible ||
        last_manager_valid != (state.manager_valid ? 1 : 0) ||
        last_native_window != state.native_window_value ||
        last_kd_window != state.kd_window ||
        strcmp(last_manager, manager) != 0;
    const int heartbeat =
        !last_heartbeat_us ||
        (ts_us >= last_heartbeat_us &&
         ts_us - last_heartbeat_us >= 10000000ULL);

    if (changed || heartbeat) {
        fprintf(stderr,
                "direct111: PHASE=DISPLAYABLE3_OWNERSHIP ts_ms=%llu "
                "reason=%s backend_ready=%d native_present=%d "
                "native=0x%lx kd=%d displayable=%d "
                "visible_valid=%d visible=%d manager_valid=%d manager='%s' "
                "first_present=%d presented=%lu gen=%u seq=%u "
                "h264_packets=%u decoded_frames=%u observe_only=1\n",
                ts_ms, changed ? "change" : "heartbeat",
                state.backend_ready ? 1 : 0,
                state.native_window_present ? 1 : 0,
                state.native_window_value,
                state.kd_window,
                state.displayable_id,
                state.visible_valid ? 1 : 0,
                state.visible,
                state.manager_valid ? 1 : 0,
                manager,
                display.first_frame_presented() ? 1 : 0,
                display.frame_count(),
                (unsigned)generation,
                (unsigned)sequence,
                (unsigned)h264_packets,
                (unsigned)decoded_frames);
        last_heartbeat_us = ts_us;
    }

    have_previous = 1;
    last_backend_ready = state.backend_ready ? 1 : 0;
    last_native_present = state.native_window_present ? 1 : 0;
    last_visible_valid = state.visible_valid ? 1 : 0;
    last_visible = state.visible;
    last_manager_valid = state.manager_valid ? 1 : 0;
    last_native_window = state.native_window_value;
    last_kd_window = state.kd_window;
    strncpy(last_manager, manager, sizeof(last_manager) - 1u);
    last_manager[sizeof(last_manager) - 1u] = 0;
}

static bool env_truth(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return false;
    return strcmp(v, "0") != 0 &&
           strcmp(v, "NO") != 0 && strcmp(v, "no") != 0 &&
           strcmp(v, "false") != 0 && strcmp(v, "FALSE") != 0;
}

static void on_signal(int) {
    g_stop = 1;
}

static void strip_eol(char *s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n &&
           (s[n - 1] == '\n' || s[n - 1] == '\r' ||
            s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static void copy_line(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, cap - 1u);
    dst[cap - 1u] = 0;
    strip_eol(dst);
}

struct OemMapPlacement {
    int dx;
    int dy;
    char view[16];
    char layout[128];
    bool state_complete;
    bool recognized;
};

static bool state_kv_text(const char *line, const char *key,
                          char *out, size_t cap) {
    const size_t n = key ? strlen(key) : 0u;
    if (!line || !key || !n || !out || !cap) return false;
    if (strncmp(line, key, n) != 0 || line[n] != '=') return false;
    copy_line(out, cap, line + n + 1u);
    return true;
}

static bool state_kv_int(const char *line, const char *key, int *out) {
    const size_t n = key ? strlen(key) : 0u;
    char *end = 0;
    long v;
    if (!line || !key || !n || !out) return false;
    if (strncmp(line, key, n) != 0 || line[n] != '=') return false;
    v = strtol(line + n + 1u, &end, 10);
    if (end == line + n + 1u || v < -8192L || v > 8192L) return false;
    *out = (int)v;
    return true;
}

/*
 * Live OEM map-plane placement.
 *
 * The stock B9Sport SMALL stage translates map planes 33/58 by (-476,0).
 * V3.1 keeps the decoded 1440x542 map canvas at 1:1 scale and projects it into
 * the 1440x455 displayable3 viewport.  The extra 87 vertical pixels are clipped
 * naturally by GLES instead of compressing 542 rows into 455 rows.
 *
 * CarPlay safeArea remains OEM map/source-local and moves with the full map
 * plane. For Sport SMALL, source-safe x=490 plus renderer dx=-476 yields
 * physical x=14; do not compensate the safeArea back toward the center.
 *
 * The OEM terminal-space Y=26 is map-plane placement metadata.  displayable3
 * already represents that map plane, so Y=26 must not be applied again inside
 * this renderer and is not interpreted as a source crop.
 */
static OemMapPlacement load_session_map_placement() {
    OemMapPlacement p;
    memset(&p, 0, sizeof(p));
    copy_line(p.view, sizeof(p.view), "UNKNOWN");
    copy_line(p.layout, sizeof(p.layout), "UNKNOWN");

    FILE *f = fopen("/tmp/mmi-mirror-hmi.state", "r");
    if (!f) return p;

    char line[256];
    int small_dx = 0, small_dy = 0;
    bool have_view = false, have_layout = false;
    bool have_dx = false, have_dy = false;

    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (state_kv_text(line, "view", p.view, sizeof(p.view)))
            have_view = true;
        else if (state_kv_text(line, "layout_name", p.layout, sizeof(p.layout)))
            have_layout = true;
        else if (state_kv_int(line, "small_stage_dx", &small_dx))
            have_dx = true;
        else if (state_kv_int(line, "small_stage_dy", &small_dy))
            have_dy = true;
    }
    fclose(f);

    p.state_complete = have_view && have_layout;
    if (!p.state_complete)
        return p;
    if (!strstr(p.layout, "LayoutMIB2HighB9"))
        return p;

    if (strcmp(p.view, "FULL") == 0) {
        p.dx = 0;
        p.dy = 0;
        p.recognized = true;
        return p;
    }

    if (strcmp(p.view, "SMALL") != 0)
        return p;

    if (strstr(p.layout, "LayoutMIB2HighB9Sport")) {
        if (!have_dx) small_dx = -476;
        if (!have_dy) small_dy = 0;
    }

    /*
     * Require at least partial overlap with the 1440x455 viewport.  This keeps
     * a corrupt HMI state from moving the whole surface off-screen.
     */
    if (small_dx <= -1440 || small_dx >= 1440 ||
        small_dy <= -455 || small_dy >= 455) {
        fprintf(stderr,
                "direct111: WARN PHASE=OEM_MAP_PLACEMENT_INVALID "
                "view=%s layout=%s dx=%d dy=%d fallback=fullscreen\n",
                p.view, p.layout, small_dx, small_dy);
        return p;
    }

    p.dx = small_dx;
    p.dy = small_dy;
    p.recognized = true;
    return p;
}

static bool same_map_placement(const OemMapPlacement &a,
                               const OemMapPlacement &b) {
    return a.dx == b.dx && a.dy == b.dy &&
           a.recognized == b.recognized &&
           strcmp(a.view, b.view) == 0 &&
           strcmp(a.layout, b.layout) == 0;
}

static bool reconcile_oem_map_placement(ClusterVideoDisplay &display,
                                        const char *reason,
                                        bool redraw_last_frame) {
    static bool have_previous = false;
    static bool transient_gap_reported = false;
    static OemMapPlacement previous;

    const OemMapPlacement p = load_session_map_placement();

    /*
     * Java replaces /tmp/mmi-mirror-hmi.state through a temp file.  There is a
     * very small delete/rename (or copy fallback) interval in which the reader
     * can observe no file or an incomplete file.  Once a valid placement has
     * been applied, keep it through that transient gap instead of flashing back
     * to fullscreen for one 50 ms poll.
     */
    if (have_previous && !p.state_complete) {
        if (!transient_gap_reported) {
            fprintf(stderr,
                    "direct111: PHASE=OEM_MAP_PLACEMENT_STATE_GAP "
                    "reason=%s action=retain_previous "
                    "previous_view=%s previous_layout=%s "
                    "renderer_offset=%d,%d\n",
                    reason ? reason : "poll",
                    previous.view, previous.layout,
                    previous.dx, previous.dy);
            transient_gap_reported = true;
        }
        return false;
    }
    if (p.state_complete)
        transient_gap_reported = false;

    if (have_previous && same_map_placement(previous, p))
        return false;

    bool applied = false;
    if (!p.recognized) {
        (void)display.set_destination_rect(0, 0, 1440, 542);
        fprintf(stderr,
                "direct111: PHASE=OEM_MAP_PLACEMENT "
                "reason=%s mode=fallback-map-plane renderer_offset=0,0 "
                "source_canvas=1440x542 sink_viewport=1440x455 "
                "destination_size=1440x542 geometry_policy=OEM_MAP_PLANE_1TO1_CLIP_V31 "
                "renderer_scale=0 renderer_scale_y=1.000 natural_clip=1 "
                "clip_bottom=87 live_switch=1\n",
                reason ? reason : "poll");
        applied = true;
    } else if (display.set_destination_rect(p.dx, p.dy, 1440, 542)) {
        fprintf(stderr,
                "direct111: PHASE=OEM_MAP_PLACEMENT "
                "reason=%s mode=%s layout=%s renderer_offset=%d,%d "
                "source_canvas=1440x542 sink_viewport=1440x455 "
                "destination_size=1440x542 geometry_policy=OEM_MAP_PLANE_1TO1_CLIP_V31 "
                "renderer_scale=0 renderer_scale_y=1.000 natural_clip=1 "
                "clip_bottom=87 live_switch=1\n",
                reason ? reason : "poll", p.view, p.layout, p.dx, p.dy);
        fprintf(stderr,
                "direct111: PHASE=OEM_GEOMETRY_V31 "
                "policy=ONE_TO_ONE_VIEWPORT_CLIP source_canvas=1440x542 "
                "sink_plane=1440x455 map_plane_terminal_y=26 "
                "map_plane_terminal_y_policy=metadata_only_not_renderer_offset "
                "safearea_space=MAP_LOCAL_UNSCALED renderer_offset=%d,%d\n",
                p.dx, p.dy);
        applied = true;
    } else {
        display.set_fullscreen_destination();
        fprintf(stderr,
                "direct111: WARN PHASE=OEM_MAP_PLACEMENT "
                "reason=%s mode=%s layout=%s requested_offset=%d,%d "
                "apply_failed=1 fallback=fullscreen renderer_scale=0 "
                "live_switch=1\n",
                reason ? reason : "poll", p.view, p.layout, p.dx, p.dy);
        applied = true;
    }

    previous = p;
    have_previous = true;

    /*
     * A layout change must be visible even while the decoded producer is
     * temporarily idle. Re-draw the already-uploaded texture at the new
     * destination immediately; the next fresh frame continues normally.
     */
    if (applied && redraw_last_frame && display.first_frame_presented()) {
        display.refresh();
        fprintf(stderr,
                "direct111: PHASE=OEM_MAP_RERENDER reason=%s "
                "view=%s layout=%s renderer_offset=%d,%d "
                "last_frame_redrawn=1\n",
                reason ? reason : "poll",
                p.view, p.layout, p.dx, p.dy);
    }
    return applied;
}

static void load_consumed_gate(char *out, size_t cap) {
    FILE *f;
    if (!out || !cap) return;
    out[0] = 0;
    f = fopen(gate_token_path(), "r");
    if (!f) return;
    if (fgets(out, (int)cap, f)) strip_eol(out);
    fclose(f);
}

static bool persist_gate(const char *line) {
    FILE *f;
    if (!line || !*line) return false;
    f = fopen(gate_token_path(), "w");
    if (!f) return false;
    fprintf(f, "%s\n", line);
    return fclose(f) == 0;
}

/*
 * Wait for the phone to explicitly request private type111 before attaching
 * either shared-memory source.  Unlike the retired Window58 readback path, this
 * gate does not create a Screen manager context and never enumerates windows.
 */
static bool wait_for_phone111_gate() {
    static const char kFlatHookLog[] = "/tmp/altscreen_hook.log";
    char consumed[1024];
    char candidate[1024];
    char candidate_source[48];
    char line[1024];
    bool have_hook_init = false;
    bool reported_waiting = false;
    bool reported_consumed = false;
    FILE *f = 0;
    const char *opened_path = 0;

    load_consumed_gate(consumed, sizeof(consumed));
    candidate[0] = 0;
    candidate_source[0] = 0;

    while (!g_stop) {
        if (!f) {
            const char *primary = hook_log_path();
            f = fopen(primary, "r");
            if (f) {
                opened_path = primary;
            } else if (strcmp(primary, kFlatHookLog) != 0) {
                f = fopen(kFlatHookLog, "r");
                if (f) opened_path = kFlatHookLog;
            }

            if (!f) {
                if (!reported_waiting) {
                    fprintf(stderr,
                            "direct111: PHASE=GATE_WAIT "
                            "for=PHONE_REQUEST_111 screen_context=NONE "
                            "window58_dependency=NONE hook_log=%s\n",
                            primary);
                    reported_waiting = true;
                }
                usleep(100000);
                continue;
            }

            fprintf(stderr,
                    "direct111: PHASE=GATE_LOG_ATTACHED path=%s "
                    "screen_context=NONE window58_dependency=NONE\n",
                    opened_path ? opened_path : "-");
        }

        bool read_any = false;
        while (fgets(line, sizeof(line), f)) {
            read_any = true;
            strip_eol(line);

            if (strstr(line, "PHASE=HOOK_INIT")) {
                have_hook_init = true;
                candidate[0] = 0;
                candidate_source[0] = 0;
                reported_consumed = false;
                continue;
            }

            /*
             * PHONE_REQUESTED_ALTSCREEN=YES is a process-level state marker and
             * may only be emitted once for the lifetime of dio_manager.
             * STREAM_111_REQUESTED=YES is emitted for every actual type111
             * setup request. Accept either, preferring the repeated request
             * marker as the per-session gate so normal disconnect/reconnect in
             * one dio_manager process cannot strand the sidecar.
             */
            if (have_hook_init &&
                strstr(line, "PHASE=PHONE_REQUEST_111")) {
                if (strstr(line, "STREAM_111_REQUESTED=YES")) {
                    copy_line(candidate, sizeof(candidate), line);
                    copy_line(candidate_source, sizeof(candidate_source),
                              "stream111-request");
                } else if (strstr(line, "PHONE_REQUESTED_ALTSCREEN=YES")) {
                    copy_line(candidate, sizeof(candidate), line);
                    copy_line(candidate_source, sizeof(candidate_source),
                              "phone-marker");
                }
            }
        }

        if (candidate[0]) {
            if (consumed[0] && strcmp(candidate, consumed) == 0) {
                if (!reported_consumed) {
                    fprintf(stderr,
                            "direct111: PHASE=GATE_CONSUMED "
                            "source=%s waiting_for_next_private111_session=1\n",
                            candidate_source[0] ? candidate_source : "unknown");
                    reported_consumed = true;
                }
            } else {
                if (!persist_gate(candidate)) {
                    fprintf(stderr,
                            "direct111: ERROR PHASE=GATE_TOKEN_WRITE path=%s\n",
                            gate_token_path());
                    usleep(100000);
                    continue;
                }
                if (f) {
                    fclose(f);
                    f = 0;
                }
                fprintf(stderr,
                        "direct111: PHASE=GATE_PASS trigger=PHONE_REQUEST_111 "
                        "gate_source=%s policy=stream111_request_or_phone_marker "
                        "next=H264_TAP_AND_DECODER_SHM\n",
                        candidate_source[0] ? candidate_source : "unknown");
                return true;
            }
        }

        clearerr(f);
        usleep(read_any ? 20000 : 50000);
    }

    if (f) fclose(f);
    return false;
}

static void marker(bool on, const char *source, const char *mode) {
    const char *ready = ready_path();
    const char *base = base_ready_path();

    if (!on) {
        unlink(ready);
        unlink(base);
        unlink(displayable_state_path());
        return;
    }

    FILE *f = fopen(ready, "w");
    if (f) {
        fprintf(f,
                "ready=1\n"
                "pid=%ld\n"
                "source=%s\n"
                "sink=displayable3\n"
                "context=80\n"
                "mode=%s\n"
                "window58_readback=0\n",
                (long)getpid(),
                source ? source : "private111-direct",
                mode ? mode : "direct-display");
        fclose(f);
    }

    f = fopen(base, "w");
    if (f) {
        fprintf(f,
                "ready=1\n"
                "pid=%ld\n"
                "mode=%s\n"
                "displayable=3\n"
                "window58_readback=0\n",
                (long)getpid(),
                mode ? mode : "direct-display");
        fclose(f);
    }
}

/* Java/HMI remains the sole terminal1 Context80 owner. */
static bool activate_context80() {
    fprintf(stderr,
            "direct111: PHASE=CONTEXT80_REQUEST owner=java "
            "composite=98,101,102,3 displayable=3 native_dmdt=0\n");
    return true;
}

static void restore_context80() {
    fprintf(stderr,
            "direct111: PHASE=CONTEXT80_RELEASE owner=java "
            "native_dmdt=0\n");
}

static int run_sink_grid(bool verbose) {
    Mhi2qBackendConfig cfg;
    cfg.width = 1440;
    cfg.height = 455;
    cfg.displayable_id = 3;
    cfg.verbose = verbose;

    ClusterVideoDisplay display;
    if (!display.init(cfg)) return 20;
    display.set_fullscreen_destination();
    if (!display.present_test_grid()) {
        display.shutdown();
        return 21;
    }

    fprintf(stderr,
            "direct111: PHASE=DISPLAYABLE3_FIRST_PRESENT "
            "mode=sink-test-grid displayable=3 size=1440x455 "
            "source_dependency=NONE\n");
    marker(true, "diagnostic-test-grid", "sink-test-grid");
    (void)activate_context80();

    while (!g_stop) {
        display.refresh();
        usleep(100000);
    }

    marker(false, 0, 0);
    restore_context80();
    display.shutdown();
    return 0;
}

int main(int argc, char **argv) {
    bool verbose = false;
    bool sink_test_grid = false;
    const char *grid_env = getenv("ALT111_SINK_TEST_GRID");

    if (grid_env &&
        (!strcmp(grid_env, "1") || !strcmp(grid_env, "YES") ||
         !strcmp(grid_env, "yes") || !strcmp(grid_env, "true")))
        sink_test_grid = true;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--verbose")) verbose = true;
        if (!strcmp(argv[i], "--sink-test-grid")) sink_test_grid = true;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    marker(false, 0, 0);

    fprintf(stderr,
            "direct111: PHASE=BUILD id=%s "
            "target_pipeline=private111->H264_TAP->decoder->displayable3->Context80 "
            "decoder_backend=stock-omx-tap compatibility_parallel_to_h264_tap=1 "
            "shm_attach=fstat_size_guard session_identity=writer_pid+generation+cookie "
            "window58_readback=0 screen_manage_window_sidecar=0\n",
            kBuildId);

    if (sink_test_grid)
        return run_sink_grid(verbose);

    Private111DirectSource source(verbose);
    bool source_initialized = false;
    bool recovered_current_session = false;

    if (env_truth("ALT111_RECOVER_CURRENT_SESSION")) {
        if (source.init()) {
            VideoFrame recovery_probe;
            unsigned fresh_frames = 0u;
            source_initialized = true;

            /*
             * Matching active flags alone are not enough: a producer that died
             * before clearing SHM can leave stale active=1 metadata behind.
             * Require two distinct stable decoded frames while H264+decoded SHM
             * identities still match before bypassing the consumed phone gate.
             */
            for (unsigned retry = 0; retry < 80u && !g_stop; ++retry) {
                if (!source.current_session_active()) {
                    fresh_frames = 0u;
                    usleep(50000);
                    continue;
                }

                if (source.read_frame(&recovery_probe)) {
                    ++fresh_frames;
                    if (fresh_frames >= 2u) {
                        recovered_current_session = true;
                        fprintf(stderr,
                                "direct111: PHASE=GATE_RECOVER_CURRENT_SESSION "
                                "validated=1 retries=%u fresh_decoded_frames=%u "
                                "policy=matching_identity_plus_frame_progress "
                                "consumed_phone_gate_bypass=1\n",
                                retry, fresh_frames);
                        break;
                    }
                }
                usleep(50000);
            }
        }
        if (!recovered_current_session) {
            fprintf(stderr,
                    "direct111: PHASE=GATE_RECOVER_CURRENT_SESSION "
                    "validated=0 reason=no_fresh_matching_frame_progress "
                    "action=FALLBACK_TO_PHONE_REQUEST_GATE\n");
            if (source_initialized) {
                source.shutdown();
                source_initialized = false;
            }
        }
    }

    if (!recovered_current_session) {
        if (!wait_for_phone111_gate())
            return 0;
    }

    if (!source_initialized) {
        if (!source.init()) {
            fprintf(stderr,
                    "direct111: ERROR PHASE=SOURCE_INIT result=FAILED\n");
            return 2;
        }
        source_initialized = true;
    }

    VideoFrame frame;
    bool wait_reported = false;
    unsigned long wait_loops = 0;
    unsigned startup_fresh_frames = 0u;
    uint32_t startup_writer = 0u;
    uint32_t startup_generation = 0u;
    uint32_t startup_cookie = 0u;
    fprintf(stderr,
            "direct111: PHASE=PIPELINE_WAIT "
            "waiting=H264_TAP+DECODER_FRESH_PROGRESS "
            "startup_frame_progress_required=2 "
            "source=/carplay111_decoded window58_readback=0\n");

    while (!g_stop) {
        if (source.read_frame(&frame)) {
            const uint32_t writer = source.writer_pid();
            const uint32_t gen = source.generation();
            const uint32_t cookie = source.stream_cookie();

            if (writer != startup_writer ||
                gen != startup_generation ||
                cookie != startup_cookie) {
                startup_writer = writer;
                startup_generation = gen;
                startup_cookie = cookie;
                startup_fresh_frames = 1u;
            } else {
                ++startup_fresh_frames;
            }

            if (startup_fresh_frames >= 2u) {
                fprintf(stderr,
                        "direct111: PHASE=PIPELINE_SOURCE_PRIMED "
                        "fresh_frames=%u writer_pid=%u generation=%u "
                        "cookie=0x%08x stale_single_frame_rejected=1\n",
                        startup_fresh_frames, (unsigned)startup_writer,
                        (unsigned)startup_generation,
                        (unsigned)startup_cookie);
                break;
            }
        }

        if (!wait_reported || (++wait_loops % 100u) == 0u) {
            wait_reported = true;
            fprintf(stderr,
                    "direct111: PHASE=PIPELINE_PROGRESS "
                    "h264_ready=%d h264_packets=%u h264_bytes=%u "
                    "decoder_ready=%d decoded_frames=%u fresh_frames=%u "
                    "displayable3=NOT_CREATED\n",
                    source.h264_ready() ? 1 : 0,
                    (unsigned)source.h264_packets(),
                    (unsigned)source.h264_bytes(),
                    source.decoded_ready() ? 1 : 0,
                    (unsigned)source.decoded_frames(),
                    startup_fresh_frames);
        }
        usleep(20000);
    }

    if (g_stop) {
        source.shutdown();
        return 0;
    }

    fprintf(stderr,
            "direct111: PHASE=FIRST_DECODED_FRAME "
            "backend=stock-omx-tap format=NV12 size=%dx%d stride=%d "
            "h264_ready=%d generation=%u\n",
            frame.width, frame.height, frame.stride,
            source.h264_ready() ? 1 : 0,
            (unsigned)source.generation());

    Mhi2qBackendConfig cfg;
    cfg.width = 1440;
    cfg.height = 455;
    cfg.displayable_id = 3;
    cfg.verbose = verbose;

    ClusterVideoDisplay display;
    if (!display.init(cfg)) {
        fprintf(stderr,
                "direct111: ERROR PHASE=DISPLAYABLE3_INIT result=FAILED\n");
        source.shutdown();
        return 3;
    }
    (void)reconcile_oem_map_placement(display, "startup", false);

    bool context_active = false;
    bool carplay_presented = false;
    /* The normal stream supervisor attaches to an already active Type111
     * producer, so RECOVER_CURRENT_SESSION is also set on first startup.
     * Only a same-session sidecar crash should skip the intro. */
    const bool skip_logo = !strcmp(
        volatile_path("ALT111_MIRROR_RESTART_REASON", ""),
        "sidecar_abnormal");
    unsigned long source_frames = 1;
    StartupLogo logo;
    if (!skip_logo && logo.open()) {
        fprintf(stderr,
                "direct111: PHASE=STARTUP_LOGO_BEGIN asset=EMBEDDED "
                "frames=%u fps=%u fade_ms=550 source=Logo.mp4 "
                "scale=0.70 center=720,227.5 visible_plane=1440x455\n",
                logo.frame_count(), logo.fps());
        const unsigned long long began = monotonic_us();
        bool complete = true;
        for (unsigned i = 0; i < logo.frame_count() && !g_stop; ++i) {
            wait_for_logo_time(began + (unsigned long long)i * 1000000ULL /
                                       logo.fps());
            if (g_stop) break;
            if (source.read_frame(&frame)) ++source_frames;
            if (!logo.next_frame() ||
                !display.present_logo_frame(logo.pixels(), 1440, 455)) {
                complete = false;
                fprintf(stderr,
                        "direct111: WARN PHASE=STARTUP_LOGO_FRAME_FAILED "
                        "index=%u action=SHOW_CARPLAY\n", i);
                break;
            }
            if (!context_active) {
                marker(true, "private111-decoded-shm", "direct-display");
                if (!activate_context80()) {
                    publish_displayable_state(display, &source,
                                              "ctx80-activate-failed");
                    marker(false, 0, 0);
                    display.shutdown();
                    source.shutdown();
                    return 5;
                }
                context_active = true;
                publish_displayable_state(display, &source, "startup-logo");
                fprintf(stderr,
                        "direct111: PHASE=DISPLAYABLE3_FIRST_PRESENT result=OK "
                        "displayable=3 output=1440x455 source=startup-logo\n");
            }
        }

        if (complete && !g_stop) {
            /* Hold the actual final video frame while the latest decoded
             * CarPlay frame is uploaded underneath it. */
            (void)reconcile_oem_map_placement(display, "startup-logo-fade", false);
            const unsigned long long fade_start = began +
                (unsigned long long)logo.frame_count() * 1000000ULL / logo.fps();
            for (unsigned step = 0; step <= 17u && !g_stop; ++step) {
                wait_for_logo_time(fade_start +
                                   (unsigned long long)step * 550000ULL / 17u);
                if (g_stop) break;
                if (source.read_frame(&frame)) ++source_frames;
                const float opacity = 1.0f - (float)step / 17.0f;
                if (!display.present_logo_fade_frame(frame, opacity)) {
                    fprintf(stderr,
                            "direct111: WARN PHASE=STARTUP_LOGO_FADE_FAILED "
                            "step=%u action=SHOW_CARPLAY\n", step);
                    break;
                }
                if (step == 17u) carplay_presented = true;
            }
        }
        display.release_logo();
        logo.close();
        fprintf(stderr,
                "direct111: PHASE=STARTUP_LOGO_END carplay_presented=%d\n",
                carplay_presented ? 1 : 0);
    } else {
        fprintf(stderr,
                "direct111: PHASE=STARTUP_LOGO_SKIP reason=%s asset=EMBEDDED\n",
                skip_logo ? "sidecar_abnormal_restart" : "embedded_data_invalid");
    }

    if (g_stop) {
        marker(false, 0, 0);
        if (context_active) restore_context80();
        display.shutdown();
        source.shutdown();
        return 0;
    }
    if (!carplay_presented && !display.present_frame(frame)) {
        fprintf(stderr,
                "direct111: ERROR PHASE=DISPLAYABLE3_FIRST_PRESENT "
                "result=FAILED input_format=NV12 "
                "reason=texture_upload_or_egl_swap "
                "see_EGL_SWAP_FAILED_above=1\n");
        marker(false, 0, 0);
        if (context_active) restore_context80();
        display.shutdown();
        source.shutdown();
        return 4;
    }
    if (!context_active) {
        marker(true, "private111-decoded-shm", "direct-display");
        if (!activate_context80()) {
            publish_displayable_state(display, &source, "ctx80-activate-failed");
            marker(false, 0, 0);
            display.shutdown();
            source.shutdown();
            return 5;
        }
        context_active = true;
    }
    fprintf(stderr,
            "direct111: PHASE=DISPLAYABLE3_FIRST_PRESENT result=OK "
            "displayable=3 output=1440x455 source=private111-decoded "
            "startup_logo=%d window58_readback=0\n",
            carplay_presented ? 1 : 0);

    /*
     * Keep observer I/O completely off the first-present -> Context80 critical
     * path.  Normal startup acquires the proven Java context first, then takes
     * the initial read-only displayable3 snapshot.
     */
    publish_displayable_state(display, &source, "ctx80-active");

    fprintf(stderr,
            "direct111: PHASE=DIRECT111_ACTIVE "
            "target_pipeline=private111->H264_TAP->decoder->displayable3->Context80 "
            "decoder_backend=stock-omx-tap h264_tap_independent=1 "
            "same_session_recovery=%d window58_readback=0 "
            "present_policy=source-driven no_success_sleep=1 "
            "oem_map_placement=live-hmi-state "
            "no_new_frame_poll_us=%u stall_report_after_ms=%u\n",
            recovered_current_session ? 1 : 0,
            kNoFramePollUs, kDecodedStallReportUs / 1000u);

    unsigned failures = 0;
    bool in_stall = false;
    unsigned long long stall_start_us = 0;
    unsigned long stats_presented_base = display.frame_count();
    unsigned long long stats_start = now_us();
    unsigned long long next_ownership_probe_us = now_us() + 500000ULL;
    unsigned long long next_layout_probe_us = now_us() + 50000ULL;

    while (!g_stop) {
        const unsigned long long frame_start = now_us();

        if (!next_layout_probe_us ||
            (frame_start && frame_start >= next_layout_probe_us)) {
            (void)reconcile_oem_map_placement(
                display, "hmi-state-change", true);
            next_layout_probe_us = frame_start + 50000ULL;
        }
        if (!next_ownership_probe_us ||
            (frame_start && frame_start >= next_ownership_probe_us)) {
            publish_displayable_state(display, &source, "periodic");
            next_ownership_probe_us = frame_start + 500000ULL;
        }

        if (source.read_frame(&frame)) {
            if (in_stall) {
                const unsigned long long now = now_us();
                const unsigned long long stall_ms =
                    (now && stall_start_us && now >= stall_start_us)
                        ? (now - stall_start_us) / 1000ULL : 0;
                fprintf(stderr,
                        "direct111: PHASE=DECODED_SOURCE_RECOVERED "
                        "stall_ms=%llu generation=%u decoded_frames=%u "
                        "h264_packets=%u\n",
                        stall_ms, (unsigned)source.generation(),
                        (unsigned)source.decoded_frames(),
                        (unsigned)source.h264_packets());
            }
            failures = 0;
            in_stall = false;
            stall_start_us = 0;
            ++source_frames;
            if (!display.present_frame(frame)) {
                fprintf(stderr,
                        "direct111: ERROR PHASE=DISPLAY_PRESENT "
                        "stopping_safely=1\n");
                break;
            }
        } else {
            ++failures;
            const unsigned long long idle_now = now_us();
            if (!stall_start_us)
                stall_start_us = idle_now;

            const unsigned long long idle_us =
                (idle_now && stall_start_us && idle_now >= stall_start_us)
                    ? idle_now - stall_start_us : 0;

            /*
             * Source-driven polling normally sees several "no new sequence"
             * iterations between ~30 fps producer frames.  Treat those as
             * expected idle time, not decoder stalls.
             */
            if (!in_stall && idle_us >= kDecodedStallReportUs) {
                in_stall = true;
                fprintf(stderr,
                        "direct111: PHASE=DECODED_SOURCE_STALL "
                        "freeze_last_frame=1 duration_ms=%llu failures=%u "
                        "h264_packets=%u decoded_frames=%u generation=%u "
                        "poll_us=%u report_after_ms=%u\n",
                        idle_us / 1000ULL, failures,
                        (unsigned)source.h264_packets(),
                        (unsigned)source.decoded_frames(),
                        (unsigned)source.generation(),
                        kNoFramePollUs,
                        kDecodedStallReportUs / 1000u);
            } else if (in_stall &&
                       (failures % kDecodedStallHeartbeatPolls) == 0u) {
                fprintf(stderr,
                        "direct111: PHASE=DECODED_SOURCE_STALL "
                        "freeze_last_frame=1 duration_ms=%llu failures=%u "
                        "h264_packets=%u decoded_frames=%u generation=%u\n",
                        idle_us / 1000ULL, failures,
                        (unsigned)source.h264_packets(),
                        (unsigned)source.decoded_frames(),
                        (unsigned)source.generation());
            }

            /* No fixed ~3s auto-exit: freeze the last frame and let the
             * stop script / signal own teardown. */
            usleep(kNoFramePollUs);
            continue;
        }

        const unsigned long long now = now_us();
        if (now && stats_start && now - stats_start >= 10000000ULL) {
            const unsigned long long span = now - stats_start;
            const unsigned long total_presented = display.frame_count();
            const unsigned long interval_presented =
                total_presented >= stats_presented_base
                    ? total_presented - stats_presented_base : 0;
            const unsigned long fps100 = span
                ? (unsigned long)(((unsigned long long)interval_presented *
                                   100000000ULL) / span)
                : 0;

            fprintf(stderr,
                    "direct111: PHASE=RUN generation=%u "
                    "h264_ready=%d h264_packets=%u h264_bytes=%u "
                    "decoded_frames=%u source_frames=%lu "
                    "presented_frames=%lu present_fps=%lu.%02lu "
                    "displayable=3 context=80 window58_readback=0 "
                    "present_policy=source-driven no_success_sleep=1\n",
                    (unsigned)source.generation(),
                    source.h264_ready() ? 1 : 0,
                    (unsigned)source.h264_packets(),
                    (unsigned)source.h264_bytes(),
                    (unsigned)source.decoded_frames(),
                    source_frames, total_presented,
                    fps100 / 100, fps100 % 100);

            stats_presented_base = total_presented;
            stats_start = now;
        }

        /*
         * Source-driven presentation: successful fresh frames are presented
         * immediately.  The only software delay is the short no-new-frame poll
         * above, so a second relative 33 ms limiter cannot halve the sink rate.
         */
    }

    const unsigned long presented_frames = display.frame_count();
    publish_displayable_state(display, &source, "pre-shutdown");
    marker(false, 0, 0);
    restore_context80();
    display.shutdown();
    source.shutdown();

    fprintf(stderr,
            "direct111: PHASE=STOP source_frames=%lu presented_frames=%lu "
            "window58_readback=0\n",
            source_frames, presented_frames);
    return 0;
}
