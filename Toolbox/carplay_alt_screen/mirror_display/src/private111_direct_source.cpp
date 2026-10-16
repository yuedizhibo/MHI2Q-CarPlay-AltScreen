#include "private111_direct_source.h"
#include "private111_direct_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>

Private111DirectSource::Private111DirectSource(bool verbose)
    : verbose_(verbose),
      h264_fd_(-1),
      frame_fd_(-1),
      h264_(0),
      frames_(0),
      local_frame_(0),
      pending_frame_(0),
      local_capacity_(0),
      generation_(0),
      writer_pid_(0),
      stream_cookie_(0),
      h264_writer_pid_(0),
      h264_generation_(0),
      h264_stream_cookie_(0),
      last_sequence_(0),
      last_h264_packets_(0),
      last_h264_bytes_(0),
      last_frame_count_(0),
      last_logged_h264_packets_(0),
      consumer_copy_count_(0),
      h264_map_attempts_(0),
      frame_map_attempts_(0),
      sample_count_(0),
      h264_ready_(false),
      decoded_ready_(false),
      first_frame_logged_(false) {
}

Private111DirectSource::~Private111DirectSource() {
    shutdown();
}

unsigned long long Private111DirectSource::now_us() const {
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) return 0;
    return (unsigned long long)(unsigned long)tv.tv_sec * 1000000ULL +
           (unsigned long long)(unsigned long)tv.tv_usec;
}

bool Private111DirectSource::map_h264() {
    struct stat st;
    void *p;
    const size_t expected = sizeof(p111_h264_shm_t);

    if (h264_) return true;
    ++h264_map_attempts_;
    h264_fd_ = shm_open(P111_H264_SHM_NAME, O_RDONLY, 0);
    if (h264_fd_ < 0) return false;

    memset(&st, 0, sizeof(st));
    if (fstat(h264_fd_, &st) != 0 ||
        st.st_size < (off_t)expected) {
        if (h264_map_attempts_ == 1u ||
            (h264_map_attempts_ % 50u) == 0u) {
            fprintf(stderr,
                    "direct111: PHASE=H264_SHM_WAIT_SIZE attempt=%u "
                    "expected=%lu actual=%lld inode=%llu errno=%d action=RETRY\n",
                    (unsigned)h264_map_attempts_, (unsigned long)expected,
                    (long long)st.st_size,
                    (unsigned long long)st.st_ino, errno);
        }
        close(h264_fd_);
        h264_fd_ = -1;
        return false;
    }

    p = mmap(0, expected, PROT_READ, MAP_SHARED, h264_fd_, 0);
    if (p == MAP_FAILED || !p) {
        if (h264_map_attempts_ == 1u ||
            (h264_map_attempts_ % 50u) == 0u) {
            fprintf(stderr,
                    "direct111: PHASE=H264_SHM_WAIT_MAP attempt=%u "
                    "expected=%lu actual=%lld inode=%llu errno=%d action=RETRY\n",
                    (unsigned)h264_map_attempts_, (unsigned long)expected,
                    (long long)st.st_size,
                    (unsigned long long)st.st_ino, errno);
        }
        close(h264_fd_);
        h264_fd_ = -1;
        return false;
    }

    h264_ = (p111_h264_shm_t *)p;
    __sync_synchronize();
    if (h264_->magic != P111_H264_SHM_MAGIC ||
        h264_->version != P111_H264_SHM_VERSION ||
        h264_->writer_pid == 0u) {
        if (h264_map_attempts_ == 1u ||
            (h264_map_attempts_ % 50u) == 0u) {
            fprintf(stderr,
                    "direct111: PHASE=H264_SHM_WAIT_READY attempt=%u "
                    "map=%p expected=%lu actual=%lld inode=%llu "
                    "magic=0x%08x version=%u writer_pid=%u active=%u "
                    "generation=%u cookie=0x%08x action=RETRY\n",
                    (unsigned)h264_map_attempts_, p,
                    (unsigned long)expected, (long long)st.st_size,
                    (unsigned long long)st.st_ino,
                    (unsigned)h264_->magic, (unsigned)h264_->version,
                    (unsigned)h264_->writer_pid, (unsigned)h264_->active,
                    (unsigned)h264_->generation,
                    (unsigned)h264_->stream_cookie);
        }
        munmap(h264_, expected);
        h264_ = 0;
        close(h264_fd_);
        h264_fd_ = -1;
        return false;
    }

    fprintf(stderr,
            "direct111: PHASE=H264_SHM_ATTACHED name=%s version=%u ring=%u "
            "writer_pid=%u generation=%u cookie=0x%08x active=%u "
            "map=%p bytes=%lu object_size=%lld inode=%llu attempts=%u\n",
            P111_H264_SHM_NAME, (unsigned)h264_->version,
            (unsigned)P111_H264_RING_SIZE, (unsigned)h264_->writer_pid,
            (unsigned)h264_->generation, (unsigned)h264_->stream_cookie,
            (unsigned)h264_->active, (void *)h264_,
            (unsigned long)expected, (long long)st.st_size,
            (unsigned long long)st.st_ino, (unsigned)h264_map_attempts_);
    return true;
}

bool Private111DirectSource::map_frame() {
    struct stat st;
    void *p;
    const size_t expected = sizeof(p111_frame_shm_t);

    if (frames_) return true;
    ++frame_map_attempts_;
    frame_fd_ = shm_open(P111_FRAME_SHM_NAME, O_RDONLY, 0);
    if (frame_fd_ < 0) return false;

    memset(&st, 0, sizeof(st));
    if (fstat(frame_fd_, &st) != 0 ||
        st.st_size < (off_t)expected) {
        if (frame_map_attempts_ == 1u ||
            (frame_map_attempts_ % 50u) == 0u) {
            fprintf(stderr,
                    "direct111: PHASE=DECODED_SHM_WAIT_SIZE attempt=%u "
                    "expected=%lu actual=%lld inode=%llu errno=%d action=RETRY\n",
                    (unsigned)frame_map_attempts_, (unsigned long)expected,
                    (long long)st.st_size,
                    (unsigned long long)st.st_ino, errno);
        }
        close(frame_fd_);
        frame_fd_ = -1;
        return false;
    }

    p = mmap(0, expected, PROT_READ, MAP_SHARED, frame_fd_, 0);
    if (p == MAP_FAILED || !p) {
        if (frame_map_attempts_ == 1u ||
            (frame_map_attempts_ % 50u) == 0u) {
            fprintf(stderr,
                    "direct111: PHASE=DECODED_SHM_WAIT_MAP attempt=%u "
                    "expected=%lu actual=%lld inode=%llu errno=%d action=RETRY\n",
                    (unsigned)frame_map_attempts_, (unsigned long)expected,
                    (long long)st.st_size,
                    (unsigned long long)st.st_ino, errno);
        }
        close(frame_fd_);
        frame_fd_ = -1;
        return false;
    }

    frames_ = (p111_frame_shm_t *)p;
    __sync_synchronize();
    if (frames_->magic != P111_FRAME_SHM_MAGIC ||
        frames_->version != P111_FRAME_SHM_VERSION ||
        frames_->writer_pid == 0u) {
        if (frame_map_attempts_ == 1u ||
            (frame_map_attempts_ % 50u) == 0u) {
            fprintf(stderr,
                    "direct111: PHASE=DECODED_SHM_WAIT_READY attempt=%u "
                    "map=%p expected=%lu actual=%lld inode=%llu "
                    "magic=0x%08x version=%u writer_pid=%u active=%u "
                    "generation=%u cookie=0x%08x action=RETRY\n",
                    (unsigned)frame_map_attempts_, p,
                    (unsigned long)expected, (long long)st.st_size,
                    (unsigned long long)st.st_ino,
                    (unsigned)frames_->magic, (unsigned)frames_->version,
                    (unsigned)frames_->writer_pid, (unsigned)frames_->active,
                    (unsigned)frames_->generation,
                    (unsigned)frames_->stream_cookie);
        }
        munmap(frames_, expected);
        frames_ = 0;
        close(frame_fd_);
        frame_fd_ = -1;
        return false;
    }

    local_capacity_ = P111_FRAME_SLOT_BYTES;
    local_frame_ = (unsigned char *)malloc(local_capacity_);
    pending_frame_ = (unsigned char *)malloc(local_capacity_);
    if (!local_frame_ || !pending_frame_) {
        free(local_frame_);
        free(pending_frame_);
        local_frame_ = 0;
        pending_frame_ = 0;
        munmap(frames_, expected);
        frames_ = 0;
        close(frame_fd_);
        frame_fd_ = -1;
        local_capacity_ = 0;
        return false;
    }

    fprintf(stderr,
            "direct111: PHASE=DECODED_SHM_ATTACHED name=%s version=%u slots=%u "
            "slot_bytes=%u writer_pid=%u generation=%u cookie=0x%08x active=%u "
            "map=%p bytes=%lu object_size=%lld inode=%llu attempts=%u\n",
            P111_FRAME_SHM_NAME, (unsigned)frames_->version,
            (unsigned)P111_FRAME_SLOTS, (unsigned)P111_FRAME_SLOT_BYTES,
            (unsigned)frames_->writer_pid, (unsigned)frames_->generation,
            (unsigned)frames_->stream_cookie, (unsigned)frames_->active,
            (void *)frames_, (unsigned long)expected, (long long)st.st_size,
            (unsigned long long)st.st_ino, (unsigned)frame_map_attempts_);
    return true;
}

bool Private111DirectSource::init() {
    shutdown();
    fprintf(stderr,
            "direct111: PHASE=SOURCE_INIT mode=private111-direct decoder_backend=stock-omx-tap h264_shm=%s decoded_shm=%s window58_readback=0\n",
            P111_H264_SHM_NAME, P111_FRAME_SHM_NAME);

    /*
     * The writer is created lazily after the phone opens private111, so lack of
     * SHM at process start is expected. read_frame() retries both attachments.
     */
    (void)map_h264();
    (void)map_frame();
    return true;
}

void Private111DirectSource::log_h264_progress() {
    if (!h264_) return;

    __sync_synchronize();
    const uint32_t writer = h264_->writer_pid;
    const uint32_t gen = h264_->generation;
    const uint32_t cookie = h264_->stream_cookie;
    const uint32_t packets = h264_->packet_count;
    const uint32_t bytes = h264_->total_bytes;

    if (writer && h264_->active &&
        (writer != h264_writer_pid_ ||
         gen != h264_generation_ ||
         cookie != h264_stream_cookie_)) {
        h264_writer_pid_ = writer;
        h264_generation_ = gen;
        h264_stream_cookie_ = cookie;
        last_logged_h264_packets_ = 0;
        h264_ready_ = false;
        fprintf(stderr,
                "direct111: PHASE=H264_SOURCE_SESSION writer_pid=%u "
                "generation=%u cookie=0x%08x active=%u packets=%u\n",
                (unsigned)writer, (unsigned)gen, (unsigned)cookie,
                (unsigned)h264_->active, (unsigned)packets);
    }

    last_h264_packets_ = packets;
    last_h264_bytes_ = bytes;

    if (!h264_ready_ && h264_->active && writer &&
        h264_->sps_count && h264_->pps_count && h264_->idr_count) {
        h264_ready_ = true;
        fprintf(stderr,
                "direct111: PHASE=H264_STREAM_VALID writer_pid=%u generation=%u "
                "cookie=0x%08x packets=%u bytes=%u sps=%u pps=%u idr=%u annexb=%u\n",
                (unsigned)writer, (unsigned)gen, (unsigned)cookie,
                (unsigned)packets, (unsigned)bytes,
                (unsigned)h264_->sps_count, (unsigned)h264_->pps_count,
                (unsigned)h264_->idr_count, (unsigned)h264_->annexb_count);
    }

    if (verbose_ && packets &&
        (last_logged_h264_packets_ == 0 ||
         packets - last_logged_h264_packets_ >= 256u)) {
        last_logged_h264_packets_ = packets;
        fprintf(stderr,
                "direct111: PHASE=H264_PROGRESS writer_pid=%u generation=%u "
                "cookie=0x%08x active=%u packets=%u bytes=%u seq=%u "
                "wraps=%u drops=%u sps=%u pps=%u idr=%u\n",
                (unsigned)writer, (unsigned)gen, (unsigned)cookie,
                (unsigned)h264_->active, (unsigned)packets,
                (unsigned)bytes, (unsigned)h264_->write_seq,
                (unsigned)h264_->wrap_count, (unsigned)h264_->drop_count,
                (unsigned)h264_->sps_count, (unsigned)h264_->pps_count,
                (unsigned)h264_->idr_count);
    }
}

bool Private111DirectSource::current_session_active() {
    if (!h264_) (void)map_h264();
    if (!frames_) (void)map_frame();
    if (!h264_ || !frames_) return false;

    __sync_synchronize();
    const uint32_t hp = h264_->writer_pid;
    const uint32_t fp = frames_->writer_pid;
    return h264_->active && frames_->active &&
           hp != 0u && fp != 0u && hp == fp &&
           h264_->generation == frames_->generation &&
           h264_->stream_cookie == frames_->stream_cookie;
}

/**
 * Check whether the mapped decoded-frame session still matches the source identity.
 * Parameters: none.
 * Returns: true when H264 and decoded-frame identities are active and equal.
 */
bool Private111DirectSource::current_frame_session_active() {
    return current_session_active() && writer_pid_ != 0u &&
           frames_->writer_pid == writer_pid_ &&
           frames_->generation == generation_ &&
           frames_->stream_cookie == stream_cookie_;
}

/**
 * Validate and copy the producer's startup marker frame for session recovery.
 * Parameters: marker is the ready-marker path; frame receives the validated NV12 frame.
 * Returns: true when the marker, live producer, and copied frame all match.
 */
bool Private111DirectSource::read_verified_startup_frame(const char *marker,
                                                        VideoFrame *frame) {
    unsigned pid, gen, cookie, count, seq, ready;
    char record[256];
    int end = 0;
    if (!marker || !frame) return false;
    FILE *f = fopen(marker, "r");
    if (!f) return false;
    const size_t bytes = fread(record, 1, sizeof(record) - 1u, f);
    const bool complete = !ferror(f) && feof(f);
    fclose(f);
    if (!complete || !bytes) return false;
    record[bytes] = 0;
    if (memchr(record, 0, bytes) || strchr(record, '-') || strchr(record, '+'))
        return false;
    if (sscanf(record, "pid=%u\ngeneration=%u\ncookie=0x%x\nframes=%u\nsequence=%u\nready=%u%n",
               &pid, &gen, &cookie, &count, &seq, &ready, &end) != 6 ||
        end <= 0 || (size_t)end + 1u != bytes || record[end] != '\n' ||
        !pid || pid > INT_MAX || !gen || count < 2u || !seq || ready != 1u ||
        kill((pid_t)pid, 0) != 0) return false;

    /* The normal supervisor has already seen two producer frames. Reuse that
     * proof only with a stable NV12 copy from the same live H264/frame session.
     * Static maps need no additional frame just to start the intro. */
    if (!current_session_active() || !read_frame(frame) ||
        !current_frame_session_active() || !h264_ready_ ||
        writer_pid_ != pid || generation_ != gen || stream_cookie_ != cookie ||
        last_frame_count_ < count || (int32_t)(last_sequence_ - seq) < 0 ||
        kill((pid_t)pid, 0) != 0) return false;
    return true;
}

bool Private111DirectSource::read_frame(VideoFrame *frame) {
    if (!frame) return false;
    if (!h264_) (void)map_h264();
    if (!frames_) (void)map_frame();

    log_h264_progress();
    if (!frames_ || !local_frame_ || !pending_frame_) return false;

    __sync_synchronize();
    const uint32_t writer1 = frames_->writer_pid;
    const uint32_t active1 = frames_->active;
    const uint32_t seq1 = frames_->sequence;
    const uint32_t gen = frames_->generation;
    const uint32_t cookie = frames_->stream_cookie;
    const uint32_t slot = frames_->current_slot;
    const uint32_t width = frames_->width;
    const uint32_t height = frames_->height;
    const uint32_t stride = frames_->stride;
    const uint32_t format = frames_->format;
    const uint32_t bytes = frames_->frame_bytes;

    if (!active1 || !writer1 || !seq1) return false;

    if (h264_ && h264_->active && h264_->writer_pid &&
        (h264_->writer_pid != writer1 ||
         h264_->generation != gen ||
         h264_->stream_cookie != cookie)) {
        if (verbose_) {
            fprintf(stderr,
                    "direct111: PHASE=DECODED_SESSION_WAIT reason=H264_FRAME_IDENTITY_MISMATCH "
                    "h264_writer=%u frame_writer=%u h264_gen=%u frame_gen=%u "
                    "h264_cookie=0x%08x frame_cookie=0x%08x\n",
                    (unsigned)h264_->writer_pid, (unsigned)writer1,
                    (unsigned)h264_->generation, (unsigned)gen,
                    (unsigned)h264_->stream_cookie, (unsigned)cookie);
        }
        return false;
    }

    if (writer_pid_ != writer1 || generation_ != gen ||
        stream_cookie_ != cookie) {
        writer_pid_ = writer1;
        generation_ = gen;
        stream_cookie_ = cookie;
        last_sequence_ = 0;
        decoded_ready_ = false;
        first_frame_logged_ = false;
        consumer_copy_count_ = 0;
        sample_count_ = 0;
        fprintf(stderr,
                "direct111: PHASE=SOURCE_SESSION writer_pid=%u generation=%u "
                "stream_cookie=0x%08x decoder_backend=stock-omx-tap\n",
                (unsigned)writer1, (unsigned)gen, (unsigned)cookie);
    }

    if (seq1 == last_sequence_) return false;

    /*
     * /carplay111_decoded is a packed-tight NV12 ABI. Reject inconsistent
     * metadata before memcpy/render so a stale or torn header can never make
     * the GLES CSC walk beyond local_frame_.
     */
    const size_t expected_bytes =
        (width && height && stride == width && width <= 4096u &&
         height <= 4096u && !(width & 1u) && !(height & 1u))
            ? (size_t)stride * (size_t)height +
              (size_t)stride * (size_t)(height >> 1)
            : 0u;

    if (slot >= P111_FRAME_SLOTS || !width || !height || !stride ||
        format != P111_FRAME_FORMAT_NV12 || stride != width ||
        (width & 1u) || (height & 1u) ||
        width > 4096u || height > 4096u ||
        !bytes || !expected_bytes || bytes != expected_bytes ||
        bytes > P111_FRAME_SLOT_BYTES || bytes > local_capacity_) {
        fprintf(stderr,
                "direct111: ERROR PHASE=DECODED_FRAME_METADATA writer_pid=%u "
                "seq=%u gen=%u cookie=0x%08x slot=%u size=%ux%u stride=%u "
                "format=%u bytes=%u expected_bytes=%lu packed_tight_required=1\n",
                (unsigned)writer1, (unsigned)seq1, (unsigned)gen,
                (unsigned)cookie, (unsigned)slot,
                (unsigned)width, (unsigned)height, (unsigned)stride,
                (unsigned)format, (unsigned)bytes,
                (unsigned long)expected_bytes);
        return false;
    }

    const unsigned char *src =
        &frames_->data[(size_t)slot * P111_FRAME_SLOT_BYTES];
    /* Keep the last accepted frame intact until this snapshot is validated.
     * Logo fade and initial presentation may use it after read_frame fails. */
    memcpy(pending_frame_, src, bytes);
    __sync_synchronize();

    const uint32_t writer2 = frames_->writer_pid;
    const uint32_t active2 = frames_->active;
    const uint32_t seq2 = frames_->sequence;
    const uint32_t gen2 = frames_->generation;
    const uint32_t cookie2 = frames_->stream_cookie;
    const uint32_t slot2 = frames_->current_slot;
    if (!active2 || !writer2 ||
        writer1 != writer2 || gen != gen2 || cookie != cookie2 ||
        seq1 != seq2 || slot != slot2) {
        if (verbose_) {
            fprintf(stderr,
                    "direct111: PHASE=DECODED_FRAME_RACE retry=1 "
                    "writer=%u->%u active=%u->%u gen=%u->%u "
                    "cookie=0x%08x->0x%08x seq=%u->%u slot=%u->%u\n",
                    (unsigned)writer1, (unsigned)writer2,
                    (unsigned)active1, (unsigned)active2,
                    (unsigned)gen, (unsigned)gen2,
                    (unsigned)cookie, (unsigned)cookie2,
                    (unsigned)seq1, (unsigned)seq2,
                    (unsigned)slot, (unsigned)slot2);
        }
        return false;
    }

    unsigned char *previous_frame = local_frame_;
    local_frame_ = pending_frame_;
    pending_frame_ = previous_frame;
    last_sequence_ = seq1;
    last_frame_count_ = frames_->frame_count;
    ++consumer_copy_count_;

    frame->data = local_frame_;
    frame->width = (int)width;
    frame->height = (int)height;
    frame->stride = (int)stride;
    frame->format = PIXEL_FORMAT_NV12;
    frame->timestamp_us = now_us();

    const char *sample_env = getenv("ALT111_CONSUMER_SAMPLE_NV12");
    if (sample_env && *sample_env &&
        strcmp(sample_env, "0") != 0 &&
        strcmp(sample_env, "NO") != 0 &&
        strcmp(sample_env, "no") != 0 &&
        strcmp(sample_env, "false") != 0 &&
        strcmp(sample_env, "FALSE") != 0 &&
        sample_count_ < 3u) {
        char sample_path[160];
        FILE *sample;
        snprintf(sample_path, sizeof(sample_path),
                 "/tmp/carplay111_consumer_%u_%ux%u.nv12",
                 (unsigned)(sample_count_ + 1u),
                 (unsigned)width, (unsigned)height);
        sample = fopen(sample_path, "wb");
        if (sample) {
            size_t wrote = fwrite(local_frame_, 1u, bytes, sample);
            if (fclose(sample) == 0 && wrote == bytes) {
                ++sample_count_;
                fprintf(stderr,
                        "direct111: PHASE=DECODED_CONSUMER_SAMPLE path=%s "
                        "bytes=%u sample=%u writer_pid=%u generation=%u "
                        "cookie=0x%08x opt_in=1\n",
                        sample_path, (unsigned)bytes,
                        (unsigned)sample_count_, (unsigned)writer1,
                        (unsigned)gen, (unsigned)cookie);
            }
        }
    }

    if (!first_frame_logged_) {
        first_frame_logged_ = true;
        decoded_ready_ = true;
        fprintf(stderr,
                "direct111: PHASE=DECODER_FIRST_FRAME backend=stock-omx-tap "
                "writer_pid=%u generation=%u cookie=0x%08x seq=%u "
                "size=%ux%u stride=%u bytes=%u H264_VALID=%s\n",
                (unsigned)writer1, (unsigned)gen, (unsigned)cookie,
                (unsigned)seq1, (unsigned)width, (unsigned)height,
                (unsigned)stride, (unsigned)bytes,
                h264_ready_ ? "YES" : "NOT_YET");
    } else if ((consumer_copy_count_ % 300u) == 0u) {
        fprintf(stderr,
                "direct111: PHASE=DECODED_CONSUMER_PROGRESS writer_pid=%u "
                "generation=%u cookie=0x%08x copies=%u producer_frames=%u "
                "seq=%u\n",
                (unsigned)writer1, (unsigned)gen, (unsigned)cookie,
                (unsigned)consumer_copy_count_,
                (unsigned)last_frame_count_, (unsigned)seq1);
    }

    return true;
}

void Private111DirectSource::shutdown() {
    if (h264_) {
        munmap(h264_, sizeof(p111_h264_shm_t));
        h264_ = 0;
    }
    if (frames_) {
        munmap(frames_, sizeof(p111_frame_shm_t));
        frames_ = 0;
    }
    if (h264_fd_ >= 0) {
        close(h264_fd_);
        h264_fd_ = -1;
    }
    if (frame_fd_ >= 0) {
        close(frame_fd_);
        frame_fd_ = -1;
    }
    if (local_frame_) {
        free(local_frame_);
        local_frame_ = 0;
    }
    if (pending_frame_) {
        free(pending_frame_);
        pending_frame_ = 0;
    }
    local_capacity_ = 0;
    generation_ = 0;
    writer_pid_ = 0;
    stream_cookie_ = 0;
    h264_writer_pid_ = 0;
    h264_generation_ = 0;
    h264_stream_cookie_ = 0;
    last_sequence_ = 0;
    last_h264_packets_ = 0;
    last_h264_bytes_ = 0;
    last_frame_count_ = 0;
    last_logged_h264_packets_ = 0;
    consumer_copy_count_ = 0;
    h264_map_attempts_ = 0;
    frame_map_attempts_ = 0;
    sample_count_ = 0;
    h264_ready_ = false;
    decoded_ready_ = false;
    first_frame_logged_ = false;
}
