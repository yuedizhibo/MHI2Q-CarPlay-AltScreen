/*
 * p1404_airplay_fullchain.c - one-shot SETUP wrapper around p1404_airplay.c.
 *
 * Keep the large measured AirPlay observer/mutator in one source file, but rename
 * its legacy PlatformControl symbol inside this translation unit. The exported
 * three-argument SessionSetup owns the wire response; the six-argument inner
 * PlatformControl is a notification with no response pointer. Legacy observation
 * is deliberately prevented from producing formal SETUP proof.
 */
void altscreen_mark_alt_feature_negotiated_legacy(void);
void altscreen_mark_phone_requested_legacy(void);
#define altscreen_mark_alt_feature_negotiated altscreen_mark_alt_feature_negotiated_legacy
#define altscreen_mark_phone_requested altscreen_mark_phone_requested_legacy
#define alt_find_stream_descriptor alt_find_stream_descriptor_legacy
#define AirPlayReceiverSessionPlatformControl AirPlayReceiverSessionPlatformControl_legacy
#include "p1404_airplay.c"
#include "p1404_lock_wait.h"
#undef AirPlayReceiverSessionPlatformControl
#undef alt_find_stream_descriptor
#undef altscreen_mark_phone_requested
#undef altscreen_mark_alt_feature_negotiated
extern void altscreen_mark_alt_feature_negotiated(void);
extern void altscreen_mark_phone_requested(void);

#include "p1404_private111.h"
#include "p1404_setup_merge.h"
#include "p1404_session_guard.c"

typedef void (*session_teardown_fn)(void *, const void *, int, unsigned char *);
static session_teardown_fn real_session_teardown;
typedef int (*session_start_fn)(void *, const void *);
static session_start_fn real_session_start;
#define TEARDOWN_MAIN 1u
#define TEARDOWN_ALT  2u
#define TEARDOWN_FULL (TEARDOWN_MAIN | TEARDOWN_ALT)
#define ALTSCREEN_K_NOT_HANDLED_ERR (-6714)

/* Stock TearDown uses an absent/empty/non-array streams value for full cleanup
 * (K1004 0x26ae8..0x26b18); a nonempty array is a per-stream operation. Main110
 * cleanup does not destroy receiver+0x13d0, so it need not stop private111. */
static unsigned fullchain_teardown_scope(const void *params, int *out_full) {
    cf_obj streams;
    unsigned i, n, scope = 0;
    *out_full = 1;
    if (!params || !alt_cf_is_dict((cf_obj)params) || !cf_dict_get ||
        !cf_array_count || !cf_array_at) return TEARDOWN_FULL;
    streams = cf_dict_get_cstr((cf_obj)params, "streams");
    if (!streams || !alt_cf_is_array(streams)) return TEARDOWN_FULL;
    n = cf_array_count(streams);
    if (!n) return TEARDOWN_FULL;
    *out_full = 0; /* mixed 110+111 is still a per-stream stop, not session close */
    for (i = 0; i < n; ++i) {
        cf_obj item = (cf_obj)cf_array_at(streams, i);
        int64_t type = -1;
        if (!item || !alt_cf_is_dict(item) ||
            !alt_cf_int64(cf_dict_get_cstr(item, "type"), &type)) continue;
        if (type == CP_STREAM_MAIN_SCREEN) scope |= TEARDOWN_MAIN;
        if (type == CP_STREAM_ALT_SCREEN) scope |= TEARDOWN_ALT;
    }
    return scope;
}

static struct { void *receiver; int main_ready, sent, pending; uint32_t token; } bootstrap[8];
static volatile unsigned bootstrap_guard;
static uint32_t bootstrap_token;

static void bootstrap_lock(void) {
    while (__sync_lock_test_and_set(&bootstrap_guard, 1u)) p1404_lock_wait_yield();
}
static void bootstrap_forget(void *receiver) {
    unsigned i;
    bootstrap_lock();
    for (i=0;i<8;++i) if (bootstrap[i].receiver==receiver)
        memset(&bootstrap[i],0,sizeof(bootstrap[i]));
    __sync_lock_release(&bootstrap_guard);
}
static void bootstrap_note(void *receiver, int main_ready) {
    unsigned i,slot=8;
    uint32_t token=0;
    int submit=0, deferred=0, rc;
    void *event_client=NULL;
    if (!receiver) return;
    bootstrap_lock();
    for (i=0;i<8;++i) {
        if (bootstrap[i].receiver==receiver) {slot=i;break;}
        if (!bootstrap[i].receiver && slot==8) slot=i;
    }
    if (slot<8) {
        if (!bootstrap[slot].receiver) {
            if (bootstrap_token == UINT32_MAX) {
                __sync_lock_release(&bootstrap_guard);
                altscreen_log("ERROR PHASE=ALT111_REQUEST_DEFERRED receiver=%p reason=bootstrap_identity_exhausted",receiver);
                return;
            }
            bootstrap[slot].token=++bootstrap_token;
        }
        bootstrap[slot].receiver=receiver;
        if (main_ready) bootstrap[slot].main_ready=1;
        /* Exact stock SendCommand checks receiver+0x2c0. SessionStart creates
         * it from the accepted event socket. Main SETUP alone is insufficient. */
        memcpy(&event_client,(const unsigned char *)receiver+0x2c0,sizeof(event_client));
        if (bootstrap[slot].main_ready && event_client &&
            !bootstrap[slot].sent && !bootstrap[slot].pending) {
            bootstrap[slot].pending=1;
            token=bootstrap[slot].token;
            submit=1;
        } else if (bootstrap[slot].main_ready && !event_client) {
            deferred=1;
        }
    }
    __sync_lock_release(&bootstrap_guard);
    /* Stock submission may block or synchronously reenter teardown. Reserve
     * under the metadata lock; submit outside it and validate identity after. */
    if (slot==8)
        altscreen_log("ERROR PHASE=ALT111_REQUEST_DEFERRED receiver=%p reason=bootstrap_slots_full",receiver);
    if (deferred)
        altscreen_log("PHASE=ALT111_REQUEST_DEFERRED receiver=%p reason=event_channel_not_started waiting=SessionStart",receiver);
    if (!submit) return;
    rc=alt_request_cluster_after_main(receiver);
    bootstrap_lock();
    if (bootstrap[slot].receiver==receiver && bootstrap[slot].token==token) {
        bootstrap[slot].pending=0;
        bootstrap[slot].sent=(rc==0);
    }
    __sync_lock_release(&bootstrap_guard);
}

static int fullchain_lifecycle_enter(void *session,
                                    enum alt_session_operation operation,
                                    struct alt_session_lease *lease) {
    int admission, retained = 0;
    /* Waiters need their own reference: private111 drops its receiver reference
     * during cleanup, and stock callers may release theirs as soon as we return. */
    if (session) {
        if (!cf_release) bind_cf();
        retained = alt_airplay_retain_object(session);
        if (!retained) {
            memset(lease, 0, sizeof(*lease));
            altscreen_log("ERROR PHASE=SESSION_LIFECYCLE_REFUSED receiver=%p operation=%d reason=retain_unavailable stock_not_called=1",
                          session, (int)operation);
            return ALT_SESSION_REFUSED;
        }
    }
    admission = alt_session_guard_enter(session, operation, lease);
    lease->retained = retained;
    altscreen_log("PHASE=SESSION_LIFECYCLE_ENTER receiver=%p operation=%d admission=%d waited=%u complete_session_serialization=1",
                  session, (int)operation, admission, lease->waited);
    return admission;
}

static void fullchain_lifecycle_leave(void *session,
                                     struct alt_session_lease *lease,
                                     int full_completed) {
    int retained = lease->retained;
    alt_session_guard_leave(lease, full_completed);
    if (retained) alt_airplay_release_object(session);
}

static int fullchain_session_start(void *session, const void *params) {
    int rc;
    if (!real_session_start && p1404_direct_stock_symbol_named)
        real_session_start=(session_start_fn)p1404_direct_stock_symbol_named("AirPlayReceiverSessionStart");
    if (!real_session_start)
        real_session_start=(session_start_fn)p1404_stock_symbol_named("AirPlayReceiverSessionStart");
    if (!real_session_start) return -1;
    altscreen_log("PHASE=SESSION_START_ENTER receiver=%p params=%p",session,params);
    rc=real_session_start(session,params);
    altscreen_log("PHASE=SESSION_START_RETURN receiver=%p rc=%d",session,rc);
    if (!rc && p1404_identity_ok && p1404_armed && p1404_mutate_armed &&
        alt_runtime_negotiation_ready("session-start") &&
        alt_flag_create111 && alt_private111_backend_ready())
        bootstrap_note(session,0);
    return rc;
}

int AirPlayReceiverSessionStart(void *session, const void *params) {
    struct alt_session_lease lease;
    int rc, admission = fullchain_lifecycle_enter(session, ALT_SESSION_START, &lease);
    if (admission != ALT_SESSION_ENTERED) {
        fullchain_lifecycle_leave(session, &lease, 0);
        return -6752; /* kEndingErr: a queued START must not revive a closed session */
    }
    rc = fullchain_session_start(session, params);
    fullchain_lifecycle_leave(session, &lease, 0);
    return rc;
}

/* Stock TearDown ignores PlatformControl's result (K1004 0x26ae8) and may
 * destroy the receiver clock. Stop our worker at the outer boundary instead.
 * The production backend holds a receiver reference until successful cleanup,
 * so a refused cleanup also survives an HTTP caller releasing its reference. */
void AirPlayReceiverSessionTearDown(void *session, const void *params,
                                  int reason, unsigned char *out_done) {
    struct alt_session_lease lease;
    struct p1404_pf_lease after_stock_cleanup = {0, 0};
    unsigned scope;
    unsigned char stock_done = 0;
    int full = 0;
    int admission = fullchain_lifecycle_enter(session, ALT_SESSION_TEARDOWN, &lease);
    if (admission != ALT_SESSION_ENTERED) {
        if (out_done) *out_done = (unsigned char)lease.completed;
        altscreen_log("PHASE=TEARDOWN_DUPLICATE receiver=%p admission=%d done=%d stock_called=0",
                      session, admission, lease.completed);
        fullchain_lifecycle_leave(session, &lease, 0);
        return;
    }
    scope = fullchain_teardown_scope(params, &full);
    if (scope & TEARDOWN_MAIN) bootstrap_forget(session);
    if (!real_session_teardown) {
        if (p1404_direct_stock_symbol_named)
            real_session_teardown = (session_teardown_fn)
                p1404_direct_stock_symbol_named("AirPlayReceiverSessionTearDown");
        if (!real_session_teardown)
            real_session_teardown = (session_teardown_fn)
                p1404_stock_symbol_named("AirPlayReceiverSessionTearDown");
    }
    if (!real_session_teardown) {
        if (out_done) *out_done = 0;
        altscreen_log("ERROR PHASE=TEARDOWN_OUTER missing_stock=1");
        fullchain_lifecycle_leave(session, &lease, 0);
        return;
    }
    /* Always cancel a pending 111 transaction on full/111 teardown, even before
     * it has installed a state mapping. Audio/Main-only changes preserve it. */
    if ((scope & TEARDOWN_ALT) &&
        !alt_private111_teardown_deferred(session, "outer_session_teardown", &after_stock_cleanup)) {
        if (out_done) *out_done = 0;
        altscreen_log("ERROR PHASE=TEARDOWN_OUTER receiver=%p private_cleanup_failed=1 stock_not_called=1 receiver_held_by_backend=1", session);
        fullchain_lifecycle_leave(session, &lease, 0);
        return;
    }
    real_session_teardown(session, params, reason, &stock_done);
    /* The worker no longer references stock clocks. Stop stock audio before
     * waiting on ancillary PF helpers, retaining lifecycle serialization. */
    alt_private111_finish_cleanup(&after_stock_cleanup);
    if (out_done) *out_done = stock_done;
    altscreen_log("PHASE=TEARDOWN_OUTER receiver=%p stock_called=1 reason=%d done=%d scope=%u",session,reason,(int)stock_done,scope);
    fullchain_lifecycle_leave(session, &lease, full && stock_done);
}

/* CFRetain returns the retained object in r0.  Callers currently ignore the
 * value, but keep the exact stock return contract so fixtures cannot normalize
 * another ABI mismatch. */
typedef cf_obj (*cf_retain_txn_fn)(cf_obj);
static cf_retain_txn_fn cf_retain_txn;

void altscreen_mark_alt_feature_negotiated_legacy(void) {
    altscreen_log("PHASE=LEGACY_ALT_FEATURE_OBSERVED negotiated_marker_suppressed=1 reason=FULLCHAIN_SETUP_REQUIRED");
}

void altscreen_mark_phone_requested_legacy(void) {
    altscreen_log("PHASE=LEGACY_PHONE_REQUEST_OBSERVED phone_request_marker_suppressed=1 reason=FULLCHAIN_CANONICAL_SETUP_REQUIRED");
}

static cf_obj fullchain_match_stream_descriptor(cf_obj obj, uint32_t target) {
    cf_obj v;
    int64_t n = -1;
    if (!obj || !alt_cf_is_dict(obj) || !cf_dict_get) return NULL;
    v = cf_dict_get_cstr(obj, "type");
    if (v && alt_cf_int64(v, &n) && n >= 0 && (uint64_t)n <= UINT32_MAX &&
        (uint32_t)n == target)
        return obj;
    return NULL;
}

/* Canonical SETUP parser. Formal stream proof is intentionally limited to the
 * top-level streams[] container because that is the schema the stock-safety
 * splitter can clone and remove without guessing. A legacy/nested/singular
 * "stream" object remains observable only; it cannot arm private111 until exact
 * P1404 evidence proves that schema and a safe stock-removal path exists.
 * LIVI dispatches every stream descriptor solely by its numeric `type` field;
 * the captured AUG22 iPhone requests use that same field. */
static cf_obj fullchain_find_stream_descriptor_inner(cf_obj obj, uint32_t target,
                                                       int depth, int descriptors) {
    cf_obj v;
    unsigned i, count;
    if (!obj || depth > PROBE_DEPTH_MAX) return NULL;

    if (alt_cf_is_array(obj)) {
        if (!cf_array_count || !cf_array_at) return NULL;
        count = cf_array_count(obj);
        if (count > PROBE_ARRAY_MAX) return NULL;
        for (i = 0; i < count; ++i) {
            cf_obj hit = fullchain_find_stream_descriptor_inner((cf_obj)cf_array_at(obj, i),
                                                                  target, depth + 1, 1);
            if (hit) return hit;
        }
        return NULL;
    }

    if (!alt_cf_is_dict(obj) || !cf_dict_get) return NULL;
    if (descriptors) return fullchain_match_stream_descriptor(obj, target);

    v = cf_dict_get_cstr(obj, "streams");
    if (v) return fullchain_find_stream_descriptor_inner(v, target, depth + 1, 1);
    return NULL;
}

void *alt_find_stream_descriptor(void *obj, uint32_t target_stream_type) {
    return fullchain_find_stream_descriptor_inner((cf_obj)obj, target_stream_type, 0, 0);
}

static unsigned fullchain_count_stream_descriptors(cf_obj obj, uint32_t target) {
    cf_obj streams;
    unsigned i, count, matches = 0;
    if (!obj || !alt_cf_is_dict(obj) || !cf_dict_get || !cf_array_count || !cf_array_at)
        return 0;
    streams = cf_dict_get_cstr(obj, "streams");
    if (!streams || !alt_cf_is_array(streams)) return 0;
    count = cf_array_count(streams);
    if (count > PROBE_ARRAY_MAX) return PROBE_ARRAY_MAX + 1u;
    for (i = 0; i < count; ++i)
        if (fullchain_match_stream_descriptor((cf_obj)cf_array_at(streams, i), target))
            ++matches;
    return matches;
}

static int fullchain_request_features_alt(cf_obj obj) {
    cf_obj v;
    int64_t n = 0;
    if (!obj || !alt_cf_is_dict(obj) || !cf_dict_get) return 0;
    v = cf_dict_get_cstr(obj, "features");
    if (v) {
        if (alt_cf_int64(v, &n) && (n & (int64_t)CP_FEATURE_ALTSCREEN)) return 1;
        if (array_has_string(v, "altScreen")) return 1;
    }
    v = cf_dict_get_cstr(obj, "enabledFeatures");
    if (!v) return 0;
    if (alt_cf_int64(v, &n) && (n & (int64_t)CP_FEATURE_ALTSCREEN)) return 1;
    return array_has_string(v, "altScreen");
}

void alt_airplay_release_object(void *object) {
    if (object && cf_release) cf_release((cf_obj)object);
}

static int txn_retain_ready(void) {
    if (!cf_retain_txn)
        cf_retain_txn = (cf_retain_txn_fn)dlsym(RTLD_DEFAULT, "CFRetain");
    return cf_retain_txn != NULL && cf_release != NULL;
}

/*
 * Private111 CF bridge.
 *
 * Keep every CF/CFL operation used by the private backend inside this translation
 * unit. p1404_airplay.c already owns the exact-target adapter layer: mutable
 * dictionaries use the stock CFL type callback tables, strings use the P1404
 * UTF-8 ABI, and CFNumberCreateInt64 is called with its measured one-int64 raw
 * shape. The backend must never rediscover convenience helpers with an assumed
 * ABI via dlsym().
 */
int alt_airplay_private_cf_ready(void) {
    return cf_dict_create_mutable && cf_dict_get && cf_dict_set && cf_str_new &&
           cf_num_new && cf_release && txn_retain_ready();
}

int alt_airplay_retain_object(void *object) {
    if (!object || !txn_retain_ready()) return 0;
    cf_retain_txn((cf_obj)object);
    return 1;
}

int alt_airplay_get_int64_cstr(const void *dict, const char *key, int64_t *out) {
    cf_obj value;
    if (out) *out = 0;
    if (!dict || !key || !out || !alt_cf_is_dict(dict)) return 0;
    value = cf_dict_get_cstr((cf_obj)dict, key);
    return value && alt_cf_int64(value, out);
}

void *alt_airplay_build_stream_response(uint32_t stream_type, uint16_t data_port) {
    cf_obj dict = NULL;
    int64_t type_check = -1, port_check = -1;

    if (!stream_type || !data_port || !alt_airplay_private_cf_ready()) return NULL;
    dict = cf_dict_create_mutable(NULL, 0u, NULL, NULL, NULL);
    if (!dict) return NULL;

    if (!set_i64(dict, "type", (int64_t)stream_type) ||
        !set_i64(dict, "dataPort", (int64_t)data_port) ||
        !alt_airplay_get_int64_cstr(dict, "type", &type_check) ||
        !alt_airplay_get_int64_cstr(dict, "dataPort", &port_check) ||
        type_check != (int64_t)stream_type || port_check != (int64_t)data_port) {
        alt_airplay_release_object(dict);
        return NULL;
    }
    return dict;
}

static int setup_ops_ready(struct alt_setup_cf_ops *o) {
    if (!o) return 0;
    memset(o, 0, sizeof(*o));
    o->is_dict = alt_cf_is_dict;
    o->is_array = alt_cf_is_array;
    o->dict_count = cf_dict_count;
    o->dict_get = cf_dict_get;
    o->dict_set = cf_dict_set;
    o->dict_get_keys_values = cf_dict_get_keys_values;
    o->dict_create_mutable = cf_dict_create_mutable;
    o->array_count = cf_array_count;
    o->array_at = cf_array_at;
    o->array_append = cf_array_append;
    o->array_create_mutable = cf_array_create_mutable;
    o->string_new = cf_str_new;
    o->release = alt_airplay_release_object;
    o->int64_value = alt_cf_int64;
    return o->is_dict && o->is_array && o->dict_count && o->dict_get && o->dict_set &&
           o->dict_get_keys_values && o->dict_create_mutable && o->array_count &&
           o->array_at && o->array_append && o->array_create_mutable &&
           o->string_new && o->release && o->int64_value;
}

int alt_airplay_make_stock_request(void *receiver_session,
                                   const void *original_request,
                                   const void *alt_descriptor,
                                   void **owned_stock_request) {
    struct alt_setup_cf_ops ops;
    alt_setup_obj found = NULL, copy;
    unsigned removed = 0;
    if (owned_stock_request) *owned_stock_request = NULL;
    if (!receiver_session || !original_request || !alt_descriptor || !owned_stock_request)
        return 0;
    if (!setup_ops_ready(&ops)) {
        altscreen_log("ERROR PHASE=SETUP_STOCK_SPLIT receiver=%p cf_ops_ready=0", receiver_session);
        return 0;
    }
    copy = alt_setup_build_stock_only(&ops, (alt_setup_obj)original_request,
                                      CP_STREAM_ALT_SCREEN, &found, &removed);
    if (!copy || removed == 0u || found != (alt_setup_obj)alt_descriptor) {
        altscreen_log("ERROR PHASE=SETUP_STOCK_SPLIT receiver=%p copy=%p removed=%u expected_descriptor=%p found=%p",
                      receiver_session, copy, removed, alt_descriptor, found);
        if (copy) alt_airplay_release_object(copy);
        return 0;
    }
    *owned_stock_request = copy;
    altscreen_log("PHASE=SETUP_STOCK_SPLIT receiver=%p original=%p stock_only=%p descriptor=%p removed=%u",
                  receiver_session, original_request, copy, alt_descriptor, removed);
    return 1;
}

int alt_airplay_merge_private_response(void *stock_response,
                                       void *private_stream_response) {
    struct alt_setup_cf_ops ops;
    cf_obj response = (cf_obj)stock_response;
    cf_obj streams_key = NULL;
    cf_obj old_streams = NULL;
    cf_obj held_streams = NULL;
    cf_obj normalized_streams = NULL;
    int streams_changed = 0;
    int empty_stock_response = 0;

    if (!stock_response || !private_stream_response || !alt_cf_is_dict(stock_response) ||
        !alt_cf_is_dict(private_stream_response)) return 0;
    if (!setup_ops_ready(&ops) || !cf_str_new || !cf_dict_get || !cf_dict_set) {
        altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_TXN cf_ops_ready=0 stock_response=%p private_response=%p",
                      stock_response, private_stream_response);
        return 0;
    }
    if (!txn_retain_ready()) {
        altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_TXN retain_api_missing=1 stock_response=%p",
                      stock_response);
        return 0;
    }

    streams_key = cf_str_new("streams", -1);
    if (!streams_key) goto fail;
    old_streams = (cf_obj)cf_dict_get(response, streams_key);
    if (!old_streams) {
        /* A type111-only phone request becomes streams=[] on the stock side.
         * P1404 accepts it and returns {}.  Normalize that valid stock result
         * to the response shape emitted by LIVI before appending type111. */
        normalized_streams = (cf_obj)cf_array_create_mutable(NULL, 0, NULL, NULL);
        if (!normalized_streams) goto fail;
        cf_dict_set(response, streams_key, normalized_streams);
        if (cf_dict_get(response, streams_key) != normalized_streams) goto fail;
        alt_airplay_release_object(normalized_streams);
        normalized_streams = NULL;
        old_streams = (cf_obj)cf_dict_get(response, streams_key);
        empty_stock_response = 1;
        altscreen_log("PHASE=STREAM_111_RESPONSE_NORMALIZE response=%p empty_stock_response=1 streams_array_created=1 livi_shape=1",
                      response);
    }
    if (!old_streams || !alt_cf_is_array(old_streams)) goto fail;

    cf_retain_txn(old_streams);
    held_streams = old_streams;

    if (!alt_setup_merge_private_response(&ops, response,
                                          (alt_setup_obj)private_stream_response,
                                          CP_STREAM_ALT_SCREEN)) {
        altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_TXN streams_merge_failed=1 response=%p private=%p",
                      response, private_stream_response);
        if (held_streams) {
            cf_dict_set(response, streams_key, held_streams);
            if (cf_dict_get(response, streams_key) != held_streams)
                altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_TXN_PARTIAL_ROLLBACK response=%p streams_restore_failed=1",
                              response);
            else
                altscreen_log("PHASE=STREAM_111_RESPONSE_TXN_PARTIAL_ROLLBACK response=%p stock_streams_restored=1",
                              response);
        }
        goto fail;
    }
    streams_changed = 1;

    if (!response_offers_alt(response)) {
        altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_TXN stream111_verify_failed=1 response=%p",
                      response);
        goto rollback;
    }

    altscreen_log("PHASE=STREAM_111_RESPONSE_TXN_READY response=%p private=%p streams_merged=1 empty_stock_response=%d enabledFeatures=stock_unchanged_livi_stream_setup stock_objects_retained=1",
                  response, private_stream_response, empty_stock_response);
    alt_airplay_release_object(streams_key);
    alt_airplay_release_object(held_streams);
    return 1;

rollback:
    if (streams_changed && held_streams) cf_dict_set(response, streams_key, held_streams);
    if (held_streams && cf_dict_get(response, streams_key) != held_streams) {
        altscreen_log("ERROR PHASE=STREAM_111_RESPONSE_TXN_ROLLBACK response=%p rollback_verify_failed=1",
                      response);
    } else {
        altscreen_log("PHASE=STREAM_111_RESPONSE_TXN_ROLLBACK response=%p stock_semantics_restored=1 retained_old_objects=1",
                      response);
    }
fail:
    alt_airplay_release_object(normalized_streams);
    alt_airplay_release_object(streams_key);
    alt_airplay_release_object(held_streams);
    return 0;
}

static int fullchain_stop_server(void *session, unsigned flags, void *command,
                                 const void *qualifier, const void *params,
                                 void **out_params) {
    struct alt_session_lease lease;
    struct p1404_pf_lease after_stock_cleanup = {0, 0};
    int r, admission = fullchain_lifecycle_enter(session, ALT_SESSION_CONTROL, &lease);
    if (admission != ALT_SESSION_ENTERED) {
        if (out_params) *out_params = NULL;
        fullchain_lifecycle_leave(session, &lease, 0);
        return admission == ALT_SESSION_CLOSED ? 0 : -1;
    }
    bootstrap_forget(session);
    if (alt_state_lookup_any(session) &&
        !alt_private111_teardown_deferred(session, "stopServer", &after_stock_cleanup)) {
        altscreen_log("ERROR PHASE=TEARDOWN_PRIVATE_BARRIER session=%p command=stopServer callback_refused=1 stock_caller_may_ignore_status=1",
                      session);
        fullchain_lifecycle_leave(session, &lease, 0);
        return -1;
    }
    r = AirPlayReceiverSessionPlatformControl_legacy(session, flags, command,
                                                   qualifier, params, out_params);
    alt_private111_finish_cleanup(&after_stock_cleanup);
    fullchain_lifecycle_leave(session, &lease, 0);
    return r;
}

int AirPlayReceiverSessionPlatformControl(void *session, unsigned flags,
                                          void *command, const void *qualifier,
                                          const void *params, void **out_params) {
    char scratch[64];
    const char *cmd_name;
    int r;
    altscreen_runtime_ensure_initialized();
    if (!real_session_control) p1404_airplay_bind_lazy();
    if (!real_session_control) return -1;
    if (!alt_runtime_negotiation_ready("platform-control") ||
        !p1404_identity_ok)
        return real_session_control(session, flags, command, qualifier, params, out_params);
    cmd_name = alt_cf_cString((cf_obj)command, scratch, sizeof(scratch));
    /* Stock SessionSetup calls this only AFTER dispatching its streams, with
     * outParams=NULL (K1004 0x278fc). This callback cannot own the wire reply.
     * The outer three-argument SessionSetup below owns split/merge instead. */
    if (cmd_name && !strcmp(cmd_name, "setUpStreams")) {
        /* Captured K1004 SessionSetup invokes this only as an internal
         * notification: valid receiver, flags=1, no qualifier, request params,
         * and no outParams. Refuse malformed/external shapes before stock. */
        if (!session || flags != 1u || qualifier || !params || out_params) {
            altscreen_log("ERROR PHASE=SETUP_PLATFORM_NOTIFICATION_REFUSED session=%p flags=%u qualifier=%p params=%p out_params=%p reason=invalid_stock_notification_shape",
                          session, flags, qualifier, params, out_params);
            return -1;
        }
        altscreen_log("PHASE=SETUP_PLATFORM_NOTIFICATION session=%p response_owner=outer_session_setup out_params=%p",
                      session, out_params);
        return real_session_control(session, flags, command, qualifier, params, out_params);
    }
    if (cmd_name && !strcmp(cmd_name, "tearDownStreams")) {
        /* Outer TearDown owns lifetime and stream selection. Legacy observation
         * unconditionally unregisters the receiver, including audio-only stops. */
        altscreen_log("PHASE=TEARDOWN_PLATFORM_NOTIFICATION session=%p lifetime_owner=outer_session_teardown",session);
        return real_session_control(session, flags, command, qualifier, params, out_params);
    }
    if (cmd_name && !strcmp(cmd_name, "stopServer")) {
        return fullchain_stop_server(session, flags, command, qualifier, params, out_params);
    }
    r = AirPlayReceiverSessionPlatformControl_legacy(session, flags, command,
                                                     qualifier, params, out_params);
    /* LIVI accepts suggestUI and deliberately does not display the offered
     * URLs. AUG22 dio does not implement it and returns kNotHandledErr, which
     * AirPlay turns into HTTP 422. Preserve every stock result except that
     * exact missing-handler result while the AltScreen mutation is armed. */
    if (cmd_name && !strcmp(cmd_name, "suggestUI") &&
        p1404_armed && p1404_mutate_armed &&
        r == ALTSCREEN_K_NOT_HANDLED_ERR) {
        altscreen_log("PHASE=SUGGEST_UI_ACCEPTED session=%p stock_rc=%d return_rc=0 policy=LIVI_NOOP urls_not_shown=1",
                      session, r);
        return 0;
    }
    return r;
}

/* HTTP SETUP -> SessionSetup(receiver, request, &response), K1004 0x2f730.
 * Authentication and session-key installation precede this call in stock code.
 * Interpose its GOT slot, not just the inner platform notification. */
static int fullchain_private_setup_result(int stock_rc, int split,
                                         int private_rc, int response_ready,
                                         void **out_params) {
    /* Stock saw a request with 111 removed. Its successful empty response is
     * not evidence that the original request succeeded. Never advertise an
     * unbound port or leak that false success back to the phone. */
    if (stock_rc == 0 && split &&
        (private_rc != ALT111_FINISH_READY || !response_ready)) {
        if (out_params && *out_params) {
            alt_airplay_release_object(*out_params);
            *out_params = NULL;
        }
        altscreen_log("ERROR PHASE=SETUP_111_REJECT private_rc=%d response_ready=%d return_rc=%d empty_success_suppressed=1 stock110_teardown=0",
                      private_rc, response_ready, ALTSCREEN_K_NOT_HANDLED_ERR);
        return ALTSCREEN_K_NOT_HANDLED_ERR;
    }
    return stock_rc;
}

static int fullchain_session_setup(void *session, const void *params, void **out_params) {
    int r, wants_alt = 0;
    int prep_rc = ALT111_PREP_PASSTHROUGH;
    int private_rc = ALT111_FINISH_NO_ALT;
    int stock_owned = 0;
    int private_policy = 0;
    int private_allowed = 0;
    int feature_response_accepted = 0;
    int feature_contract_ready = 0;
    int alt_advertised = 0;
    int display_published = 0;
    int feature_contract_proven = 0;
    unsigned alt_desc_count = 0;
    const void *stock_params = params;
    void *lr = __builtin_return_address(0);
    cf_obj alt_desc = NULL;
    struct altscreen_ctx state_snap;

    memset(&state_snap, 0, sizeof(state_snap));
    altscreen_runtime_ensure_initialized();
    if (!real_session_setup) p1404_airplay_bind_lazy();
    if (!real_session_setup && p1404_direct_stock_symbol_named)
        real_session_setup = (setup_fn)p1404_direct_stock_symbol_named("AirPlayReceiverSessionSetup");
    if (!real_session_setup) return -1;
    if (!alt_runtime_negotiation_ready("session-setup") ||
        !p1404_identity_ok)
        return real_session_setup(session, params, out_params);

    alt_desc = (cf_obj)alt_find_stream_descriptor((void *)params, CP_STREAM_ALT_SCREEN);
    alt_desc_count = fullchain_count_stream_descriptors((cf_obj)params, CP_STREAM_ALT_SCREEN);
    wants_alt = alt_desc != NULL || alt_desc_count != 0u || fullchain_request_features_alt((cf_obj)params);
    altscreen_log("PHASE=SETUP_REQUEST_ENTER session=%p wants_alt=%d alt_descriptor=%p alt_descriptor_count=%u params=%p wrapper=outer_session_setup canonical_parser=1",
                  session, wants_alt, alt_desc, alt_desc_count, params);
    probe_container("setup-request-original", (cf_obj)params, 0);

    /* Real AUG22 dio does not necessarily query the server-level "features"
     * property while building /info.  It can publish the independent display
     * through SessionPlatformCopyProperty and complete the feature contract in
     * the descriptor-free SETUP response instead.  Either observed feature
     * proof is valid once the display itself was actually published. */
    display_published = altscreen_get_state()->info_alt_display;
    feature_contract_proven =
        altscreen_get_state()->info_alt_feature_advertised ||
        altscreen_get_state()->alt_feature_negotiated;
    alt_advertised = display_published && feature_contract_proven;
    if (alt_desc) {
        altscreen_log("PHASE=SETUP_111_DESCRIPTOR session=%p descriptor=%p count=%u advertised=%d display_published=%d feature_property_advertised=%d feature_negotiated=%d create111_armed=%d backend_ready=%d backend=%s",
                      session, alt_desc, alt_desc_count, alt_advertised,
                      display_published,
                      altscreen_get_state()->info_alt_feature_advertised,
                      altscreen_get_state()->alt_feature_negotiated,
                      alt_flag_create111, alt_private111_backend_ready(),
                      alt_private111_backend_name());
    }
    if (wants_alt) {
        altscreen_mark_phone_requested();
        altscreen_log("PHASE=SETUP_111_REQUEST create111_armed=%d descriptor=%p count=%u canonical=1",
                      alt_flag_create111, alt_desc, alt_desc_count);
    }

    if (alt_desc && alt_runtime_negotiation_ready("private111-setup") &&
        p1404_armed && p1404_mutate_armed) {
        /* LIVI dispatches an explicit type-111 descriptor by type alone. Prior
         * feature/display observations are diagnostics, never an extra gate. */
        private_policy = alt_flag_create111 && alt_private111_backend_ready() &&
                         alt_desc_count >= 1u;
        prep_rc = alt_private111_prepare_stock_policy(session, params, alt_desc,
                                                      private_policy,
                                                      &stock_params, &stock_owned);
        private_allowed = private_policy && prep_rc != ALT111_PREP_BLOCKED;
        if (prep_rc != ALT111_PREP_BLOCKED && alt_desc_count > 1u)
            altscreen_log("WARN PHASE=SETUP_111_MULTIPLE receiver=%p count=%u first_descriptor_used=1 stock111_removed=%d",
                          session, alt_desc_count, stock_params && stock_params != params);
    }

    altscreen_log("PHASE=SETUP_STOCK_PREP session=%p prep_rc=%d private_policy=%d private_allowed=%d stock_params=%p original_params=%p owned=%d stock110_protected=%d backend=%s",
                  session, prep_rc, private_policy, private_allowed, stock_params, params, stock_owned,
                  stock_params && stock_params != params,
                  alt_private111_backend_name());
    if (stock_params && stock_params != params)
        probe_container("setup-request-stock", (cf_obj)stock_params, 0);
    else
        altscreen_log("PHASE=SETUP_STOCK_REQUEST strategy=passthrough_or_split_failed params=%p", stock_params);

    r = real_session_setup(session, stock_params, out_params);
    alt_private111_release_prepared(stock_params, stock_owned);
    obs_call("SessionSetup", session, lr, (long)r);

    altscreen_log("PHASE=SETUP_STOCK_RETURN rc=%d response=%p wants_alt=%d alt_descriptor=%p prep_rc=%d",
                  r, (out_params ? *out_params : NULL), wants_alt, alt_desc, prep_rc);
    if (out_params && *out_params)
        probe_container("setup-response-stock", (cf_obj)*out_params, 0);

    /* The first setUpStreams transaction negotiates session features and the
     * later screen transaction carries the explicit type-111 descriptor.  The
     * fullchain wrapper previously suppressed the legacy response patch but did
     * not replace it for this descriptor-free first phase, so iPhone never saw
     * altScreen/viewAreas and therefore never sent the independent 111 SETUP.
     * Re-read feature-property state after stock returns because stock's
     * initial setup invokes that redirected callback; /info display publication
     * may legally follow, so READY's geometry preflight closes that ordering gap. */
    if (!alt_desc && !cf_dict_get_cstr((cf_obj)params, "streams") &&
        r == 0 && out_params && *out_params &&
        p1404_armed && p1404_mutate_armed && alt_flag_feature &&
        alt_flag_info && alt_flag_create111 && alt_private111_backend_ready()) {
        /* Like LIVI, declare accessory features in session SETUP without
         * waiting for the phone to ask for altScreen or /info to run first. */
        feature_contract_ready = alt_airplay_negotiation_geometry_ready();
        if (feature_contract_ready)
            feature_response_accepted = accept_alt_in_setup_response((cf_obj)*out_params);
        altscreen_log("PHASE=SETUP_FEATURE_NEGOTIATION receiver=%p requested=%d feature_property_advertised=%d display_published=%d geometry_preflight=%d response_patched=%d descriptor_phase=0 backend_ready=1",
                      session, wants_alt,
                      altscreen_get_state()->info_alt_feature_advertised,
                      altscreen_get_state()->info_alt_display,
                      alt_airplay_negotiation_geometry_ready(),
                      feature_response_accepted);
        if (feature_response_accepted)
            altscreen_mark_alt_feature_negotiated();
    }

    if (alt_desc && private_allowed) {
        /* A permitted prepare owns exactly one generation. Always finish it,
         * including stock errors and NULL responses, so retries cannot inherit a
         * stale pending token. finish_setup fails open without private creation
         * unless stock_rc==0 and stock_response is non-NULL. */
        private_rc = alt_private111_finish_setup(session, alt_desc, r,
                                                 out_params ? *out_params : NULL);
        (void)alt_state_snapshot(session, 1, &state_snap);
        altscreen_log("PHASE=SETUP_111_FINISH receiver=%p id=%u generation=%u session=%p stream=%p private_rc=%d stock_rc=%d response=%p generation_consumed=1",
                      session, state_snap.id, state_snap.generation,
                      state_snap.alt_screen_session, state_snap.alt_screen_stream,
                      private_rc, r, out_params ? *out_params : NULL);
    } else if (alt_desc && !private_allowed) {
        altscreen_log("ERROR PHASE=SETUP_111_PRIVATE_BLOCKED receiver=%p prep_rc=%d backend_ready=%d descriptor_count=%u stock110_protected=%d",
                      session, prep_rc, alt_private111_backend_ready(), alt_desc_count,
                      stock_params && stock_params != params);
    } else if (alt_desc && (r != 0 || !out_params || !*out_params)) {
        altscreen_log("ERROR PHASE=SETUP_111_STOCK_NOT_READY receiver=%p stock_rc=%d response=%p no_private_attempt=1",
                      session, r, out_params ? *out_params : NULL);
    }

    if (out_params && *out_params) {
        probe_container("setup-response-final", (cf_obj)*out_params, 0);
        if (private_rc == ALT111_FINISH_READY) {
            if (response_offers_alt((cf_obj)*out_params)) {
                (void)alt_state_snapshot(session, 1, &state_snap);
                altscreen_log("PHASE=SETUP_111_FINAL_READY receiver=%p id=%u generation=%u session=%p stream=%p response=%p type111=1 dataPort_merged=1 stock_rc=0 return_to_phone=1",
                              session, state_snap.id, state_snap.generation,
                              state_snap.alt_screen_session, state_snap.alt_screen_stream,
                              *out_params);
                altscreen_mark_alt_feature_negotiated();
            } else {
                int cleanup_ok;
                altscreen_log("ERROR PHASE=SETUP_111_FINAL_VERIFY receiver=%p private_ready=1 type111_missing=1 backend_contract_violation=1 cleanup_required=1",
                              session);
                cleanup_ok = alt_private111_teardown(session, "setup-final-verify");
                altscreen_log("%s PHASE=SETUP_111_FINAL_VERIFY_CLEANUP receiver=%p result=%s context74_restore_requested=1",
                              cleanup_ok ? "WARN" : "ERROR", session,
                              cleanup_ok ? "PASS" : "REFUSED");
            }
        } else if (response_offers_alt((cf_obj)*out_params) &&
                   !feature_response_accepted) {
            altscreen_log("WARN PHASE=SETUP_ALT_STOCK_ONLY receiver=%p private_rc=%d response_offers_alt=1 no_negotiated_marker=1",
                          session, private_rc);
        }
    }

    if (wants_alt && r != 0)
        altscreen_log("ERROR PHASE=SETUP_111_STOCK_REJECT rc=%d descriptor=%p prep_rc=%d backend=%s",
                      r, alt_desc, prep_rc, alt_private111_backend_name());
    if (alt_desc && alt_flag_create111 && !alt_state_lookup(session))
        altscreen_log("ERROR PHASE=STREAM_111_PRIVATE_PATH_NOT_BOUND session=%p descriptor=%p descriptor_count=%u private_rc=%d prep_rc=%d",
                      session, alt_desc, alt_desc_count, private_rc, prep_rc);

    if (r == 0 && out_params && *out_params && !alt_desc &&
        p1404_armed && p1404_mutate_armed && alt_flag_create111 &&
        alt_private111_backend_ready() &&
        fullchain_count_stream_descriptors((cf_obj)params, CP_STREAM_MAIN_SCREEN) == 1u) {
        /* The stock Main110 setup has succeeded; it is now meaningful to ask
         * the phone for the cluster. Waiting for private111 here is circular. */
        bootstrap_note(session,1);
    }
    return fullchain_private_setup_result(r,
                prep_rc == ALT111_PREP_STOCK_ONLY,
                private_rc,
                out_params && *out_params && response_offers_alt((cf_obj)*out_params),
                out_params);
}

int AirPlayReceiverSessionSetup(void *session, const void *params, void **out_params) {
    struct alt_session_lease lease;
    int rc, admission = fullchain_lifecycle_enter(session, ALT_SESSION_SETUP, &lease);
    if (admission != ALT_SESSION_ENTERED) {
        if (out_params) *out_params = NULL;
        fullchain_lifecycle_leave(session, &lease, 0);
        return -6752;
    }
    rc = fullchain_session_setup(session, params, out_params);
    fullchain_lifecycle_leave(session, &lease, 0);
    return rc;
}
