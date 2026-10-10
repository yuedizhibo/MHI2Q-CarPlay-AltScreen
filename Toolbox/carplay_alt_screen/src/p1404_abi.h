/*
 * p1404_abi.h - P1404 (MHI2Q_CN_AUG22_P1404) CarPlay stack ABI contract.
 *
 * Every address in this file was measured from the binaries stored in this
 * repository under Dump/MHI2Q_CN_AUG22_P1404/unknown/CarPlayProtocol,
 * using Research/AltScreen/P1404_ALTSCR_ABI_MAP.md as the index. No offset is
 * guessed: runtime identity proof must succeed before any derived entry is used.
 *
 * Important: do not put generic CoreFoundation prototypes in this file merely
 * because they match public CF headers. P1404 carries CF/CFL convenience exports
 * whose call shapes must be measured independently. Implementation-local adapter
 * typedefs belong next to the dlsym/use site until their exact P1404 ABI is proven.
 */
#ifndef P1404_ABI_H
#define P1404_ABI_H

#include <stddef.h>
#include <stdint.h>
#include "altscreen_core.h"

#define P1404_FIRMWARE_ID            "MHI2Q_CN_AUG22_P1404"
#define MHI2Q_AUG22_TRAIN_PREFIX     "MHI2Q_CN_AUG22"
#define P1404_ABI_BASELINE_ID        "MHI2Q_CN_AUG22_P1404"
#define P1404_HOST_PROCESS           "dio_manager"
#define K1004_HOST_PROCESS           "smartphone_integrator"
#ifdef ALTSCREEN_DIRECT_PROXY
#define P1404_LIBAIRPLAY_PATH_APP    "/mnt/app/root/lib-target/libairplax.so"
#define P1404_LIBAIRPLAY_PATH_ESO    "/mnt/app/root/lib-target/libairplax.so"
#if defined(ALTSCREEN_PROFILE_P1404) && ALTSCREEN_PROFILE_P1404
#define P1404_DIO_PATH_APP           "/mnt/app/eso/bin/apps/dio_manager"
#define P1404_DIO_PATH_ESO           "/eso/bin/apps/dio_manager"
#else
#define P1404_DIO_PATH_APP           "/mnt/app/eso/bin/apps/smartphone_integrator"
#define P1404_DIO_PATH_ESO           "/eso/bin/apps/smartphone_integrator"
#endif
#else
#define P1404_LIBAIRPLAY_PATH_APP    "/mnt/app/eso/lib/libairplay.so"
#define P1404_LIBAIRPLAY_PATH_ESO    "/eso/lib/libairplay.so"
#define P1404_DIO_PATH_APP           "/mnt/app/eso/bin/apps/dio_manager"
#define P1404_DIO_PATH_ESO           "/eso/bin/apps/dio_manager"
#endif

#define LIBAIRPLAY_SIZE_BYTES        761564u
#define LIBAIRPLAY_CKSUM             1012372429u
#define DIO_MANAGER_SIZE_BYTES       691569u
#define DIO_MANAGER_CKSUM            1260773095u
#define K1004_LIBAIRPLAY_SIZE_BYTES  761564u
#define K1004_LIBAIRPLAY_CKSUM       2678655048u
#define K1004_DIO_MANAGER_SIZE_BYTES 694029u
#define K1004_DIO_MANAGER_CKSUM      3274834670u
#define K1004_ABI_BASELINE_ID        "MHI2Q_CN_AUG22_K1004"
#define K1004_OFFSET_DELTA           0x18u
#ifndef ALTSCREEN_STOCK_OFFSET_DELTA
#define ALTSCREEN_STOCK_OFFSET_DELTA K1004_OFFSET_DELTA
#endif

#define OFF_AIRPLAY_SERVER_PLATFORM_COPY_PROPERTY   0x01fcf8u
#define OFF_AIRPLAY_RECEIVER_SERVER_CREATE           0x020a30u
#define OFF_AIRPLAY_SESSION_PLATFORM_COPY_PROPERTY  0x01d5b0u
#define OFF_AIRPLAY_SESSION_PLATFORM_CONTROL        0x01f080u
#define OFF_AIRPLAY_SCREEN_COPY_DISPLAYS_INFO       0x02ab10u
#define OFF_AIRPLAY_SCREEN_CREATE                   0x02afccu
#define OFF_AIRPLAY_SCREEN_SETUP                    0x02a7f0u
#define OFF_AIRPLAY_SCREEN_START_SESSION            0x02a340u
#define OFF_AIRPLAY_SCREEN_STOP_SESSION             0x02a258u
#define OFF_AIRPLAY_SCREEN_DELETE                   0x02a7c0u
#define OFF_AIRPLAY_SCREEN_SET_VISIBLE              0x029e08u
#define OFF_SCREEN_STREAM_PROCESS_DATA              0x084a04u
#define OFF_SCREEN_STREAM_CREATE                    0x0856fcu
#define OFF_AIRPLAY_SESSION_SETUP                   0x027240u
#define OFF_AIRPLAY_SESSION_CREATE                  0x029698u
#define OFF_AIRPLAY_SESSION_TEARDOWN                0x026a50u
#define OFF_SCREEN_STREAM_START                     0x0847b8u
#define OFF_AIRPLAY_SCREEN_SET_SECURITY_INFO        0x02a960u
#define OFF_AIRPLAY_DERIVE_AES_KEY_SHA512_FOR_SCREEN 0x02d584u
#define OFF_SCREEN_COPY_MAIN                        0x084058u
#define OFF_SCREEN_CREATE                           0x083d98u
#define OFF_SCREEN_COPY_DELEGATES                   0x08341cu
#define OFF_SCREEN_REGISTER_DELEGATES               0x0834e4u
#define OFF_NET_SOCKET_WRITE_INTERNAL               0x07eec4u
#define OFF_NET_SOCKET_DELETE                       0x07ec18u
#define OFF_NET_SOCKET_DISCONNECT                   0x07d890u
#define SIZE_NET_SOCKET_WRITE_INTERNAL              240u
#define SIZE_NET_SOCKET_DELETE                      184u
#define SIZE_NET_SOCKET_DISCONNECT                  400u
#define OFF_CFL_RETAIN                              0x04fefcu
#define OFF_CSCREEN_RENDER                          0x08b048u
#define OFF_CSCREEN_CONFIG                          0x08b6ccu

#define SCREEN_STREAM_MAX_INSTANCES                 50

#define IAP2_CSM_IDENTIFICATION_INFORMATION         0x1d01u
#define IAP2_CSM_CARPLAY_AVAILABILITY               0x4300u
#define IAP2_CSM_START_IDENTIFICATION               0x1d00u
#define IAP2_LINK_START_OF_PACKET                   0xffffu
#define IAP2_LINK_HEADER_LEN                        9u

struct p1404_symbols {
    void *server_platform_copy_property;
    void *session_platform_copy_property;
    void *session_platform_control;
    void *screen_copy_displays_info;
    void *screen_create;
    void *screen_setup;
    void *screen_start_session;
    void *screen_stop_session;
    void *screen_delete;
    void *screen_set_visibility;
    void *screen_stream_process_data;
    void *session_setup;
    void *screen_stream_start;
    void *screen_stream_create;
    void *screen_set_security_info;
    void *derive_aes_key_sha512_for_screen;
    void *screen_copy_main;
    /* Exported CF/CFL entry addresses. Keep them opaque here until each raw
     * call shape is measured; callers use implementation-local adapter typedefs. */
    void *cf_dictionary_create_mutable;
    void *cf_dictionary_set_value;
    void *cf_dictionary_get_value;
    void *cf_dictionary_get_count;
    void *cf_array_create_mutable;
    void *cf_array_append_value;
    void *cf_array_get_count;
    void *cf_array_get_value;
    void *cf_array_create_copy;
    void *cf_string_create_with_cstring;
    void *cf_string_get_cstring;
    void *cf_number_create_int64;
    void *cf_number_get_value;
    void *cf_retain;
    void *cf_release;
    void *cfl_string_get_cstring_ptr;
};

extern struct p1404_symbols p1404;
extern int p1404_identity_ok;
extern int p1404_armed;
extern int p1404_mutate_armed;
extern unsigned long p1404_libairplay_base;

typedef void *cf_obj;

/* Bind the six public AirPlay entry points to their stock libairplay exports.
 * The two CScreenRender wrappers bind independently through
 * p1404_stock_symbol_named(). A refused probe still forwards normal CarPlay;
 * observation and mutation remain gated by p1404_probe_stack. */
int  p1404_bind_stock_exports(void);
/* Resolve a stock symbol after this preload, with the same handle-scoped
 * fallback used by the public fail-open wrappers. */
void *p1404_stock_symbol_named(const char *name);
/* Exact K1004 direct-overlay fallback. This uses a relocation-resolved stock
 * CFLRetain anchor, so forwarding is available even while dependency
 * constructors are still running and QNX dlsym cannot yet see libairplax. */
void *p1404_direct_stock_symbol_named(const char *name);
/* K1004/QNX binds libairplax internal PLT/GOT references back to itself rather
 * than ELF-preempting them through the proxy. Install the exact measured GOT
 * redirects before dio_manager main; refusal leaves runtime permanently inert. */
int  p1404_direct_install_internal_redirects(void);
int  p1404_direct_internal_redirects_ready(void);
int  p1404_probe_stack(void);
void altscreen_runtime_ensure_initialized(void) __attribute__((weak));
int  altscreen_runtime_is_ready(void) __attribute__((weak));
const char *p1404_process_name(void);
int  process_is_allowed(const char *name);
int  p1404_read_marker(const char *path);

struct altscreen_ctx {
    void     *receiver_session;
    void     *alt_screen_session;
    void     *alt_screen_stream;
    uint64_t  video_bytes;
    uint64_t  video_packets;
    uint64_t  first_seen_at;
    uint32_t  stream_type;
    uint32_t  id;
    uint32_t  generation;
    uint32_t  nal_sps;
    uint32_t  nal_pps;
    uint32_t  nal_idr;
    uint32_t  nal_other;
    uint8_t   nal_carry[4];
    uint8_t   nal_carry_len;
    int       state;
    int       process_frames_started;
    int       video_config_seen;
    int       ui_active;
    int       decoder_ready;
    int       cockpit_visible;
    int       main_blackout_guard;
};

struct altscreen_ctx *alt_state_register(void *receiver_session);
struct altscreen_ctx *alt_state_lookup(void *receiver_session);
struct altscreen_ctx *alt_state_lookup_any(void *receiver_session);
struct altscreen_ctx *alt_state_lookup_stream(void *stream);
int  alt_state_snapshot(void *receiver_session, int committed_only,
                        struct altscreen_ctx *out);
int  alt_state_set_generation(void *receiver_session, uint32_t generation);
void alt_state_table_lock(void);
void alt_state_table_unlock(void);
struct altscreen_ctx *alt_state_lookup_any_locked(void *receiver_session);
struct altscreen_ctx *alt_state_lookup_locked(void *receiver_session);
struct altscreen_ctx *alt_state_register_locked(void *receiver_session);
int  alt_state_stage_private(void *receiver_session, void *alt_screen_session,
                             void *alt_screen_stream);
int  alt_state_commit_private(void *receiver_session);
int  alt_state_bind_private(void *receiver_session, void *alt_screen_session,
                            void *alt_screen_stream);
void alt_state_mark_video_config(struct altscreen_ctx *c, const char *proof);
void alt_state_mark_ui_active(struct altscreen_ctx *c, const char *proof);
void alt_state_mark_decoder_ready(struct altscreen_ctx *c, const char *proof);
void alt_state_mark_cockpit_visible(struct altscreen_ctx *c, const char *proof);
void alt_state_mark_cockpit_hidden(struct altscreen_ctx *c, const char *proof);
#define ALT_STATE_NATIVE_VIDEO_CONFIG    1
#define ALT_STATE_NATIVE_UI_ACTIVE       2
#define ALT_STATE_NATIVE_DECODER_READY   3
#define ALT_STATE_NATIVE_COCKPIT_VISIBLE 4
#define ALT_STATE_NATIVE_COCKPIT_HIDDEN  5
void alt_state_mark_native(void *receiver_session, uint32_t generation,
                           int event_kind, const char *proof);
int  alt_state_feed_private_video(void *stream, const void *data, size_t bytes);
void alt_state_unregister(void *receiver_session);
void alt_state_count_video(struct altscreen_ctx *c, size_t bytes, int h264_nal_present);
int  alt_state_video_total(void *stream, size_t len, uint64_t *bytes_out, uint64_t *calls_out);
void alt_state_forget_stream(void *stream);
int  alt_state_streams_live(void);
int  alt_state_live_count(void);
void alt_state_reset(void);

#define ALT111_EVENT_SHOW_UI          1
#define ALT111_EVENT_FORCE_KEYFRAME   2
#define ALT111_EVENT_STOP_UI          3
#define ALT111_EVENT_UPDATE_VIEW_AREA 4
#define ALT111_EVENT_ZOOM             5
#define ALT111_EVENT_RECOVERY_KEYFRAME 6
int alt_send_cluster_recovery(void *receiver, void *stream,
                               uint32_t generation, uint32_t event_seq);
void p1404_cockpit_native_recovery_result(void *receiver, void *stream,
    uint32_t generation, uint32_t event_seq, int status, int response_received);
int alt_send_cluster_event(void *receiver, void *stream, uint32_t generation,
                           int event_kind);
int alt_send_cluster_view_area(void *receiver, void *stream,
                               uint32_t generation, uint32_t event_seq,
                               int view_area_index);
int alt_send_cluster_zoom(void *receiver, void *stream,
                          uint32_t generation, uint32_t event_seq,
                          int direction);
void p1404_cockpit_native_event_result(void *receiver, void *stream,
                                        uint32_t generation, int event_kind,
                                        int status, int response_received);
void p1404_cockpit_native_view_area_result(void *receiver, void *stream,
                                            uint32_t generation,
                                            uint32_t event_seq,
                                            int view_area_index,
                                            int status,
                                            int response_received);
void p1404_cockpit_native_zoom_result(void *receiver, void *stream,
                                      uint32_t generation,
                                      uint32_t event_seq,
                                      int direction,
                                      int status,
                                      int response_received);

#endif /* P1404_ABI_H */
