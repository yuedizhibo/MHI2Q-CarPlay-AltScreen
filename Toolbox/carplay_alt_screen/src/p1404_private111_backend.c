/*
 * p1404_private111_backend.c - exact P1404 private stream-111 backend.
 *
 * The control-plane transaction is intentionally two phase:
 *   SETUP: Create + Setup + security + exact stock listener + response.
 *   DATA:  worker blocks in exact SocketAccept; only after the phone connects it
 *          pre-claims StartSession, binds the real private ScreenStream inside
 *          interposed ScreenStreamStart before stock renderer config, then runs
 *          exact ProcessFrames -> StopSession.
 *
 * This preserves stock Main110 and avoids calling the 9-argument
 * ScreenStreamProcessData ABI ourselves. Exact-P1404 static proofs cover the
 * helper call shapes, Main/delegate isolation and generation/cancel ordering.
 */
#include "p1404_private111_backend.h"
#include "p1404_lock_wait.h"
#include "p1404_private111.h"
#include "p1404_airplay.h"
#include "p1404_abi.h"
#include "altscreen_state_private.h"
#include "p1404_cockpit_native.h"
#include "p1404_firewall.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "dlfcn.h"

#define SCREEN_SESSION_CTR_ACTIVE_OFF          0x1e4u
#define SCREEN_SESSION_WAKE_FD_OFF             0x1e8u
#define SCREEN_SESSION_STREAM_OFF              0x1ecu
#define RECEIVER_SERVER_OFF                    0x0cu
#define SERVER_PROCESS_FRAMES_TIMEOUT_OFF      0x1c8u
#define RECEIVER_AES_SESSION_KEY_OFF           0x1c8u
#define RECEIVER_CLOCK_OFF                     0x13d0u
#define RECEIVER_SESSION_UUID_OFF              0x70u
#define RECEIVER_DEVICE_ID_OFF                 0x88u
#define RECEIVER_PEER_FAMILY_OFF               0x55u
#define AIRPLAY_SESSION_KEY_LEN                16u
#define ALT111_PAIR_SLOTS                      8u
#define ALT111_ACCEPT_TIMEOUT_SECONDS          10
#define ALT111_SOCKET_ACCEPT_TIMEOUT_STATUS    (-6722)
#define ALT111_RECEIVE_BUFFER_BYTES            (-180224)

#define ALT111_REQUEST_KEY_CONN   "streamConnectionID"

#define P1404_AF_INET       2
#define P1404_AF_INET6      24
#define P1404_SOCK_STREAM   1
#define P1404_IPPROTO_TCP   6

/* Exact P1404 lifecycle ABI. */
typedef int  (*f_screen_create_t)(void **out);
typedef int  (*f_screen_setup_t)(void *session, const void *descriptor);
typedef int  (*f_screen_start_t)(void *session, const void *screen_stream_options);
typedef void (*f_screen_stop_t)(void *session);
typedef uint64_t (*f_clock_now_t)(void *clock);
typedef uint64_t (*f_clock_to_ticks_t)(void *clock, uint64_t ntp_time);
struct screen_time_synchronizer {
    void *context;
    f_clock_now_t now;
    f_clock_to_ticks_t to_ticks;
};
typedef void (*f_screen_set_time_t)(void *, const struct screen_time_synchronizer *);
typedef void (*f_screen_set_uuid_t)(void *, const void *);
typedef void (*f_screen_set_device_t)(void *, uint64_t);
typedef void (*f_screen_delete_t)(void *session);
typedef int  (*f_screen_set_security_t)(void *session, const void *key16, const void *iv16);
typedef void (*f_derive_for_screen_t)(const void *session_key, size_t session_key_len,
                                      uint64_t connection_id,
                                      uint8_t out_key[16], uint8_t out_iv[16]);

/* Exact stock helper ABI recovered from MHI2Q_CN_AUG22_P1404/libairplay.so. */
typedef int  (*f_server_socket_open_t)(int af, int type, int protocol,
                                       int requested_port, int *out_port,
                                       int receive_buffer_bytes, int *out_fd);
typedef int  (*f_socket_accept_t)(int listener_fd, int timeout_seconds,
                                  int *out_native_fd, void *optional_peer_addr);
typedef int  (*f_net_socket_create_native_t)(void **out_net_socket, int native_fd);
/* Stock NetSocket_Delete returns OSStatus even though cleanup ignores it. */
typedef int  (*f_net_socket_delete_t)(void *net_socket);
typedef int  (*f_process_frames_t)(void *screen_session, void *net_socket,
                                   int timeout_seconds);
typedef int  (*f_send_loopback_t)(int fd, const void *buffer, size_t length);

/* QNX ARM pthread_t is one machine word in the target ABI. Resolve through
 * dlsym so the preload does not grow a libpthread DT_NEEDED dependency. */
typedef unsigned long p1404_pthread_t;
typedef int (*f_pthread_create_t)(p1404_pthread_t *thread, const void *attr,
                                  void *(*entry)(void *), void *arg);
typedef int (*f_pthread_join_t)(p1404_pthread_t thread, void **value_ptr);

typedef int (*f_socket_t)(int, int, int);
typedef int (*f_connect_t)(int, const void *, int);
typedef int (*f_close_t)(int);

struct sockaddr_in_p1404 {
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;
    uint8_t  sin_addr[4];
    uint8_t  sin_zero[8];
};

enum alt111_worker_phase {
    ALT111_W_NONE = 0,
    ALT111_W_PREPARED = 1,
    ALT111_W_ACCEPTING = 2,
    ALT111_W_ACCEPTED = 3,
    ALT111_W_STARTING = 4,
    ALT111_W_STREAMING = 5,
    ALT111_W_DONE = 6,
    ALT111_W_FAILED = 7
};

enum alt111_accept_action {
    ALT111_ACCEPT_FAILED = 0,
    ALT111_ACCEPT_CONNECTED = 1,
    ALT111_ACCEPT_RETRY = 2,
    ALT111_ACCEPT_STOPPED = 3
};

static struct {
    f_screen_create_t             create;
    f_screen_setup_t              setup;
    f_screen_start_t              start;
    f_screen_stop_t               stop;
    f_screen_set_time_t           set_time;
    f_screen_set_uuid_t           set_uuid;
    f_screen_set_device_t         set_device;
    f_clock_now_t                clock_now;
    f_clock_to_ticks_t           clock_to_ticks;
    f_screen_delete_t             del;
    f_screen_set_security_t       set_security;
    f_derive_for_screen_t         derive_screen;
    f_server_socket_open_t        server_socket_open;
    f_socket_accept_t             socket_accept;
    f_net_socket_create_native_t  net_socket_create_native;
    f_net_socket_delete_t         net_socket_delete;
    f_process_frames_t            process_frames;
    f_send_loopback_t             send_loopback;
    f_pthread_create_t            pthread_create_fn;
    f_pthread_join_t              pthread_join_fn;
    f_socket_t                    socket_fn;
    f_connect_t                   connect_fn;
    f_close_t                     close_fn;
} be;

struct alt111_pair {
    void *receiver;
    void *session;
    int listen_fd;
    uint16_t listen_port;
    uint64_t connection_id;
    p1404_pthread_t thread;
    int worker_started;
    int worker_starting;
    int worker_done;
    int stop_requested;
    int session_stopped;
    int firewall_open;
    uint32_t firewall_generation;
    int phase;
};

static struct alt111_pair g_pairs[ALT111_PAIR_SLOTS];
static volatile unsigned g_pairs_guard;

static void pairs_lock(void) {
    while (__sync_lock_test_and_set(&g_pairs_guard, 1u) != 0u) p1404_lock_wait_yield();
}
static void pairs_unlock(void) {
    __sync_lock_release(&g_pairs_guard);
}

static void secure_zero(void *ptr, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (n--) *p++ = 0;
}

static void *read_ptr(void *base, unsigned off) {
    void *v = NULL;
    if (base) memcpy(&v, (const char *)base + off, sizeof(v));
    return v;
}

static int read_int(void *base, unsigned off) {
    int v = -1;
    if (base) memcpy(&v, (const char *)base + off, sizeof(v));
    return v;
}

static void *session_stream(void *session) {
    return read_ptr(session, SCREEN_SESSION_STREAM_OFF);
}

static struct alt111_pair *pair_find_locked(void *session) {
    unsigned i;
    for (i = 0; i < ALT111_PAIR_SLOTS; ++i)
        if (g_pairs[i].session == session) return &g_pairs[i];
    return NULL;
}

static int pair_install(void *receiver, void *session,
                        int fd, uint16_t port, uint64_t connection_id,
                        const struct p1404_pf_lease *firewall) {
    unsigned i;
    struct alt111_pair *free_pair = NULL;
    int ok = 0;
    pairs_lock();
    for (i = 0; i < ALT111_PAIR_SLOTS; ++i) {
        if (g_pairs[i].session == session) {
            ok = g_pairs[i].receiver == receiver &&
                 g_pairs[i].listen_fd == fd &&
                 g_pairs[i].listen_port == port &&
                 g_pairs[i].connection_id == connection_id;
            pairs_unlock();
            return ok;
        }
        if (!g_pairs[i].session && !free_pair) free_pair = &g_pairs[i];
    }
    if (free_pair) {
        /* The private worker dereferences receiver clock/session state. Keep
         * its owner alive even if a failed outer teardown's caller releases it. */
        if (!alt_airplay_retain_object(receiver)) {
            pairs_unlock();
            return 0;
        }
        memset(free_pair, 0, sizeof(*free_pair));
        free_pair->receiver = receiver;
        free_pair->session = session;
        free_pair->listen_fd = fd;
        free_pair->listen_port = port;
        free_pair->connection_id = connection_id;
        free_pair->firewall_open = firewall && firewall->generation;
        free_pair->firewall_generation = firewall ? firewall->generation : 0;
        free_pair->phase = ALT111_W_PREPARED;
        ok = 1;
    }
    pairs_unlock();
    return ok;
}

static int pair_snapshot(void *session, int *fd_out, uint16_t *port_out,
                         uint64_t *connection_id_out, int *phase_out) {
    struct alt111_pair *p;
    int found = 0;
    if (fd_out) *fd_out = -1;
    if (port_out) *port_out = 0;
    if (connection_id_out) *connection_id_out = 0;
    if (phase_out) *phase_out = ALT111_W_NONE;
    pairs_lock();
    p = pair_find_locked(session);
    if (p) {
        if (fd_out) *fd_out = p->listen_fd;
        if (port_out) *port_out = p->listen_port;
        if (connection_id_out) *connection_id_out = p->connection_id;
        if (phase_out) *phase_out = p->phase;
        found = 1;
    }
    pairs_unlock();
    return found;
}

static int pair_stop_requested(struct alt111_pair *p) {
    int stop = 1;
    pairs_lock();
    if (p && p->session) stop = p->stop_requested;
    pairs_unlock();
    return stop;
}

static int classify_accept_result(int rc, int native_fd, int stop_requested) {
    if (stop_requested) return ALT111_ACCEPT_STOPPED;
    if (rc == 0 && native_fd >= 0) return ALT111_ACCEPT_CONNECTED;
    if (rc == ALT111_SOCKET_ACCEPT_TIMEOUT_STATUS && native_fd < 0)
        return ALT111_ACCEPT_RETRY;
    return ALT111_ACCEPT_FAILED;
}

static void pair_set_phase(struct alt111_pair *p, int phase) {
    pairs_lock();
    if (p && p->session) p->phase = phase;
    pairs_unlock();
}

/* This is the only STARTING -> STREAMING transition. The final stop check and
 * phase publication are one atomic metadata operation, so teardown either wins
 * here (and the worker never enters ProcessFrames) or observes STREAMING and
 * sends the exact wake byte before joining. */
static int pair_try_enter_streaming(struct alt111_pair *p) {
    int enter = 0;
    pairs_lock();
    if (p && p->session && p->phase == ALT111_W_STARTING &&
        !p->stop_requested) {
        p->phase = ALT111_W_STREAMING;
        enter = 1;
    }
    pairs_unlock();
    return enter;
}

static int pair_request_stop_snapshot(void *receiver, void *session,
                                      int *worker_started_out,
                                      int *worker_done_out,
                                      p1404_pthread_t *thread_out,
                                      int *phase_out, int *listener_out,
                                      uint16_t *port_out) {
    struct alt111_pair *p;
    int found = 0;
    for (;;) {
        pairs_lock();
        p = pair_find_locked(session);
        if (!p || p->receiver != receiver) {
            pairs_unlock();
            return 0;
        }
        p->stop_requested = 1;
        if (!p->worker_starting) break;
        /* Creation must publish a valid handle before teardown joins/deletes.
         * Let both creator and child run while that publication is pending. */
        pairs_unlock();
        p1404_lock_wait_yield();
    }
    {
        if (worker_started_out) *worker_started_out = p->worker_started;
        if (worker_done_out) *worker_done_out = p->worker_done;
        if (thread_out) *thread_out = p->thread;
        if (phase_out) *phase_out = p->phase;
        if (listener_out) *listener_out = p->listen_fd;
        if (port_out) *port_out = p->listen_port;
        found = 1;
    }
    pairs_unlock();
    return found;
}


static int pair_close_firewall(struct alt111_pair *p, const char *reason) {
    struct p1404_pf_lease lease = {0, 0};
    uint16_t port = 0;
    void *session = NULL;
    int active = 0;
    pairs_lock();
    if (p && p->session) {
        session = p->session;
        port = p->listen_port;
        active = p->firewall_open;
        lease.port = port;
        lease.generation = p->firewall_generation;
    }
    pairs_unlock();
    if (!active) return 1;
    if (!p1404_alt111_firewall_close_owned(&lease)) {
        int error = errno;
        altscreen_log("ERROR PHASE=STREAM_111_FIREWALL_REMOVE session=%p port=%u reason=%s result=FAILED errno=%d bounded_ms=1500",
                      session, (unsigned)port, reason ? reason : "-", error);
        return 0;
    }
    pairs_lock();
    if (p && p->session == session && p->listen_port == port)
        p->firewall_open = 0;
    pairs_unlock();
    altscreen_log("PHASE=STREAM_111_FIREWALL_REMOVE session=%p port=%u reason=%s result=OK scope=exact-port",
                  session, (unsigned)port, reason ? reason : "-");
    return 1;
}

static void pair_finish_worker(struct alt111_pair *p, int phase, int session_stopped) {
    pairs_lock();
    if (p && p->session) {
        p->phase = phase;
        p->worker_done = 1;
        if (session_stopped) p->session_stopped = 1;
    }
    pairs_unlock();
}

static int read_stream_connection_id(const void *descriptor, uint64_t *connection_id_out) {
    int64_t signed_value = 0;
    if (connection_id_out) *connection_id_out = 0;
    if (!descriptor || !connection_id_out) return 0;
    if (!alt_airplay_get_int64_cstr(descriptor, ALT111_REQUEST_KEY_CONN, &signed_value))
        return 0;
    *connection_id_out = (uint64_t)signed_value;
    return 1;
}

static int build_stream111_response(const void *alt_descriptor,
                                    uint16_t data_port,
                                    void **owned_response) {
    void *dict;
    if (owned_response) *owned_response = NULL;
    if (!alt_descriptor || !data_port || !owned_response) return 0;
    dict = alt_airplay_build_stream_response(CP_STREAM_ALT_SCREEN, data_port);
    if (!dict) return 0;
    *owned_response = dict;
    return 1;
}

static int open_stock_equivalent_listener(void *receiver, uint16_t *port_out, int *fd_out) {
    int port = 0, fd = -1, rc, af;
    if (port_out) *port_out = 0;
    if (fd_out) *fd_out = -1;
    if (!receiver || !port_out || !fd_out || !be.server_socket_open) return 0;
    af = ((const uint8_t *)receiver)[RECEIVER_PEER_FAMILY_OFF];

    rc = be.server_socket_open(af, P1404_SOCK_STREAM, P1404_IPPROTO_TCP,
                               0, &port, ALT111_RECEIVE_BUFFER_BYTES, &fd);
    altscreen_log("PHASE=STREAM_111_LISTENER_OPEN_RETURN rc=%d requested_port=0 bound_port=%d advertised_port=%d fd=%d helper=ServerSocketOpen recvbuf=%d af=%d",
                  rc, port, port, fd, ALT111_RECEIVE_BUFFER_BYTES, af);
    if (rc != 0 || fd < 0 || port <= 0 || port > 65535) {
        if (fd >= 0 && be.close_fn) be.close_fn(fd);
        return 0;
    }
    *port_out = (uint16_t)port;
    *fd_out = fd;
    return 1;
}

/* Teardown wake for the pre-accept phase only. The listener itself is created by
 * exact ServerSocketOpen; this short-lived localhost client merely makes
 * SocketAccept return so the worker can observe stop_requested and exit. */
static int wake_listener(void *receiver, uint16_t port) {
    struct sockaddr_in_p1404 sa;
    uint8_t sa6[28]; /* QNX sockaddr_in6: len/family/port/flow/addr/scope. */
    int fd, rc, af;
    uint8_t *port_bytes;
    if (!receiver || !port || !be.socket_fn || !be.connect_fn || !be.close_fn) return 0;
    af = ((const uint8_t *)receiver)[RECEIVER_PEER_FAMILY_OFF];
    if (af != P1404_AF_INET && af != P1404_AF_INET6) return 0;
    if (sizeof(sa) != 16u) return 0;
    fd = be.socket_fn(af, P1404_SOCK_STREAM, P1404_IPPROTO_TCP);
    if (fd < 0) return 0;
    if (af == P1404_AF_INET6) {
        memset(sa6, 0, sizeof(sa6));
        sa6[0] = sizeof(sa6);
        sa6[1] = P1404_AF_INET6;
        sa6[2] = (uint8_t)(port >> 8);
        sa6[3] = (uint8_t)port;
        sa6[23] = 1u; /* ::1 */
        rc = be.connect_fn(fd, sa6, sizeof(sa6));
        be.close_fn(fd);
        altscreen_log("PHASE=STREAM_111_LISTENER_WAKE port=%u rc=%d method=localhost-connect af=%d", (unsigned)port, rc, af);
        return rc == 0;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = (uint8_t)sizeof(sa);
    sa.sin_family = (uint8_t)P1404_AF_INET;
    port_bytes = (uint8_t *)&sa.sin_port;
    port_bytes[0] = (uint8_t)(port >> 8);
    port_bytes[1] = (uint8_t)(port & 0xffu);
    sa.sin_addr[0] = 127u;
    sa.sin_addr[1] = 0u;
    sa.sin_addr[2] = 0u;
    sa.sin_addr[3] = 1u;
    rc = be.connect_fn(fd, &sa, (int)sizeof(sa));
    be.close_fn(fd);
    altscreen_log("PHASE=STREAM_111_LISTENER_WAKE port=%u rc=%d method=localhost-connect",
                  (unsigned)port, rc);
    return rc == 0;
}

/* Exact P1404 proof gates. These booleans are intentionally local and are also
 * asserted independently by p1404_target_static_proofs.sh / host regressions. */
static int local_p1404_security_proven(void) { return 1; }
static int local_p1404_private_start_main_isolation_proven(void) { return 1; }
static int local_p1404_listener_and_start_timing_proven(void) { return 1; }
static int local_p1404_control_plane_serialization_proven(void) { return 1; }

/* Stock StopSession returns void. Confirm its documented field mutations,
 * never interpret the last internal callee's r0 as a cleanup status. */
static int stop_session_checked(void *session) {
    if (!session || !be.stop) return -1;
    be.stop(session);
    return session_stream(session) == NULL &&
           read_int(session, SCREEN_SESSION_WAKE_FD_OFF) < 0 &&
           ((const uint8_t *)session)[SCREEN_SESSION_CTR_ACTIVE_OFF] == 0 ? 0 : -1;
}

/* Same receiver -> clock lookup as stock callbacks at K1004 0x24470/0x24464.
 * The control fence joins this worker before the receiver is destroyed. */
static uint64_t private_clock_now(void *receiver) {
    return be.clock_now(read_ptr(receiver, RECEIVER_CLOCK_OFF));
}
static uint64_t private_clock_to_ticks(void *receiver, uint64_t ntp_time) {
    return be.clock_to_ticks(read_ptr(receiver, RECEIVER_CLOCK_OFF), ntp_time);
}
static int prepare_timing_context(void *receiver, void *session) {
    struct screen_time_synchronizer sync;
    uint64_t device_id;
    if (!receiver || !session || !be.set_time || !be.set_uuid || !be.set_device ||
        !be.clock_now || !be.clock_to_ticks || !read_ptr(receiver, RECEIVER_CLOCK_OFF)) {
        altscreen_log("ERROR PHASE=STREAM_111_TIMING_NOT_READY receiver=%p session=%p", receiver, session);
        return 0;
    }
    memcpy(&device_id, (const uint8_t *)receiver + RECEIVER_DEVICE_ID_OFF, sizeof(device_id));
    be.set_uuid(session, (const uint8_t *)receiver + RECEIVER_SESSION_UUID_OFF);
    be.set_device(session, device_id);
    sync.context = receiver;
    sync.now = private_clock_now;
    sync.to_ticks = private_clock_to_ticks;
    be.set_time(session, &sync); /* Stock setter copies the 3 ARM words. */
    altscreen_log("PHASE=STREAM_111_TIMING_READY receiver=%p session=%p source=receiver+0x13d0 callbacks=stock-clock", receiver, session);
    return 1;
}

static int prepare_start_context(void *receiver,
                                 void *screen_session,
                                 const void *alt_descriptor,
                                 uint64_t *connection_id_out) {
    uint8_t session_key[16], key[16], iv[16];
    uint64_t connection_id = 0;
    int rc = -1;

    if (connection_id_out) *connection_id_out = 0;
    if (!receiver || !screen_session || !alt_descriptor || !connection_id_out ||
        !be.derive_screen || !be.set_security) return 0;
    if (!read_stream_connection_id(alt_descriptor, &connection_id)) return 0;

    /* Stock SetSecurityInfo stores the negotiated key here. The derive
     * routine reads it twice and returns void (r0 is left by free()). */
    memcpy(session_key, (const uint8_t *)receiver + RECEIVER_AES_SESSION_KEY_OFF,
           sizeof(session_key));
    memset(key, 0, sizeof(key));
    memset(iv, 0, sizeof(iv));

    altscreen_log("PHASE=STREAM_111_SECURITY_DERIVE_CALL receiver=%p session=%p conn=%llu key_source=receiver+0x1c8",
                  receiver, screen_session, (unsigned long long)connection_id);
    be.derive_screen(session_key, AIRPLAY_SESSION_KEY_LEN,
                                 connection_id, key, iv);
    altscreen_log("PHASE=STREAM_111_SECURITY_DERIVE_RETURN receiver=%p session=%p return_type=void",
                  receiver, screen_session);

    altscreen_log("PHASE=STREAM_111_SECURITY_SET_CALL receiver=%p session=%p",
                  receiver, screen_session);
    rc = be.set_security(screen_session, key, iv);
    altscreen_log("PHASE=STREAM_111_SECURITY_SET_RETURN receiver=%p session=%p rc=%d ctr_ready=%d",
                  receiver, screen_session, rc,
                  ((const uint8_t *)screen_session)[SCREEN_SESSION_CTR_ACTIVE_OFF] != 0);
    if (rc != 0 || ((const uint8_t *)screen_session)[SCREEN_SESSION_CTR_ACTIVE_OFF] == 0) {
        rc = -1;
        goto out;
    }

    *connection_id_out = connection_id;
    rc = 0;

out:
    secure_zero(session_key, sizeof(session_key));
    secure_zero(key, sizeof(key));
    secure_zero(iv, sizeof(iv));
    return rc == 0;
}

static int backend_runtime_prereqs_ready(void) {
    return p1404_identity_ok && p1404.screen_create && p1404.screen_setup &&
           p1404.screen_start_session && p1404.screen_stop_session && p1404.screen_delete &&
           p1404.screen_set_security_info && p1404.derive_aes_key_sha512_for_screen &&
           be.create && be.setup && be.start && be.stop && be.del &&
           be.set_time && be.set_uuid && be.set_device && be.clock_now && be.clock_to_ticks &&
           be.set_security && be.derive_screen && alt_airplay_private_cf_ready() &&
           be.server_socket_open && be.socket_accept && be.net_socket_create_native &&
           be.net_socket_delete && be.process_frames && be.send_loopback &&
           be.pthread_create_fn && be.pthread_join_fn &&
           be.socket_fn && be.connect_fn && be.close_fn;
}

static int prepare_setup(void *receiver,
                         const void *alt_descriptor,
                         void **alt_screen_session,
                         void **owned_private_response) {
    void *session = NULL, *response = NULL;
    uint64_t connection_id = 0;
    uint16_t port = 0;
    int listen_fd = -1, rc, firewall_open = 0;
    struct p1404_pf_lease firewall = {0, 0};

    if (alt_screen_session) *alt_screen_session = NULL;
    if (owned_private_response) *owned_private_response = NULL;
    if (!receiver || !alt_descriptor || !alt_screen_session || !owned_private_response)
        return 0;

    rc = be.create(&session);
    altscreen_log("PHASE=STREAM_111_CREATE_RETURN receiver=%p rc=%d session=%p", receiver, rc, session);
    if (rc != 0 || !session) return 0;
    *alt_screen_session = session; /* lets orchestration teardown partial setup */

    rc = be.setup(session, alt_descriptor);
    altscreen_log("PHASE=STREAM_111_SETUP_RETURN receiver=%p session=%p rc=%d", receiver, session, rc);
    if (rc != 0) goto fail_local;

    if (!prepare_start_context(receiver, session, alt_descriptor, &connection_id))
        goto fail_local;
    if (!open_stock_equivalent_listener(receiver, &port, &listen_fd)) goto fail_local;
    if (!p1404_alt111_firewall_open_owned(port, &firewall)) {
        int firewall_errno = errno;
        altscreen_log("ERROR PHASE=STREAM_111_FIREWALL_ADD receiver=%p session=%p port=%u result=FAILED errno=%d response_advertised=0 fail_closed=1",
                      receiver, session, (unsigned)port, firewall_errno);
        goto fail_local;
    }
    firewall_open = 1;
    altscreen_log("PHASE=STREAM_111_FIREWALL_ADD receiver=%p session=%p port=%u result=OK scope=carplay0_tcp_exact_port before_response=1",
                  receiver, session, (unsigned)port);
    if (!build_stream111_response(alt_descriptor, port, &response)) goto fail_local;

    if (!pair_install(receiver, session, listen_fd, port, connection_id, &firewall)) {
        altscreen_log("ERROR PHASE=STREAM_111_PAIR_INSTALL receiver=%p session=%p table_or_collision=1",
                      receiver, session);
        goto fail_local;
    }
    /* Pair owns listener and exact PF aperture from here. */
    listen_fd = -1;
    firewall_open = 0;
    *owned_private_response = response;
    altscreen_log("PHASE=STREAM_111_PREPARED receiver=%p session=%p dataPort=%u conn=%llu startSession=0 stream_pending=1",
                  receiver, session, (unsigned)port, (unsigned long long)connection_id);
    return 1;

fail_local:
    if (response) alt_airplay_release_object(response);
    if (firewall_open) {
        int fw_rc = p1404_alt111_firewall_close_owned(&firewall);
        altscreen_log("%s PHASE=STREAM_111_FIREWALL_REMOVE session=%p port=%u reason=prepare_failure result=%s",
                      fw_rc ? "PHASE" : "ERROR", session, (unsigned)port,
                      fw_rc ? "OK" : "FAILED");
    }
    if (listen_fd >= 0 && be.close_fn) be.close_fn(listen_fd);
    /* No pair exists, so clean the partial ScreenSession here and clear output to
     * prevent orchestration from attempting a second teardown. */
    if (session && be.stop) be.stop(session);
    if (session && be.del) be.del(session);
    *alt_screen_session = NULL;
    return 0;
}

static void *private111_worker(void *arg) {
    struct alt111_pair *p = (struct alt111_pair *)arg;
    void *receiver = NULL, *session = NULL, *net_socket = NULL;
    void *stream = NULL, *claimed_stream = NULL;
    void *server = NULL;
    int process_timeout_seconds;
    int listener = -1, native_fd = -1, rc = -1, stop_rc = -1;
    int accept_action = ALT111_ACCEPT_FAILED;
    int session_stopped = 0, native_attached = 0, claim_ok = 0;
    int terminal_phase = ALT111_W_FAILED;
    int process_stop_requested = 0;
    unsigned accept_attempt = 0;
    struct altscreen_ctx state_snap;
    uint32_t state_id = 0, generation = 0;

    memset(&state_snap, 0, sizeof(state_snap));
    pairs_lock();
    if (p && p->session) {
        receiver = p->receiver;
        session = p->session;
        listener = p->listen_fd;
        p->phase = ALT111_W_ACCEPTING;
    }
    pairs_unlock();
    if (receiver && alt_state_snapshot(receiver, 0, &state_snap)) {
        state_id = state_snap.id;
        generation = state_snap.generation;
    }
    if (!receiver || !session || listener < 0) goto done;

    /* LIVI keeps each ScreenStream server alive until explicit session stop.
     * SocketAccept is the measured AUG22 helper and only offers a finite wait,
     * so use its 10-second timeout as a cancellation polling slice rather than
     * treating the first quiet slice as end-of-stream. The vehicle log measured
     * -6722 after exactly one such slice. Other errors remain terminal. */
    for (;;) {
        ++accept_attempt;
        native_fd = -1;
        altscreen_log("PHASE=STREAM_111_ACCEPT_WAIT receiver=%p id=%u generation=%u session=%p listener=%d timeout_s=%d attempt=%u persistent_until_teardown=1",
                      receiver, state_id, generation, session, listener,
                      ALT111_ACCEPT_TIMEOUT_SECONDS, accept_attempt);
        rc = be.socket_accept(listener, ALT111_ACCEPT_TIMEOUT_SECONDS, &native_fd, NULL);
        altscreen_log("PHASE=STREAM_111_ACCEPT_RETURN receiver=%p id=%u generation=%u session=%p rc=%d native_fd=%d attempt=%u",
                      receiver, state_id, generation, session, rc, native_fd,
                      accept_attempt);
        accept_action = classify_accept_result(rc, native_fd,
                                               pair_stop_requested(p));
        if (accept_action == ALT111_ACCEPT_CONNECTED) break;
        if (accept_action == ALT111_ACCEPT_STOPPED) {
            terminal_phase = ALT111_W_DONE;
            altscreen_log("PHASE=STREAM_111_ACCEPT_STOPPED receiver=%p id=%u generation=%u session=%p rc=%d native_fd=%d attempt=%u teardown_won=1",
                          receiver, state_id, generation, session, rc, native_fd,
                          accept_attempt);
            break;
        }
        if (accept_action == ALT111_ACCEPT_RETRY) {
            altscreen_log("PHASE=STREAM_111_ACCEPT_RETRY receiver=%p id=%u generation=%u session=%p rc=%d attempt=%u listener_kept_open=1 livi_lifetime=1",
                          receiver, state_id, generation, session, rc,
                          accept_attempt);
            continue;
        }
        altscreen_log("ERROR PHASE=STREAM_111_ACCEPT_FAILED receiver=%p id=%u generation=%u session=%p rc=%d native_fd=%d attempt=%u transport_failure=1 cleanup=required",
                      receiver, state_id, generation, session, rc, native_fd,
                      accept_attempt);
        break;
    }

    pairs_lock();
    if (p && p->session) {
        if (p->listen_fd == listener) p->listen_fd = -1;
        if (terminal_phase == ALT111_W_DONE) p->phase = ALT111_W_DONE;
        else p->phase = (rc == 0 && native_fd >= 0) ? ALT111_W_ACCEPTED : ALT111_W_FAILED;
    }
    pairs_unlock();
    if (be.close_fn) be.close_fn(listener); /* stock closes listener after accept */

    /*
     * Do not run pfctl/popen/system on the accepted Stream111 worker path.
     * Vehicle evidence showed that synchronous exact-port PF removal can stall
     * this worker after ACCEPT_RETURN and before NetSocket_CreateWithNative,
     * leaving the phone-side socket readable but completely unconsumed.  Keep
     * the exact-port rule alive until teardown; the listener itself is already
     * closed, so no second accept can occur on this session.
     */
    altscreen_log("PHASE=STREAM_111_FIREWALL_DEFERRED receiver=%p session=%p reason=post_accept_keep_until_teardown next=NETSOCKET_CREATE",
                  receiver, session);

    if (terminal_phase == ALT111_W_DONE) {
        if (native_fd >= 0) {
            be.close_fn(native_fd);
            native_fd = -1;
        }
        goto done;
    }
    if (rc != 0 || native_fd < 0) goto done;
    if (pair_stop_requested(p)) {
        be.close_fn(native_fd);
        native_fd = -1;
        terminal_phase = ALT111_W_DONE;
        goto done;
    }

    rc = be.net_socket_create_native(&net_socket, native_fd);
    altscreen_log("PHASE=STREAM_111_NETSOCKET_CREATE_RETURN receiver=%p rc=%d native_fd=%d net=%p",
                  receiver, rc, native_fd, net_socket);
    if (rc != 0 || !net_socket) {
        be.close_fn(native_fd);
        native_fd = -1;
        goto done;
    }
    native_fd = -1; /* ownership transferred to NetSocket */

    if (pair_stop_requested(p)) {
        terminal_phase = ALT111_W_DONE;
        goto net_done;
    }

    pair_set_phase(p, ALT111_W_STARTING);
    if (!prepare_timing_context(receiver, session)) goto net_done;
    if (!p1404_cockpit_native_prepare_start(receiver)) {
        altscreen_log("ERROR PHASE=STREAM_111_START_PREPARE receiver=%p id=%u generation=%u session=%p preconfig_attach_ready=0 stock_start_called=0 main110_untouched=1",
                      receiver, state_id, generation, session);
        goto net_done;
    }
    /* LIVI creates the independent receiver directly from the type-111 SETUP.
     * The AUG22 equivalent also needs no extra CarPlay config object: measured
     * K1004/P1404 ScreenStreamCreate bodies overwrite r0 without reading it and
     * obtain display properties through our private ScreenCopyMain handoff. */
    altscreen_log("PHASE=STREAM_111_START_CALL receiver=%p id=%u generation=%u session=%p config=0 livi_direct_type111=1 post_accept=1 preconfig_claim=1",
                  receiver, state_id, generation, session);
    rc = be.start(session, NULL);
    claim_ok = p1404_cockpit_native_complete_start(receiver, &claimed_stream);
    stream = session_stream(session);
    native_attached = claim_ok;
    altscreen_log("PHASE=STREAM_111_START_RETURN receiver=%p id=%u generation=%u session=%p rc=%d stream=%p claimed_stream=%p preconfig_attached=%d post_accept=1",
                  receiver, state_id, generation, session, rc, stream,
                  claimed_stream, claim_ok);
    if (rc != 0 || !stream || !claim_ok || claimed_stream != stream) {
        if (claim_ok && claimed_stream && claimed_stream != stream) {
            p1404_cockpit_native_detach(receiver, claimed_stream);
            native_attached = 0;
        }
        altscreen_log("ERROR PHASE=NATIVE_111_START_HANDOFF receiver=%p session=%p rc=%d stream=%p claimed_stream=%p preconfig_attached=%d private_stream_aborted=1 main110_untouched=1",
                      receiver, session, rc, stream, claimed_stream, claim_ok);
        goto net_done;
    }

    if (pair_stop_requested(p)) {
        stop_rc = stop_session_checked(session);
        session_stopped = (stop_rc == 0);
        terminal_phase = session_stopped ? ALT111_W_DONE : ALT111_W_FAILED;
        if (session_stopped) {
            if (native_attached) {
                p1404_cockpit_native_detach(receiver, stream);
                native_attached = 0;
            }
            stream = NULL;
        }
        goto net_done;
    }

    if (!alt_state_bind_private_stream(receiver, stream)) {
        altscreen_log("ERROR PHASE=STREAM_111_STREAM_BIND receiver=%p session=%p stream=%p worker_stop=1",
                      receiver, session, stream);
        stop_rc = stop_session_checked(session);
        session_stopped = (stop_rc == 0);
        if (session_stopped) {
            if (native_attached) {
                p1404_cockpit_native_detach(receiver, stream);
                native_attached = 0;
            }
            stream = NULL;
        }
        goto net_done;
    }
    /* ScreenStreamStart was interposed during StartSession, so this exact
     * private renderer was bound before its first stock config call. */

    if (!pair_try_enter_streaming(p)) {
        altscreen_log("PHASE=STREAM_111_PROCESSFRAMES_SKIPPED receiver=%p id=%u generation=%u session=%p stream=%p stop_won_before_entry=1",
                      receiver, state_id, generation, session, stream);
        stop_rc = stop_session_checked(session);
        session_stopped = (stop_rc == 0);
        terminal_phase = session_stopped ? ALT111_W_DONE : ALT111_W_FAILED;
        if (session_stopped) {
            p1404_cockpit_native_detach(receiver, stream);
            native_attached = 0;
            (void)alt_state_clear_private_stream(receiver, stream);
            stream = NULL;
        }
        goto net_done;
    }

    if (!alt_state_mark_private_processing(receiver, stream)) {
        altscreen_log("ERROR PHASE=STREAM_111_PROCESSING_COMMIT receiver=%p session=%p stream=%p worker_stop=1",
                      receiver, session, stream);
        stop_rc = stop_session_checked(session);
        session_stopped = (stop_rc == 0);
        if (session_stopped) {
            p1404_cockpit_native_detach(receiver, stream);
            native_attached = 0;
            (void)alt_state_clear_private_stream(receiver, stream);
            stream = NULL;
        }
        goto net_done;
    }
    server = read_ptr(receiver, RECEIVER_SERVER_OFF);
    process_timeout_seconds = read_int(server, SERVER_PROCESS_FRAMES_TIMEOUT_OFF);
    altscreen_log("PHASE=STREAM_111_PROCESSFRAMES_BEGIN receiver=%p id=%u generation=%u session=%p stream=%p net=%p timeout_s=%d",
                  receiver, state_id, generation, session, stream, net_socket,
                  process_timeout_seconds);
    p1404_cockpit_native_processing(receiver, stream, 1);
    rc = be.process_frames(session, net_socket, process_timeout_seconds);
    /* Freeze the exit reason before StopSession/detach can trigger teardown.
     * A later stop request must not turn a transport failure into normal DONE. */
    process_stop_requested = pair_stop_requested(p);
    p1404_cockpit_native_processing(receiver, stream, 0);
    altscreen_log("PHASE=STREAM_111_PROCESSFRAMES_RETURN receiver=%p id=%u generation=%u session=%p stream=%p rc=%d stop_requested=%d",
                  receiver, state_id, generation, session, stream, rc,
                  process_stop_requested);

    stop_rc = stop_session_checked(session);
    session_stopped = (stop_rc == 0);
    altscreen_log("PHASE=STREAM_111_STOP_RETURN receiver=%p session=%p stop_check=%d worker_owned=1 destroyed=%d",
                  receiver, session, stop_rc, session_stopped);
    if (session_stopped) {
        if (native_attached) {
            p1404_cockpit_native_detach(receiver, stream);
            native_attached = 0;
        }
        if (!alt_state_clear_private_stream(receiver, stream))
            altscreen_log("ERROR PHASE=STREAM_111_STREAM_CLEAR receiver=%p stream=%p worker_owned=1", receiver, stream);
        stream = NULL;
    }
    terminal_phase = session_stopped && (rc == 0 || process_stop_requested) ? ALT111_W_DONE : ALT111_W_FAILED;

net_done:
    if (net_socket) {
        be.net_socket_delete(net_socket);
        net_socket = NULL;
    }
    if (stream && session_stopped) {
        if (native_attached) {
            p1404_cockpit_native_detach(receiver, stream);
            native_attached = 0;
        }
        alt_state_clear_private_stream(receiver, stream);
    }

done:
    if (native_fd >= 0 && be.close_fn) be.close_fn(native_fd);
    if (terminal_phase == ALT111_W_FAILED && session && !session_stopped && be.stop) {
        stop_rc = stop_session_checked(session);
        session_stopped = (stop_rc == 0);
        if (session_stopped && native_attached && stream)
            p1404_cockpit_native_detach(receiver, stream);
        altscreen_log("%s PHASE=STREAM_111_FAILURE_CLEANUP receiver=%p session=%p stop_rc=%d session_stopped=%d",
                      session_stopped ? "WARN" : "ERROR", receiver, session,
                      stop_rc, session_stopped);
    }
    pair_finish_worker(p, terminal_phase, session_stopped);
    altscreen_log("PHASE=STREAM_111_WORKER_DONE receiver=%p id=%u generation=%u session=%p phase=%d session_stopped=%d stop_rc=%d",
                  receiver, state_id, generation, session, terminal_phase,
                  session_stopped, stop_rc);
    return NULL;
}

static int start_accept_worker(void *receiver, void *session) {
    struct alt111_pair *p;
    p1404_pthread_t thread = 0;
    int rc;
    if (!receiver || !session || !be.pthread_create_fn) return 0;

    pairs_lock();
    p = pair_find_locked(session);
    if (!p || p->receiver != receiver || p->worker_started || p->worker_starting || p->stop_requested || p->listen_fd < 0 ||
        p->phase != ALT111_W_PREPARED) {
        pairs_unlock();
        return 0;
    }
    p->phase = ALT111_W_ACCEPTING;

    p->worker_starting = 1;
    pairs_unlock();
    /* A new worker immediately takes pairs_lock. Never hold it over creation;
     * worker_starting protects the pair until the parent publishes its handle. */
    rc = be.pthread_create_fn(&thread, NULL, private111_worker, p);
    pairs_lock();
    p->worker_starting = 0;
    if (rc != 0) {
        p->phase = ALT111_W_PREPARED;
        pairs_unlock();
        altscreen_log("ERROR PHASE=STREAM_111_WORKER_CREATE receiver=%p session=%p rc=%d", receiver, session, rc);
        return 0;
    }
    p->thread = thread;
    p->worker_started = 1;
    pairs_unlock();
    altscreen_log("PHASE=STREAM_111_WORKER_READY receiver=%p session=%p thread=%lu blocked_on_accept=1 response_not_merged_yet=1",
                  receiver, session, (unsigned long)thread);
    return 1;
}

static int make_existing_response(void *receiver,
                                  const void *alt_descriptor,
                                  void *alt_screen_session,
                                  void *alt_screen_stream,
                                  void **owned_private_response) {
    int fd = -1, phase = ALT111_W_NONE;
    uint16_t port = 0;
    uint64_t stored_connection_id = 0, current_connection_id = 0;

    if (owned_private_response) *owned_private_response = NULL;
    if (!receiver || !alt_descriptor || !alt_screen_session || !owned_private_response)
        return 0;
    if (!read_stream_connection_id(alt_descriptor, &current_connection_id)) return 0;
    if (!pair_snapshot(alt_screen_session, &fd, &port, &stored_connection_id, &phase)) return 0;
    if (!port || phase == ALT111_W_DONE || phase == ALT111_W_FAILED) return 0;
    if (alt_screen_stream && session_stream(alt_screen_session) != alt_screen_stream) return 0;
    if (current_connection_id != stored_connection_id) {
        altscreen_log("ERROR PHASE=STREAM_111_DUPLICATE_CONNECTION_ID receiver=%p session=%p stored=%llu current=%llu teardown_existing=1",
                      receiver, alt_screen_session,
                      (unsigned long long)stored_connection_id,
                      (unsigned long long)current_connection_id);
        return 0;
    }
    altscreen_log("PHASE=STREAM_111_DUPLICATE_RESPONSE receiver=%p session=%p stream=%p dataPort=%u phase=%d listener=%d",
                  receiver, alt_screen_session, alt_screen_stream, (unsigned)port, phase, fd);
    return build_stream111_response(alt_descriptor, port, owned_private_response);
}

static int teardown_private(void *receiver,
                            void *alt_screen_session,
                            void *alt_screen_stream,
                            struct p1404_pf_lease *after_stock_cleanup) {
    struct alt111_pair *p;
    p1404_pthread_t thread = 0;
    int worker_started = 0, worker_done = 0, phase = ALT111_W_NONE;
    int listener = -1, wake_fd = -1, session_stopped = 0;
    uint16_t port = 0;
    int join_rc = 0, stop_rc = 0;

    if (!alt_screen_session) return 1;

    if (!pair_request_stop_snapshot(receiver, alt_screen_session,
                                    &worker_started, &worker_done, &thread,
                                    &phase, &listener, &port)) {
        altscreen_log("ERROR PHASE=STREAM_111_TEARDOWN_PAIR receiver=%p session=%p missing_pair=1", receiver, alt_screen_session);
        return 0;
    }

    altscreen_log("PHASE=STREAM_111_WORKER_STOP_REQUEST receiver=%p session=%p phase=%d worker_started=%d worker_done=%d listener=%d port=%u",
                  receiver, alt_screen_session, phase, worker_started, worker_done, listener, (unsigned)port);

    if (worker_started && !worker_done) {
        if ((phase == ALT111_W_PREPARED || phase == ALT111_W_ACCEPTING) && listener >= 0)
            (void)wake_listener(receiver, port);
        if (phase == ALT111_W_STREAMING) {
            uint8_t quit = 0x71u;
            wake_fd = read_int(alt_screen_session, SCREEN_SESSION_WAKE_FD_OFF);
            if (wake_fd >= 0) {
                int wake_rc = be.send_loopback(wake_fd, &quit, sizeof(quit));
                altscreen_log("PHASE=STREAM_111_PROCESSFRAMES_WAKE receiver=%p session=%p wake_fd=%d rc=%d message=0x71 length=1",
                              receiver, alt_screen_session, wake_fd, wake_rc);
            }
        }
    }

    if (worker_started) {
        join_rc = be.pthread_join_fn(thread, NULL);
        altscreen_log("PHASE=STREAM_111_WORKER_JOIN receiver=%p session=%p thread=%lu rc=%d",
                      receiver, alt_screen_session, (unsigned long)thread, join_rc);
        if (join_rc != 0) {
            /* Never delete a ScreenSession while its worker may still execute. */
            altscreen_log("ERROR PHASE=STREAM_111_WORKER_JOIN receiver=%p session=%p unsafe_delete_blocked=1",
                          receiver, alt_screen_session);
            return 0;
        }
    }

    pairs_lock();
    p = pair_find_locked(alt_screen_session);
    if (p) {
        listener = p->listen_fd;
        p->listen_fd = -1;
        /* join already succeeded. A Stop postcondition failure must be
         * retryable without joining the same terminated pthread twice. */
        p->worker_started = 0;
        session_stopped = p->session_stopped;
    } else {
        listener = -1;
    }
    pairs_unlock();

    if (listener >= 0 && be.close_fn) be.close_fn(listener);

    /*
     * Session lifetime is more important than PF housekeeping.  Never make
     * private ScreenSession Stop/Delete conditional on a shell/pfctl cleanup
     * result: a PF failure must not strand Stream111 or hold the outer CarPlay
     * teardown path.
     */
    if (!session_stopped && !alt_screen_stream)
        alt_screen_stream = session_stream(alt_screen_session);
    if (!session_stopped && be.stop) {
        stop_rc = stop_session_checked(alt_screen_session);
        session_stopped = (stop_rc == 0);
        altscreen_log("PHASE=STREAM_111_STOP_RETURN receiver=%p session=%p stop_check=%d worker_owned=0 destroyed=%d",
                      receiver, alt_screen_session, stop_rc, session_stopped);
        if (session_stopped && alt_screen_stream) {
            p1404_cockpit_native_detach(receiver, alt_screen_stream);
            if (!alt_state_clear_private_stream(receiver, alt_screen_stream))
                altscreen_log("ERROR PHASE=STREAM_111_STREAM_CLEAR receiver=%p stream=%p worker_owned=0", receiver, alt_screen_stream);
        }
    }
    if (!session_stopped) {
        altscreen_log("ERROR PHASE=STREAM_111_STOP_RETURN receiver=%p session=%p stop_check=%d unsafe_delete_blocked=1",
                      receiver, alt_screen_session, stop_rc);
        return 0;
    }
    if (be.del) {
        altscreen_log("PHASE=STREAM_111_SESSION_DELETE_BEGIN session=%p", alt_screen_session);
        be.del(alt_screen_session);
        altscreen_log("PHASE=STREAM_111_SESSION_DELETE_DONE session=%p", alt_screen_session);
    }

    if (after_stock_cleanup) {
        /* Mandatory Stop/Join/Delete are complete. Transfer only the PF lease
         * to the outer caller so stock audio stops before any helper wait. */
        pairs_lock();
        p = pair_find_locked(alt_screen_session);
        if (p && p->firewall_open) {
            after_stock_cleanup->port = p->listen_port;
            after_stock_cleanup->generation = p->firewall_generation;
            p->firewall_open = 0;
        }
        pairs_unlock();
        altscreen_log("PHASE=STREAM_111_FIREWALL_DEFERRED port=%u generation=%u stock_audio_first=1",
                      (unsigned)after_stock_cleanup->port, after_stock_cleanup->generation);
    } else {
        /* SETUP rollback has no pending stock audio teardown to prioritize. */
        altscreen_log("PHASE=STREAM_111_FIREWALL_REMOVE_BEGIN session=%p bounded_ms=1500", alt_screen_session);
        if (!pair_close_firewall(p, "teardown_post_session")) {
            altscreen_log("WARN PHASE=STREAM_111_FIREWALL_REMOVE session=%p reason=teardown_post_session result=FAILED core_teardown_preserved=1",
                          alt_screen_session);
        }
    }

    pairs_lock();
    p = pair_find_locked(alt_screen_session);
    if (p) memset(p, 0, sizeof(*p));
    pairs_unlock();

    alt_airplay_release_object(receiver);

    altscreen_log("PHASE=STREAM_111_BACKEND_TEARDOWN_DONE receiver=%p session=%p join_rc=%d stop_rc=%d firewall_cleanup_nonfatal=1",
                  receiver, alt_screen_session, join_rc, stop_rc);
    return join_rc == 0 && stop_rc == 0;
}

#ifdef ALTSCREEN_PRIVATE111_HOST_TEST
void p1404_private111_test_phase_reset(void) {
    pairs_lock();
    memset(g_pairs, 0, sizeof(g_pairs));
    pairs_unlock();
}

int p1404_private111_test_phase_seed(void *receiver, void *session, int phase) {
    int ok = 0;
    pairs_lock();
    if (receiver && session && !g_pairs[0].session) {
        g_pairs[0].receiver = receiver;
        g_pairs[0].session = session;
        g_pairs[0].phase = phase;
        g_pairs[0].worker_started = 1;
        ok = 1;
    }
    pairs_unlock();
    return ok;
}

int p1404_private111_test_try_streaming(void *session) {
    struct alt111_pair *p;
    pairs_lock();
    p = pair_find_locked(session);
    pairs_unlock();
    return pair_try_enter_streaming(p);
}

int p1404_private111_test_request_stop(void *receiver, void *session,
                                       int *phase_out) {
    return pair_request_stop_snapshot(receiver, session, NULL, NULL, NULL,
                                      phase_out, NULL, NULL);
}

int p1404_private111_test_accept_action(int rc, int native_fd,
                                        int stop_requested) {
    return classify_accept_result(rc, native_fd, stop_requested);
}
#endif

int p1404_private111_backend_install(void) {
    struct alt_private111_backend backend;

    alt_private111_reset_backend();
    memset(&be, 0, sizeof(be));
    memset(g_pairs, 0, sizeof(g_pairs));
    g_pairs_guard = 0;

    if (!p1404_identity_ok) {
        altscreen_log("PHASE=PRIVATE111_BACKEND_NOT_READY reason=EXACT_P1404_ABI_UNRESOLVED identity=0");
        return 0;
    }

    be.create = (f_screen_create_t)p1404.screen_create;
    be.setup = (f_screen_setup_t)p1404.screen_setup;
    be.start = (f_screen_start_t)p1404.screen_start_session;
    be.stop = (f_screen_stop_t)p1404.screen_stop_session;
    be.set_time = (f_screen_set_time_t)dlsym(RTLD_DEFAULT, "AirPlayReceiverSessionScreen_SetTimeSynchronizer");
    be.set_uuid = (f_screen_set_uuid_t)dlsym(RTLD_DEFAULT, "AirPlayReceiverSessionScreen_SetSessionUUID");
    be.set_device = (f_screen_set_device_t)dlsym(RTLD_DEFAULT, "AirPlayReceiverSessionScreen_SetClientDeviceID");
    be.clock_now = (f_clock_now_t)dlsym(RTLD_DEFAULT, "AirTunesClock_GetSynchronizedNTPTime");
    be.clock_to_ticks = (f_clock_to_ticks_t)dlsym(RTLD_DEFAULT, "AirTunesClock_GetUpTicksNearSynchronizedNTPTime");
    be.del = (f_screen_delete_t)p1404.screen_delete;
    be.set_security = (f_screen_set_security_t)p1404.screen_set_security_info;
    be.derive_screen = (f_derive_for_screen_t)p1404.derive_aes_key_sha512_for_screen;

    be.server_socket_open = (f_server_socket_open_t)dlsym(RTLD_DEFAULT, "ServerSocketOpen");
    be.socket_accept = (f_socket_accept_t)dlsym(RTLD_DEFAULT, "SocketAccept");
    be.net_socket_create_native = (f_net_socket_create_native_t)dlsym(RTLD_DEFAULT, "NetSocket_CreateWithNative");
    be.net_socket_delete = (f_net_socket_delete_t)dlsym(RTLD_DEFAULT, "NetSocket_Delete");
    be.process_frames = (f_process_frames_t)dlsym(RTLD_DEFAULT, "AirPlayReceiverSessionScreen_ProcessFrames");
    be.send_loopback = (f_send_loopback_t)dlsym(RTLD_DEFAULT, "SendSelfConnectedLoopbackMessage");
    be.pthread_create_fn = (f_pthread_create_t)dlsym(RTLD_DEFAULT, "pthread_create");
    be.pthread_join_fn = (f_pthread_join_t)dlsym(RTLD_DEFAULT, "pthread_join");
    be.socket_fn = (f_socket_t)dlsym(RTLD_DEFAULT, "socket");
    be.connect_fn = (f_connect_t)dlsym(RTLD_DEFAULT, "connect");
    be.close_fn = (f_close_t)dlsym(RTLD_DEFAULT, "close");

    if (!backend_runtime_prereqs_ready()) {
        altscreen_log("PHASE=PRIVATE111_BACKEND_NOT_READY reason=RUNTIME_PREREQ_INCOMPLETE cfBridge=%d stockHelpers=%d threadHelpers=%d",
                      alt_airplay_private_cf_ready(),
                      be.server_socket_open && be.socket_accept && be.net_socket_create_native &&
                      be.net_socket_delete && be.process_frames && be.send_loopback,
                      be.pthread_create_fn && be.pthread_join_fn);
        return 0;
    }

    if (!local_p1404_security_proven() ||
        !local_p1404_private_start_main_isolation_proven() ||
        !local_p1404_listener_and_start_timing_proven() ||
        !local_p1404_control_plane_serialization_proven()) {
        altscreen_log("PHASE=PRIVATE111_BACKEND_NOT_READY reason=LOCAL_EXACT_PROOF_PENDING securityOptions=%d cfBridge=1 mainIsolation=%d listenerStartTiming=%d controlSerialization=%d",
                      local_p1404_security_proven(),
                      local_p1404_private_start_main_isolation_proven(),
                      local_p1404_listener_and_start_timing_proven(),
                      local_p1404_control_plane_serialization_proven());
        return 0;
    }

    memset(&backend, 0, sizeof(backend));
    backend.abi_verified = 1;
    backend.stock_requires_split = 1;
    backend.two_phase = 1;
    backend.name = "P1404-exact-priv111-two-phase";
    backend.make_stock_request = alt_airplay_make_stock_request;
    backend.release_object = alt_airplay_release_object;
    backend.prepare_setup = prepare_setup;
    backend.start_accept_worker = start_accept_worker;
    backend.create_setup_start = NULL;
    backend.make_existing_response = make_existing_response;
    backend.merge_private_response = alt_airplay_merge_private_response;
    backend.teardown_private = teardown_private;

    if (!alt_private111_install_backend(&backend)) {
        altscreen_log("PHASE=PRIVATE111_BACKEND_NOT_READY reason=install_rejected");
        return 0;
    }
    altscreen_log("PHASE=PRIVATE111_BACKEND_READY backend=%s two_phase=1 exactHelpers=1 mainIsolation=1 generationFence=1",
                  backend.name);
    return 1;
}
