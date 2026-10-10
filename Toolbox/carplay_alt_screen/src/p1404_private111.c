/* p1404_private111.c - ABI-neutral private stream111 transaction orchestration. */
#include "p1404_private111.h"
#include "p1404_abi.h"
#include "p1404_airplay.h"
#include "altscreen_state_private.h"
#include "p1404_control_fence.c"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct alt_private111_backend g_backend;
static int g_backend_ready;

#define ALT111_PENDING_SLOTS 8u
struct alt111_pending_txn {
    void *receiver;
    const void *descriptor;
    uint32_t generation;
};
static struct alt111_pending_txn g_pending[ALT111_PENDING_SLOTS];
static volatile unsigned g_pending_guard;

static void pending_lock(void) {
    while (__sync_lock_test_and_set(&g_pending_guard, 1u) != 0u) { }
}
static void pending_unlock(void) {
    __sync_lock_release(&g_pending_guard);
}

static int pending_begin(void *receiver, const void *descriptor) {
    unsigned i;
    int free_slot = -1;
    uint32_t generation;
    if (!receiver || !descriptor) return 0;

    /* Begin first: even a table-full failure conservatively invalidates an older
     * private SETUP rather than allowing two candidate commits to overlap. */
    generation = alt_control_fence_begin();
    pending_lock();
    for (i = 0; i < ALT111_PENDING_SLOTS; ++i) {
        if (!g_pending[i].receiver && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot >= 0) {
        g_pending[free_slot].receiver = receiver;
        g_pending[free_slot].descriptor = descriptor;
        g_pending[free_slot].generation = generation;
    }
    pending_unlock();

    if (free_slot < 0) {
        altscreen_log("ERROR PHASE=STREAM_111_FENCE_BEGIN receiver=%p descriptor=%p generation=%u pending_table_full=1 private_blocked=1",
                      receiver, descriptor, generation);
        return 0;
    }
    altscreen_log("PHASE=STREAM_111_FENCE_BEGIN receiver=%p descriptor=%p generation=%u slot=%d",
                  receiver, descriptor, generation, free_slot);
    return 1;
}

static uint32_t pending_take(void *receiver, const void *descriptor) {
    unsigned i;
    int best = -1;
    uint32_t generation = 0;
    pending_lock();
    for (i = 0; i < ALT111_PENDING_SLOTS; ++i) {
        if (g_pending[i].receiver != receiver || g_pending[i].descriptor != descriptor)
            continue;
        if (best < 0 || g_pending[i].generation < g_pending[(unsigned)best].generation)
            best = (int)i;
    }
    if (best >= 0) {
        generation = g_pending[(unsigned)best].generation;
        memset(&g_pending[(unsigned)best], 0, sizeof(g_pending[(unsigned)best]));
    }
    pending_unlock();
    return generation;
}

static void pending_cancel_receiver(void *receiver) {
    unsigned i;
    pending_lock();
    for (i = 0; i < ALT111_PENDING_SLOTS; ++i)
        if (g_pending[i].receiver == receiver)
            memset(&g_pending[i], 0, sizeof(g_pending[i]));
    pending_unlock();
}

static void pending_reset(void) {
    memset(g_pending, 0, sizeof(g_pending));
    g_pending_guard = 0u;
}

static void cleanup_partial(void *receiver, void *alt_session, void *alt_stream,
                            void *private_response, const char *why) {
    int td = 1;
    if (private_response && g_backend.release_object) g_backend.release_object(private_response);
    if ((alt_session || alt_stream) && g_backend.teardown_private)
        td = g_backend.teardown_private(receiver, alt_session, alt_stream, NULL);
    altscreen_log("PHASE=STREAM_111_ABORT receiver=%p alt_session=%p alt_stream=%p teardown_ok=%d reason=%s stock110_untouched=1 state_mapping=%s",
                  receiver, alt_session, alt_stream, td, why ? why : "-",
                  td ? "cleared" : "retained_for_safe_retry");
    /* A failed join/stop means the private worker or ScreenSession may still
     * reference receiver state. Dropping the mapping would make the next stock
     * teardown free its owner with no way to retry private cleanup. */
    if (td) alt_state_unregister(receiver);
}

int alt_private111_install_backend(const struct alt_private111_backend *backend) {
    int shape_ok = 0;
    memset(&g_backend, 0, sizeof(g_backend));
    g_backend_ready = 0;
    if (backend) {
        if (backend->two_phase)
            shape_ok = backend->prepare_setup && backend->start_accept_worker;
        else
            shape_ok = backend->create_setup_start != NULL;
    }
    if (!backend || !backend->abi_verified || !backend->name || !*backend->name ||
        !backend->release_object || !shape_ok ||
        !backend->make_existing_response || !backend->merge_private_response ||
        !backend->teardown_private ||
        (backend->stock_requires_split && !backend->make_stock_request)) {
        altscreen_log("ERROR PHASE=PRIVATE111_BACKEND rejected abi_verified=%d split=%d two_phase=%d shape_ok=%d name=%s",
                      backend ? backend->abi_verified : 0,
                      backend ? backend->stock_requires_split : 0,
                      backend ? backend->two_phase : 0,
                      shape_ok,
                      (backend && backend->name) ? backend->name : "-");
        return 0;
    }
    g_backend = *backend;
    g_backend_ready = 1;
    altscreen_log("PHASE=PRIVATE111_BACKEND_READY name=%s stock_requires_split=%d two_phase=%d abi_verified=1",
                  g_backend.name, g_backend.stock_requires_split, g_backend.two_phase);
    return 1;
}

void alt_private111_reset_backend(void) {
    memset(&g_backend, 0, sizeof(g_backend));
    g_backend_ready = 0;
    pending_reset();
    alt_control_fence_reset();
}

int alt_private111_backend_ready(void) { return g_backend_ready; }
const char *alt_private111_backend_name(void) {
    return g_backend_ready && g_backend.name ? g_backend.name : "NOT_READY";
}

int alt_private111_prepare_stock_policy(void *receiver_session,
                                        const void *original_request,
                                        const void *alt_descriptor,
                                        int allow_private,
                                        const void **stock_request_out,
                                        int *stock_request_owned_out) {
    void *owned = NULL;
    if (stock_request_out) *stock_request_out = original_request;
    if (stock_request_owned_out) *stock_request_owned_out = 0;
    if (!alt_descriptor) return ALT111_PREP_PASSTHROUGH;
    if (!receiver_session || !original_request || !stock_request_out || !stock_request_owned_out) {
        altscreen_log("ERROR PHASE=STREAM_111_PREP invalid receiver=%p request=%p descriptor=%p",
                      receiver_session, original_request, alt_descriptor);
        return ALT111_PREP_BLOCKED;
    }

    /* Exact P1404 stock SessionSetup rejects type111. Backend readiness therefore
     * cannot decide whether 111 is removed before the stock call: when private111
     * is unavailable, still build a stock-only request with the common CF helper. */
    if (!g_backend_ready) {
        if (!alt_airplay_make_stock_request(receiver_session, original_request,
                                            alt_descriptor, &owned) ||
            !owned || owned == original_request) {
            altscreen_log("ERROR PHASE=STREAM_111_PREP backend_not_ready=1 split_failed=1 receiver=%p descriptor=%p stock110_at_risk=1 returned=%p",
                          receiver_session, alt_descriptor, owned);
            if (owned && owned != original_request)
                alt_airplay_release_object(owned);
            return ALT111_PREP_BLOCKED;
        }
        *stock_request_out = owned;
        *stock_request_owned_out = 1;
        altscreen_log("PHASE=STREAM_111_PREP receiver=%p backend_not_ready=1 private_blocked=1 stock110_protected=1 stock_only_request=%p descriptor=%p",
                      receiver_session, owned, alt_descriptor);
        return ALT111_PREP_BLOCKED;
    }

    if (!g_backend.stock_requires_split) {
        if (!allow_private) {
            altscreen_log("PHASE=STREAM_111_PREP receiver=%p strategy=%s stock_request=original private_policy=BLOCKED generation_allocated=0",
                          receiver_session, g_backend.name);
            return ALT111_PREP_PASSTHROUGH;
        }
        if (!pending_begin(receiver_session, alt_descriptor)) {
            altscreen_log("ERROR PHASE=STREAM_111_PREP receiver=%p strategy=%s fence_begin_failed=1 private_blocked=1 stock_request=original",
                          receiver_session, g_backend.name);
            return ALT111_PREP_BLOCKED;
        }
        altscreen_log("PHASE=STREAM_111_PREP receiver=%p strategy=%s stock_request=original descriptor=%p",
                      receiver_session, g_backend.name, alt_descriptor);
        return ALT111_PREP_PASSTHROUGH;
    }
    if (!g_backend.make_stock_request(receiver_session, original_request,
                                      alt_descriptor, &owned) || !owned || owned == original_request) {
        altscreen_log("ERROR PHASE=STREAM_111_PREP split_failed receiver=%p strategy=%s descriptor=%p stock110_at_risk=1 returned=%p",
                      receiver_session, g_backend.name, alt_descriptor, owned);
        if (owned && owned != original_request && g_backend.release_object)
            g_backend.release_object(owned);
        return ALT111_PREP_BLOCKED;
    }
    *stock_request_out = owned;
    *stock_request_owned_out = 1;
    if (!allow_private) {
        altscreen_log("PHASE=STREAM_111_PREP receiver=%p strategy=%s stock_only_request=%p original=%p descriptor=%p private_policy=BLOCKED generation_allocated=0 stock110_protected=1",
                      receiver_session, g_backend.name, owned, original_request,
                      alt_descriptor);
        return ALT111_PREP_STOCK_ONLY;
    }
    if (!pending_begin(receiver_session, alt_descriptor)) {
        altscreen_log("ERROR PHASE=STREAM_111_PREP receiver=%p strategy=%s fence_begin_failed=1 private_blocked=1 stock110_protected=1 stock_only_request=%p",
                      receiver_session, g_backend.name, owned);
        return ALT111_PREP_BLOCKED;
    }
    altscreen_log("PHASE=STREAM_111_PREP receiver=%p strategy=%s stock_only_request=%p original=%p descriptor=%p",
                  receiver_session, g_backend.name, owned, original_request, alt_descriptor);
    return ALT111_PREP_STOCK_ONLY;
}

int alt_private111_prepare_stock(void *receiver_session,
                                 const void *original_request,
                                 const void *alt_descriptor,
                                 const void **stock_request_out,
                                 int *stock_request_owned_out) {
    return alt_private111_prepare_stock_policy(receiver_session, original_request,
                                               alt_descriptor, 1,
                                               stock_request_out,
                                               stock_request_owned_out);
}

void alt_private111_release_prepared(const void *stock_request, int owned) {
    if (!owned || !stock_request) return;
    if (g_backend_ready && g_backend.release_object)
        g_backend.release_object((void *)stock_request);
    else
        alt_airplay_release_object((void *)stock_request);
}

static int teardown_locked(void *receiver_session, const char *reason,
                           struct p1404_pf_lease *after_stock_cleanup);

static int finish_duplicate(void *receiver_session, const void *alt_descriptor,
                            void *stock_response, const struct altscreen_ctx *c) {
    void *private_response = NULL;
    int rc, teardown_ok;
    altscreen_log("PHASE=STREAM_111_DUPLICATE_BEGIN receiver=%p id=%u generation=%u private_session=%p private_stream=%p two_phase=%d",
                  receiver_session, c->id, c->generation, c->alt_screen_session,
                  c->alt_screen_stream, g_backend.two_phase);
    rc = g_backend.make_existing_response(receiver_session, alt_descriptor,
                                          c->alt_screen_session, c->alt_screen_stream,
                                          &private_response);
    if (!rc || !private_response) {
        altscreen_log("WARN PHASE=STREAM_111_DUPLICATE_RESPONSE receiver=%p id=%u generation=%u rc=%d response=%p teardown_existing=1 recreate_current_generation=1",
                      receiver_session, c->id, c->generation, rc, private_response);
        if (private_response) g_backend.release_object(private_response);
        teardown_ok = teardown_locked(receiver_session, "duplicate_response_failed_recreate", NULL);
        return teardown_ok ? 2 : ALT111_FINISH_FAIL_OPEN;
    }
    if (!g_backend.merge_private_response(stock_response, private_response)) {
        altscreen_log("ERROR PHASE=STREAM_111_DUPLICATE_MERGE receiver=%p id=%u generation=%u response=%p teardown_existing=1",
                      receiver_session, c->id, c->generation, private_response);
        g_backend.release_object(private_response);
        teardown_locked(receiver_session, "duplicate_merge_failed", NULL);
        return ALT111_FINISH_FAIL_OPEN;
    }
    g_backend.release_object(private_response);
    altscreen_log("PHASE=STREAM_111_DUPLICATE_READY receiver=%p id=%u generation=%u response_merged=1 recreate=0",
                  receiver_session, c->id, c->generation);
    return ALT111_FINISH_READY;
}

static int finish_two_phase_locked(void *receiver_session,
                                   const void *alt_descriptor,
                                   void *stock_response,
                                   uint32_t generation) {
    void *alt_session = NULL, *private_response = NULL;
    struct altscreen_ctx snap;
    int rc;

    memset(&snap, 0, sizeof(snap));
    altscreen_log("PHASE=STREAM_111_PREPARE_BEGIN receiver=%p descriptor=%p generation=%u backend=%s two_phase=1",
                  receiver_session, alt_descriptor, generation, g_backend.name);
    rc = g_backend.prepare_setup(receiver_session, alt_descriptor,
                                 &alt_session, &private_response);
    if (!rc || !alt_session || !private_response) {
        altscreen_log("ERROR PHASE=STREAM_111_PREPARE receiver=%p generation=%u rc=%d alt_session=%p private_response=%p no_startsession=1 stock110_untouched=1",
                      receiver_session, generation, rc, alt_session, private_response);
        cleanup_partial(receiver_session, alt_session, NULL, private_response,
                        "prepare_setup_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }

    if (!alt_state_stage_private_session(receiver_session, alt_session)) {
        altscreen_log("ERROR PHASE=STREAM_111_SESSION_STAGE receiver=%p generation=%u backend=%s stage_failed=1",
                      receiver_session, generation, g_backend.name);
        cleanup_partial(receiver_session, alt_session, NULL, private_response,
                        "session_stage_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }
    if (!alt_state_set_generation(receiver_session, generation)) {
        altscreen_log("ERROR PHASE=STREAM_111_SESSION_STAGE receiver=%p generation=%u generation_bind_failed=1",
                      receiver_session, generation);
        cleanup_partial(receiver_session, alt_session, NULL, private_response,
                        "session_generation_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }
    if (!alt_state_snapshot(receiver_session, 0, &snap) || snap.state >= 2 ||
        snap.alt_screen_session != alt_session || snap.alt_screen_stream != NULL ||
        alt_state_lookup(receiver_session) != NULL) {
        altscreen_log("ERROR PHASE=STREAM_111_SESSION_STAGE receiver=%p generation=%u invariant_failed=1 state=%d stream=%p committed_lookup=%p",
                      receiver_session, generation, snap.state, snap.alt_screen_stream,
                      alt_state_lookup(receiver_session));
        cleanup_partial(receiver_session, alt_session, NULL, private_response,
                        "session_stage_invariant");
        return ALT111_FINISH_FAIL_OPEN;
    }

    rc = g_backend.start_accept_worker(receiver_session, alt_session);
    if (!rc) {
        altscreen_log("ERROR PHASE=STREAM_111_WORKER_ARM receiver=%p generation=%u alt_session=%p worker_ready=0 response_not_merged=1",
                      receiver_session, generation, alt_session);
        cleanup_partial(receiver_session, alt_session, NULL, private_response,
                        "accept_worker_start_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }
    altscreen_log("PHASE=STREAM_111_WORKER_ARM receiver=%p generation=%u alt_session=%p worker_ready=1 accept_pending=1 response_not_merged=1",
                  receiver_session, generation, alt_session);

    altscreen_log("PHASE=STREAM_111_RESPONSE_MERGE_BEGIN receiver=%p generation=%u stock_response=%p private_response=%p two_phase=1",
                  receiver_session, generation, stock_response, private_response);
    if (!g_backend.merge_private_response(stock_response, private_response)) {
        altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_MERGE receiver=%p generation=%u merge_failed=1 worker_cancel_required=1 stock110_untouched=1",
                      receiver_session, generation);
        cleanup_partial(receiver_session, alt_session, NULL, private_response,
                        "response_merge_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }
    g_backend.release_object(private_response);

    if (!alt_state_commit_private_session(receiver_session)) {
        altscreen_log("ERROR PHASE=STREAM_111_SESSION_COMMIT receiver=%p generation=%u post_merge_commit_failed=1 protocol_state_corrupt=1",
                      receiver_session, generation);
        cleanup_partial(receiver_session, alt_session, NULL, NULL,
                        "post_merge_session_commit_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }
    if (!alt_state_snapshot(receiver_session, 1, &snap) ||
        snap.alt_screen_session != alt_session || snap.alt_screen_stream != NULL ||
        snap.generation != generation) {
        altscreen_log("ERROR PHASE=STREAM_111_SESSION_COMMIT receiver=%p generation=%u committed_lookup_invariant_failed=1 stream=%p",
                      receiver_session, generation, snap.alt_screen_stream);
        cleanup_partial(receiver_session, alt_session, NULL, NULL,
                        "post_commit_session_lookup_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }

    altscreen_log("PHASE=STREAM_111_PRIVATE_READY id=%u generation=%u receiver=%p alt_session=%p alt_stream=PENDING backend=%s final_response_merged=1 listener_worker_ready=1 two_phase=1 stock110_untouched=1",
                  snap.id, generation, receiver_session, alt_session, g_backend.name);
    return ALT111_FINISH_READY;
}

static int finish_legacy_locked(void *receiver_session,
                                const void *alt_descriptor,
                                void *stock_response,
                                uint32_t generation) {
    void *alt_session = NULL, *alt_stream = NULL, *private_response = NULL;
    struct altscreen_ctx snap;
    int rc;

    memset(&snap, 0, sizeof(snap));

    altscreen_log("PHASE=STREAM_111_CREATE_BEGIN receiver=%p descriptor=%p backend=%s two_phase=0 legacy=1",
                  receiver_session, alt_descriptor, g_backend.name);
    rc = g_backend.create_setup_start(receiver_session, alt_descriptor,
                                      &alt_session, &alt_stream, &private_response);
    if (!rc || !alt_session || !alt_stream || !private_response) {
        altscreen_log("ERROR PHASE=STREAM_111_CREATE_SETUP_START receiver=%p rc=%d alt_session=%p alt_stream=%p private_response=%p stock110_untouched=1",
                      receiver_session, rc, alt_session, alt_stream, private_response);
        cleanup_partial(receiver_session, alt_session, alt_stream, private_response,
                        "create_setup_start_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }

    if (!alt_state_stage_private(receiver_session, alt_session, alt_stream)) {
        altscreen_log("ERROR PHASE=STREAM_111_STAGE receiver=%p backend=%s stage_failed=1",
                      receiver_session, g_backend.name);
        cleanup_partial(receiver_session, alt_session, alt_stream, private_response,
                        "state_stage_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }

    if (!alt_state_set_generation(receiver_session, generation) ||
        !alt_state_snapshot(receiver_session, 0, &snap) || snap.state >= 2 ||
        snap.alt_screen_session != alt_session || snap.alt_screen_stream != alt_stream ||
        alt_state_lookup(receiver_session) != NULL) {
        altscreen_log("ERROR PHASE=STREAM_111_STAGE receiver=%p generation=%u invariant_failed=1 state=%d committed_lookup=%p",
                      receiver_session, generation, snap.state,
                      alt_state_lookup(receiver_session));
        cleanup_partial(receiver_session, alt_session, alt_stream, private_response,
                        "state_stage_invariant");
        return ALT111_FINISH_FAIL_OPEN;
    }

    altscreen_log("PHASE=STREAM_111_RESPONSE_MERGE_BEGIN receiver=%p stock_response=%p private_response=%p two_phase=0",
                  receiver_session, stock_response, private_response);
    if (!g_backend.merge_private_response(stock_response, private_response)) {
        altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_MERGE receiver=%p merge_failed=1 stock110_untouched=1",
                      receiver_session);
        cleanup_partial(receiver_session, alt_session, alt_stream, private_response,
                        "response_merge_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }
    g_backend.release_object(private_response);

    if (!alt_state_commit_private(receiver_session)) {
        altscreen_log("ERROR PHASE=STREAM_111_COMMIT receiver=%p post_merge_commit_failed=1 protocol_state_corrupt=1",
                      receiver_session);
        cleanup_partial(receiver_session, alt_session, alt_stream, NULL,
                        "post_merge_commit_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }

    if (!alt_state_snapshot(receiver_session, 1, &snap) ||
        snap.alt_screen_session != alt_session || snap.alt_screen_stream != alt_stream ||
        snap.generation != generation) {
        altscreen_log("ERROR PHASE=STREAM_111_COMMIT receiver=%p generation=%u committed_lookup_invariant_failed=1",
                      receiver_session, generation);
        cleanup_partial(receiver_session, alt_session, alt_stream, NULL,
                        "post_commit_lookup_failed");
        return ALT111_FINISH_FAIL_OPEN;
    }

    altscreen_log("PHASE=STREAM_111_PRIVATE_READY id=%u generation=%u receiver=%p alt_session=%p alt_stream=%p backend=%s final_response_merged=1 two_phase=0 legacy=1 stock110_untouched=1",
                  snap.id, generation, receiver_session, alt_session, alt_stream,
                  g_backend.name);
    return ALT111_FINISH_READY;
}

static int finish_setup_locked(void *receiver_session,
                               const void *alt_descriptor,
                               int stock_rc,
                               void *stock_response,
                               uint32_t generation) {
    struct altscreen_ctx snap;
    int duplicate_rc;

    memset(&snap, 0, sizeof(snap));

    if (!alt_descriptor) return ALT111_FINISH_NO_ALT;
    if (!receiver_session) return ALT111_FINISH_FAIL_OPEN;
    if (!g_backend_ready) {
        altscreen_log("ERROR PHASE=STREAM_111_CREATE backend_not_ready=1 receiver=%p descriptor=%p stock_rc=%d stock110_untouched=1",
                      receiver_session, alt_descriptor, stock_rc);
        return ALT111_FINISH_FAIL_OPEN;
    }
    if (stock_rc != 0 || !stock_response) {
        altscreen_log("ERROR PHASE=STREAM_111_CREATE stock_not_ready receiver=%p stock_rc=%d stock_response=%p descriptor=%p no_private_attempt=1",
                      receiver_session, stock_rc, stock_response, alt_descriptor);
        return ALT111_FINISH_FAIL_OPEN;
    }

    if (alt_state_snapshot(receiver_session, 1, &snap) && snap.alt_screen_session) {
        duplicate_rc = finish_duplicate(receiver_session, alt_descriptor,
                                        stock_response, &snap);
        if (duplicate_rc != 2) return duplicate_rc;
        memset(&snap, 0, sizeof(snap));
        altscreen_log("PHASE=STREAM_111_DUPLICATE_RECREATE receiver=%p generation=%u same_setup_transaction=1",
                      receiver_session, generation);
    }

    if (alt_state_snapshot(receiver_session, 0, &snap) && snap.state < 2 &&
        (snap.alt_screen_session || snap.alt_screen_stream)) {
        altscreen_log("WARN PHASE=STREAM_111_STALE_STAGE receiver=%p id=%u generation=%u alt_session=%p alt_stream=%p retry_cleanup=1",
                      receiver_session, snap.id, snap.generation,
                      snap.alt_screen_session, snap.alt_screen_stream);
        cleanup_partial(receiver_session, snap.alt_screen_session, snap.alt_screen_stream,
                        NULL, "stale_stage_before_retry");
    }

    return g_backend.two_phase
        ? finish_two_phase_locked(receiver_session, alt_descriptor, stock_response, generation)
        : finish_legacy_locked(receiver_session, alt_descriptor, stock_response, generation);
}

int alt_private111_finish_setup(void *receiver_session,
                                const void *alt_descriptor,
                                int stock_rc,
                                void *stock_response) {
    uint32_t generation;
    int rc;
    if (!alt_descriptor) return ALT111_FINISH_NO_ALT;
    if (!receiver_session) return ALT111_FINISH_FAIL_OPEN;

    generation = pending_take(receiver_session, alt_descriptor);
#ifdef ALTSCREEN_HOST_TEST_ALLOW_UNFENCED_FINISH
    /* Unit fixtures historically call finish_setup directly. Production QNX
     * builds never define this and therefore fail closed if prepare_stock did not
     * register the transaction before the stock call. */
    if (!generation) generation = alt_control_fence_begin();
#endif
    if (!generation) {
        altscreen_log("ERROR PHASE=STREAM_111_FENCE_COMMIT receiver=%p descriptor=%p missing_generation=1 private_blocked=1 stock110_untouched=1",
                      receiver_session, alt_descriptor);
        return ALT111_FINISH_FAIL_OPEN;
    }
    if (!alt_control_fence_lock_current(generation)) {
        altscreen_log("WARN PHASE=STREAM_111_FENCE_STALE receiver=%p descriptor=%p generation=%u current=0 private_skipped=1 stock110_untouched=1",
                      receiver_session, alt_descriptor, generation);
        return ALT111_FINISH_FAIL_OPEN;
    }

    altscreen_log("PHASE=STREAM_111_FENCE_COMMIT_ENTER receiver=%p descriptor=%p generation=%u serialized=1",
                  receiver_session, alt_descriptor, generation);
    rc = finish_setup_locked(receiver_session, alt_descriptor, stock_rc,
                             stock_response, generation);
    alt_control_fence_unlock();
    altscreen_log("PHASE=STREAM_111_FENCE_COMMIT_LEAVE receiver=%p generation=%u private_rc=%d serialized=1",
                  receiver_session, generation, rc);
    return rc;
}

static int teardown_locked(void *receiver_session, const char *reason,
                           struct p1404_pf_lease *after_stock_cleanup) {
    struct altscreen_ctx snap;
    int ok = 1;
    memset(&snap, 0, sizeof(snap));
    if (!alt_state_snapshot(receiver_session, 0, &snap)) return 1;
    altscreen_log("PHASE=STREAM_111_TEARDOWN_BEGIN receiver=%p id=%u generation=%u alt_session=%p alt_stream=%p state=%d committed=%d reason=%s",
                  receiver_session, snap.id, snap.generation, snap.alt_screen_session,
                  snap.alt_screen_stream, snap.state, snap.state >= 2,
                  reason ? reason : "-");
    if (snap.alt_screen_session || snap.alt_screen_stream) {
        if (!g_backend_ready || !g_backend.teardown_private) {
            ok = 0;
            altscreen_log("ERROR PHASE=STREAM_111_TEARDOWN receiver=%p id=%u generation=%u backend_not_ready=1 mapping_will_clear=1",
                          receiver_session, snap.id, snap.generation);
        } else {
            ok = g_backend.teardown_private(receiver_session, snap.alt_screen_session,
                                            snap.alt_screen_stream, after_stock_cleanup);
            if (!ok)
                altscreen_log("ERROR PHASE=STREAM_111_TEARDOWN receiver=%p id=%u generation=%u backend=%s teardown_failed=1 mapping_will_clear=1",
                              receiver_session, snap.id, snap.generation, g_backend.name);
        }
    }
    if (ok) alt_state_unregister(receiver_session);
    altscreen_log("PHASE=STREAM_111_TEARDOWN_DONE receiver=%p id=%u generation=%u session=%p stream=%p ok=%d state_mapping=%s reason=%s",
                  receiver_session, snap.id, snap.generation, snap.alt_screen_session,
                  snap.alt_screen_stream, ok,
                  ok ? "cleared" : "retained_for_safe_retry",
                  reason ? reason : "-");
    return ok;
}

int alt_private111_teardown_deferred(void *receiver_session, const char *reason,
                                    struct p1404_pf_lease *after_stock_cleanup) {
    uint32_t generation;
    int ok;
    if (after_stock_cleanup) memset(after_stock_cleanup, 0, sizeof(*after_stock_cleanup));
    if (!receiver_session) return 1;

    /* cancel_and_lock establishes a total order with finish_setup: either an
     * in-flight commit finishes first and is immediately torn down, or teardown
     * wins first and the old SETUP token becomes stale before it can create 111. */
    generation = alt_control_fence_cancel_and_lock();
    pending_cancel_receiver(receiver_session);
    altscreen_log("PHASE=STREAM_111_FENCE_CANCEL receiver=%p generation=%u reason=%s serialized=1",
                  receiver_session, generation, reason ? reason : "-");
    ok = teardown_locked(receiver_session, reason, after_stock_cleanup);
    alt_control_fence_unlock();
    return ok;
}

int alt_private111_teardown(void *receiver_session, const char *reason) {
    return alt_private111_teardown_deferred(receiver_session, reason, NULL);
}

void alt_private111_finish_cleanup(struct p1404_pf_lease *cleanup) {
    struct p1404_pf_lease owned;
    int ok;
    if (!cleanup || !cleanup->generation) return;
    owned = *cleanup;
    memset(cleanup, 0, sizeof(*cleanup));
    altscreen_log("PHASE=STREAM_111_FIREWALL_REMOVE_BEGIN port=%u generation=%u after_stock_audio=1 bounded_ms=1500",
                  (unsigned)owned.port, owned.generation);
    ok = p1404_alt111_firewall_close_owned(&owned);
    altscreen_log("%s PHASE=STREAM_111_FIREWALL_REMOVE port=%u generation=%u after_stock_audio=1 result=%s core_teardown_preserved=1",
                  ok ? "INFO" : "WARN", (unsigned)owned.port, owned.generation, ok ? "OK" : "FAILED");
}
