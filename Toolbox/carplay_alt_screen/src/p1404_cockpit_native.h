/*
 * private111 stock-decoder compatibility adapter.
 *
 * Direct-display V1 exports decoded private NV12 before stock CScreenRender and
 * sends it to the separate displayable3/Context80 sidecar.  The legacy
 * displayable58 binding remains only to keep stock OMX lifecycle intact during
 * this fallback stage; Window58 is not read or used as the direct display
 * source. MainScreen type110 remains on the untouched stock path.
 */
#ifndef P1404_COCKPIT_NATIVE_H
#define P1404_COCKPIT_NATIVE_H

#include <stdint.h>

#define ALT111_TARGET_DISPLAY_ID      1u
#define ALT111_DISPLAYABLE_ID        58u
#define ALT111_COCKPIT_CONTEXT       76u /* historical only; native writer disabled */
#define ALT111_JAVA_CONTEXT          80u
#define ALT111_STOCK_CONTEXT         74u
#define ALT111_TRANSITION_CONTEXT    72u

/* Exact 44-byte dio::st_screen_config layout recovered from libairplay.so. */
struct p1404_screen_config {
    uint32_t window_width;
    uint32_t window_height;
    uint32_t source_width;
    uint32_t source_height;
    int32_t  offset_x;
    int32_t  offset_y;
    uint32_t display_type;
    uint32_t buffer_count;
    uint32_t format;
    uint32_t window_id;
    uint32_t usage;
};

/* Bind stock renderer methods and discover the target display's live geometry. */
int p1404_cockpit_native_bind_stock(void);
int p1404_cockpit_native_refresh_geometry(void);
int p1404_cockpit_native_get_geometry(uint32_t *width, uint32_t *height);
void p1404_cockpit_native_set_test_geometry(uint32_t width, uint32_t height);

/* Arm one thread-qualified private StartSession handoff. ScreenCopyMain returns
 * a fresh runtime Alt display only for that worker; ScreenStreamStart stages the
 * private stream and the first CScreenRender::config binds its new renderer before
 * forwarding. Any missing/mismatched handoff fails without exposing Main110. */
int  p1404_cockpit_native_prepare_start(void *receiver);
int  p1404_cockpit_native_complete_start(void *receiver, void **stream);

/* Attach/detach the exact private stream. Attach fails closed if its stock OMX
 * renderer cannot be identified, preventing type 111 from falling through to
 * MainScreen's displayable 59. */
int  p1404_cockpit_native_attach(void *receiver, void *stream);
void p1404_cockpit_native_detach(void *receiver, void *stream);
/* Set true immediately before ProcessFrames; clear before StopSession.
 * Clearing joins any recovery command submission without joining the monitor. */
void p1404_cockpit_native_processing(void *receiver, void *stream, int live);

/* Screen calls reached from stock CScreenRender::config. The private config
 * scope replaces its static window group with DisplayManager registration;
 * calls outside that scope are exact stock passthrough. */
int screen_create_window_group(void *window, const char *name);
int screen_create_window_buffers(void *window, int count);

/* Geometry transform uses the discovered/requested size; no fixed pixel size. */
int p1404_cockpit_native_rewrite_config(const struct p1404_screen_config *input,
                                        struct p1404_screen_config *output);

#endif
