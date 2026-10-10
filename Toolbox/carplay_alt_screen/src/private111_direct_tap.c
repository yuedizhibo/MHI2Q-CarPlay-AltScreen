#include "private111_direct_tap.h"
#include "p1404_lock_wait.h"
#include "private111_direct_shm.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern void altscreen_log(const char *fmt, ...);

#define P111_AVCC_CACHE_SLOTS 8u
#define P111_AVCC_CACHE_MAX   512u
#define P111_MAP_MAX_ATTEMPTS 3u
/* QNX Neutrino dlopen flag; same ABI value already used by the P1404 hook. */
#define P111_RTLD_NOW 2

/* QNX Screen/WFD NV12 format observed on the 1440x542 private111 buffers. */
#define P111_QNX_NV12_FORMAT 65548u
#define P111_QNX_NV12_FORMAT_LEGACY 12u

/* V3.4 fixed volatile handoff: never written to persistent /mnt/app or SD. */
#define P111_STREAM_READY_PATH "/tmp/altscreen-private111.stream-ready"

struct p111_avcc_cache {
    void *stream;
    uint32_t bytes;
    uint32_t sps;
    uint32_t pps;
    uint32_t emitted_generation;
    uint8_t length_size;
    uint8_t valid;
    uint8_t property_logged;
    uint8_t data[P111_AVCC_CACHE_MAX];
};

static p111_h264_shm_t *g_h264;
static p111_frame_shm_t *g_frame;
static volatile unsigned g_tap_lock;
static void *g_stream;
static uint32_t g_generation;
static uint32_t g_stale_callback_count;
static uint32_t g_attach_logged_generation;
static uint32_t g_frame_reserve_seq;
static uint32_t g_last_frame_publish_us32;
/* Process-local slot ownership. A slot being copied must never be reused, and
 * the currently published slot must never be overwritten before a newer frame
 * is fully ready. This keeps the existing SHM v1 ABI while closing the
 * producer/consumer tearing window. Value is the reserved frame sequence, 0=free. */
static uint32_t g_frame_slot_owner[P111_FRAME_SLOTS];
static unsigned g_h264_map_attempts;
static unsigned g_frame_map_attempts;
static int g_seen_h264;
static int g_seen_frame;
static int g_seen_sps;
static int g_seen_pps;
static int g_seen_idr;
static uint32_t g_layout_error_logged_generation;
static uint32_t g_stream_ready_generation;
static struct p111_avcc_cache g_avcc[P111_AVCC_CACHE_SLOTS];
static unsigned g_avcc_recycle;

static void tap_lock(void) {
    while (__sync_lock_test_and_set(&g_tap_lock, 1u) != 0u) p1404_lock_wait_yield();
}

static void tap_unlock(void) {
    __sync_lock_release(&g_tap_lock);
}

static uint32_t stream_cookie(void *stream) {
    return (uint32_t)(uintptr_t)stream;
}

static int env_truth(const char *name, int default_value) {
    const char *v = getenv(name);
    if (!v || !*v) return default_value;
    if (!strcmp(v, "0") || !strcmp(v, "NO") || !strcmp(v, "no") ||
        !strcmp(v, "false") || !strcmp(v, "FALSE"))
        return 0;
    return 1;
}

static uint32_t tap_now_us32(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0u;
    /* Modular 32-bit microseconds are sufficient for sub-second readback timing
     * and avoid pulling 64-bit divide helpers into the freestanding ARM hook. */
    return (uint32_t)tv.tv_sec * 1000000u + (uint32_t)tv.tv_usec;
}

static void stream_ready_clear_locked(void) {
    (void)unlink(P111_STREAM_READY_PATH);
    g_stream_ready_generation = 0u;
}

static int stream_ready_publish_locked(void) {
    char payload[192];
    int fd, n;
    ssize_t wr;
    uint32_t pid;

    if (!g_stream || !g_generation || !g_h264 || !g_frame) return 0;
    pid = (uint32_t)getpid();
    if (!g_h264->active || !g_frame->active ||
        g_h264->writer_pid != pid || g_frame->writer_pid != pid ||
        g_h264->generation != g_generation ||
        g_frame->generation != g_generation ||
        !g_h264->packet_count || g_frame->frame_count < 2u ||
        !g_frame->sequence)
        return 0;
    if (g_stream_ready_generation == g_generation) return 1;

    n = snprintf(payload, sizeof(payload),
                 "pid=%u\ngeneration=%u\ncookie=0x%08x\nframes=%u\nsequence=%u\nready=1\n",
                 (unsigned)pid, (unsigned)g_generation,
                 (unsigned)stream_cookie(g_stream),
                 (unsigned)g_frame->frame_count,
                 (unsigned)g_frame->sequence);
    if (n <= 0 || (size_t)n >= sizeof(payload)) return 0;

    /*
     * QNX target headers used by this hook do not expose rename() in the
     * freestanding build profile. Publish the fixed volatile marker in-place.
     * A short/partial write is immediately unlinked; the supervisor also
     * validates every field plus producer PID/generation before acting, so a
     * concurrently observed partial file is fail-closed for display startup.
     */
    fd = open(P111_STREAM_READY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        altscreen_log("WARN PHASE=PRIVATE111_STREAM_READY marker_open_failed errno=%d fail_open=YES",
                      errno);
        return 0;
    }
    wr = write(fd, payload, (size_t)n);
    (void)close(fd);
    if (wr != (ssize_t)n) {
        int saved = errno;
        (void)unlink(P111_STREAM_READY_PATH);
        altscreen_log("WARN PHASE=PRIVATE111_STREAM_READY marker_publish_failed errno=%d fail_open=YES",
                      saved);
        return 0;
    }

    g_stream_ready_generation = g_generation;
    altscreen_log("PHASE=PRIVATE111_STREAM_READY pid=%u generation=%u cookie=0x%08x frames=%u sequence=%u policy=two_fresh_decoded_frames",
                  (unsigned)pid, (unsigned)g_generation,
                  (unsigned)stream_cookie(g_stream),
                  (unsigned)g_frame->frame_count,
                  (unsigned)g_frame->sequence);
    return 1;
}

static uint8_t byte_or_zero(const uint8_t *d, size_t n, size_t i) {
    return (d && i < n) ? d[i] : 0u;
}

static uint32_t be_len(const uint8_t *d, unsigned bytes) {
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < bytes; ++i) v = (v << 8) | d[i];
    return v;
}

static int parse_avcc_config(const uint8_t *d, size_t n,
                             uint8_t *length_size,
                             unsigned *sps_out, unsigned *pps_out) {
    size_t pos;
    unsigned i, sps_count, pps_count;
    unsigned sps_seen = 0, pps_seen = 0;
    uint8_t ls;

    if (length_size) *length_size = 0;
    if (sps_out) *sps_out = 0;
    if (pps_out) *pps_out = 0;
    if (!d || n < 7u || d[0] != 1u) return 0;

    ls = (uint8_t)((d[4] & 3u) + 1u);
    if (ls < 1u || ls > 4u) return 0;

    sps_count = d[5] & 0x1fu;
    if (!sps_count) return 0;
    pos = 6u;
    for (i = 0; i < sps_count; ++i) {
        uint32_t len;
        if (pos + 2u > n) return 0;
        len = ((uint32_t)d[pos] << 8) | d[pos + 1u];
        pos += 2u;
        if (!len || pos + len > n) return 0;
        if ((d[pos] & 0x1fu) == 7u) ++sps_seen;
        pos += len;
    }
    if (pos + 1u > n) return 0;

    pps_count = d[pos++];
    for (i = 0; i < pps_count; ++i) {
        uint32_t len;
        if (pos + 2u > n) return 0;
        len = ((uint32_t)d[pos] << 8) | d[pos + 1u];
        pos += 2u;
        if (!len || pos + len > n) return 0;
        if ((d[pos] & 0x1fu) == 8u) ++pps_seen;
        pos += len;
    }

    if (!sps_seen || !pps_seen) return 0;
    if (length_size) *length_size = ls;
    if (sps_out) *sps_out = sps_seen;
    if (pps_out) *pps_out = pps_seen;
    return 1;
}

static struct p111_avcc_cache *find_avcc_locked(void *stream, int create) {
    unsigned i, free_slot = P111_AVCC_CACHE_SLOTS;
    if (!stream) return NULL;
    for (i = 0; i < P111_AVCC_CACHE_SLOTS; ++i) {
        if (g_avcc[i].stream == stream) return &g_avcc[i];
        if (!g_avcc[i].stream && free_slot == P111_AVCC_CACHE_SLOTS)
            free_slot = i;
    }
    if (!create) return NULL;
    if (free_slot == P111_AVCC_CACHE_SLOTS) {
        free_slot = g_avcc_recycle++ % P111_AVCC_CACHE_SLOTS;
    }
    memset(&g_avcc[free_slot], 0, sizeof(g_avcc[free_slot]));
    g_avcc[free_slot].stream = stream;
    return &g_avcc[free_slot];
}

void p111_h264_tap_note_avcc(void *stream, const void *data, size_t bytes) {
    const uint8_t *d = (const uint8_t *)data;
    struct p111_avcc_cache *c;
    uint8_t length_size = 0;
    unsigned sps = 0, pps = 0;
    int valid;

    if (!stream || !data || !bytes || bytes > P111_AVCC_CACHE_MAX) return;
    valid = parse_avcc_config(d, bytes, &length_size, &sps, &pps);

    tap_lock();
    c = find_avcc_locked(stream, 1);
    if (c) {
        int first_property = !c->property_logged;
        c->bytes = (uint32_t)bytes;
        c->valid = valid ? 1u : 0u;
        c->length_size = length_size;
        c->sps = sps;
        c->pps = pps;
        c->emitted_generation = 0u;
        memcpy(c->data, data, bytes);
        c->property_logged = 1u;
        if (first_property) {
            altscreen_log("PHASE=H264_AVCC_PROPERTY stream=%p bytes=%u valid=%d nal_length_size=%u sps=%u pps=%u first16=%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x shm_dependency=NONE",
                          stream, (unsigned)bytes, valid,
                          (unsigned)length_size, sps, pps,
                          byte_or_zero(d,bytes,0), byte_or_zero(d,bytes,1),
                          byte_or_zero(d,bytes,2), byte_or_zero(d,bytes,3),
                          byte_or_zero(d,bytes,4), byte_or_zero(d,bytes,5),
                          byte_or_zero(d,bytes,6), byte_or_zero(d,bytes,7),
                          byte_or_zero(d,bytes,8), byte_or_zero(d,bytes,9),
                          byte_or_zero(d,bytes,10), byte_or_zero(d,bytes,11),
                          byte_or_zero(d,bytes,12), byte_or_zero(d,bytes,13),
                          byte_or_zero(d,bytes,14), byte_or_zero(d,bytes,15));
        }
    }
    tap_unlock();
}

/* Map failures are diagnostic, not fatal to stock CarPlay.  Retry a few times
 * per private generation because the resource manager can become ready just
 * after the first callback. */
static p111_h264_shm_t *map_h264(void) {
    int fd, err;
    void *p;
    uint32_t old_writer = 0u, old_generation = 0u, old_active = 0u;
    const size_t bytes = sizeof(p111_h264_shm_t);

    if (g_h264) return g_h264;
    if (g_h264_map_attempts >= P111_MAP_MAX_ATTEMPTS) return NULL;
    ++g_h264_map_attempts;

    errno = 0;
    fd = shm_open(P111_H264_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        err = errno;
        altscreen_log("ERROR PHASE=H264_TAP_SHM_OPEN name=%s attempt=%u fd=%d bytes=%u errno=%d result=FAILED",
                      P111_H264_SHM_NAME, g_h264_map_attempts, fd,
                      (unsigned)bytes, err);
        return NULL;
    }

    /*
     * Producer objects are never intentionally shrunk. ftruncate() is used only
     * to establish/retain the full ABI size before any reader can safely touch
     * the header. Readers independently fstat() before mmap.
     */
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        errno = 0;
        if (fstat(fd, &st) != 0) {
            err = errno;
            altscreen_log("ERROR PHASE=H264_TAP_SHM_STAT name=%s attempt=%u fd=%d expected=%u errno=%d result=FAILED",
                          P111_H264_SHM_NAME, g_h264_map_attempts, fd,
                          (unsigned)bytes, err);
            close(fd);
            return NULL;
        }
        if ((st.st_size < 0 || (uint32_t)st.st_size < (uint32_t)bytes)) {
            errno = 0;
            if (ftruncate(fd, (off_t)bytes) != 0) {
                err = errno;
                altscreen_log("ERROR PHASE=H264_TAP_SHM_SIZE name=%s attempt=%u fd=%d old_bytes=%u target_bytes=%u errno=%d result=FAILED",
                              P111_H264_SHM_NAME, g_h264_map_attempts, fd,
                              (unsigned)st.st_size, (unsigned)bytes, err);
                close(fd);
                return NULL;
            }
        }
    }

    errno = 0;
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    err = errno;
    close(fd);
    if (p == MAP_FAILED || !p) {
        altscreen_log("ERROR PHASE=H264_TAP_SHM_MAP name=%s attempt=%u bytes=%u prot=0x%x flags=0x%x result=%p errno=%d",
                      P111_H264_SHM_NAME, g_h264_map_attempts,
                      (unsigned)bytes, (unsigned)(PROT_READ | PROT_WRITE),
                      (unsigned)MAP_SHARED, p, err);
        return NULL;
    }

    g_h264 = (p111_h264_shm_t *)p;
    if (g_h264->magic == P111_H264_SHM_MAGIC &&
        g_h264->version == P111_H264_SHM_VERSION) {
        old_writer = g_h264->writer_pid;
        old_generation = g_h264->generation;
        old_active = g_h264->active;
    } else {
        memset(g_h264, 0, sizeof(*g_h264));
        g_h264->magic = P111_H264_SHM_MAGIC;
        g_h264->version = P111_H264_SHM_VERSION;
        __sync_synchronize();
    }

    /*
     * writer_pid doubles as the existing-ABI ready marker. Do not publish the
     * new owner here: begin_stream_locked() first resets the complete session
     * header, then publishes writer_pid last.
     */
    if (old_writer && old_writer != (uint32_t)getpid()) {
        altscreen_log("PHASE=H264_TAP_SHM_TAKEOVER old_writer_pid=%u old_generation=%u old_active=%u new_writer_pid=%u action=RESET_ON_NEXT_SESSION",
                      old_writer, old_generation, old_active,
                      (unsigned)getpid());
    }
    altscreen_log("PHASE=H264_TAP_SHM_MAPPED name=%s bytes=%u ring=%u map=%p old_writer_pid=%u old_generation=%u old_active=%u attempt=%u",
                  P111_H264_SHM_NAME, (unsigned)sizeof(*g_h264),
                  (unsigned)P111_H264_RING_SIZE, (void *)g_h264,
                  old_writer, old_generation, old_active, g_h264_map_attempts);
    if (g_h264_map_attempts > 1u)
        altscreen_log("PHASE=H264_TAP_SHM_RECOVERED attempt=%u previous_failures=%u",
                      g_h264_map_attempts, g_h264_map_attempts - 1u);
    return g_h264;
}

static p111_frame_shm_t *map_frame(void) {
    int fd, err;
    void *p;
    uint32_t old_writer = 0u, old_generation = 0u, old_active = 0u;
    const size_t bytes = sizeof(p111_frame_shm_t);

    if (g_frame) return g_frame;
    if (g_frame_map_attempts >= P111_MAP_MAX_ATTEMPTS) return NULL;
    ++g_frame_map_attempts;

    errno = 0;
    fd = shm_open(P111_FRAME_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        err = errno;
        altscreen_log("ERROR PHASE=FRAME_TAP_SHM_OPEN name=%s attempt=%u fd=%d bytes=%u errno=%d result=FAILED",
                      P111_FRAME_SHM_NAME, g_frame_map_attempts, fd,
                      (unsigned)bytes, err);
        return NULL;
    }

    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        errno = 0;
        if (fstat(fd, &st) != 0) {
            err = errno;
            altscreen_log("ERROR PHASE=FRAME_TAP_SHM_STAT name=%s attempt=%u fd=%d expected=%u errno=%d result=FAILED",
                          P111_FRAME_SHM_NAME, g_frame_map_attempts, fd,
                          (unsigned)bytes, err);
            close(fd);
            return NULL;
        }
        if ((st.st_size < 0 || (uint32_t)st.st_size < (uint32_t)bytes)) {
            errno = 0;
            if (ftruncate(fd, (off_t)bytes) != 0) {
                err = errno;
                altscreen_log("ERROR PHASE=FRAME_TAP_SHM_SIZE name=%s attempt=%u fd=%d old_bytes=%u target_bytes=%u errno=%d result=FAILED",
                              P111_FRAME_SHM_NAME, g_frame_map_attempts, fd,
                              (unsigned)st.st_size, (unsigned)bytes, err);
                close(fd);
                return NULL;
            }
        }
    }

    errno = 0;
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    err = errno;
    close(fd);
    if (p == MAP_FAILED || !p) {
        altscreen_log("ERROR PHASE=FRAME_TAP_SHM_MAP name=%s attempt=%u bytes=%u prot=0x%x flags=0x%x result=%p errno=%d",
                      P111_FRAME_SHM_NAME, g_frame_map_attempts,
                      (unsigned)bytes, (unsigned)(PROT_READ | PROT_WRITE),
                      (unsigned)MAP_SHARED, p, err);
        return NULL;
    }

    g_frame = (p111_frame_shm_t *)p;
    if (g_frame->magic == P111_FRAME_SHM_MAGIC &&
        g_frame->version == P111_FRAME_SHM_VERSION) {
        old_writer = g_frame->writer_pid;
        old_generation = g_frame->generation;
        old_active = g_frame->active;
    } else {
        memset(g_frame, 0, sizeof(*g_frame));
        g_frame->magic = P111_FRAME_SHM_MAGIC;
        g_frame->version = P111_FRAME_SHM_VERSION;
        __sync_synchronize();
    }

    if (old_writer && old_writer != (uint32_t)getpid()) {
        altscreen_log("PHASE=FRAME_TAP_SHM_TAKEOVER old_writer_pid=%u old_generation=%u old_active=%u new_writer_pid=%u action=RESET_ON_NEXT_SESSION",
                      old_writer, old_generation, old_active,
                      (unsigned)getpid());
    }
    altscreen_log("PHASE=FRAME_TAP_SHM_MAPPED name=%s bytes=%u slots=%u slot_bytes=%u map=%p old_writer_pid=%u old_generation=%u old_active=%u attempt=%u",
                  P111_FRAME_SHM_NAME, (unsigned)sizeof(*g_frame),
                  (unsigned)P111_FRAME_SLOTS, (unsigned)P111_FRAME_SLOT_BYTES,
                  (void *)g_frame, old_writer, old_generation, old_active,
                  g_frame_map_attempts);
    if (g_frame_map_attempts > 1u)
        altscreen_log("PHASE=FRAME_TAP_SHM_RECOVERED attempt=%u previous_failures=%u",
                      g_frame_map_attempts, g_frame_map_attempts - 1u);
    return g_frame;
}

static uint32_t write_h264_record_locked(const void *data, size_t bytes,
                                         uint32_t flags) {
    p111_h264_record_t rec;
    uint32_t need, pos;

    if (!g_h264 || !g_h264->active || !data || !bytes) return 0u;
    if (bytes > (size_t)(P111_H264_RING_SIZE - sizeof(rec))) {
        ++g_h264->drop_count;
        return 0u;
    }

    rec.magic = P111_H264_RECORD_MAGIC;
    rec.sequence = g_h264->write_seq + 1u;
    rec.payload_bytes = (uint32_t)bytes;
    rec.flags = flags;

    need = (uint32_t)sizeof(rec) + (uint32_t)bytes;
    pos = g_h264->write_pos;
    if (pos > P111_H264_RING_SIZE || need > P111_H264_RING_SIZE - pos) {
        if (pos < P111_H264_RING_SIZE &&
            P111_H264_RING_SIZE - pos >= sizeof(rec)) {
            p111_h264_record_t wrap;
            memset(&wrap, 0, sizeof(wrap));
            wrap.magic = P111_H264_RECORD_MAGIC;
            wrap.sequence = rec.sequence;
            wrap.flags = P111_H264_FLAG_WRAP;
            memcpy(&g_h264->ring[pos], &wrap, sizeof(wrap));
        }
        pos = 0u;
        ++g_h264->wrap_count;
    }

    memcpy(&g_h264->ring[pos], &rec, sizeof(rec));
    memcpy(&g_h264->ring[pos + sizeof(rec)], data, bytes);
    __sync_synchronize();
    g_h264->write_pos = pos + need;
    g_h264->write_seq = rec.sequence;
    g_h264->last_payload_bytes = (uint32_t)bytes;
    g_h264->total_bytes += (uint32_t)bytes;
    ++g_h264->packet_count;
    return rec.sequence;
}

static void emit_cached_avcc_locked(void *stream) {
    struct p111_avcc_cache *c = find_avcc_locked(stream, 0);
    uint32_t flags, seq;
    if (!c || !c->valid || !c->bytes || !g_h264 || !g_h264->active ||
        c->emitted_generation == g_generation)
        return;

    flags = P111_H264_FLAG_AVCC | P111_H264_FLAG_CONFIG;
    if (c->sps) flags |= P111_H264_FLAG_SPS;
    if (c->pps) flags |= P111_H264_FLAG_PPS;
    seq = write_h264_record_locked(c->data, c->bytes, flags);
    if (!seq) return;

    g_h264->sps_count += c->sps;
    g_h264->pps_count += c->pps;
    c->emitted_generation = g_generation;
    if (c->sps) g_seen_sps = 1;
    if (c->pps) g_seen_pps = 1;

    altscreen_log("PHASE=H264_AVCC_CONFIG stream=%p generation=%u seq=%u bytes=%u nal_length_size=%u sps=%u pps=%u first16=%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x",
                  stream, g_generation, seq, c->bytes,
                  (unsigned)c->length_size, c->sps, c->pps,
                  byte_or_zero(c->data,c->bytes,0), byte_or_zero(c->data,c->bytes,1),
                  byte_or_zero(c->data,c->bytes,2), byte_or_zero(c->data,c->bytes,3),
                  byte_or_zero(c->data,c->bytes,4), byte_or_zero(c->data,c->bytes,5),
                  byte_or_zero(c->data,c->bytes,6), byte_or_zero(c->data,c->bytes,7),
                  byte_or_zero(c->data,c->bytes,8), byte_or_zero(c->data,c->bytes,9),
                  byte_or_zero(c->data,c->bytes,10), byte_or_zero(c->data,c->bytes,11),
                  byte_or_zero(c->data,c->bytes,12), byte_or_zero(c->data,c->bytes,13),
                  byte_or_zero(c->data,c->bytes,14), byte_or_zero(c->data,c->bytes,15));
}

static void reset_h264_for_generation_locked(int force) {
    uint32_t old_writer, old_generation, old_active;
    if (!g_h264) return;
    if (!force &&
        g_h264->generation == g_generation &&
        g_h264->writer_pid == (uint32_t)getpid() &&
        g_h264->active)
        return;

    old_writer = g_h264->writer_pid;
    old_generation = g_h264->generation;
    old_active = g_h264->active;

    /* Existing ABI ready protocol: writer_pid==0 means header transition. */
    g_h264->writer_pid = 0u;
    g_h264->active = 0u;
    __sync_synchronize();

    g_h264->generation = g_generation;
    g_h264->stream_cookie = stream_cookie(g_stream);
    g_h264->write_pos = 0;
    g_h264->write_seq = 0;
    g_h264->total_bytes = 0;
    g_h264->packet_count = 0;
    g_h264->drop_count = 0;
    g_h264->wrap_count = 0;
    g_h264->last_payload_bytes = 0;
    g_h264->sps_count = 0;
    g_h264->pps_count = 0;
    g_h264->idr_count = 0;
    g_h264->annexb_count = 0;
    __sync_synchronize();

    g_h264->active = 1u;
    __sync_synchronize();
    g_h264->writer_pid = (uint32_t)getpid();
    __sync_synchronize();

    altscreen_log("PHASE=H264_TAP_SESSION_RESET old_writer_pid=%u old_generation=%u old_active=%u new_writer_pid=%u generation=%u cookie=0x%08x force=%d ready_published_last=1",
                  old_writer, old_generation, old_active,
                  (unsigned)g_h264->writer_pid, g_generation,
                  g_h264->stream_cookie, force);
    altscreen_log("PHASE=H264_TAP_SHM_READY name=%s writer_pid=%u generation=%u active=%u",
                  P111_H264_SHM_NAME, (unsigned)g_h264->writer_pid,
                  (unsigned)g_h264->generation, (unsigned)g_h264->active);
}

static void reset_frame_for_generation_locked(int force) {
    uint32_t old_writer, old_generation, old_active;
    if (!g_frame) return;
    if (!force &&
        g_frame->generation == g_generation &&
        g_frame->writer_pid == (uint32_t)getpid() &&
        g_frame->active)
        return;

    old_writer = g_frame->writer_pid;
    old_generation = g_frame->generation;
    old_active = g_frame->active;

    g_frame->writer_pid = 0u;
    g_frame->active = 0u;
    __sync_synchronize();

    g_frame->generation = g_generation;
    g_frame->stream_cookie = stream_cookie(g_stream);
    g_frame->width = 0;
    g_frame->height = 0;
    g_frame->stride = 0;
    g_frame->format = P111_FRAME_FORMAT_NV12;
    g_frame->frame_bytes = 0;
    g_frame->sequence = 0;
    g_frame->current_slot = 0;
    g_frame->frame_count = 0;
    g_frame->drop_count = 0;
    g_frame->last_copy_bytes = 0;
    g_frame_reserve_seq = 0;
    g_last_frame_publish_us32 = 0u;
    /*
     * Do not clear g_frame_slot_owner here. A callback from the previous
     * process-local generation may still be outside the lock copying its slot.
     */
    __sync_synchronize();

    g_frame->active = 1u;
    __sync_synchronize();
    g_frame->writer_pid = (uint32_t)getpid();
    __sync_synchronize();

    altscreen_log("PHASE=FRAME_TAP_SESSION_RESET old_writer_pid=%u old_generation=%u old_active=%u new_writer_pid=%u generation=%u cookie=0x%08x force=%d ready_published_last=1",
                  old_writer, old_generation, old_active,
                  (unsigned)g_frame->writer_pid, g_generation,
                  g_frame->stream_cookie, force);
    altscreen_log("PHASE=FRAME_TAP_SHM_READY name=%s writer_pid=%u generation=%u active=%u",
                  P111_FRAME_SHM_NAME, (unsigned)g_frame->writer_pid,
                  (unsigned)g_frame->generation, (unsigned)g_frame->active);
}

static int begin_stream_locked(void *stream) {
    int new_session = 0;

    if (!stream) return 0;

    /*
     * Session ownership is monotonic until explicit teardown. A late callback
     * from an older stream (or an early callback from a replacement stream
     * before current teardown completes) must never switch g_stream and reset
     * the shared-memory publication back to another session.
     */
    if (g_stream && g_stream != stream) {
        ++g_stale_callback_count;
        if (g_stale_callback_count == 1u ||
            (g_stale_callback_count & 255u) == 0u) {
            altscreen_log("PHASE=DIRECT111_TAP_STALE_CALLBACK stream=%p current_stream=%p generation=%u count=%u action=DROP_NO_SESSION_SWITCH",
                          stream, g_stream, g_generation,
                          g_stale_callback_count);
        }
        return 0;
    }

    if (!g_stream || !g_generation) {
        /* A new producer generation must never inherit a prior stream-ready
         * marker. The supervisor will wait for two fresh decoded frames. */
        stream_ready_clear_locked();
        g_stream = stream;
        ++g_generation;
        if (!g_generation) ++g_generation;
        g_seen_h264 = 0;
        g_seen_frame = 0;
        g_seen_sps = 0;
        g_seen_pps = 0;
        g_seen_idr = 0;
        g_layout_error_logged_generation = 0u;
        g_h264_map_attempts = 0;
        g_frame_map_attempts = 0;
        g_attach_logged_generation = 0;
        g_frame_reserve_seq = 0;
        g_stale_callback_count = 0u;
        new_session = 1;
    }

    if (!g_h264) (void)map_h264();
    if (env_truth("ALT111_DIRECT_FRAME_TAP", 1) && !g_frame)
        (void)map_frame();

    /*
     * A new stream is a new publication session even if a restarted producer
     * happens to reuse numeric generation=1 from a persistent named SHM.
     * Force the existing ABI headers active again and reset all counters.
     */
    reset_h264_for_generation_locked(new_session);
    reset_frame_for_generation_locked(new_session);
    emit_cached_avcc_locked(stream);

    if (new_session || g_attach_logged_generation != g_generation) {
        g_attach_logged_generation = g_generation;
        altscreen_log("PHASE=DIRECT111_TAP_ATTACH stream=%p generation=%u cookie=0x%08x h264=%d decoded_fallback=%d stock_forward=1 mmap_prot=0x%x session_reset=%d writer_pid=%u",
                      stream, g_generation, stream_cookie(stream),
                      g_h264 != NULL, g_frame != NULL,
                      (unsigned)(PROT_READ | PROT_WRITE), new_session,
                      (unsigned)getpid());
    }
    return 1;
}

static uint32_t scan_annexb_flags(const uint8_t *d, size_t n,
                                  unsigned *sps, unsigned *pps,
                                  unsigned *idr, unsigned *annexb) {
    size_t i;
    uint32_t flags = 0;
    if (sps) *sps = 0;
    if (pps) *pps = 0;
    if (idr) *idr = 0;
    if (annexb) *annexb = 0;
    if (!d || n < 4u) return 0;

    for (i = 0; i + 3u < n; ++i) {
        size_t h = 0;
        uint8_t nal;
        if (i + 4u < n && d[i] == 0 && d[i + 1u] == 0 &&
            d[i + 2u] == 0 && d[i + 3u] == 1) h = i + 4u;
        else if (d[i] == 0 && d[i + 1u] == 0 && d[i + 2u] == 1)
            h = i + 3u;
        if (!h || h >= n) continue;
        flags |= P111_H264_FLAG_ANNEXB;
        if (annexb) ++*annexb;
        nal = (uint8_t)(d[h] & 0x1fu);
        if (nal == 7u) {
            flags |= P111_H264_FLAG_SPS;
            if (sps) ++*sps;
        } else if (nal == 8u) {
            flags |= P111_H264_FLAG_PPS;
            if (pps) ++*pps;
        } else if (nal == 5u) {
            flags |= P111_H264_FLAG_IDR;
            if (idr) ++*idr;
        }
        i = h;
    }
    return flags;
}

static uint32_t scan_avcc_flags_len(const uint8_t *d, size_t n,
                                    unsigned length_size,
                                    unsigned *sps, unsigned *pps,
                                    unsigned *idr, unsigned *nals) {
    size_t pos = 0;
    uint32_t flags = 0;
    unsigned count = 0;
    unsigned lsps = 0, lpps = 0, lidr = 0;

    if (!d || !n || length_size < 1u || length_size > 4u) return 0;
    while (pos + length_size <= n) {
        uint32_t len = be_len(d + pos, length_size);
        uint8_t nal;
        pos += length_size;
        if (!len || pos + len > n) return 0;
        nal = d[pos] & 0x1fu;
        if (!nal) return 0;
        if (nal == 7u) { ++lsps; flags |= P111_H264_FLAG_SPS; }
        else if (nal == 8u) { ++lpps; flags |= P111_H264_FLAG_PPS; }
        else if (nal == 5u) { ++lidr; flags |= P111_H264_FLAG_IDR; }
        ++count;
        pos += len;
    }
    if (!count || pos != n) return 0;
    if (sps) *sps = lsps;
    if (pps) *pps = lpps;
    if (idr) *idr = lidr;
    if (nals) *nals = count;
    return flags | P111_H264_FLAG_AVCC;
}

static uint32_t scan_avcc_flags(const uint8_t *d, size_t n,
                                unsigned preferred_length_size,
                                unsigned *sps, unsigned *pps,
                                unsigned *idr, unsigned *nals,
                                unsigned *used_length_size) {
    unsigned candidates[4];
    unsigned i, count = 0;
    uint32_t flags;

    if (preferred_length_size >= 1u && preferred_length_size <= 4u)
        candidates[count++] = preferred_length_size;
    for (i = 4u; i >= 1u; --i) {
        unsigned j, duplicate = 0;
        for (j = 0; j < count; ++j)
            if (candidates[j] == i) duplicate = 1;
        if (!duplicate) candidates[count++] = i;
        if (i == 1u) break;
    }

    for (i = 0; i < count; ++i) {
        unsigned lsps = 0, lpps = 0, lidr = 0, lnals = 0;
        flags = scan_avcc_flags_len(d, n, candidates[i],
                                    &lsps, &lpps, &lidr, &lnals);
        if (!(flags & P111_H264_FLAG_AVCC)) continue;
        if (sps) *sps = lsps;
        if (pps) *pps = lpps;
        if (idr) *idr = lidr;
        if (nals) *nals = lnals;
        if (used_length_size) *used_length_size = candidates[i];
        return flags;
    }
    return 0;
}

void p111_h264_tap_write(void *stream, const void *data, size_t bytes) {
    const uint8_t *d = (const uint8_t *)data;
    struct p111_avcc_cache *cfg;
    uint32_t seq, flags;
    unsigned sps = 0, pps = 0, idr = 0, annexb = 0, avcc_nals = 0;
    unsigned preferred = 0, used_length = 0;
    const char *format_name = "unknown";

    if (!stream || !data || !bytes) return;
    tap_lock();
    if (!begin_stream_locked(stream) || !g_h264 || !g_h264->active) {
        tap_unlock();
        return;
    }

    cfg = find_avcc_locked(stream, 0);
    if (cfg && cfg->valid) preferred = cfg->length_size;

    flags = scan_annexb_flags(d, bytes, &sps, &pps, &idr, &annexb);
    if (flags & P111_H264_FLAG_ANNEXB) {
        format_name = "annexb";
    } else {
        sps = pps = idr = 0;
        flags = scan_avcc_flags(d, bytes, preferred,
                                &sps, &pps, &idr, &avcc_nals, &used_length);
        if (flags & P111_H264_FLAG_AVCC) format_name = "avcc";
    }
    flags |= P111_H264_FLAG_FRAME;

    g_h264->sps_count += sps;
    g_h264->pps_count += pps;
    g_h264->idr_count += idr;
    g_h264->annexb_count += annexb;

    seq = write_h264_record_locked(data, bytes, flags);
    if (!seq) {
        tap_unlock();
        return;
    }

    if (!g_seen_h264) {
        g_seen_h264 = 1;
        altscreen_log("PHASE=H264_TAP_FIRST_DATA stream=%p generation=%u seq=%u bytes=%u format=%s nal_length_size=%u annexb_nals=%u avcc_nals=%u flags=0x%x first16=%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x",
                      stream, g_generation, seq, (unsigned)bytes, format_name,
                      used_length ? used_length : preferred, annexb, avcc_nals,
                      (unsigned)flags,
                      byte_or_zero(d,bytes,0), byte_or_zero(d,bytes,1),
                      byte_or_zero(d,bytes,2), byte_or_zero(d,bytes,3),
                      byte_or_zero(d,bytes,4), byte_or_zero(d,bytes,5),
                      byte_or_zero(d,bytes,6), byte_or_zero(d,bytes,7),
                      byte_or_zero(d,bytes,8), byte_or_zero(d,bytes,9),
                      byte_or_zero(d,bytes,10), byte_or_zero(d,bytes,11),
                      byte_or_zero(d,bytes,12), byte_or_zero(d,bytes,13),
                      byte_or_zero(d,bytes,14), byte_or_zero(d,bytes,15));
    }
    if (sps && !g_seen_sps) {
        g_seen_sps = 1;
        altscreen_log("PHASE=H264_TAP_FIRST_SPS stream=%p generation=%u seq=%u source=%s",
                      stream, g_generation, seq, format_name);
    }
    if (pps && !g_seen_pps) {
        g_seen_pps = 1;
        altscreen_log("PHASE=H264_TAP_FIRST_PPS stream=%p generation=%u seq=%u source=%s",
                      stream, g_generation, seq, format_name);
    }
    if (idr && !g_seen_idr) {
        g_seen_idr = 1;
        altscreen_log("PHASE=H264_TAP_FIRST_IDR stream=%p generation=%u seq=%u source=%s H264_STREAM_VALID=%s",
                      stream, g_generation, seq, format_name,
                      (g_seen_sps && g_seen_pps) ? "YES" : "WAITING_CONFIG");
    }
    if ((g_h264->packet_count & 255u) == 0u) {
        altscreen_log("PHASE=H264_TAP_PROGRESS stream=%p generation=%u packets=%u bytes=%u seq=%u wraps=%u drops=%u sps=%u pps=%u idr=%u format=%s",
                      stream, g_generation, g_h264->packet_count,
                      g_h264->total_bytes, g_h264->write_seq,
                      g_h264->wrap_count, g_h264->drop_count,
                      g_h264->sps_count, g_h264->pps_count,
                      g_h264->idr_count, format_name);
    }

    tap_unlock();
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int qnx_nv12_padded_layout(uint32_t width, uint32_t height,
                                  uint32_t format, const unsigned char *buffer,
                                  uint32_t *src_stride, uint32_t *uv_offset) {
    uint32_t stride, padded_y;
    if (!buffer || !width || !height) return 0;
    if (format != P111_QNX_NV12_FORMAT &&
        format != P111_QNX_NV12_FORMAT_LEGACY)
        return 0;

    /*
     * P1404 real-car evidence for private111:
     * 1440x542 format=65548 stride=1536 UV offset=0xCC000.
     * Those values are exactly align(width,128) and
     * align(width,128)*align(height,32).  Apply this only to the measured QNX
     * NV12 formats; all unknown formats remain on the conservative tight path.
     */
    stride = align_up_u32(width, 128u);
    padded_y = align_up_u32(height, 32u);
    if (stride < width || padded_y < height ||
        stride > 8192u || padded_y > 8192u)
        return 0;
    if (src_stride) *src_stride = stride;
    if (uv_offset) *uv_offset = stride * padded_y;
    return 1;
}


/*
 * Direct-display V2: the stock OMX/Screen decoder path exposes Screen format
 * 0x0001000c (65548).  V1 proved the stride/plane offsets but incorrectly
 * treated the CPU pointer as a linear raster.  The resulting picture moved
 * with CarPlay but was visibly tiled/garbled.
 *
 * V2 deliberately does not guess the vendor tiling formula.  After stock
 * CScreenRender posts the frame, Screen is asked to read that exact window
 * into an off-screen pixmap.  Screen therefore owns the vendor-layout ->
 * ordinary-raster conversion.  We first request standard NV12 (format 12);
 * if this target is unsupported on the vehicle, RGBA8888 is used and converted
 * to packed NV12 in software.  The existing /carplay111_decoded ABI and the
 * proven displayable3/Context80 sidecar remain unchanged.
 */
#define P111_SCREEN_APPLICATION_CONTEXT 0
#define P111_SCREEN_PROPERTY_BUFFER_SIZE 5
#define P111_SCREEN_PROPERTY_FORMAT 14
#define P111_SCREEN_PROPERTY_PLANAR_OFFSETS 33
#define P111_SCREEN_PROPERTY_POINTER 34
#define P111_SCREEN_PROPERTY_RENDER_BUFFERS 37
#define P111_SCREEN_PROPERTY_SIZE 40
#define P111_SCREEN_PROPERTY_STRIDE 44
#define P111_SCREEN_PROPERTY_USAGE 48
#define P111_SCREEN_FORMAT_RGBA8888 8
#define P111_SCREEN_FORMAT_NV12 12
#define P111_SCREEN_USAGE_READ (1u << 1)
#define P111_SCREEN_USAGE_NATIVE (1u << 3)
#define P111_LINEARIZER_NV12 1
#define P111_LINEARIZER_RGBA 2

typedef void *p111_screen_context_t;
typedef void *p111_screen_pixmap_t;
typedef void *p111_screen_buffer_t;
typedef void *p111_screen_window_t;
typedef int (*p111_screen_create_context_fn)(p111_screen_context_t *, int);
typedef int (*p111_screen_destroy_context_fn)(p111_screen_context_t);
typedef int (*p111_screen_create_pixmap_fn)(p111_screen_pixmap_t *,
                                             p111_screen_context_t);
typedef int (*p111_screen_destroy_pixmap_fn)(p111_screen_pixmap_t);
typedef int (*p111_screen_set_pixmap_iv_fn)(p111_screen_pixmap_t, int,
                                             const int *);
typedef int (*p111_screen_create_pixmap_buffer_fn)(p111_screen_pixmap_t);
typedef int (*p111_screen_get_pixmap_pv_fn)(p111_screen_pixmap_t, int, void **);
typedef int (*p111_screen_get_buffer_pv_fn)(p111_screen_buffer_t, int, void **);
typedef int (*p111_screen_get_buffer_iv_fn)(p111_screen_buffer_t, int, int *);
typedef int (*p111_screen_get_window_iv_fn)(p111_screen_window_t, int, int *);
typedef int (*p111_screen_read_window_fn)(p111_screen_window_t,
                                           p111_screen_buffer_t,
                                           int, const int *, int);

struct p111_linearizer_state {
    void *lib;
    p111_screen_context_t context;
    p111_screen_pixmap_t pixmap;
    p111_screen_buffer_t buffer;
    unsigned char *pixels;
    unsigned char *scratch;
    size_t scratch_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t source_format;
    uint32_t source_usage;
    int backend;
    int stride;
    int offsets[3];
    uint32_t requests;
    uint32_t readback_success;
    uint32_t publish_success;
    uint32_t publish_drop;
    uint32_t failures;
    uint32_t fallback_count;
    uint32_t slow_readback_count;
    uint32_t readback_max_us;
    uint32_t readback_hist_ms[65];
    uint32_t sample_count;

    p111_screen_create_context_fn create_context;
    p111_screen_destroy_context_fn destroy_context;
    p111_screen_create_pixmap_fn create_pixmap;
    p111_screen_destroy_pixmap_fn destroy_pixmap;
    p111_screen_set_pixmap_iv_fn set_pixmap_iv;
    p111_screen_create_pixmap_buffer_fn create_pixmap_buffer;
    p111_screen_get_pixmap_pv_fn get_pixmap_pv;
    p111_screen_get_buffer_pv_fn get_buffer_pv;
    p111_screen_get_buffer_iv_fn get_buffer_iv;
    p111_screen_get_window_iv_fn get_window_iv;
    p111_screen_read_window_fn read_window;
};

static struct p111_linearizer_state g_linearizer;
static volatile unsigned g_linearizer_lock;

static void linearizer_lock(void) {
    while (__sync_lock_test_and_set(&g_linearizer_lock, 1u) != 0u) p1404_lock_wait_yield();
}

static void linearizer_unlock(void) {
    __sync_lock_release(&g_linearizer_lock);
}

static void linearizer_release_pixmap_locked(void) {
    if (g_linearizer.pixmap && g_linearizer.destroy_pixmap)
        (void)g_linearizer.destroy_pixmap(g_linearizer.pixmap);
    g_linearizer.pixmap = NULL;
    g_linearizer.buffer = NULL;
    g_linearizer.pixels = NULL;
    g_linearizer.width = 0;
    g_linearizer.height = 0;
    g_linearizer.backend = 0;
    g_linearizer.stride = 0;
    g_linearizer.offsets[0] = 0;
    g_linearizer.offsets[1] = 0;
    g_linearizer.offsets[2] = 0;
}

static void linearizer_shutdown_locked(void) {
    linearizer_release_pixmap_locked();
    if (g_linearizer.context && g_linearizer.destroy_context)
        (void)g_linearizer.destroy_context(g_linearizer.context);
    g_linearizer.context = NULL;
    if (g_linearizer.lib) dlclose(g_linearizer.lib);
    g_linearizer.lib = NULL;
    if (g_linearizer.scratch) free(g_linearizer.scratch);
    g_linearizer.scratch = NULL;
    g_linearizer.scratch_bytes = 0;
    g_linearizer.create_context = NULL;
    g_linearizer.destroy_context = NULL;
    g_linearizer.create_pixmap = NULL;
    g_linearizer.destroy_pixmap = NULL;
    g_linearizer.set_pixmap_iv = NULL;
    g_linearizer.create_pixmap_buffer = NULL;
    g_linearizer.get_pixmap_pv = NULL;
    g_linearizer.get_buffer_pv = NULL;
    g_linearizer.get_buffer_iv = NULL;
    g_linearizer.get_window_iv = NULL;
    g_linearizer.read_window = NULL;
    g_linearizer.requests = 0;
    g_linearizer.readback_success = 0;
    g_linearizer.publish_success = 0;
    g_linearizer.publish_drop = 0;
    g_linearizer.failures = 0;
    g_linearizer.fallback_count = 0;
    g_linearizer.slow_readback_count = 0;
    g_linearizer.readback_max_us = 0;
    memset(g_linearizer.readback_hist_ms, 0,
           sizeof(g_linearizer.readback_hist_ms));
    g_linearizer.sample_count = 0;
}

static int linearizer_open_api_locked(void) {
    void *lib;
    if (g_linearizer.lib) return 1;

    lib = dlopen("libscreen.so.1", P111_RTLD_NOW);
    if (!lib) lib = dlopen("libscreen.so", P111_RTLD_NOW);
    if (!lib) {
        altscreen_log("ERROR PHASE=FRAME_LINEARIZER_API backend=screen-read-window dlopen=FAILED");
        return 0;
    }

    g_linearizer.create_context =
        (p111_screen_create_context_fn)dlsym(lib, "screen_create_context");
    g_linearizer.destroy_context =
        (p111_screen_destroy_context_fn)dlsym(lib, "screen_destroy_context");
    g_linearizer.create_pixmap =
        (p111_screen_create_pixmap_fn)dlsym(lib, "screen_create_pixmap");
    g_linearizer.destroy_pixmap =
        (p111_screen_destroy_pixmap_fn)dlsym(lib, "screen_destroy_pixmap");
    g_linearizer.set_pixmap_iv =
        (p111_screen_set_pixmap_iv_fn)dlsym(lib, "screen_set_pixmap_property_iv");
    g_linearizer.create_pixmap_buffer =
        (p111_screen_create_pixmap_buffer_fn)dlsym(lib, "screen_create_pixmap_buffer");
    g_linearizer.get_pixmap_pv =
        (p111_screen_get_pixmap_pv_fn)dlsym(lib, "screen_get_pixmap_property_pv");
    g_linearizer.get_buffer_pv =
        (p111_screen_get_buffer_pv_fn)dlsym(lib, "screen_get_buffer_property_pv");
    g_linearizer.get_buffer_iv =
        (p111_screen_get_buffer_iv_fn)dlsym(lib, "screen_get_buffer_property_iv");
    g_linearizer.get_window_iv =
        (p111_screen_get_window_iv_fn)dlsym(lib, "screen_get_window_property_iv");
    g_linearizer.read_window =
        (p111_screen_read_window_fn)dlsym(lib, "screen_read_window");

    if (!g_linearizer.create_context || !g_linearizer.destroy_context ||
        !g_linearizer.create_pixmap || !g_linearizer.destroy_pixmap ||
        !g_linearizer.set_pixmap_iv || !g_linearizer.create_pixmap_buffer ||
        !g_linearizer.get_pixmap_pv || !g_linearizer.get_buffer_pv ||
        !g_linearizer.get_buffer_iv || !g_linearizer.read_window) {
        altscreen_log("ERROR PHASE=FRAME_LINEARIZER_API backend=screen-read-window symbols=INCOMPLETE create_ctx=%d pixmap=%d pixbuf=%d buffer_iv=%d read_window=%d",
                      g_linearizer.create_context != NULL,
                      g_linearizer.create_pixmap != NULL,
                      g_linearizer.create_pixmap_buffer != NULL,
                      g_linearizer.get_buffer_iv != NULL,
                      g_linearizer.read_window != NULL);
        dlclose(lib);
        memset(&g_linearizer, 0, sizeof(g_linearizer));
        return 0;
    }

    g_linearizer.lib = lib;
    errno = 0;
    if (g_linearizer.create_context(&g_linearizer.context,
                                    P111_SCREEN_APPLICATION_CONTEXT) != 0 ||
        !g_linearizer.context) {
        int err = errno;
        altscreen_log("ERROR PHASE=FRAME_LINEARIZER_CONTEXT type=APPLICATION rc=FAILED errno=%d", err);
        linearizer_shutdown_locked();
        return 0;
    }

    altscreen_log("PHASE=FRAME_LINEARIZER_API backend=screen-read-window context=APPLICATION result=READY");
    return 1;
}

static int linearizer_ensure_scratch_locked(uint32_t width, uint32_t height) {
    uint32_t stride = align_up_u32(width, 128u);
    uint32_t padded_y = align_up_u32(height, 32u);
    uint32_t uv_rows = (height + 1u) >> 1;
    size_t need;
    unsigned char *next;

    if (!stride || !padded_y || stride > 8192u || padded_y > 8192u)
        return 0;
    need = (size_t)stride * padded_y + (size_t)stride * uv_rows;
    if (g_linearizer.scratch && g_linearizer.scratch_bytes >= need)
        return 1;
    next = (unsigned char *)realloc(g_linearizer.scratch, need);
    if (!next) return 0;
    g_linearizer.scratch = next;
    g_linearizer.scratch_bytes = need;
    return 1;
}

static int linearizer_create_pixmap_locked(uint32_t width, uint32_t height,
                                            int format, int backend) {
    int usage = (int)(P111_SCREEN_USAGE_READ | P111_SCREEN_USAGE_NATIVE);
    int size[2];
    int offsets[3] = {0, 0, 0};
    int stride = 0;
    int actual_format = -1;
    void *buffer = NULL;
    unsigned char *pixels = NULL;

    linearizer_release_pixmap_locked();
    size[0] = (int)width;
    size[1] = (int)height;

    if (g_linearizer.create_pixmap(&g_linearizer.pixmap,
                                   g_linearizer.context) != 0 ||
        !g_linearizer.pixmap ||
        g_linearizer.set_pixmap_iv(g_linearizer.pixmap,
            P111_SCREEN_PROPERTY_USAGE, &usage) != 0 ||
        g_linearizer.set_pixmap_iv(g_linearizer.pixmap,
            P111_SCREEN_PROPERTY_FORMAT, &format) != 0 ||
        g_linearizer.set_pixmap_iv(g_linearizer.pixmap,
            P111_SCREEN_PROPERTY_BUFFER_SIZE, size) != 0 ||
        g_linearizer.create_pixmap_buffer(g_linearizer.pixmap) != 0 ||
        g_linearizer.get_pixmap_pv(g_linearizer.pixmap,
            P111_SCREEN_PROPERTY_RENDER_BUFFERS, &buffer) != 0 ||
        !buffer ||
        g_linearizer.get_buffer_pv(buffer,
            P111_SCREEN_PROPERTY_POINTER, (void **)&pixels) != 0 ||
        !pixels ||
        g_linearizer.get_buffer_iv(buffer,
            P111_SCREEN_PROPERTY_STRIDE, &stride) != 0 ||
        stride <= 0) {
        altscreen_log("WARN PHASE=FRAME_LINEARIZER_PIXMAP backend=%s format=%d size=%ux%u result=FAILED errno=%d",
                      backend == P111_LINEARIZER_NV12 ? "screen-nv12" : "screen-rgba",
                      format, width, height, errno);
        linearizer_release_pixmap_locked();
        return 0;
    }

    if (g_linearizer.get_buffer_iv(buffer,
            P111_SCREEN_PROPERTY_FORMAT, &actual_format) != 0 ||
        actual_format != format) {
        altscreen_log("WARN PHASE=FRAME_LINEARIZER_PIXMAP backend=%s requested_format=%d actual_format=%d result=FORMAT_MISMATCH",
                      backend == P111_LINEARIZER_NV12 ? "screen-nv12" : "screen-rgba",
                      format, actual_format);
        linearizer_release_pixmap_locked();
        return 0;
    }

    if (g_linearizer.get_buffer_iv(buffer,
            P111_SCREEN_PROPERTY_PLANAR_OFFSETS, offsets) != 0) {
        offsets[0] = offsets[1] = offsets[2] = 0;
        if (backend == P111_LINEARIZER_NV12) {
            altscreen_log("WARN PHASE=FRAME_LINEARIZER_PIXMAP backend=screen-nv12 planar_offsets=UNAVAILABLE action=RGBA_FALLBACK");
            linearizer_release_pixmap_locked();
            return 0;
        }
    }

    if (backend == P111_LINEARIZER_NV12 && offsets[1] <= offsets[0]) {
        altscreen_log("WARN PHASE=FRAME_LINEARIZER_PIXMAP backend=screen-nv12 planar_offsets=%d,%d,%d result=INVALID action=RGBA_FALLBACK",
                      offsets[0], offsets[1], offsets[2]);
        linearizer_release_pixmap_locked();
        return 0;
    }

    if ((backend == P111_LINEARIZER_NV12 && stride < (int)width) ||
        (backend == P111_LINEARIZER_RGBA && stride < (int)(width * 4u))) {
        altscreen_log("WARN PHASE=FRAME_LINEARIZER_PIXMAP backend=%s stride=%d size=%ux%u result=INVALID_STRIDE",
                      backend == P111_LINEARIZER_NV12 ? "screen-nv12" : "screen-rgba",
                      stride, width, height);
        linearizer_release_pixmap_locked();
        return 0;
    }

    g_linearizer.buffer = buffer;
    g_linearizer.pixels = pixels;
    g_linearizer.width = width;
    g_linearizer.height = height;
    g_linearizer.backend = backend;
    g_linearizer.stride = stride;
    g_linearizer.offsets[0] = offsets[0];
    g_linearizer.offsets[1] = offsets[1];
    g_linearizer.offsets[2] = offsets[2];

    altscreen_log("PHASE=FRAME_LINEARIZER_PIXMAP backend=%s format=%d size=%ux%u stride=%d offsets=%d,%d,%d usage=0x%x result=READY",
                  backend == P111_LINEARIZER_NV12 ? "screen-nv12" : "screen-rgba",
                  actual_format, width, height, stride,
                  offsets[0], offsets[1], offsets[2], usage);
    return 1;
}

static int linearizer_configure_locked(p111_screen_window_t window,
                                       uint32_t width, uint32_t height,
                                       uint32_t source_format,
                                       uint32_t source_usage) {
    int win_format = -1, win_usage = -1;
    int win_size[2] = {0, 0};

    if (!window || !width || !height) return 0;
    if (!linearizer_open_api_locked()) return 0;
    if (!linearizer_ensure_scratch_locked(width, height)) {
        altscreen_log("ERROR PHASE=FRAME_LINEARIZER_SCRATCH size=%ux%u result=OOM", width, height);
        return 0;
    }

    if (g_linearizer.pixmap &&
        g_linearizer.width == width &&
        g_linearizer.height == height)
        return 1;

    if (g_linearizer.get_window_iv) {
        (void)g_linearizer.get_window_iv(window, P111_SCREEN_PROPERTY_FORMAT,
                                         &win_format);
        (void)g_linearizer.get_window_iv(window, P111_SCREEN_PROPERTY_USAGE,
                                         &win_usage);
        (void)g_linearizer.get_window_iv(window, P111_SCREEN_PROPERTY_SIZE,
                                         win_size);
    }
    altscreen_log("PHASE=FRAME_NATIVE_WINDOW_METADATA window=%p config_format=%u config_usage=0x%x screen_format=%d screen_usage=0x%x screen_size=%dx%d measured_vendor_format=0x0001000c",
                  window, source_format, source_usage,
                  win_format, win_usage, win_size[0], win_size[1]);

    g_linearizer.source_format = source_format;
    g_linearizer.source_usage = source_usage;

    if (linearizer_create_pixmap_locked(width, height,
                                        P111_SCREEN_FORMAT_NV12,
                                        P111_LINEARIZER_NV12))
        return 1;

    ++g_linearizer.fallback_count;
    altscreen_log("PHASE=FRAME_LINEARIZER_FALLBACK from=screen-nv12 to=screen-rgba reason=NV12_PIXMAP_UNAVAILABLE count=%u",
                  g_linearizer.fallback_count);
    return linearizer_create_pixmap_locked(width, height,
                                           P111_SCREEN_FORMAT_RGBA8888,
                                           P111_LINEARIZER_RGBA);
}

static unsigned char linearizer_clamp_u8(int v) {
    if (v < 0) return 0u;
    if (v > 255) return 255u;
    return (unsigned char)v;
}

static int linearizer_copy_nv12_to_scratch_locked(uint32_t width,
                                                   uint32_t height) {
    uint32_t dst_stride = align_up_u32(width, 128u);
    uint32_t dst_padded_y = align_up_u32(height, 32u);
    uint32_t uv_rows = (height + 1u) >> 1;
    const unsigned char *src_y;
    const unsigned char *src_uv;
    uint32_t y;
    int uv_offset;

    if (!g_linearizer.pixels || g_linearizer.stride < (int)width)
        return 0;
    uv_offset = g_linearizer.offsets[1];
    if (uv_offset <= g_linearizer.offsets[0]) return 0;

    src_y = g_linearizer.pixels + g_linearizer.offsets[0];
    src_uv = g_linearizer.pixels + uv_offset;
    for (y = 0; y < height; ++y)
        memcpy(g_linearizer.scratch + (size_t)y * dst_stride,
               src_y + (size_t)y * g_linearizer.stride, width);
    for (y = 0; y < uv_rows; ++y)
        memcpy(g_linearizer.scratch +
                   (size_t)dst_stride * dst_padded_y +
                   (size_t)y * dst_stride,
               src_uv + (size_t)y * g_linearizer.stride, width);
    return 1;
}

static int linearizer_rgba_to_nv12_locked(uint32_t width, uint32_t height) {
    uint32_t dst_stride = align_up_u32(width, 128u);
    uint32_t dst_padded_y = align_up_u32(height, 32u);
    unsigned char *dst_y;
    unsigned char *dst_uv;
    uint32_t x, y;

    if (!g_linearizer.pixels ||
        g_linearizer.stride < (int)(width * 4u))
        return 0;

    dst_y = g_linearizer.scratch;
    dst_uv = g_linearizer.scratch + (size_t)dst_stride * dst_padded_y;

    /*
     * On the vehicle-tested MHI2Q Screen stack, SCREEN_FORMAT_RGBA8888 is
     * CPU-visible as BGRA byte order. Convert to limited-range BT.601 NV12.
     */
    for (y = 0; y < height; ++y) {
        const unsigned char *src =
            g_linearizer.pixels + g_linearizer.offsets[0] +
            (size_t)y * g_linearizer.stride;
        unsigned char *dy = dst_y + (size_t)y * dst_stride;
        for (x = 0; x < width; ++x) {
            int b = src[(size_t)x * 4u + 0u];
            int g = src[(size_t)x * 4u + 1u];
            int r = src[(size_t)x * 4u + 2u];
            dy[x] = linearizer_clamp_u8(
                ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    for (y = 0; y < height; y += 2u) {
        const unsigned char *r0 =
            g_linearizer.pixels + g_linearizer.offsets[0] +
            (size_t)y * g_linearizer.stride;
        const unsigned char *r1 =
            g_linearizer.pixels + g_linearizer.offsets[0] +
            (size_t)((y + 1u < height) ? y + 1u : y) * g_linearizer.stride;
        unsigned char *duv = dst_uv + (size_t)(y >> 1) * dst_stride;

        for (x = 0; x < width; x += 2u) {
            uint32_t x1 = (x + 1u < width) ? x + 1u : x;
            int b = (r0[(size_t)x * 4u + 0u] +
                     r0[(size_t)x1 * 4u + 0u] +
                     r1[(size_t)x * 4u + 0u] +
                     r1[(size_t)x1 * 4u + 0u] + 2) >> 2;
            int g = (r0[(size_t)x * 4u + 1u] +
                     r0[(size_t)x1 * 4u + 1u] +
                     r1[(size_t)x * 4u + 1u] +
                     r1[(size_t)x1 * 4u + 1u] + 2) >> 2;
            int r = (r0[(size_t)x * 4u + 2u] +
                     r0[(size_t)x1 * 4u + 2u] +
                     r1[(size_t)x * 4u + 2u] +
                     r1[(size_t)x1 * 4u + 2u] + 2) >> 2;
            duv[x] = linearizer_clamp_u8(
                ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            if (x + 1u < width)
                duv[x + 1u] = linearizer_clamp_u8(
                    ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
    return 1;
}

static int linearizer_picture_has_detail_locked(uint32_t width,
                                                 uint32_t height) {
    unsigned minv = 255u, maxv = 0u, samples = 0u;
    uint32_t x, y;

    if (!g_linearizer.scratch || !width || !height) return 0;
    for (y = 0; y < height; y += 16u) {
        const unsigned char *row =
            g_linearizer.scratch + (size_t)y * align_up_u32(width, 128u);
        for (x = 0; x < width; x += 16u) {
            unsigned v = row[x];
            if (v < minv) minv = v;
            if (v > maxv) maxv = v;
            ++samples;
        }
    }
    return samples >= 16u && maxv > minv + 6u;
}

static void linearizer_record_readback_us_locked(uint32_t us) {
    unsigned bucket = (unsigned)(us / 1000u);
    if (bucket > 64u) bucket = 64u;
    ++g_linearizer.readback_hist_ms[bucket];
    if (us > g_linearizer.readback_max_us)
        g_linearizer.readback_max_us = us;
}

static unsigned linearizer_percentile_ms_locked(unsigned percent) {
    uint32_t total = 0u, target, seen = 0u;
    unsigned i;
    for (i = 0; i < 65u; ++i) total += g_linearizer.readback_hist_ms[i];
    if (!total) return 0u;
    target = (total * percent + 99u) / 100u;
    if (!target) target = 1u;
    for (i = 0; i < 65u; ++i) {
        seen += g_linearizer.readback_hist_ms[i];
        if (seen >= target) return i;
    }
    return 64u;
}

static void linearizer_dump_sample_locked(uint32_t width, uint32_t height) {
    FILE *fp;
    char path[160];
    uint32_t stride, padded_y, uv_rows, y;
    if (!env_truth("ALT111_LINEARIZER_SAMPLE_NV12", 0) ||
        g_linearizer.sample_count >= 3u || !g_linearizer.scratch)
        return;

    stride = align_up_u32(width, 128u);
    padded_y = align_up_u32(height, 32u);
    uv_rows = (height + 1u) >> 1;
    snprintf(path, sizeof(path),
             "/tmp/carplay111_linear_%u_%ux%u.nv12",
             g_linearizer.sample_count + 1u, width, height);
    fp = fopen(path, "wb");
    if (!fp) {
        altscreen_log("WARN PHASE=FRAME_LINEARIZER_SAMPLE path=%s result=OPEN_FAILED errno=%d",
                      path, errno);
        return;
    }
    for (y = 0; y < height; ++y)
        (void)fwrite(g_linearizer.scratch + (size_t)y * stride, 1u, width, fp);
    for (y = 0; y < uv_rows; ++y)
        (void)fwrite(g_linearizer.scratch +
                         (size_t)stride * padded_y + (size_t)y * stride,
                     1u, width, fp);
    if (fclose(fp) == 0) {
        ++g_linearizer.sample_count;
        altscreen_log("PHASE=FRAME_LINEARIZER_SAMPLE path=%s result=WRITTEN bytes=%u sample=%u opt_in=1",
                      path, width * height + width * uv_rows,
                      g_linearizer.sample_count);
    }
}

int p111_frame_tap_write_window(void *stream, void *screen_window,
                                uint32_t width, uint32_t height,
                                uint32_t source_format,
                                uint32_t source_usage) {
    int rc;
    int packed = 0;
    int backend;
    int published;
    uint32_t t0, t1, elapsed;

    if (!stream || !screen_window || !width || !height) return 0;
    if (!env_truth("ALT111_DIRECT_FRAME_TAP", 1)) return 1;

    tap_lock();
    if (g_stream && g_stream != stream) {
        ++g_stale_callback_count;
        if (g_stale_callback_count == 1u ||
            (g_stale_callback_count & 255u) == 0u) {
            altscreen_log("PHASE=FRAME_LINEARIZER_STALE_CALLBACK stream=%p current_stream=%p generation=%u count=%u action=DROP_BEFORE_READBACK",
                          stream, g_stream, g_generation,
                          g_stale_callback_count);
        }
        tap_unlock();
        return 1;
    }
    tap_unlock();

    linearizer_lock();
    ++g_linearizer.requests;

    /*
     * Do not rate-limit the producer here. Every valid stock private111 render
     * callback is linearized and published so /carplay111_decoded always
     * carries the freshest frame the phone/stock renderer supplied.
     *
     * The proven sidecar/displayable3 path remains independently paced at
     * 30 fps; this change removes only the producer-side 1/2 or time cap.
     */
    if (!linearizer_configure_locked((p111_screen_window_t)screen_window,
                                     width, height,
                                     source_format, source_usage)) {
        ++g_linearizer.failures;
        linearizer_unlock();
        return 0;
    }

    t0 = tap_now_us32();
    errno = 0;
    rc = g_linearizer.read_window((p111_screen_window_t)screen_window,
                                  g_linearizer.buffer, 0, NULL, 0);
    if (rc != 0 && g_linearizer.backend == P111_LINEARIZER_NV12) {
        ++g_linearizer.fallback_count;
        altscreen_log("WARN PHASE=FRAME_LINEARIZER_READ backend=screen-nv12 rc=%d errno=%d action=RGBA_RETRY count=%u",
                      rc, errno, g_linearizer.fallback_count);
        if (linearizer_create_pixmap_locked(width, height,
                                            P111_SCREEN_FORMAT_RGBA8888,
                                            P111_LINEARIZER_RGBA)) {
            errno = 0;
            rc = g_linearizer.read_window((p111_screen_window_t)screen_window,
                                          g_linearizer.buffer, 0, NULL, 0);
        }
    }
    t1 = tap_now_us32();
    elapsed = t1 - t0;
    linearizer_record_readback_us_locked(elapsed);
    if (elapsed >= 20000u) {
        ++g_linearizer.slow_readback_count;
        if (g_linearizer.slow_readback_count == 1u ||
            (g_linearizer.slow_readback_count % 60u) == 0u) {
            altscreen_log("WARN PHASE=FRAME_LINEARIZER_SLOW readback_us=%u threshold_us=20000 backend=%s slow_count=%u requests=%u",
                          (unsigned)elapsed,
                          g_linearizer.backend == P111_LINEARIZER_NV12 ?
                              "screen-nv12" : "screen-rgba",
                          g_linearizer.slow_readback_count,
                          g_linearizer.requests);
        }
    }

    if (rc != 0) {
        int had_good = g_linearizer.publish_success != 0u;
        ++g_linearizer.failures;
        if (g_linearizer.failures == 1u ||
            (g_linearizer.failures % 60u) == 0u) {
            altscreen_log("ERROR PHASE=FRAME_LINEARIZER_READ backend=%s rc=%d errno=%d failures=%u readback_us=%u action=%s",
                          g_linearizer.backend == P111_LINEARIZER_NV12 ?
                              "screen-nv12" : "screen-rgba",
                          rc, errno, g_linearizer.failures,
                          (unsigned)elapsed,
                          had_good ? "FREEZE_LAST_GOOD" : "DROP_AUX_FRAME");
        }
        linearizer_unlock();
        return had_good ? 1 : 0;
    }

    ++g_linearizer.readback_success;
    backend = g_linearizer.backend;
    if (backend == P111_LINEARIZER_NV12)
        packed = linearizer_copy_nv12_to_scratch_locked(width, height);
    else if (backend == P111_LINEARIZER_RGBA)
        packed = linearizer_rgba_to_nv12_locked(width, height);

    if (!packed) {
        int had_good = g_linearizer.publish_success != 0u;
        ++g_linearizer.failures;
        if (g_linearizer.failures == 1u ||
            (g_linearizer.failures % 60u) == 0u) {
            altscreen_log("WARN PHASE=FRAME_LINEARIZER_PIXEL backend=%s packed=0 failures=%u action=%s",
                          backend == P111_LINEARIZER_NV12 ?
                              "screen-nv12" : "screen-rgba",
                          g_linearizer.failures,
                          had_good ? "FREEZE_LAST_GOOD" : "DROP_AUX_FRAME");
        }
        linearizer_unlock();
        return had_good ? 1 : 0;
    }

    if (!linearizer_picture_has_detail_locked(width, height) &&
        (g_linearizer.readback_success == 1u ||
         (g_linearizer.readback_success % 300u) == 0u)) {
        altscreen_log("PHASE=FRAME_LINEARIZER_PIXEL backend=%s detail=LOW accepted=1 reason=valid_uniform_frame",
                      backend == P111_LINEARIZER_NV12 ?
                          "screen-nv12" : "screen-rgba");
    }

    linearizer_dump_sample_locked(width, height);

    published = p111_frame_tap_write(stream, g_linearizer.scratch,
                                     width, height,
                                     P111_QNX_NV12_FORMAT_LEGACY,
                                     source_usage);
    if (!published) {
        ++g_linearizer.publish_drop;
        if (g_linearizer.publish_drop == 1u ||
            (g_linearizer.publish_drop % 60u) == 0u) {
            altscreen_log("WARN PHASE=FRAME_LINEARIZER_PUBLISH result=DROP readbacks=%u published=%u drops=%u generation_mismatch_or_inactive=1 action=%s",
                          g_linearizer.readback_success,
                          g_linearizer.publish_success,
                          g_linearizer.publish_drop,
                          g_linearizer.publish_success ?
                              "FREEZE_LAST_GOOD" : "WAIT_NOT_READY");
        }
        linearizer_unlock();
        return 1;
    }

    ++g_linearizer.publish_success;
    if (g_linearizer.publish_success == 1u) {
        altscreen_log("PHASE=FRAME_LINEARIZER_FIRST_FRAME backend=%s source_format=%u source_usage=0x%x size=%ux%u readback_us=%u readback_success=%u shm_publish_success=1 output=packed-nv12 window_readback=exact-stock-handle rate_policy=uncapped_source_callbacks sink_target_fps=30",
                      backend == P111_LINEARIZER_NV12 ?
                          "screen-nv12" : "screen-rgba-bt601",
                      source_format, source_usage, width, height,
                      (unsigned)elapsed, g_linearizer.readback_success);
    } else if ((g_linearizer.publish_success % 300u) == 0u) {
        unsigned p50 = linearizer_percentile_ms_locked(50u);
        unsigned p95 = linearizer_percentile_ms_locked(95u);
        altscreen_log("PHASE=FRAME_LINEARIZER_PROGRESS backend=%s readbacks=%u published=%u publish_drops=%u requests=%u rate_policy=uncapped_source_callbacks sink_target_fps=30 failures=%u fallbacks=%u slow_readbacks=%u readback_p50_ms=%u readback_p95_ms=%u readback_max_us=%u size=%ux%u",
                      backend == P111_LINEARIZER_NV12 ?
                          "screen-nv12" : "screen-rgba-bt601",
                      g_linearizer.readback_success,
                      g_linearizer.publish_success,
                      g_linearizer.publish_drop,
                      g_linearizer.requests,
                      g_linearizer.failures,
                      g_linearizer.fallback_count,
                      g_linearizer.slow_readback_count,
                      p50, p95, (unsigned)g_linearizer.readback_max_us,
                      width, height);
    }

    linearizer_unlock();
    return 1;
}

int p111_frame_tap_write(void *stream, const unsigned char *buffer,
                          uint32_t width, uint32_t height,
                          uint32_t format, uint32_t usage) {
    uint32_t pixels, bytes, uv_rows, slot, seq, generation;
    uint32_t src_stride = 0, uv_offset = 0;
    unsigned char *dst;
    int padded;
    uint32_t y;
    unsigned i, start_slot;

    if (!stream || !buffer || !width || !height) return 0;
    if (!env_truth("ALT111_DIRECT_FRAME_TAP", 1)) return 0;

    if (width > 4096u || height > 4096u || width > 0xffffffffu / height)
        return 0;
    pixels = width * height;
    uv_rows = (height + 1u) >> 1;
    if (uv_rows > 0xffffffffu / width) return 0;
    bytes = pixels + width * uv_rows;
    if (!bytes || bytes > P111_FRAME_SLOT_BYTES) return 0;

    tap_lock();
    if (!begin_stream_locked(stream) || !g_frame || !g_frame->active) {
        tap_unlock();
        return 0;
    }

    generation = g_generation;
    seq = ++g_frame_reserve_seq;
    if (!seq) seq = ++g_frame_reserve_seq;

    /*
     * Never copy into the currently published slot: the sidecar may be in the
     * middle of memcpy() from it while global sequence/current_slot still point
     * there. Also never share a slot with another in-flight callback. With three
     * slots this leaves two producer slots behind one stable published slot.
     */
    slot = P111_FRAME_SLOTS;
    start_slot = (unsigned)(seq % P111_FRAME_SLOTS);
    for (i = 0; i < P111_FRAME_SLOTS; ++i) {
        unsigned candidate = (start_slot + i) % P111_FRAME_SLOTS;
        if (g_frame_slot_owner[candidate] != 0u)
            continue;
        if (g_frame->sequence && candidate == g_frame->current_slot)
            continue;
        slot = candidate;
        break;
    }
    if (slot >= P111_FRAME_SLOTS) {
        ++g_frame->drop_count;
        tap_unlock();
        return 0;
    }
    g_frame_slot_owner[slot] = seq;

    dst = &g_frame->data[(size_t)slot * P111_FRAME_SLOT_BYTES];
    padded = qnx_nv12_padded_layout(width, height, format, buffer,
                                    &src_stride, &uv_offset);
    if (!padded) {
        if (g_frame_slot_owner[slot] == seq)
            g_frame_slot_owner[slot] = 0u;
        ++g_frame->drop_count;
        if (g_layout_error_logged_generation != generation) {
            g_layout_error_logged_generation = generation;
            altscreen_log("ERROR PHASE=FRAME_TAP_UNSUPPORTED_LAYOUT stream=%p generation=%u buffer=%p config_format=%u config_usage=0x%x visible=%ux%u action=DROP_STOCK_FORWARD_UNCHANGED log_once_per_generation=1",
                          stream, generation, buffer, format, usage, width, height);
        }
        tap_unlock();
        return 0;
    }
    tap_unlock();

    /*
     * Never hold g_tap_lock across the ~1.2 MB frame copy. ProcessData can
     * continue feeding compressed H264 while the stock-OMX fallback is packed.
     * Only the vehicle-measured QNX NV12 layouts are accepted; unknown layouts
     * are dropped instead of being guessed as tight NV12.
     */
    for (y = 0; y < height; ++y)
        memcpy(dst + (size_t)y * width,
               buffer + (size_t)y * src_stride, width);
    for (y = 0; y < uv_rows; ++y)
        memcpy(dst + (size_t)pixels + (size_t)y * width,
               buffer + (size_t)uv_offset + (size_t)y * src_stride,
               width);

    __sync_synchronize();
    tap_lock();
    if (g_stream != stream || g_generation != generation ||
        !g_frame || !g_frame->active) {
        if (g_frame) ++g_frame->drop_count;
        if (slot < P111_FRAME_SLOTS && g_frame_slot_owner[slot] == seq)
            g_frame_slot_owner[slot] = 0u;
        tap_unlock();
        return 0;
    }
    /*
     * CScreenRender is normally serialized, but do not depend on that ABI
     * detail. If two callbacks copied different slots concurrently, an older
     * reserved sequence must never publish after a newer frame already won.
     * This strengthens the 3-slot producer/consumer contract without changing
     * the SHM layout seen by the existing QNX sidecar.
     */
    if (g_frame->sequence &&
        (int32_t)(seq - g_frame->sequence) <= 0) {
        ++g_frame->drop_count;
        if (g_frame_slot_owner[slot] == seq)
            g_frame_slot_owner[slot] = 0u;
        tap_unlock();
        return 0;
    }

    /*
     * Publish with an explicit invalidation window. The existing reader samples
     * sequence before and after its memcpy. Setting sequence=0 before changing
     * metadata/current_slot guarantees that any mixed old/new snapshot is
     * rejected without changing the SHM ABI or requiring a sidecar rebuild.
     */
    g_frame->sequence = 0u;
    __sync_synchronize();

    g_frame->width = width;
    g_frame->height = height;
    g_frame->stride = width; /* SHM is always packed tight for the sidecar. */
    g_frame->format = P111_FRAME_FORMAT_NV12;
    g_frame->frame_bytes = bytes;
    g_frame->current_slot = slot;
    g_frame->last_copy_bytes = bytes;
    __sync_synchronize();
    g_frame->sequence = seq;
    if (g_frame_slot_owner[slot] == seq)
        g_frame_slot_owner[slot] = 0u;
    ++g_frame->frame_count;
    g_last_frame_publish_us32 = tap_now_us32();

    /* Display is event-driven in V3.4. Publish only after both the compressed
     * producer and at least two decoded frames are live in this generation. */
    if (g_frame->frame_count >= 2u &&
        g_stream_ready_generation != generation)
        (void)stream_ready_publish_locked();

    if (!g_seen_frame) {
        g_seen_frame = 1;
        altscreen_log("PHASE=FRAME_TAP_LAYOUT stream=%p generation=%u buffer=%p config_format=%u config_usage=0x%x visible=%ux%u source_layout=%s source_stride=%u uv_offset=%u packed_stride=%u packed_bytes=%u first16=%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x_%02x%02x%02x%02x",
                      stream, generation, buffer, format, usage,
                      width, height,
                      "qnx_nv12_128x32",
                      src_stride, uv_offset, width, bytes,
                      byte_or_zero(buffer,16,0), byte_or_zero(buffer,16,1),
                      byte_or_zero(buffer,16,2), byte_or_zero(buffer,16,3),
                      byte_or_zero(buffer,16,4), byte_or_zero(buffer,16,5),
                      byte_or_zero(buffer,16,6), byte_or_zero(buffer,16,7),
                      byte_or_zero(buffer,16,8), byte_or_zero(buffer,16,9),
                      byte_or_zero(buffer,16,10), byte_or_zero(buffer,16,11),
                      byte_or_zero(buffer,16,12), byte_or_zero(buffer,16,13),
                      byte_or_zero(buffer,16,14), byte_or_zero(buffer,16,15));
        altscreen_log("PHASE=DECODER_FIRST_FRAME backend=stock-omx-tap stream=%p generation=%u seq=%u format=NV12 size=%ux%u bytes=%u source_stride=%u window58_readback=0",
                      stream, generation, seq, width, height, bytes, src_stride);
    } else if ((g_frame->frame_count % 300u) == 0u) {
        altscreen_log("PHASE=DECODER_PROGRESS backend=stock-omx-tap stream=%p generation=%u frames=%u seq=%u size=%ux%u source_stride=%u drops=%u",
                      stream, generation, g_frame->frame_count, seq,
                      width, height, src_stride, g_frame->drop_count);
    }

    tap_unlock();
    return 1;

}

int p111_frame_tap_get_progress(
        void *stream, struct p111_frame_progress_snapshot *out) {
    int ok = 0;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    tap_lock();
    if (stream && g_stream == stream && g_generation &&
        g_frame && g_frame->active && g_frame->writer_pid &&
        g_frame->frame_count && g_frame->sequence &&
        g_last_frame_publish_us32) {
        out->generation = g_generation;
        out->frame_count = g_frame->frame_count;
        out->sequence = g_frame->sequence;
        out->last_publish_us32 = g_last_frame_publish_us32;
        out->h264_packets = g_h264 && g_h264->active &&
            g_h264->generation == g_generation ? g_h264->packet_count : 0u;
        out->active = 1;
        ok = 1;
    }
    tap_unlock();
    return ok;
}

void p111_direct_tap_stream_end(void *stream) {
    unsigned i;
    int ended_current = 0;

    tap_lock();
    if (!stream || g_stream == stream) {
        ended_current = 1;
        if (g_h264) {
            __sync_synchronize();
            g_h264->active = 0;
        }
        if (g_frame) {
            __sync_synchronize();
            g_frame->active = 0;
        }
        altscreen_log("PHASE=DIRECT111_TAP_STOP stream=%p generation=%u h264_packets=%u decoded_frames=%u",
                      stream, g_generation,
                      g_h264 ? g_h264->packet_count : 0u,
                      g_frame ? g_frame->frame_count : 0u);
        stream_ready_clear_locked();
        g_last_frame_publish_us32 = 0u;
        g_stream = NULL;
    } else {
        altscreen_log("PHASE=DIRECT111_TAP_STOP_STALE stream=%p current_stream=%p generation=%u action=IGNORE_LINEARIZER_TEARDOWN",
                      stream, g_stream, g_generation);
    }
    if (stream) {
        for (i = 0; i < P111_AVCC_CACHE_SLOTS; ++i) {
            if (g_avcc[i].stream == stream)
                memset(&g_avcc[i], 0, sizeof(g_avcc[i]));
        }
    }
    tap_unlock();

    /*
     * A late teardown from an older stream must never destroy the Screen
     * context/pixmap currently serving a newer private111 session.
     */
    if (ended_current) {
        linearizer_lock();
        linearizer_shutdown_locked();
        linearizer_unlock();
    }
}
