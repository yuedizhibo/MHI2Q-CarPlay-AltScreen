#ifndef PRIVATE111_DIRECT_SOURCE_H
#define PRIVATE111_DIRECT_SOURCE_H

#include <stdlib.h>
#include "video_frame.h"
#include "private111_direct_shm.h"
#include <stdint.h>


class Private111DirectSource {
public:
    explicit Private111DirectSource(bool verbose);
    ~Private111DirectSource();

    bool init();
    bool read_frame(VideoFrame *frame);
    bool current_session_active();
    /**
     * Check whether the mapped decoded-frame session still matches the source identity.
     * Parameters: none.
     * Returns: true when H264 and decoded-frame identities are active and equal.
     */
    bool current_frame_session_active();
    /**
     * Validate and copy the producer's startup marker frame for session recovery.
     * Parameters: marker is the ready-marker path; frame receives the validated NV12 frame.
     * Returns: true when the marker, live producer, and copied frame all match.
     */
    bool read_verified_startup_frame(const char *marker, VideoFrame *frame);
    void shutdown();

    bool h264_ready() const { return h264_ready_; }
    bool decoded_ready() const { return decoded_ready_; }
    uint32_t generation() const { return generation_; }
    uint32_t writer_pid() const { return writer_pid_; }
    uint32_t stream_cookie() const { return stream_cookie_; }
    uint32_t h264_packets() const { return last_h264_packets_; }
    uint32_t h264_bytes() const { return last_h264_bytes_; }
    uint32_t decoded_frames() const { return last_frame_count_; }
    uint32_t sequence() const { return last_sequence_; }

private:
    Private111DirectSource(const Private111DirectSource &);
    Private111DirectSource &operator=(const Private111DirectSource &);

    bool map_h264();
    bool map_frame();
    void log_h264_progress();
    unsigned long long now_us() const;

    bool verbose_;

    int h264_fd_;
    int frame_fd_;
    p111_h264_shm_t *h264_;
    p111_frame_shm_t *frames_;

    unsigned char *local_frame_;
    unsigned char *pending_frame_;
    size_t local_capacity_;

    uint32_t generation_;
    uint32_t writer_pid_;
    uint32_t stream_cookie_;
    uint32_t h264_writer_pid_;
    uint32_t h264_generation_;
    uint32_t h264_stream_cookie_;
    uint32_t last_sequence_;
    uint32_t last_h264_packets_;
    uint32_t last_h264_bytes_;
    uint32_t last_frame_count_;
    uint32_t last_logged_h264_packets_;
    uint32_t consumer_copy_count_;
    uint32_t h264_map_attempts_;
    uint32_t frame_map_attempts_;
    uint32_t sample_count_;

    bool h264_ready_;
    bool decoded_ready_;
    bool first_frame_logged_;
};

#endif
