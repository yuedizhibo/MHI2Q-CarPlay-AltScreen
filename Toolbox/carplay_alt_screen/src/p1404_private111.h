/* p1404_private111.h - ABI-neutral one-shot private stream111 transaction. */
#ifndef P1404_PRIVATE111_H
#define P1404_PRIVATE111_H
#include "p1404_firewall.h"

#define ALT111_PREP_BLOCKED     (-1)
#define ALT111_PREP_PASSTHROUGH   0
#define ALT111_PREP_STOCK_ONLY    1

#define ALT111_FINISH_FAIL_OPEN (-1)
#define ALT111_FINISH_NO_ALT      0
#define ALT111_FINISH_READY       1

/*
 * The orchestration layer deliberately knows nothing about P1404 native object
 * layouts. Every backend callback returns 1 on success and 0 on failure.
 *
 * two_phase=1 is the stock-equivalent contract and is the only mode that may be
 * promoted for private111:
 *   prepare_setup: Create+Setup+security+listener+response, NO StartSession.
 *   start_accept_worker: create a worker that is ready and blocked on accept.
 *   orchestration: merge final response, then commit G5B.
 *   worker: only after the phone connects -> StartSession -> bind real stream ->
 *           ProcessFrames -> StopSession.
 *
 * create_setup_start remains temporarily for host/regression compatibility with
 * the older candidate. A formal P1404 backend must set two_phase=1.
 */
struct alt_private111_backend {
    int abi_verified;
    int stock_requires_split;
    int two_phase;
    const char *name;

    int (*make_stock_request)(void *receiver_session,
                              const void *original_request,
                              const void *alt_descriptor,
                              void **owned_stock_request);

    void (*release_object)(void *object);

    /* Preferred stock-equivalent path. Returns a private ScreenSession and an
     * owned response containing type=111/dataPort. It MUST NOT call StartSession
     * and MUST NOT invent a ScreenStream pointer. */
    int (*prepare_setup)(void *receiver_session,
                         const void *alt_descriptor,
                         void **alt_screen_session,
                         void **owned_private_response);

    /* Called after the session has been staged but before response merge. On
     * success a worker exists and is able to block on the advertised listener.
     * Therefore a later merge failure can close the listener, join the worker and
     * roll back without ever exposing a dead dataPort to the phone. */
    int (*start_accept_worker)(void *receiver_session,
                               void *alt_screen_session);

    /* Legacy pre-two-phase candidate. Formal promotion must not use this. */
    int (*create_setup_start)(void *receiver_session,
                              const void *alt_descriptor,
                              void **alt_screen_session,
                              void **alt_screen_stream,
                              void **owned_private_response);

    /* A repeated/reconfigure SETUP must still return a current 111 response, but
     * must not recreate the committed private receiver. alt_screen_stream may be
     * NULL while the two-phase worker is still waiting for the phone data socket. */
    int (*make_existing_response)(void *receiver_session,
                                  const void *alt_descriptor,
                                  void *alt_screen_session,
                                  void *alt_screen_stream,
                                  void **owned_private_response);

    /* TRANSACTIONAL final-response merge contract. On failure stock_response must
     * keep its pre-call stock semantics. */
    int (*merge_private_response)(void *stock_response,
                                  void *private_response);

    /* Must cancel/close a pending listener, stop any live ProcessFrames worker,
     * join it before deleting the ScreenSession, and tolerate alt_screen_stream
     * being NULL in the two-phase pre-accept state. */
    int (*teardown_private)(void *receiver_session,
                            void *alt_screen_session,
                            void *alt_screen_stream,
                            struct p1404_pf_lease *after_stock_cleanup);
};

int  alt_private111_install_backend(const struct alt_private111_backend *backend);
void alt_private111_reset_backend(void);
int  alt_private111_backend_ready(void);
const char *alt_private111_backend_name(void);

int  alt_private111_prepare_stock_policy(void *receiver_session,
                                         const void *original_request,
                                         const void *alt_descriptor,
                                         int allow_private,
                                         const void **stock_request_out,
                                         int *stock_request_owned_out);
int  alt_private111_prepare_stock(void *receiver_session,
                                  const void *original_request,
                                  const void *alt_descriptor,
                                  const void **stock_request_out,
                                  int *stock_request_owned_out);
void alt_private111_release_prepared(const void *stock_request, int owned);

int  alt_private111_finish_setup(void *receiver_session,
                                 const void *alt_descriptor,
                                 int stock_rc,
                                 void *stock_response);
int  alt_private111_teardown(void *receiver_session, const char *reason);
/* Mandatory worker/session destruction first; value-only PF cleanup afterward.
 * Caller must finish cleanup after stock audio teardown, before leaving its
 * receiver lifecycle lease. Other rollback paths use synchronous teardown. */
int  alt_private111_teardown_deferred(void *receiver_session, const char *reason,
                                     struct p1404_pf_lease *after_stock_cleanup);
void alt_private111_finish_cleanup(struct p1404_pf_lease *cleanup);

#endif
