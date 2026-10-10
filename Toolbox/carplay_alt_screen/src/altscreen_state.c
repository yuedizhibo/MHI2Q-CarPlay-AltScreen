/*
 * altscreen_state.c - synchronized per-receiver AltScreen side table.
 *
 * The stock receiver owns one ScreenSession. Private 111 state stays in this
 * fixed side table and is protected by a compiler-atomic spin lock. No CF,
 * Screen, socket or pthread ABI call is made while the table lock is held.
 */
#include "p1404_abi.h"
#include "p1404_lock_wait.h"
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#define ALT_STATE_MAX 8
#define AS_STREAM_SLOTS 8

static struct altscreen_ctx g_ctx[ALT_STATE_MAX];
static int g_next_id = 1;
static void *g_stream[AS_STREAM_SLOTS];
static uint64_t g_stream_bytes[AS_STREAM_SLOTS];
static uint64_t g_stream_calls[AS_STREAM_SLOTS];
static volatile unsigned g_state_guard;

void alt_state_table_lock(void) {
    while (__sync_lock_test_and_set(&g_state_guard, 1u) != 0u) p1404_lock_wait_yield();
}

void alt_state_table_unlock(void) {
    __sync_lock_release(&g_state_guard);
}

struct altscreen_ctx *alt_state_lookup_any_locked(void *receiver_session) {
    int i;
    if (!receiver_session) return NULL;
    for (i = 0; i < ALT_STATE_MAX; ++i)
        if (g_ctx[i].receiver_session == receiver_session) return &g_ctx[i];
    return NULL;
}

struct altscreen_ctx *alt_state_lookup_locked(void *receiver_session) {
    struct altscreen_ctx *c = alt_state_lookup_any_locked(receiver_session);
    return c && c->state >= 2 ? c : NULL;
}

static struct altscreen_ctx *lookup_stream_locked(void *stream) {
    int i;
    if (!stream) return NULL;
    for (i = 0; i < ALT_STATE_MAX; ++i)
        if (g_ctx[i].receiver_session && g_ctx[i].process_frames_started &&
            g_ctx[i].alt_screen_stream == stream)
            return &g_ctx[i];
    return NULL;
}

static int live_count_locked(void) {
    int i, n = 0;
    for (i = 0; i < ALT_STATE_MAX; ++i) if (g_ctx[i].receiver_session) ++n;
    return n;
}

struct altscreen_ctx *alt_state_register_locked(void *receiver_session) {
    int i;
    struct altscreen_ctx *free_slot = NULL;
    if (!receiver_session) return NULL;
    for (i = 0; i < ALT_STATE_MAX; ++i) {
        if (g_ctx[i].receiver_session == receiver_session) return &g_ctx[i];
        if (!g_ctx[i].receiver_session && !free_slot) free_slot = &g_ctx[i];
    }
    if (!free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->receiver_session = receiver_session;
    free_slot->stream_type = CP_STREAM_ALT_SCREEN;
    free_slot->id = (uint32_t)g_next_id++;
    free_slot->state = 1;
    return free_slot;
}

int alt_state_live_count(void) {
    int n;
    alt_state_table_lock();
    n = live_count_locked();
    alt_state_table_unlock();
    return n;
}

void alt_state_reset(void) {
    alt_state_table_lock();
    memset(g_ctx, 0, sizeof(g_ctx));
    memset(g_stream, 0, sizeof(g_stream));
    memset(g_stream_bytes, 0, sizeof(g_stream_bytes));
    memset(g_stream_calls, 0, sizeof(g_stream_calls));
    g_next_id = 1;
    alt_state_table_unlock();
}

struct altscreen_ctx *alt_state_lookup_any(void *receiver_session) {
    struct altscreen_ctx *c;
    alt_state_table_lock();
    c = alt_state_lookup_any_locked(receiver_session);
    alt_state_table_unlock();
    return c;
}

struct altscreen_ctx *alt_state_lookup(void *receiver_session) {
    struct altscreen_ctx *c;
    alt_state_table_lock();
    c = alt_state_lookup_locked(receiver_session);
    alt_state_table_unlock();
    return c;
}

struct altscreen_ctx *alt_state_lookup_stream(void *stream) {
    struct altscreen_ctx *c;
    alt_state_table_lock();
    c = lookup_stream_locked(stream);
    alt_state_table_unlock();
    return c;
}

int alt_state_snapshot(void *receiver_session, int committed_only,
                       struct altscreen_ctx *out) {
    struct altscreen_ctx *c;
    int found = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!receiver_session || !out) return 0;
    alt_state_table_lock();
    c = committed_only ? alt_state_lookup_locked(receiver_session)
                       : alt_state_lookup_any_locked(receiver_session);
    if (c) {
        memcpy(out, c, sizeof(*out));
        found = 1;
    }
    alt_state_table_unlock();
    return found;
}

int alt_state_set_generation(void *receiver_session, uint32_t generation) {
    struct altscreen_ctx *c;
    int ok = 0;
    if (!receiver_session || !generation) return 0;
    alt_state_table_lock();
    c = alt_state_lookup_any_locked(receiver_session);
    if (c && (!c->generation || c->generation == generation)) {
        c->generation = generation;
        ok = 1;
    }
    alt_state_table_unlock();
    return ok;
}

struct altscreen_ctx *alt_state_register(void *receiver_session) {
    struct altscreen_ctx *c;
    uint32_t id = 0;
    int live = 0, is_new = 0;
    if (!receiver_session) return NULL;
    alt_state_table_lock();
    c = alt_state_lookup_any_locked(receiver_session);
    if (!c) {
        c = alt_state_register_locked(receiver_session);
        is_new = c != NULL;
    }
    if (c) { id = c->id; live = live_count_locked(); }
    alt_state_table_unlock();
    if (!c) {
        altscreen_log("ERROR STATE table full max=%d refusing_alt=1", ALT_STATE_MAX);
        return NULL;
    }
    if (is_new)
        altscreen_log("PHASE=ALT_CTX_REGISTER id=%u generation=0 session=%p live=%d stock110_untouched=1",
                      id, receiver_session, live);
    return c;
}

int alt_state_stage_private(void *receiver_session, void *alt_screen_session,
                            void *alt_screen_stream) {
    struct altscreen_ctx *c;
    uint32_t id = 0;
    int result = 0, reason = 0;
    void *old_session = NULL, *old_stream = NULL;
    if (!receiver_session || !alt_screen_session || !alt_screen_stream) {
        altscreen_log("ERROR PHASE=STREAM_111_STAGE invalid session=%p alt_session=%p alt_stream=%p",
                      receiver_session, alt_screen_session, alt_screen_stream);
        return 0;
    }
    alt_state_table_lock();
    c = alt_state_lookup_any_locked(receiver_session);
    if (!c) c = alt_state_register_locked(receiver_session);
    if (!c) reason = 1;
    else {
        id = c->id;
        old_session = c->alt_screen_session;
        old_stream = c->alt_screen_stream;
        if (c->state >= 2) {
            if (old_session == alt_screen_session && old_stream == alt_screen_stream)
                result = 1;
            else reason = 2;
        } else if ((old_session && old_session != alt_screen_session) ||
                   (old_stream && old_stream != alt_screen_stream)) {
            reason = 3;
        } else {
            c->alt_screen_session = alt_screen_session;
            c->alt_screen_stream = alt_screen_stream;
            c->state = 1;
            result = 1;
        }
    }
    alt_state_table_unlock();
    if (reason == 1) altscreen_log("ERROR STATE table full max=%d refusing_alt=1", ALT_STATE_MAX);
    else if (reason == 2)
        altscreen_log("ERROR PHASE=STREAM_111_STAGE id=%u committed_mismatch old_session=%p new_session=%p old_stream=%p new_stream=%p",
                      id, old_session, alt_screen_session, old_stream, alt_screen_stream);
    else if (reason == 3)
        altscreen_log("ERROR PHASE=STREAM_111_STAGE id=%u duplicate_mismatch old_session=%p new_session=%p old_stream=%p new_stream=%p",
                      id, old_session, alt_screen_session, old_stream, alt_screen_stream);
    else if (result)
        altscreen_log("PHASE=STREAM_111_STAGE id=%u generation=0 receiver=%p alt_session=%p alt_stream=%p committed=0 stock110_untouched=1",
                      id, receiver_session, alt_screen_session, alt_screen_stream);
    return result;
}

int alt_state_commit_private(void *receiver_session) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int ok = 0, first = 0;
    memset(&snap, 0, sizeof(snap));
    alt_state_table_lock();
    c = alt_state_lookup_any_locked(receiver_session);
    if (c && c->alt_screen_session && c->alt_screen_stream) {
        first = c->state < 2;
        c->state = 2;
        c->process_frames_started = 1;
        memcpy(&snap, c, sizeof(snap));
        ok = 1;
    }
    alt_state_table_unlock();
    if (!ok) {
        altscreen_log("ERROR PHASE=STREAM_111_COMMIT receiver=%p missing_staged_pair=1", receiver_session);
        return 0;
    }
    altscreen_log("PHASE=STREAM_111_COMMIT id=%u generation=%u receiver=%p alt_session=%p alt_stream=%p final_response_merged=1 processFrames=STARTED legacy=1 stock110_untouched=1 already_committed=%d",
                  snap.id, snap.generation, receiver_session, snap.alt_screen_session,
                  snap.alt_screen_stream, first ? 0 : 1);
    if (first) altscreen_mark_stream_setup(CP_STREAM_ALT_SCREEN);
    return 1;
}

int alt_state_bind_private(void *receiver_session, void *alt_screen_session,
                           void *alt_screen_stream) {
    if (!alt_state_stage_private(receiver_session, alt_screen_session, alt_screen_stream)) return 0;
    return alt_state_commit_private(receiver_session);
}

static int ctx_live_locked(struct altscreen_ctx *want, struct altscreen_ctx **actual) {
    int i;
    if (actual) *actual = NULL;
    if (!want) return 0;
    for (i = 0; i < ALT_STATE_MAX; ++i) {
        if (&g_ctx[i] == want && g_ctx[i].receiver_session) {
            if (actual) *actual = &g_ctx[i];
            return 1;
        }
    }
    return 0;
}

void alt_state_mark_video_config(struct altscreen_ctx *want, const char *proof) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int first = 0;
    alt_state_table_lock();
    if (ctx_live_locked(want, &c) && c->state >= 2) {
        first = !c->video_config_seen;
        c->video_config_seen = 1;
        if (c->state < 3) c->state = 3;
        memcpy(&snap, c, sizeof(snap));
    }
    alt_state_table_unlock();
    if (!first) return;
    altscreen_log("PHASE=VIDEO_111_CONFIG_CTX id=%u generation=%u stream=%p proof=%s",
                  snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
    altscreen_mark_video_config(proof);
}

void alt_state_mark_ui_active(struct altscreen_ctx *want, const char *proof) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int first = 0;
    alt_state_table_lock();
    if (ctx_live_locked(want, &c) && c->state >= 2) {
        first = !c->ui_active;
        c->ui_active = 1;
        memcpy(&snap, c, sizeof(snap));
    }
    alt_state_table_unlock();
    if (!first) return;
    altscreen_log("PHASE=ALT_UI_CTX_ACTIVE id=%u generation=%u proof=%s",
                  snap.id, snap.generation, proof ? proof : "-");
    altscreen_mark_ui_active(proof);
}

void alt_state_mark_decoder_ready(struct altscreen_ctx *want, const char *proof) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int first = 0;
    alt_state_table_lock();
    if (ctx_live_locked(want, &c) && c->state >= 2) {
        first = !c->decoder_ready;
        c->decoder_ready = 1;
        if (c->state < 5) c->state = 5;
        memcpy(&snap, c, sizeof(snap));
    }
    alt_state_table_unlock();
    if (!first) return;
    altscreen_log("PHASE=DECODER_111_CTX_READY id=%u generation=%u stream=%p proof=%s",
                  snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
    altscreen_mark_decoder_ready(proof);
}

void alt_state_mark_cockpit_visible(struct altscreen_ctx *want, const char *proof) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int first = 0;
    alt_state_table_lock();
    if (ctx_live_locked(want, &c) && c->state >= 2) {
        first = !c->cockpit_visible;
        c->cockpit_visible = 1;
        if (c->state < 6) c->state = 6;
        memcpy(&snap, c, sizeof(snap));
    }
    alt_state_table_unlock();
    if (!first) return;
    altscreen_log("PHASE=COCKPIT_111_CTX_VISIBLE id=%u generation=%u stream=%p proof=%s",
                  snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
    altscreen_mark_cockpit_visible(proof);
}

void alt_state_mark_cockpit_hidden(struct altscreen_ctx *want, const char *proof) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int changed = 0;
    alt_state_table_lock();
    if (ctx_live_locked(want, &c) && c->state >= 2) {
        changed = c->cockpit_visible || c->ui_active;
        c->cockpit_visible = 0;
        c->ui_active = 0;
        if (c->state > 5) c->state = 5;
        memcpy(&snap, c, sizeof(snap));
    }
    alt_state_table_unlock();
    if (!changed) return;
    altscreen_log("PHASE=COCKPIT_111_CTX_HIDDEN id=%u generation=%u stream=%p proof=%s",
                  snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
    altscreen_mark_cockpit_hidden(proof);
}

/* Generation-addressed update for asynchronous native workers. A raw pointer
 * returned by alt_state_lookup() can refer to a recycled table slot after the
 * lookup lock is released; matching receiver+generation under the table lock
 * prevents a late callback from mutating a newer CarPlay session. */
void alt_state_mark_native(void *receiver_session, uint32_t generation,
                           int event_kind, const char *proof) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int changed = 0;
    memset(&snap, 0, sizeof(snap));
    if (!receiver_session || !generation) return;
    alt_state_table_lock();
    c = alt_state_lookup_locked(receiver_session);
    if (c && c->generation == generation && c->state >= 2) {
        if (event_kind == ALT_STATE_NATIVE_VIDEO_CONFIG) {
            changed = !c->video_config_seen;
            c->video_config_seen = 1;
            if (c->state < 3) c->state = 3;
        } else if (event_kind == ALT_STATE_NATIVE_UI_ACTIVE) {
            changed = !c->ui_active;
            c->ui_active = 1;
        } else if (event_kind == ALT_STATE_NATIVE_DECODER_READY) {
            changed = !c->decoder_ready;
            c->decoder_ready = 1;
            if (c->state < 5) c->state = 5;
        } else if (event_kind == ALT_STATE_NATIVE_COCKPIT_VISIBLE) {
            changed = !c->cockpit_visible;
            c->cockpit_visible = 1;
            if (c->state < 6) c->state = 6;
        } else if (event_kind == ALT_STATE_NATIVE_COCKPIT_HIDDEN) {
            changed = c->cockpit_visible || c->ui_active;
            c->cockpit_visible = 0;
            c->ui_active = 0;
            if (c->state > 5) c->state = 5;
        }
        if (changed) memcpy(&snap, c, sizeof(snap));
    }
    alt_state_table_unlock();
    if (!changed) return;
    if (event_kind == ALT_STATE_NATIVE_VIDEO_CONFIG) {
        altscreen_log("PHASE=VIDEO_111_CONFIG_CTX id=%u generation=%u stream=%p proof=%s",
                      snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
        altscreen_mark_video_config(proof);
    } else if (event_kind == ALT_STATE_NATIVE_UI_ACTIVE) {
        altscreen_log("PHASE=ALT_UI_CTX_ACTIVE id=%u generation=%u proof=%s",
                      snap.id, snap.generation, proof ? proof : "-");
        altscreen_mark_ui_active(proof);
    } else if (event_kind == ALT_STATE_NATIVE_DECODER_READY) {
        altscreen_log("PHASE=DECODER_111_CTX_READY id=%u generation=%u stream=%p proof=%s",
                      snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
        altscreen_mark_decoder_ready(proof);
    } else if (event_kind == ALT_STATE_NATIVE_COCKPIT_VISIBLE) {
        altscreen_log("PHASE=COCKPIT_111_CTX_VISIBLE id=%u generation=%u stream=%p proof=%s",
                      snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
        altscreen_mark_cockpit_visible(proof);
    } else if (event_kind == ALT_STATE_NATIVE_COCKPIT_HIDDEN) {
        altscreen_log("PHASE=COCKPIT_111_CTX_HIDDEN id=%u generation=%u stream=%p proof=%s",
                      snap.id, snap.generation, snap.alt_screen_stream, proof ? proof : "-");
        altscreen_mark_cockpit_hidden(proof);
    }
}

static void forget_stream_locked(void *stream) {
    int i;
    for (i = 0; i < AS_STREAM_SLOTS; ++i) {
        if (g_stream[i] != stream) continue;
        g_stream[i] = NULL;
        g_stream_bytes[i] = 0;
        g_stream_calls[i] = 0;
        return;
    }
}

void alt_state_unregister(void *receiver_session) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int found = 0;
    memset(&snap, 0, sizeof(snap));
    alt_state_table_lock();
    c = alt_state_lookup_any_locked(receiver_session);
    if (c) {
        memcpy(&snap, c, sizeof(snap));
        if (c->alt_screen_stream) forget_stream_locked(c->alt_screen_stream);
        memset(c, 0, sizeof(*c));
        found = 1;
    }
    alt_state_table_unlock();
    if (!found) return;
    altscreen_log("PHASE=ALT_CTX_RELEASE id=%u generation=%u receiver=%p alt_session=%p alt_stream=%p state=%d bytes=%llu packets=%llu sps=%u pps=%u idr=%u decoder=%d cockpit=%d",
                  snap.id, snap.generation, receiver_session, snap.alt_screen_session,
                  snap.alt_screen_stream, snap.state,
                  (unsigned long long)snap.video_bytes,
                  (unsigned long long)snap.video_packets,
                  snap.nal_sps, snap.nal_pps, snap.nal_idr,
                  snap.decoder_ready, snap.cockpit_visible);
}

static uint8_t virtual_byte(const struct altscreen_ctx *c, const uint8_t *data,
                            size_t bytes, size_t at) {
    if (at < c->nal_carry_len) return c->nal_carry[at];
    at -= c->nal_carry_len;
    return at < bytes ? data[at] : 0;
}

static void save_nal_suffix(struct altscreen_ctx *c, const uint8_t *data,
                            size_t bytes, size_t total) {
    uint8_t tail[4];
    size_t take = total < 4u ? total : 4u;
    size_t i, keep = 0;
    for (i = 0; i < take; ++i)
        tail[i] = virtual_byte(c, data, bytes, total - take + i);
    if (take >= 4 && tail[take - 4] == 0 && tail[take - 3] == 0 &&
        tail[take - 2] == 0 && tail[take - 1] == 1) keep = 4;
    else if (take >= 3 && tail[take - 3] == 0 && tail[take - 2] == 0 &&
             tail[take - 1] == 1) keep = 3;
    else {
        for (i = take; i > 0 && tail[i - 1] == 0 && keep < 3u; --i) ++keep;
    }
    c->nal_carry_len = (uint8_t)keep;
    for (i = 0; i < keep; ++i) c->nal_carry[i] = tail[take - keep + i];
}

static unsigned scan_nals_locked(struct altscreen_ctx *c, const uint8_t *data,
                                 size_t bytes, unsigned *first_sps,
                                 unsigned *first_pps, unsigned *first_idr) {
    size_t i, total = (size_t)c->nal_carry_len + bytes;
    unsigned seen = 0;
    for (i = 0; i + 3u < total; ++i) {
        size_t header = 0;
        uint8_t nal;
        if (i + 4u < total && virtual_byte(c, data, bytes, i) == 0 &&
            virtual_byte(c, data, bytes, i + 1u) == 0 &&
            virtual_byte(c, data, bytes, i + 2u) == 0 &&
            virtual_byte(c, data, bytes, i + 3u) == 1) header = i + 4u;
        else if (virtual_byte(c, data, bytes, i) == 0 &&
                 virtual_byte(c, data, bytes, i + 1u) == 0 &&
                 virtual_byte(c, data, bytes, i + 2u) == 1) header = i + 3u;
        if (!header || header >= total) continue;
        nal = (uint8_t)(virtual_byte(c, data, bytes, header) & 0x1fu);
        if (nal == 7) { if (c->nal_sps++ == 0) *first_sps = 1; }
        else if (nal == 8) { if (c->nal_pps++ == 0) *first_pps = 1; }
        else if (nal == 5) { if (c->nal_idr++ == 0) *first_idr = 1; }
        else if (nal != 0) ++c->nal_other;
        if (nal) ++seen;
        i = header;
    }
    save_nal_suffix(c, data, bytes, total);
    return seen;
}

void alt_state_count_video(struct altscreen_ctx *want, size_t bytes, int h264_nal_present) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    int log_progress = 0, accepted = 0;
    if (!want || !bytes) return;
    alt_state_table_lock();
    if (ctx_live_locked(want, &c) && c->process_frames_started) {
        c->video_bytes += bytes;
        c->video_packets++;
        if (c->state < 4) c->state = 4;
        log_progress = c->video_packets == 1 || ((c->video_packets & 255u) == 0);
        memcpy(&snap, c, sizeof(snap));
        accepted = 1;
    }
    alt_state_table_unlock();
    if (!accepted) return;
    if (log_progress)
        altscreen_log("PHASE=VIDEO_111_PROGRESS id=%u generation=%u stream=%p ALT_VIDEO_BYTES=%llu ALT_VIDEO_PACKETS=%llu nal_present=%d last=%u",
                      snap.id, snap.generation, snap.alt_screen_stream,
                      (unsigned long long)snap.video_bytes,
                      (unsigned long long)snap.video_packets, h264_nal_present,
                      (unsigned)bytes);
    altscreen_mark_video(CP_STREAM_ALT_SCREEN, bytes);
}

int alt_state_feed_private_video(void *stream, const void *data, size_t bytes) {
    struct altscreen_ctx *c;
    struct altscreen_ctx snap;
    unsigned first_sps = 0, first_pps = 0, first_idr = 0, nals = 0;
    int first_payload = 0, progress = 0;
    if (!stream || !data || !bytes) return 0;
    alt_state_table_lock();
    c = lookup_stream_locked(stream);
    if (!c) {
        alt_state_table_unlock();
        return 0;
    }
    first_payload = c->video_packets == 0;
    nals = scan_nals_locked(c, (const uint8_t *)data, bytes,
                            &first_sps, &first_pps, &first_idr);
    c->video_bytes += bytes;
    c->video_packets++;
    if (c->state < 4) c->state = 4;
    progress = c->video_packets == 1 || ((c->video_packets & 255u) == 0);
    memcpy(&snap, c, sizeof(snap));
    alt_state_table_unlock();

    if (first_payload)
        altscreen_log("PHASE=VIDEO_111_FIRST_PAYLOAD receiver=%p id=%u generation=%u session=%p stream=%p bytes=%u annexb_nals=%u",
                      snap.receiver_session, snap.id, snap.generation,
                      snap.alt_screen_session, stream, (unsigned)bytes, nals);
    if (first_sps) altscreen_log("PHASE=VIDEO_111_FIRST_SPS id=%u generation=%u receiver=%p session=%p stream=%p",
                                 snap.id, snap.generation, snap.receiver_session,
                                 snap.alt_screen_session, stream);
    if (first_pps) altscreen_log("PHASE=VIDEO_111_FIRST_PPS id=%u generation=%u receiver=%p session=%p stream=%p",
                                 snap.id, snap.generation, snap.receiver_session,
                                 snap.alt_screen_session, stream);
    if (first_idr) altscreen_log("PHASE=VIDEO_111_FIRST_IDR id=%u generation=%u receiver=%p session=%p stream=%p",
                                 snap.id, snap.generation, snap.receiver_session,
                                 snap.alt_screen_session, stream);
    if (progress)
        altscreen_log("PHASE=VIDEO_111_PROGRESS id=%u generation=%u receiver=%p session=%p stream=%p ALT_VIDEO_BYTES=%llu ALT_VIDEO_PACKETS=%llu annexb_nals=%u last=%u",
                      snap.id, snap.generation, snap.receiver_session,
                      snap.alt_screen_session, stream,
                      (unsigned long long)snap.video_bytes,
                      (unsigned long long)snap.video_packets, nals,
                      (unsigned)bytes);
    altscreen_mark_video(CP_STREAM_ALT_SCREEN, bytes);
    return 1;
}

int alt_state_video_total(void *stream, size_t len, uint64_t *bytes_out, uint64_t *calls_out) {
    int i, slot = -1, is_new = 0;
    uint64_t total_bytes = 0, total_calls = 0;
    if (!stream) return 0;
    alt_state_table_lock();
    for (i = 0; i < AS_STREAM_SLOTS; ++i) if (g_stream[i] == stream) { slot = i; break; }
    if (slot < 0) {
        for (i = 0; i < AS_STREAM_SLOTS; ++i) if (!g_stream[i]) { slot = i; break; }
        if (slot >= 0) { g_stream[slot] = stream; is_new = 1; }
    }
    if (slot >= 0) {
        if (len) { g_stream_bytes[slot] += len; g_stream_calls[slot]++; }
        total_bytes = g_stream_bytes[slot];
        total_calls = g_stream_calls[slot];
    }
    alt_state_table_unlock();
    if (slot < 0) {
        altscreen_log("VIDEOSTREAM table full stream=%p dropped=1", stream);
        return 0;
    }
    if (is_new) altscreen_log("VIDEOSTREAM new slot=%d stream=%p", slot, stream);
    if (bytes_out) *bytes_out = total_bytes;
    if (calls_out) *calls_out = total_calls;
    return slot + 1;
}

void alt_state_forget_stream(void *stream) {
    int i, found = 0;
    uint64_t bytes = 0, calls = 0;
    alt_state_table_lock();
    for (i = 0; i < AS_STREAM_SLOTS; ++i) {
        if (g_stream[i] != stream) continue;
        bytes = g_stream_bytes[i]; calls = g_stream_calls[i];
        forget_stream_locked(stream); found = 1; break;
    }
    alt_state_table_unlock();
    if (found)
        altscreen_log("VIDEOSTREAM drop slot=%d stream=%p bytes=%llu calls=%llu", i, stream,
                      (unsigned long long)bytes, (unsigned long long)calls);
}

int alt_state_streams_live(void) {
    int i, n = 0;
    alt_state_table_lock();
    for (i = 0; i < AS_STREAM_SLOTS; ++i) if (g_stream[i]) ++n;
    alt_state_table_unlock();
    return n;
}
