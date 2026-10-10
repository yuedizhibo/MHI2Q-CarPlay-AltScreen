#ifndef PRIVATE111_DIRECT_TAP_H
#define PRIVATE111_DIRECT_TAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * All functions are fail-open. A tap failure must never change the return
 * value or timing contract of stock CarPlay Main110/private111 processing.
 */

/* Cache the stock ScreenStream "avcc" codec configuration.  This call never
 * creates SHM or claims a stream private by itself; the cache is emitted only
 * after the existing private111 identity gate observes ProcessData. */
void p111_h264_tap_note_avcc(void *stream, const void *data, size_t bytes);

void p111_h264_tap_write(void *stream, const void *data, size_t bytes);

/* Publish a known-linear NV12 frame into /carplay111_decoded.
 * Returns non-zero only when a new SHM frame was actually committed. */
int p111_frame_tap_write(void *stream, const unsigned char *buffer,
                         uint32_t width, uint32_t height,
                         uint32_t format, uint32_t usage);

/*
 * Read-only, process-local decoded-frame progress for control-plane pacing.
 * This does not change the /carplay111_decoded SHM ABI and never touches the
 * renderer. last_publish_us32 is modular microseconds from gettimeofday();
 * callers only use it for short (< seconds) age checks.
 */
struct p111_frame_progress_snapshot {
    uint32_t generation;
    uint32_t frame_count;
    uint32_t sequence;
    uint32_t last_publish_us32;
    uint32_t h264_packets;
    int active;
};

int p111_frame_tap_get_progress(
    void *stream, struct p111_frame_progress_snapshot *out);

/*
 * V2 preferred path. Call this only after stock CScreenRender::render() has
 * posted the decoded vendor buffer. Screen is then asked to read the exact
 * stock window into a normal pixmap, which lets the platform linearize the
 * vendor 0x0001000c layout before the existing packed-NV12 SHM contract.
 *
 * Returns non-zero when the frame was published, deliberately frame-paced,
 * or a post-success transient failure is handled by freezing the last good
 * auxiliary frame. Returns zero only before the first good auxiliary frame
 * when Screen linearization cannot provide a safe pixel source. Raw vendor
 * buffers are never published as a fallback.
 */
int p111_frame_tap_write_window(void *stream, void *screen_window,
                                uint32_t width, uint32_t height,
                                uint32_t source_format,
                                uint32_t source_usage);

void p111_direct_tap_stream_end(void *stream);

#endif
