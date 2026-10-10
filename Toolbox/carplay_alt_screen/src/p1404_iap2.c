/* p1404_iap2.c - Gate 1 plus iAP2 control-plane evidence. See p1404_iap2.h. */
#include "p1404_iap2.h"
#include "p1404_lock_wait.h"
#include "p1404_abi.h"
#include "altscreen_profile.h"
#include <string.h>
#include <stddef.h>
#include <unistd.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((unsigned)p[0] << 8) | p[1]); }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

uint8_t iap2_link_checksum(const uint8_t *p, size_t n) {
    unsigned sum = 0, i;
    for (i = 0; i < n; ++i) sum = (sum + p[i]) & 0xffu;
    return (uint8_t)((0u - sum) & 0xffu);
}

int iap2_link_checksum_ok(const uint8_t *p, size_t n) {
    unsigned sum = 0, i;
    if (!p || n < 2) return 0;
    for (i = 0; i < n; ++i) sum = (sum + p[i]) & 0xffu;
    return sum == 0;
}

int iap2_csm_params_consistent(const uint8_t *body, size_t blen) {
    size_t off = 0;
    if (!body) return 0;
    while (off + 4 <= blen) {
        uint16_t tl = be16(body + off);
        if (tl < 4 || off + tl > blen) return 0;
        off += tl;
    }
    return off == blen;
}

long iap2_find_csm(const uint8_t *buf, size_t n, size_t from, uint16_t *msg_id) {
    size_t i;
    if (!buf) return -1;
    for (i = from; i + IAP2_CSM_HDR_LEN <= n; ++i) {
        uint16_t start = be16(buf + i), len = be16(buf + i + 2), id = be16(buf + i + 4);
        if (start != IAP2_CSM_START) continue;
        if (len < IAP2_CSM_HDR_LEN || i + len > n) continue;
        if (id == 0) continue;
        if (!iap2_csm_params_consistent(buf + i + IAP2_CSM_HDR_LEN,
                                        (size_t)len - IAP2_CSM_HDR_LEN)) continue;
        if (msg_id) *msg_id = id;
        return (long)i;
    }
    return -1;
}

void iap2_dump_hex(const char *tag, const uint8_t *buf, size_t n) {
    static const char hx[] = "0123456789abcdef";
    char line[3 * IAP2_DUMP_MAX + 48];
    size_t i, used = 0, lim;
    if (!buf || !tag) return;
    lim = n < IAP2_DUMP_MAX ? n : IAP2_DUMP_MAX;
    line[0] = 0;
    line[used++] = (char)91;
    for (i = 0; i < lim; ++i) {
        if (used + 3 >= sizeof(line)) break;
        line[used++] = hx[(buf[i] >> 4) & 0xfu];
        line[used++] = hx[buf[i] & 0xfu];
        line[used++] = (char)32;
    }
    line[used++] = (char)93;
    line[used] = 0;
    altscreen_log("HEX %s n=%u shown=%u %s", tag, (unsigned)n, (unsigned)lim, line);
    if (lim < n)
        altscreen_log("HEX %s truncated at %u of %u", tag, (unsigned)lim, (unsigned)n);
}

int iap2_dump_frame(const char *tag, const uint8_t *buf, size_t n) {
    uint16_t id = 0;
    long at = iap2_find_csm(buf, n, 0, &id);
    uint16_t csm_len;
    if (at < 0) {
        altscreen_log("FRAME %s no-consistent-csm in %u bytes", tag, (unsigned)n);
        return -1;
    }
    csm_len = be16(buf + at + 2);
    altscreen_log("FRAME %s csm=0x%04x at=0x%lx len=%u body=%u payload=metadata-only",
                  tag, id, at, (unsigned)csm_len,
                  (unsigned)(csm_len - IAP2_CSM_HDR_LEN));
    return (int)id;
}

static int is_identification(uint16_t id) {
    /* LIVI and CatPlay both define IdentificationInformation as 0x1D01.
     * 0xEA00/0xEA01 are EAP session control and must never be TLV-mutated. */
    return id == IAP2_CSM_IDENTIFICATION_INFORMATION;
}

static int frame_parts(const uint8_t *pkt, size_t pkt_len, uint16_t *msg_id,
                       const uint8_t **csm, size_t *csm_len,
                       const uint8_t **body, size_t *blen) {
    uint16_t link_len, clen, id;
    if (pkt_len < IAP2_LINK_HDR_LEN + IAP2_CSM_HDR_LEN + 1u) return 0;
    if (be16(pkt) != IAP2_LINK_START) return 0;
    if (!iap2_link_checksum_ok(pkt, IAP2_LINK_HDR_LEN)) return 0;
    link_len = be16(pkt + 2);
    if ((size_t)link_len != pkt_len) return 0;
    *csm = pkt + IAP2_LINK_HDR_LEN;
    if (be16(*csm) != IAP2_CSM_START) return 0;
    clen = be16(*csm + 2);
    id = be16(*csm + 4);
    if (clen < IAP2_CSM_HDR_LEN) return 0;
    if (IAP2_LINK_HDR_LEN + (size_t)clen + 1u != pkt_len) return 0;
    if (!iap2_link_checksum_ok(*csm, (size_t)clen + 1u)) return 0;
    *csm_len = clen;
    if (msg_id) *msg_id = id;
    *body = *csm + IAP2_CSM_HDR_LEN;
    *blen = (size_t)clen - IAP2_CSM_HDR_LEN;
    if (!iap2_csm_params_consistent(*body, *blen)) return 0;
    return 1;
}

/* Runtime evidence records framing metadata and explicitly parsed, allowlisted
 * availability values. Raw CSM bodies can contain authentication material and
 * are never emitted by the production observer. */
static void log_control_csm(const char *dir, const uint8_t *csm, size_t csm_len,
                            const uint8_t *body, size_t blen, uint16_t id,
                            unsigned frame_len, unsigned offset) {
    (void)csm;
    (void)body;
    altscreen_log("IAP2CTRL dir=%s csm=0x%04x frame=%u at=%u csm_len=%u body=%u payload=metadata-only",
                  dir ? dir : "?", id, frame_len, offset,
                  (unsigned)csm_len, (unsigned)blen);
}

int iap2_observe_outgoing(const uint8_t *buf, size_t n) {
    size_t off = 0, found = 0;
    while (off + IAP2_LINK_HDR_LEN + IAP2_CSM_HDR_LEN <= n) {
        const uint8_t *csm, *body;
        size_t csm_len, blen;
        uint16_t id = 0;
        uint16_t total;
        if (be16(buf + off) != IAP2_LINK_START ||
            !iap2_link_checksum_ok(buf + off, IAP2_LINK_HDR_LEN)) { ++off; continue; }
        total = be16(buf + off + 2);
        if (total < IAP2_LINK_HDR_LEN || off + (size_t)total > n) { ++off; continue; }
        if (!frame_parts(buf + off, total, &id, &csm, &csm_len, &body, &blen)) {
            off += total;
            continue;
        }
        log_control_csm("OUT", csm, csm_len, body, blen, id,
                        (unsigned)total, (unsigned)off);
        if (is_identification(id)) {
            ++found;
            altscreen_log("IAP2 OUT csm=0x%04x frame=%u csm_len=%u body=%u profile=%s will_mutate=%d",
                          id, (unsigned)total, (unsigned)csm_len, (unsigned)blen,
                          altscreen_profile_current()->name,
                          altscreen_profile_current()->modifies_bytes);
        }
        off += total;
    }
    return (int)found;
}

static int patch_link_frame(uint8_t *pkt, size_t pkt_len, size_t room) {
    const struct altscreen_profile *p = altscreen_profile_current();
    uint8_t *csm, *body;
    const uint8_t *kcsm, *kbody;
    size_t csm_len, blen, blen_before;
    uint16_t csm_len_new, msg_id;
    uint8_t hdr_cksum, pay_cksum;
    int r;

    if (!frame_parts(pkt, pkt_len, &msg_id, &kcsm, &csm_len, &kbody, &blen)) return 0;
    if (!is_identification(msg_id)) return 0;
    altscreen_log("IAP2 IDENTIFY csm=0x%04x frame=%u csm_len=%u body=%u profile=%s",
                  msg_id, (unsigned)pkt_len, (unsigned)csm_len, (unsigned)blen, p->name);
    altscreen_log("IAP2 IDENTIFY payload=metadata-only csm=0x%04x body=%u",
                  msg_id, (unsigned)blen);

    if (!p->modifies_bytes) {
        altscreen_log("IAP2 OBSERVE profile=%s action=none bytes_changed=0", p->name);
        return 0;
    }
    if (room < pkt_len + IAP2_GROW + 1u) {
        altscreen_log("IAP2 refuse reason=no-room len=%u room=%u",
                      (unsigned)pkt_len, (unsigned)room);
        return 0;
    }
    csm = pkt + IAP2_LINK_HDR_LEN;
    body = csm + IAP2_CSM_HDR_LEN;
    blen_before = blen;
    memmove(body + blen + IAP2_GROW, body + blen, 1);
    r = altscreen_tlv_advertise(body, &blen, blen_before + IAP2_GROW, p);
    if (r <= 0) {
        memmove(body + blen, body + blen + IAP2_GROW, 1);
        altscreen_log("IAP2 no-patch profile=%s result=%d bytes_changed=0", p->name, r);
        return 0;
    }
    csm_len_new = (uint16_t)(IAP2_CSM_HDR_LEN + blen);
    put_be16(csm + 2, csm_len_new);
    put_be16(pkt + 2, (uint16_t)(IAP2_LINK_HDR_LEN + csm_len_new + 1u));
    pay_cksum = iap2_link_checksum(csm, csm_len_new);
    pkt[IAP2_LINK_HDR_LEN + csm_len_new] = pay_cksum;
    hdr_cksum = iap2_link_checksum(pkt, IAP2_LINK_HDR_LEN - 1);
    pkt[IAP2_LINK_HDR_LEN - 1] = hdr_cksum;
    altscreen_log("IAP2 PATCHED profile=%s param=%u sub=%u csm=0x%04x body=%u->%u",
                  p->name, p->param_id, p->sub_id, msg_id,
                  (unsigned)blen_before, (unsigned)blen);
    altscreen_log("IAP2 PATCHED frame=%u->%u csm_len=%u->%u hdr=0x%02x pay=0x%02x",
                  (unsigned)pkt_len,
                  (unsigned)(IAP2_LINK_HDR_LEN + csm_len_new + 1u),
                  (unsigned)csm_len, (unsigned)csm_len_new, hdr_cksum, pay_cksum);
    altscreen_log("IAP2 PATCHED payload=metadata-only inserted_param=%u inserted_sub=%u",
                  p->param_id, p->sub_id);
    altscreen_mark_theme_advertised();
    return (int)(IAP2_LINK_HDR_LEN + csm_len_new + 1u);
}

int iap2_scan_and_patch(uint8_t *buf, size_t n, size_t capacity) {
    size_t off = 0;
    while (off + IAP2_LINK_HDR_LEN + IAP2_CSM_HDR_LEN <= n) {
        uint16_t total;
        if (be16(buf + off) != IAP2_LINK_START ||
            !iap2_link_checksum_ok(buf + off, IAP2_LINK_HDR_LEN)) { ++off; continue; }
        total = be16(buf + off + 2);
        if (total < IAP2_LINK_HDR_LEN || off + total > n) { ++off; continue; }
        if (off + total == n)
            return patch_link_frame(buf + off, total, capacity - off);
        off += total;
    }
    return 0;
}

int iap2_patch_outgoing(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    int r;
    if (!src || !dst || n == 0) return 0;
    if (n + IAP2_GROW + 1u > cap) return 0;
    memcpy(dst, src, n);
    r = iap2_scan_and_patch(dst, n, cap);
    return r;
}

static int parse_group_available(const uint8_t *body, size_t blen, uint16_t param,
                                 const char *label) {
    size_t off = 0;
    while (off + 4 <= blen) {
        uint16_t tl = be16(body + off), id = be16(body + off + 2);
        size_t sub;
        if (tl < 4 || off + tl > blen) {
            altscreen_log("AVAIL %s malformed=truncated_param at=0x%x len=%u body=%u",
                          label, (unsigned)off, (unsigned)tl, (unsigned)blen);
            return CP_AVAIL_MALFORMED;
        }
        if (id != param) { off += tl; continue; }
        sub = off + 4;
        while (sub + 4 <= off + tl) {
            uint16_t sl = be16(body + sub), sid = be16(body + sub + 2);
            if (sl < 4 || sub + sl > off + tl) {
                altscreen_log("AVAIL %s malformed=truncated_sub param=%u", label, param);
                return CP_AVAIL_MALFORMED;
            }
            if (sid == CP_AVAIL_AVAILABLE_SUB) {
                unsigned plen = (unsigned)(sl - 4);
                if (plen != CP_AVAIL_BOOL_LEN) {
                    altscreen_log("AVAIL %s malformed=bool_length param=%u sub=%u len=%u",
                                  label, param, sid, plen);
                    return CP_AVAIL_MALFORMED;
                }
                {
                    unsigned v = body[sub + 4];
                    altscreen_log("AVAIL %s param=%u sub=%u byte=0x%02x -> %s",
                                  label, param, sid, v, v ? "YES" : "NO");
                    return v ? CP_AVAIL_YES : CP_AVAIL_NO;
                }
            }
            sub += sl;
        }
        altscreen_log("AVAIL %s param=%u present_without_Available_sub -> UNKNOWN",
                      label, param);
        return CP_AVAIL_UNKNOWN;
    }
    if (off != blen) {
        altscreen_log("AVAIL %s malformed=ragged_tail body=%u", label, (unsigned)blen);
        return CP_AVAIL_MALFORMED;
    }
    altscreen_log("AVAIL %s param=%u absent -> UNKNOWN", label, param);
    return CP_AVAIL_UNKNOWN;
}

int iap2_parse_attr_available(const uint8_t *body, size_t blen, uint16_t param,
                              const char *label) {
    if (!body || blen < 4) return CP_AVAIL_MALFORMED;
    return parse_group_available(body, blen, param, label ? label : "attr");
}

int iap2_parse_theme_availability(const uint8_t *body, size_t blen) {
    return iap2_parse_attr_available(body, blen, CP_AVAIL_THEME_PARAM, "theme_assets");
}

#define IAP2_CONTROL_FD_SLOTS 8u
#define IAP2_CONTROL_BUFFER_BYTES IAP2_ASSEMBLER_MAX
#define IAP2_MUTATION_BUFFER_BYTES IAP2_ASSEMBLER_MAX

struct iap2_control_fd_slot {
    int active;
    int fd;
    uint32_t generation;
    int confirmed;
    size_t incoming_used;
    size_t outgoing_used;
    size_t mutate_used;
    size_t mutate_next_used;
    /* A checksum-valid frame above the mutation policy limit is streamed
     * unchanged.  The remaining byte count prevents nested payload bytes from
     * being mistaken for a new frame across callbacks. */
    size_t oversize_remaining;
    size_t oversize_next_remaining;
    int mutate_busy;
    uint8_t incoming[IAP2_CONTROL_BUFFER_BYTES];
    uint8_t outgoing[IAP2_CONTROL_BUFFER_BYTES];
    uint8_t mutate_source[IAP2_MUTATION_BUFFER_BYTES];
    /* OUTPUT is tentative until real libc I/O makes positive progress. Keeping
     * the next state separate makes a zero-progress cancel mutation-only: FD
     * confirmation, incoming observation and prior fragments remain intact. */
    uint8_t mutate_next[IAP2_MUTATION_BUFFER_BYTES];
};

static struct iap2_control_fd_slot g_control_fds[IAP2_CONTROL_FD_SLOTS];
static volatile unsigned g_control_fd_guard;

static void control_fd_lock(void) {
    while (__sync_lock_test_and_set(&g_control_fd_guard, 1u) != 0u) p1404_lock_wait_yield();
}

static void control_fd_unlock(void) {
    __sync_lock_release(&g_control_fd_guard);
}

static struct iap2_control_fd_slot *control_fd_find_locked(int fd,
                                                            uint32_t generation) {
    unsigned i;
    for (i = 0; i < IAP2_CONTROL_FD_SLOTS; ++i)
        if (g_control_fds[i].active && g_control_fds[i].fd == fd &&
            g_control_fds[i].generation == generation)
            return &g_control_fds[i];
    return NULL;
}

static struct iap2_control_fd_slot *control_fd_find_any_locked(int fd) {
    unsigned i;
    for (i = 0; i < IAP2_CONTROL_FD_SLOTS; ++i)
        if (g_control_fds[i].active && g_control_fds[i].fd == fd)
            return &g_control_fds[i];
    return NULL;
}

static struct iap2_control_fd_slot *control_fd_allocate_locked(int fd,
                                                                uint32_t generation) {
    struct iap2_control_fd_slot *unconfirmed = NULL;
    unsigned i;
    for (i = 0; i < IAP2_CONTROL_FD_SLOTS; ++i) {
        if (!g_control_fds[i].active) {
            unconfirmed = &g_control_fds[i];
            break;
        }
        if (!g_control_fds[i].confirmed && !g_control_fds[i].mutate_busy &&
            g_control_fds[i].mutate_used == 0u &&
            g_control_fds[i].oversize_remaining == 0u && !unconfirmed)
            unconfirmed = &g_control_fds[i];
    }
    /* An unconfirmed observation-only candidate is evictable. In particular,
     * source bytes already acknowledged into mutate_source are not: they must
     * survive unrelated descriptor churn and TX queue exhaustion. */
    if (!unconfirmed) return NULL;
    memset(unconfirmed, 0, sizeof(*unconfirmed));
    unconfirmed->active = 1;
    unconfirmed->fd = fd;
    unconfirmed->generation = generation;
    return unconfirmed;
}

static int buffer_has_candidate(const uint8_t *buf, size_t n) {
    size_t i;
    if (!buf) return 0;
    for (i = 0; i < n; ++i)
        if (buf[i] == 0xffu && (i + 1u == n || buf[i + 1u] == 0x5au)) return 1;
    return 0;
}

int iap2_mutation_buffer_candidate(const uint8_t *buf, size_t n) {
    return buffer_has_candidate(buf, n);
}

/* Extract one complete checksum-valid CSM frame from a per-FD byte stream.
 * Return 1 for a frame, 0 for an incomplete buffer, and -1 after discarding one
 * invalid byte. This helper performs no logging and is called only under the
 * short metadata lock. */
static int control_extract_frame(uint8_t *data, size_t data_capacity,
                                 size_t *used, uint8_t *frame,
                                 size_t frame_capacity, uint16_t *total_out,
                                 uint16_t *id_out) {
    uint16_t total, id;
    const uint8_t *csm, *body;
    size_t csm_len, blen;
    if (total_out) *total_out = 0;
    if (id_out) *id_out = 0;
    if (!data || !used || !frame || *used > data_capacity) return -1;
    while (*used >= 2u && be16(data) != IAP2_LINK_START) {
        memmove(data, data + 1, --*used);
    }
    if (*used < IAP2_LINK_HDR_LEN) return 0;
    if (!iap2_link_checksum_ok(data, IAP2_LINK_HDR_LEN)) {
        memmove(data, data + 1, --*used);
        return -1;
    }
    total = be16(data + 2);
    if (total < IAP2_LINK_HDR_LEN + IAP2_CSM_HDR_LEN + 1u ||
        (size_t)total > frame_capacity) {
        memmove(data, data + 1, --*used);
        return -1;
    }
    if ((size_t)total > *used || (size_t)total > data_capacity) return 0;
    if (!frame_parts(data, total, &id, &csm, &csm_len, &body, &blen)) {
        memmove(data, data + 1, --*used);
        return -1;
    }
    (void)csm; (void)csm_len; (void)body; (void)blen;
    memcpy(frame, data, total);
    memmove(data, data + total, *used - total);
    *used -= total;
    if (total_out) *total_out = total;
    if (id_out) *id_out = id;
    return 1;
}

void iap2_control_fd_reset(void) {
    control_fd_lock();
    memset(g_control_fds, 0, sizeof(g_control_fds));
    control_fd_unlock();
}

void iap2_control_fd_close_gen(int fd, uint32_t generation) {
    struct iap2_control_fd_slot *slot;
    if (fd < 0) return;
    for (;;) {
        control_fd_lock();
        slot = control_fd_find_locked(fd, generation);
        if (!slot || !slot->mutate_busy) {
            if (slot) memset(slot, 0, sizeof(*slot));
            control_fd_unlock();
            return;
        }
        control_fd_unlock();
        p1404_lock_wait_yield();
    }
}

int iap2_control_fd_note_outgoing_gen(int fd, uint32_t generation,
                                      const uint8_t *buf, size_t n) {
    struct iap2_control_fd_slot *slot;
    uint8_t frame[IAP2_DUMP_MAX];
    uint16_t total = 0, id = 0;
    int identified = 0, overflow = 0, extracted;
    if (fd < 0 || !buf || !n) return 0;

    control_fd_lock();
    slot = control_fd_find_locked(fd, generation);
    if (!slot && buffer_has_candidate(buf, n)) slot = control_fd_allocate_locked(fd, generation);
    if (!slot) {
        control_fd_unlock();
        return 0;
    }
    if (n > sizeof(slot->outgoing) - slot->outgoing_used) {
        slot->outgoing_used = 0;
        if (!slot->confirmed && !slot->mutate_busy)
            memset(slot, 0, sizeof(*slot));
        overflow = 1;
    } else {
        memcpy(slot->outgoing + slot->outgoing_used, buf, n);
        slot->outgoing_used += n;
    }
    control_fd_unlock();
    if (overflow) {
        altscreen_log("WARN IAP2_FD_BUFFER_RESET fd=%d direction=OUT incoming=%u capacity=%u payload=metadata-only",
                      fd, (unsigned)n, (unsigned)IAP2_CONTROL_BUFFER_BYTES);
        return 0;
    }

    for (;;) {
        control_fd_lock();
        slot = control_fd_find_locked(fd, generation);
        if (!slot) { control_fd_unlock(); break; }
        extracted = control_extract_frame(slot->outgoing, sizeof(slot->outgoing),
                                          &slot->outgoing_used, frame,
                                          sizeof(frame), &total, &id);
        if (extracted == 1 && is_identification(id)) {
            slot->confirmed = 1;
            identified = 1;
        }
        if (extracted == 0 && !slot->confirmed &&
            slot->outgoing_used == 0 && !slot->mutate_busy)
            memset(slot, 0, sizeof(*slot));
        control_fd_unlock();
        if (extracted == 0) break;
        if (extracted < 0) continue;
        (void)iap2_observe_outgoing(frame, total);
        if (is_identification(id)) {
            altscreen_log("IAP2_FD_IDENTIFIED fd=%d direction=OUT frame=%u csm=0x%04x framing=stream-validated payload=metadata-only",
                          fd, (unsigned)total, id);
            identified = 1;
        }
    }
    return identified;
}

int iap2_control_fd_mutation_candidate_gen(int fd, uint32_t generation,
                                            const uint8_t *buf, size_t n) {
    struct iap2_control_fd_slot *slot;
    int candidate = 0;
    if (fd < 0 || !buf || !n) return 0;
    control_fd_lock();
    slot = control_fd_find_locked(fd, generation);
    if (slot && (slot->mutate_used != 0u ||
                 slot->oversize_remaining != 0u)) candidate = 1;
    control_fd_unlock();
    /* The wrapper may receive arbitrary bytes before a link frame, or only the
     * first sync byte at the end of this call. Both must reserve the per-FD TX
     * slot before the bounded assembler accepts anything. */
    return candidate || buffer_has_candidate(buf, n);
}

/* Return the longest prefix that can be forwarded without losing a possible
 * fragmented link frame. Invalid bytes are forwarded unchanged; a valid header
 * with an incomplete declared length remains buffered for the next call. */
static size_t mutation_complete_prefix(const uint8_t *data, size_t used,
                                       size_t *oversize_remaining) {
    size_t off = 0;
    if (oversize_remaining) *oversize_remaining = 0u;
    while (off < used) {
        uint16_t total;
        if (data[off] != 0xffu) { ++off; continue; }
        if (used - off < 2u) break;
        if (data[off + 1u] != 0x5au) { ++off; continue; }
        if (used - off < IAP2_LINK_HDR_LEN) break;
        if (!iap2_link_checksum_ok(data + off, IAP2_LINK_HDR_LEN)) {
            ++off;
            continue;
        }
        total = be16(data + off + 2u);
        if (total < IAP2_LINK_HDR_LEN + IAP2_CSM_HDR_LEN + 1u) {
            ++off;
            continue;
        }
        if ((size_t)total > IAP2_MUTATABLE_FRAME_MAX) {
            size_t initialized = used - off;
            if (initialized < (size_t)total) {
                if (oversize_remaining)
                    *oversize_remaining = (size_t)total - initialized;
                /* Emit only initialized bytes now.  Subsequent callbacks use
                 * oversize_remaining and never scan the payload for sync. */
                return used;
            }
            off += (size_t)total;
            continue;
        }
        if (used - off < (size_t)total) break;
        off += (size_t)total;
    }
    return off;
}

/* Transform every complete frame in an assembled prefix, preserving unrelated
 * bytes and coalesced frames byte-for-byte. The caller supplies enough room for
 * the bounded worst case; failure leaves no partial output. */
static int mutation_transform_prefix(const uint8_t *src, size_t n,
                                     uint8_t *out, size_t cap,
                                     size_t *out_len) {
    size_t in = 0, written = 0;
    uint8_t patched[IAP2_PATCH_SCRATCH_MAX];
    if (out_len) *out_len = 0;
    if (!src || !out || !out_len) return 0;
    while (in < n) {
        uint16_t total = 0;
        int patch_len = 0;
        if (n - in >= IAP2_LINK_HDR_LEN && be16(src + in) == IAP2_LINK_START &&
            iap2_link_checksum_ok(src + in, IAP2_LINK_HDR_LEN)) {
            total = be16(src + in + 2u);
            if (total >= IAP2_LINK_HDR_LEN + IAP2_CSM_HDR_LEN + 1u &&
                total <= IAP2_MUTATABLE_FRAME_MAX &&
                n - in >= (size_t)total)
                patch_len = iap2_patch_outgoing(src + in, total, patched,
                                                sizeof(patched));
        }
        if (written > cap) return 0;
        if (patch_len > 0) {
            if ((size_t)patch_len > cap - written) return 0;
            memcpy(out + written, patched, (size_t)patch_len);
            written += (size_t)patch_len;
            in += (size_t)total;
        } else if (total >= IAP2_LINK_HDR_LEN + IAP2_CSM_HDR_LEN + 1u &&
                   (size_t)total <= n - in) {
            if ((size_t)total > cap - written) return 0;
            memcpy(out + written, src + in, (size_t)total);
            written += (size_t)total;
            in += (size_t)total;
        } else {
            if (written == cap) return 0;
            out[written++] = src[in++];
        }
    }
    *out_len = written;
    return 1;
}

int iap2_control_fd_prepare_mutation_gen(int fd, uint32_t generation,
                                         const uint8_t *buf, size_t n,
                                     uint8_t *out, size_t out_cap,
                                     size_t *out_len, int *had_pending) {
    struct iap2_control_fd_slot *slot;
    uint8_t assembled[IAP2_MUTATION_BUFFER_BYTES];
    size_t prior = 0, prefix = 0, remaining = 0;
    size_t old_skip = 0, new_skip = 0, raw_skip = 0;
    int allocated = 0, transformed = 0;
    if (out_len) *out_len = 0;
    if (had_pending) *had_pending = 0;
    if (fd < 0 || !buf || !n || !out || !out_len || !had_pending)
        return IAP2_MUTATE_PASS;

    /* Production reserves/serializes the bearer TX slot before entering here.
     * Keep this guard as a second line of defense for direct callers and close. */
    for (;;) {
        control_fd_lock();
        slot = control_fd_find_locked(fd, generation);
        if (!slot || !slot->mutate_busy) break;
        control_fd_unlock();
        p1404_lock_wait_yield();
    }
    if (!slot) {
        /* A possible sync can start after unrelated prefix bytes, including a
         * lone trailing 0xff completed by the next accepted call. */
        if (!buffer_has_candidate(buf, n)) {
            control_fd_unlock();
            return IAP2_MUTATE_PASS;
        }
        slot = control_fd_allocate_locked(fd, generation);
        allocated = slot != NULL;
    } else if (slot->mutate_used == 0u &&
               slot->oversize_remaining == 0u &&
               !buffer_has_candidate(buf, n)) {
        control_fd_unlock();
        return IAP2_MUTATE_PASS;
    }
    if (!slot) {
        control_fd_unlock();
        return IAP2_MUTATE_PASS;
    }
    prior = slot->mutate_used;
    old_skip = slot->oversize_remaining;
    *had_pending = prior != 0u || old_skip != 0u;
    if (prior > sizeof(slot->mutate_source) ||
        n > sizeof(slot->mutate_source) - prior) {
        /* Never acknowledge bytes we cannot retain. With no prior fragment the
         * original call can safely pass through; with prior accepted bytes keep
         * the transaction buffered and make no new progress. */
        if (!prior && !old_skip && allocated) memset(slot, 0, sizeof(*slot));
        control_fd_unlock();
        return (prior || old_skip) ? IAP2_MUTATE_ERROR : IAP2_MUTATE_PASS;
    }

    /* Build a tentative state without consuming the persistent fragment. */
    if (prior) memcpy(assembled, slot->mutate_source, prior);
    memcpy(assembled + prior, buf, n);
    if (old_skip) {
        raw_skip = n < old_skip ? n : old_skip;
        if (n <= old_skip) {
            prefix = n;
            new_skip = old_skip - n;
        } else {
            prefix = raw_skip + mutation_complete_prefix(buf + raw_skip,
                                                         n - raw_skip,
                                                         &new_skip);
        }
    } else {
        prefix = mutation_complete_prefix(assembled, prior + n, &new_skip);
    }
    if (!prefix) {
        memcpy(slot->mutate_source + prior, buf, n);
        slot->mutate_used = prior + n;
        control_fd_unlock();
        return IAP2_MUTATE_BUFFERED;
    }
    remaining = prior + n - prefix;
    slot->mutate_busy = 1;
    control_fd_unlock();

    if (old_skip) {
        size_t tail_len = 0;
        if (raw_skip <= out_cap) {
            memcpy(out, buf, raw_skip);
            *out_len = raw_skip;
            transformed = prefix == raw_skip ? 1 :
                mutation_transform_prefix(buf + raw_skip, prefix - raw_skip,
                                          out + raw_skip, out_cap - raw_skip,
                                          &tail_len);
            if (transformed) *out_len += tail_len;
        }
    } else {
        transformed = mutation_transform_prefix(assembled, prefix, out, out_cap,
                                                 out_len);
    }

    control_fd_lock();
    slot = control_fd_find_locked(fd, generation);
    if (!slot || !slot->mutate_busy) {
        control_fd_unlock();
        return IAP2_MUTATE_ERROR;
    }
    if (transformed) {
        if (remaining)
            memcpy(slot->mutate_next, assembled + prefix, remaining);
        slot->mutate_next_used = remaining;
        slot->oversize_next_remaining = new_skip;
        /* Keep mutate_busy set. The caller owns the matching bearer TX slot
         * and will atomically choose commit or cancel after real libc I/O. */
    } else {
        slot->mutate_next_used = 0;
        slot->oversize_next_remaining = 0;
        slot->mutate_busy = 0;
        if (allocated && !slot->confirmed && slot->incoming_used == 0u &&
            slot->outgoing_used == 0u && slot->mutate_used == 0u)
            memset(slot, 0, sizeof(*slot));
    }
    control_fd_unlock();

    if (!transformed) {
        /* Persistent fragment state is still byte-for-byte unchanged. */
        altscreen_log("ERROR IAP2_MUTATION_OUTPUT_CAP fd=%d source=%u capacity=%u rollback=1 payload=metadata-only",
                      fd, (unsigned)prefix, (unsigned)out_cap);
        return prior ? IAP2_MUTATE_ERROR : IAP2_MUTATE_PASS;
    }
    return IAP2_MUTATE_OUTPUT;
}

int iap2_control_fd_prepare_drain_gen(int fd, uint32_t generation,
                                      uint8_t *out, size_t out_cap,
                                      size_t *out_len) {
    struct iap2_control_fd_slot *slot;
    if (out_len) *out_len = 0u;
    if (fd < 0 || !out || !out_len) return IAP2_MUTATE_ERROR;
    for (;;) {
        control_fd_lock();
        slot = control_fd_find_locked(fd, generation);
        if (!slot || !slot->mutate_busy) break;
        control_fd_unlock();
        p1404_lock_wait_yield();
    }
    if (!slot || slot->mutate_used == 0u) {
        control_fd_unlock();
        return IAP2_MUTATE_PASS;
    }
    if (slot->mutate_used > out_cap) {
        control_fd_unlock();
        return IAP2_MUTATE_ERROR;
    }
    memcpy(out, slot->mutate_source, slot->mutate_used);
    *out_len = slot->mutate_used;
    slot->mutate_next_used = 0u;
    slot->oversize_next_remaining = slot->oversize_remaining;
    slot->mutate_busy = 1;
    control_fd_unlock();
    return IAP2_MUTATE_OUTPUT;
}

int iap2_control_fd_commit_mutation_gen(int fd, uint32_t generation) {
    struct iap2_control_fd_slot *slot;
    int committed = 0;
    control_fd_lock();
    slot = control_fd_find_locked(fd, generation);
    if (slot && slot->mutate_busy) {
        if (slot->mutate_next_used)
            memcpy(slot->mutate_source, slot->mutate_next,
                   slot->mutate_next_used);
        slot->mutate_used = slot->mutate_next_used;
        slot->oversize_remaining = slot->oversize_next_remaining;
        slot->mutate_next_used = 0;
        slot->oversize_next_remaining = 0;
        slot->mutate_busy = 0;
        committed = 1;
    }
    control_fd_unlock();
    return committed;
}

int iap2_control_fd_cancel_mutation_gen(int fd, uint32_t generation) {
    struct iap2_control_fd_slot *slot;
    int cancelled = 0;
    control_fd_lock();
    slot = control_fd_find_locked(fd, generation);
    if (slot && slot->mutate_busy) {
        /* mutate_source is the exact pre-call state and was never overwritten.
         * Do not clear any other per-FD observer or confirmation state. */
        slot->mutate_next_used = 0;
        slot->oversize_next_remaining = 0;
        slot->mutate_busy = 0;
        cancelled = 1;
    }
    control_fd_unlock();
    return cancelled;
}

int iap2_control_fd_feed_incoming_gen(int fd, uint32_t generation,
                                      const uint8_t *buf, size_t n) {
    struct iap2_control_fd_slot *slot;
    uint8_t frame[IAP2_DUMP_MAX];
    uint16_t total = 0, id = 0;
    int observed = 0, overflow = 0, extracted;
    if (fd < 0 || !buf || !n) return 0;

    /* Incoming bytes alone never identify a descriptor. Only an outgoing,
     * checksum-valid Identification CSM can bind this FD to iAP2 control. */
    control_fd_lock();
    slot = control_fd_find_locked(fd, generation);
    if (!slot && generation == 0u) slot = control_fd_find_any_locked(fd);
    if (!slot || !slot->confirmed) {
        control_fd_unlock();
        return 0;
    }
    if (n > sizeof(slot->incoming) - slot->incoming_used) {
        slot->incoming_used = 0;
        overflow = 1;
    } else {
        memcpy(slot->incoming + slot->incoming_used, buf, n);
        slot->incoming_used += n;
    }
    control_fd_unlock();
    if (overflow) {
        altscreen_log("WARN IAP2_FD_BUFFER_RESET fd=%d direction=IN incoming=%u capacity=%u payload=metadata-only",
                      fd, (unsigned)n, (unsigned)IAP2_CONTROL_BUFFER_BYTES);
        return 0;
    }

    for (;;) {
        control_fd_lock();
        slot = control_fd_find_locked(fd, generation);
        if (!slot && generation == 0u) slot = control_fd_find_any_locked(fd);
        if (!slot || !slot->confirmed) { control_fd_unlock(); break; }
        extracted = control_extract_frame(slot->incoming, sizeof(slot->incoming),
                                          &slot->incoming_used, frame,
                                          sizeof(frame), &total, &id);
        control_fd_unlock();
        if (extracted == 0) break;
        if (extracted < 0) continue;
        altscreen_log("IAP2_FD_FRAME fd=%d direction=IN frame=%u csm=0x%04x payload=metadata-only",
                      fd, (unsigned)total, id);
        (void)iap2_scan_incoming(frame, total);
        ++observed;
    }
    return observed;
}

/* Compatibility entry points keep direct unit users on generation zero. */
void iap2_control_fd_close(int fd) {
    iap2_control_fd_close_gen(fd, 0u);
}
int iap2_control_fd_note_outgoing(int fd, const uint8_t *buf, size_t n) {
    return iap2_control_fd_note_outgoing_gen(fd, 0u, buf, n);
}
int iap2_control_fd_mutation_candidate(int fd, const uint8_t *buf, size_t n) {
    return iap2_control_fd_mutation_candidate_gen(fd, 0u, buf, n);
}
int iap2_control_fd_prepare_mutation(int fd, const uint8_t *buf, size_t n,
                                     uint8_t *out, size_t out_cap,
                                     size_t *out_len, int *had_pending) {
    return iap2_control_fd_prepare_mutation_gen(fd, 0u, buf, n, out, out_cap,
                                                out_len, had_pending);
}
int iap2_control_fd_commit_mutation(int fd) {
    return iap2_control_fd_commit_mutation_gen(fd, 0u);
}
int iap2_control_fd_cancel_mutation(int fd) {
    return iap2_control_fd_cancel_mutation_gen(fd, 0u);
}
int iap2_control_fd_feed_incoming(int fd, const uint8_t *buf, size_t n) {
    return iap2_control_fd_feed_incoming_gen(fd, 0u, buf, n);
}
void iap2_control_fd_peer_eof_gen(int fd, uint32_t generation) {
    struct iap2_control_fd_slot *slot;
    control_fd_lock();
    slot = control_fd_find_locked(fd, generation);
    if (slot) slot->incoming_used = 0u;
    control_fd_unlock();
}

int iap2_scan_incoming(const uint8_t *buf, size_t n) {
    uint16_t msg_id = 0;
    size_t off = 0;
    int found = 0;
    int any_csm = 0;
    while (off + IAP2_CSM_HDR_LEN <= n) {
        long at = iap2_find_csm(buf, n, off, &msg_id);
        uint16_t csm_len;
        const uint8_t *body;
        size_t blen;
        if (at < 0) break;
        csm_len = be16(buf + at + 2);
        body = buf + at + IAP2_CSM_HDR_LEN;
        blen = (size_t)csm_len - IAP2_CSM_HDR_LEN;
        any_csm = 1;
        log_control_csm("IN", buf + at, (size_t)csm_len, body, blen, msg_id,
                        (unsigned)n, (unsigned)at);
        if (msg_id == IAP2_CSM_CARPLAY_AVAILABILITY) {
            int theme;
            altscreen_log("CARPLAY_AVAILABILITY_SEEN csm=0x4300 at=0x%lx len=%u body=%u",
                          at, (unsigned)csm_len, (unsigned)blen);
            (void)iap2_parse_attr_available(body, blen, CP_AVAIL_WIRED_PARAM, "wired_aux");
            (void)iap2_parse_attr_available(body, blen, CP_AVAIL_WIRELESS_PARAM, "wireless_aux");
            theme = iap2_parse_theme_availability(body, blen);
            altscreen_mark_theme_available(theme);
            found = 1;
        }
        off = (size_t)at + csm_len;
    }
    if (!any_csm && n >= IAP2_CSM_HDR_LEN)
        altscreen_log("FRAME incoming-unclassified bytes=%u payload=metadata-only",
                      (unsigned)n);
    return found;
}
