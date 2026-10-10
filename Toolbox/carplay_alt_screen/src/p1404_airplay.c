/* p1404_airplay.c - AltScreen /info + SETUP negotiation and stream evidence. */
#include "p1404_airplay.h"
#include "p1404_abi.h"
#include "altscreen_paths.h"
#include "p1404_observe.h"
#include "p1404_cockpit_native.h"
#include "private111_direct_tap.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

/* Optional outside the exact K1004 direct overlay (not linked by some host
 * fixtures and legacy preload builds). */
extern void *p1404_direct_stock_symbol_named(const char *name)
    __attribute__((weak));
extern int altscreen_runtime_wait_ready(unsigned timeout_ms)
    __attribute__((weak));

#define ALTSCREEN_NEGOTIATION_WAIT_MS 500u
#define ALT111_BOOTSTRAP_WIDTH 1440u
#define ALT111_BOOTSTRAP_HEIGHT 542u

static volatile uint32_t g_alt_advertised_width;
static volatile uint32_t g_alt_advertised_height;
static volatile int g_alt_advertised_geometry_provisional;

int alt_flag_info;
int alt_flag_feature;
int alt_flag_create111;
int alt_flag_iap2;

void *alt_real_screen_stream_process_data;
void *alt_real_screen_stream_create;

void alt_note_displays_container(const char *tag, void *container);
static void bind_real(void);
static void bind_cf(void);

static int alt_runtime_negotiation_ready(const char *phase) {
    if (!altscreen_runtime_is_ready || altscreen_runtime_is_ready()) return 1;
    if (altscreen_runtime_wait_ready &&
        altscreen_runtime_wait_ready(ALTSCREEN_NEGOTIATION_WAIT_MS)) {
        altscreen_log("PHASE=COLD_START_NEGOTIATION_WAIT result=READY phase=%s max_wait_ms=%u fixed_delay=0",
                      phase ? phase : "unknown", ALTSCREEN_NEGOTIATION_WAIT_MS);
        return 1;
    }
    altscreen_log("WARN PHASE=COLD_START_NEGOTIATION_WAIT result=TIMEOUT phase=%s max_wait_ms=%u action=STOCK_PASSTHROUGH",
                  phase ? phase : "unknown", ALTSCREEN_NEGOTIATION_WAIT_MS);
    return 0;
}

int alt_airplay_negotiation_geometry_ready(void) {
    uint32_t width = 0, height = 0;
    if (p1404_cockpit_native_get_geometry(&width, &height)) return 1;
    return ALT111_BOOTSTRAP_WIDTH > 0u && ALT111_BOOTSTRAP_HEIGHT > 0u;
}

int alt_airplay_validate_runtime_geometry(uint32_t width, uint32_t height) {
    uint32_t advertised_width =
        __sync_fetch_and_add(&g_alt_advertised_width, 0u);
    uint32_t advertised_height =
        __sync_fetch_and_add(&g_alt_advertised_height, 0u);
    int provisional =
        __sync_fetch_and_add(&g_alt_advertised_geometry_provisional, 0);

    if (!advertised_width || !advertised_height) return 1;
    if (advertised_width == width && advertised_height == height) {
        if (provisional)
            altscreen_log("PHASE=ALT111_BOOTSTRAP_GEOMETRY_CONFIRMED advertised=%ux%u runtime=%ux%u",
                          advertised_width, advertised_height, width, height);
        return 1;
    }
    altscreen_log("ERROR PHASE=ALT111_GEOMETRY_CONTRACT_MISMATCH advertised=%ux%u runtime=%ux%u provisional=%d action=REFUSE_PRIVATE_RENDERER reconnect_required=1",
                  advertised_width, advertised_height, width, height, provisional);
    return 0;
}

/* QNX libairplay does not supply the ARM EABI division helpers. Keep geometry
 * arithmetic self-contained so the overlay has no hidden loader dependency. */
static uint32_t alt_div_u32(uint32_t numerator, uint32_t denominator) {
    uint32_t quotient = 0, bit = 1;
    if (!denominator) return 0;
    while (denominator <= (numerator >> 1) && !(denominator & 0x80000000u)) {
        denominator <<= 1;
        bit <<= 1;
    }
    while (bit) {
        if (numerator >= denominator) {
            numerator -= denominator;
            quotient |= bit;
        }
        denominator >>= 1;
        bit >>= 1;
    }
    return quotient;
}

/* Exact call shapes cross-checked against the P1404 call sites and the 210.x
 * receiver headers. CopyProperty returns a retained CF object and takes outErr as
 * arg5. PlatformControl is SIX args (r0-r3 plus two stack words). DisplaysInfo is
 * (screenSession, outErr), not a one-argument function. */
typedef void *(*scp_fn)(void *self, unsigned flags, void *property,
                        void *qualifier, int *out_err);
typedef int   (*ctl_fn)(void *session, unsigned flags, void *command,
                        const void *qualifier, const void *params,
                        void **out_params);
typedef int   (*setup_fn)(void *session, const void *params, void **out_response);
typedef void *(*cdi_fn)(void *screen_session, int *out_err);
typedef int   (*ssp_fn)(void *stream, const void *data, unsigned len);

static scp_fn real_server_copy;
static scp_fn real_session_copy;
static ctl_fn real_session_control;
static setup_fn real_session_setup;
static cdi_fn real_copy_displays;
static ssp_fn real_stream_process;

typedef unsigned long cf_typeid;
static cf_typeid (*cf_get_typeid)(const void *);
static cf_typeid (*cf_array_typeid)(void);
static cf_typeid (*cf_dict_typeid)(void);
static cf_typeid (*cf_number_typeid)(void);
static cf_typeid (*cf_string_typeid)(void);
static cf_typeid (*cf_data_typeid)(void);
static unsigned (*cf_array_count)(cf_obj);
static const void *(*cf_array_at)(cf_obj, unsigned);
static void (*cf_array_append)(cf_obj, const void *);
static unsigned (*cf_dict_count)(cf_obj);
static const void *(*cf_dict_get)(cf_obj, const void *);
static void (*cf_dict_set)(cf_obj, const void *, const void *);
static void (*cf_dict_get_keys_values)(cf_obj, const void **, const void **);
static int (*cf_num_get)(cf_obj, int, void *);
static long (*cf_data_len)(cf_obj);
static const uint8_t *(*cf_data_ptr)(cf_obj);
/* Stock CFLStringGetCStringPtr is an OSStatus-style THREE-argument helper:
 * (string, out_c_string, out_length).  It does not return the character
 * pointer.  Declaring it as a one-argument pointer leaves r1/r2 as garbage on
 * ARM; the stock helper then stores through those garbage output pointers. */
static int (*cfl_cstr_ptr)(cf_obj, const char **, size_t *);
static void (*cf_release)(cf_obj);

/* Raw P1404/CoreFoundation ABI. On ARM AAPCS, wrong arity changes register
 * placement; notably CFNumberCreateInt64(int64_t) consumes r0:r1. */
#define P1404_CF_STRING_ENCODING_UTF8 0x08000100u
static cf_obj (*raw_cf_array_create_mutable)(cf_obj, unsigned, const void *);
static cf_obj (*raw_cf_dict_create_mutable)(cf_obj, unsigned, const void *, const void *);
static cf_obj (*raw_cf_str_new)(cf_obj, const char *, unsigned);
/* Stock AUG22 CFStringGetCString is the local three-argument variant
 * (string, buffer, capacity).  It does not consume an encoding argument. */
static int (*raw_cf_str_cstr)(cf_obj, char *, long);
static cf_obj (*raw_cf_num_new)(int64_t);

/* Stock P1404 uses these CFL callback tables for retained container semantics.
 * Our own mutable arrays/dictionaries default to the same tables. */
static const void *cf_default_array_callbacks;
static const void *cf_default_dict_key_callbacks;
static const void *cf_default_dict_value_callbacks;

/* Compatibility adapter shapes used inside this translation unit and by the
 * fullchain wrapper after it includes this file. They are not raw CF ABI. */
static cf_obj (*cf_array_create_mutable)(cf_obj, unsigned, const void *, const void *);
static cf_obj (*cf_dict_create_mutable)(cf_obj, unsigned, const void *, const void *, const void *);
static cf_obj (*cf_str_new)(const char *, int);
static int (*cf_str_cstr)(cf_obj, char *, int);
static cf_obj (*cf_num_new)(const void *, int64_t);

static cf_obj cf_array_create_adapter(cf_obj allocator, unsigned capacity,
                                      const void *callbacks, const void *unused) {
    (void)unused;
    if (!callbacks) callbacks = cf_default_array_callbacks;
    if (!raw_cf_array_create_mutable || !callbacks) return NULL;
    return raw_cf_array_create_mutable(allocator, capacity, callbacks);
}

static cf_obj cf_dict_create_adapter(cf_obj allocator, unsigned capacity,
                                     const void *key_callbacks,
                                     const void *value_callbacks,
                                     const void *unused) {
    (void)unused;
    if (!key_callbacks) key_callbacks = cf_default_dict_key_callbacks;
    if (!value_callbacks) value_callbacks = cf_default_dict_value_callbacks;
    if (!raw_cf_dict_create_mutable || !key_callbacks || !value_callbacks) return NULL;
    return raw_cf_dict_create_mutable(allocator, capacity, key_callbacks, value_callbacks);
}

static cf_obj cf_str_new_adapter(const char *s, int legacy_encoding) {
    (void)legacy_encoding;
    return raw_cf_str_new ? raw_cf_str_new(NULL, s, P1404_CF_STRING_ENCODING_UTF8) : NULL;
}

static int cf_str_cstr_adapter(cf_obj s, char *buf, int cap) {
    return raw_cf_str_cstr ? raw_cf_str_cstr(s, buf, (long)cap) : 0;
}

static cf_obj cf_num_new_adapter(const void *unused_allocator, int64_t v) {
    (void)unused_allocator;
    return raw_cf_num_new ? raw_cf_num_new(v) : NULL;
}

#define CF_NUMBER_SINT64 4
#define DISPLAY_FEATURE_KNOBS 0x02
#define DISPLAY_FEATURE_HIGH_FIDELITY_TOUCH 0x08
#define PRIMARY_INPUT_KNOBS 3
#define PROBE_DEPTH_MAX 6
#define PROBE_ARRAY_MAX 32u
#define PROBE_DICT_MAX 48u
#define PROBE_DATA_MAX 64u

static void cf_release_safe(cf_obj o) {
    if (o && cf_release) cf_release(o);
}

static cf_obj cf_dict_get_cstr(cf_obj dict, const char *key) {
    cf_obj ks, v = NULL;
    if (!dict || !key || !cf_str_new || !cf_dict_get || !cf_release) return NULL;
    ks = cf_str_new(key, -1);
    if (!ks) return NULL;
    v = (cf_obj)cf_dict_get(dict, ks);
    cf_release(ks);
    return v;
}

static int cf_dict_set_cstr_obj(cf_obj dict, const char *key, cf_obj value) {
    cf_obj ks;
    if (!dict || !key || !value || !cf_str_new || !cf_dict_set || !cf_release) return 0;
    ks = cf_str_new(key, -1);
    if (!ks) return 0;
    cf_dict_set(dict, ks, value);
    cf_release(ks);
    return 1;
}

void p1404_airplay_load_flags(void) {
    /*
     * V3.4: AltScreen is a normal capability of every CarPlay session while
     * this preload is installed.  INFO/FEATURE/CREATE111 are therefore not
     * controlled by SD-resident test markers.  Keep the legacy iAP2
     * ThemeAssets mutation disabled; V3.3 proved private111 without it.
     */
    alt_flag_info      = 1;
    alt_flag_feature   = 1;
    alt_flag_create111 = 1;
    alt_flag_iap2      = 0;
    altscreen_log("PHASE=FLAGS_LOADED policy=V34_ALWAYS_ON mutate=%d info=%d feature=%d create111=%d iap2=%d",
                  p1404_mutate_armed, alt_flag_info, alt_flag_feature,
                  alt_flag_create111, alt_flag_iap2);
}

static void bind_cf(void) {
    cf_get_typeid = (cf_typeid(*)(const void *))dlsym(RTLD_DEFAULT, "CFGetTypeID");
    cf_array_typeid = (cf_typeid(*)(void))dlsym(RTLD_DEFAULT, "CFArrayGetTypeID");
    cf_dict_typeid = (cf_typeid(*)(void))dlsym(RTLD_DEFAULT, "CFDictionaryGetTypeID");
    cf_number_typeid = (cf_typeid(*)(void))dlsym(RTLD_DEFAULT, "CFNumberGetTypeID");
    cf_string_typeid = (cf_typeid(*)(void))dlsym(RTLD_DEFAULT, "CFStringGetTypeID");
    cf_data_typeid = (cf_typeid(*)(void))dlsym(RTLD_DEFAULT, "CFDataGetTypeID");
    cf_array_count = (unsigned(*)(cf_obj))dlsym(RTLD_DEFAULT, "CFArrayGetCount");
    cf_array_at = (const void *(*)(cf_obj,unsigned))dlsym(RTLD_DEFAULT, "CFArrayGetValueAtIndex");
    cf_array_append = (void(*)(cf_obj,const void *))dlsym(RTLD_DEFAULT, "CFArrayAppendValue");
    raw_cf_array_create_mutable = (cf_obj(*)(cf_obj,unsigned,const void *))
                                  dlsym(RTLD_DEFAULT, "CFArrayCreateMutable");
    cf_default_array_callbacks = dlsym(RTLD_DEFAULT, "kCFLArrayCallBacksCFLTypes");
    cf_array_create_mutable = raw_cf_array_create_mutable && cf_default_array_callbacks ?
                              cf_array_create_adapter : NULL;
    cf_dict_count = (unsigned(*)(cf_obj))dlsym(RTLD_DEFAULT, "CFDictionaryGetCount");
    cf_dict_get = (const void *(*)(cf_obj,const void *))dlsym(RTLD_DEFAULT, "CFDictionaryGetValue");
    cf_dict_set = (void(*)(cf_obj,const void *,const void *))dlsym(RTLD_DEFAULT, "CFDictionarySetValue");
    cf_dict_get_keys_values = (void(*)(cf_obj,const void **,const void **))
                              dlsym(RTLD_DEFAULT, "CFDictionaryGetKeysAndValues");
    raw_cf_dict_create_mutable = (cf_obj(*)(cf_obj,unsigned,const void *,const void *))
                                 dlsym(RTLD_DEFAULT, "CFDictionaryCreateMutable");
    cf_default_dict_key_callbacks = dlsym(RTLD_DEFAULT, "kCFLDictionaryKeyCallBacksCFLTypes");
    cf_default_dict_value_callbacks = dlsym(RTLD_DEFAULT, "kCFLDictionaryValueCallBacksCFLTypes");
    cf_dict_create_mutable = raw_cf_dict_create_mutable && cf_default_dict_key_callbacks &&
                             cf_default_dict_value_callbacks ? cf_dict_create_adapter : NULL;
    raw_cf_str_new = (cf_obj(*)(cf_obj,const char *,unsigned))
                     dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    cf_str_new = raw_cf_str_new ? cf_str_new_adapter : NULL;
    raw_cf_str_cstr = (int(*)(cf_obj,char *,long))
                      dlsym(RTLD_DEFAULT, "CFStringGetCString");
    cf_str_cstr = raw_cf_str_cstr ? cf_str_cstr_adapter : NULL;
    raw_cf_num_new = (cf_obj(*)(int64_t))dlsym(RTLD_DEFAULT, "CFNumberCreateInt64");
    cf_num_new = raw_cf_num_new ? cf_num_new_adapter : NULL;
    cf_num_get = (int(*)(cf_obj,int,void *))dlsym(RTLD_DEFAULT, "CFNumberGetValue");
    cf_data_len = (long(*)(cf_obj))dlsym(RTLD_DEFAULT, "CFDataGetLength");
    cf_data_ptr = (const uint8_t *(*)(cf_obj))dlsym(RTLD_DEFAULT, "CFDataGetBytePtr");
    cfl_cstr_ptr = (int(*)(cf_obj,const char **,size_t *))
                   dlsym(RTLD_DEFAULT, "CFLStringGetCStringPtr");
    cf_release = (void(*)(cf_obj))dlsym(RTLD_DEFAULT, "CFRelease");
    altscreen_log("CF bind rawabi=standard array=%d dict=%d string=%d cfl_cstr3=%d number64=%d callbacks=%d/%d/%d typeid=%d release=%d",
                  raw_cf_array_create_mutable != 0, raw_cf_dict_create_mutable != 0,
                  raw_cf_str_new != 0, cfl_cstr_ptr != 0, raw_cf_num_new != 0,
                  cf_default_array_callbacks != 0, cf_default_dict_key_callbacks != 0,
                  cf_default_dict_value_callbacks != 0, cf_get_typeid != 0,
                  cf_release != 0);
}

int alt_cf_is_array(const void *o) {
    return o && cf_get_typeid && cf_array_typeid && cf_get_typeid(o) == cf_array_typeid();
}

int alt_cf_is_dict(const void *o) {
    return o && cf_get_typeid && cf_dict_typeid && cf_get_typeid(o) == cf_dict_typeid();
}

static int alt_cf_is_string(const void *o) {
    return o && cf_get_typeid && cf_string_typeid && cf_get_typeid(o) == cf_string_typeid();
}

static int alt_cf_is_data(const void *o) {
    return o && cf_get_typeid && cf_data_typeid && cf_get_typeid(o) == cf_data_typeid();
}

int alt_cf_int64(const void *o, int64_t *out) {
    if (!o || !out || !cf_get_typeid || !cf_number_typeid || !cf_num_get) return 0;
    if (cf_get_typeid(o) != cf_number_typeid()) return 0;
    return cf_num_get((cf_obj)o, CF_NUMBER_SINT64, out) != 0;
}

const char *alt_cf_cString(void *o, char *buf, size_t cap) {
    const char *p = NULL;
    size_t length = 0u;
    if (!buf || !cap) return "";
    buf[0] = 0;
    if (!o) return buf;
    if (cfl_cstr_ptr &&
        cfl_cstr_ptr((cf_obj)o, &p, &length) == 0 && p != NULL) {
        (void)length;
        return p;
    }
    if (cf_str_cstr) cf_str_cstr((cf_obj)o, buf, (int)cap);
    return buf;
}

int alt_cf_data_bytes(const void *o, const uint8_t **data, size_t *bytes) {
    long n;
    const uint8_t *p;
    if (data) *data = NULL;
    if (bytes) *bytes = 0u;
    if (!o || !data || !bytes) return 0;
    if (!cf_data_len || !cf_data_ptr || !cf_get_typeid || !cf_data_typeid)
        bind_cf();
    if (!alt_cf_is_data(o) || !cf_data_len || !cf_data_ptr) return 0;
    n = cf_data_len((cf_obj)o);
    if (n <= 0) return 0;
    p = cf_data_ptr((cf_obj)o);
    if (!p) return 0;
    *data = p;
    *bytes = (size_t)n;
    return 1;
}

void p1404_airplay_bind(void) { bind_cf(); bind_real(); }

/* Public interposers may be reached from an earlier dependency constructor.
 * Resolve their stock targets lazily instead of fabricating NULL/-1 before this
 * library's constructor has run. Mutation remains disabled until identity and
 * transaction authorization complete. */
static void p1404_airplay_bind_lazy(void) {
    if (real_server_copy && real_session_copy && real_session_control && real_session_setup &&
        real_copy_displays && real_stream_process) return;
    (void)p1404_bind_stock_exports();
    bind_real();
}

static void bind_real(void) {
    real_server_copy = (scp_fn)p1404.server_platform_copy_property;
    real_session_copy = (scp_fn)p1404.session_platform_copy_property;
    real_session_control = (ctl_fn)p1404.session_platform_control;
    real_session_setup = (setup_fn)p1404.session_setup;
    real_copy_displays = (cdi_fn)p1404.screen_copy_displays_info;
    real_stream_process = (ssp_fn)p1404.screen_stream_process_data;
    alt_real_screen_stream_process_data = p1404.screen_stream_process_data;
    alt_real_screen_stream_create = p1404.screen_stream_create;
}

/* The assembly trampolines can be called by libairplax constructors before our
 * constructor or asynchronous worker. They must obtain the relocation-backed
 * K1004 stock target before doing anything else; returning synthetic -1 here
 * can leave Main110 partially constructed and later crash stock teardown/audio
 * threads. */
static void *stock_stream_target(int create) {
    const char *name = create ? "ScreenStreamCreate" : "ScreenStreamProcessData";
    void **slot = create ? &alt_real_screen_stream_create :
                           &alt_real_screen_stream_process_data;
    void *target = *slot;
    if (target) return target;
    target = p1404_direct_stock_symbol_named ?
        p1404_direct_stock_symbol_named(name) : NULL;
    if (target) {
        *slot = target;
        return target;
    }
    (void)p1404_bind_stock_exports();
    bind_real();
    return *slot;
}

static int set_i64(cf_obj dict, const char *key, int64_t v) {
    cf_obj ks = NULL, ns = NULL;
    int ok = 0;
    if (!cf_str_new || !cf_num_new || !cf_dict_set || !cf_release) return 0;
    ks = cf_str_new(key, -1);
    ns = cf_num_new(NULL, v);
    if (ks && ns) {
        cf_dict_set(dict, ks, ns);
        ok = 1;
    }
    cf_release_safe(ks);
    cf_release_safe(ns);
    return ok;
}

static cf_obj rect_dict(uint32_t w, uint32_t h, uint32_t x, uint32_t y) {
    cf_obj d;
    if (!cf_dict_create_mutable) return NULL;
    d = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    if (!d) return NULL;
    if (!set_i64(d, "widthPixels", (int64_t)w) ||
        !set_i64(d, "heightPixels", (int64_t)h) ||
        !set_i64(d, "originXPixels", (int64_t)x) ||
        !set_i64(d, "originYPixels", (int64_t)y)) {
        cf_release_safe(d);
        return NULL;
    }
    return d;
}

struct alt_safe_rect {
    /* safeArea coordinates sent to CarPlay (source/canvas space). */
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;

    /* Same safe region after renderer translation, in physical VC space. */
    int physical_x;
    int physical_y;
    uint32_t physical_w;
    uint32_t physical_h;

    /* Live full-size map-plane translation. No scaling. */
    int renderer_dx;
    int renderer_dy;

    char view[16];
    char layout[128];
    char source[32];
};

static void alt_trim_line(char *s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static int alt_kv_text(const char *line, const char *key,
                       char *out, size_t cap) {
    size_t n;
    if (!line || !key || !out || !cap) return 0;
    n = strlen(key);
    if (strncmp(line, key, n) || line[n] != '=') return 0;
    strncpy(out, line + n + 1u, cap - 1u);
    out[cap - 1u] = 0;
    alt_trim_line(out);
    return 1;
}

static int alt_kv_i32(const char *line, const char *key, int *out) {
    char *end = NULL;
    long value;
    size_t n;
    if (!line || !key || !out) return 0;
    n = strlen(key);
    if (strncmp(line, key, n) || line[n] != '=') return 0;
    value = strtol(line + n + 1u, &end, 10);
    if (end == line + n + 1u || value < -8192L || value > 8192L) return 0;
    *out = (int)value;
    return 1;
}

static int alt_safe_rect_valid(const struct alt_safe_rect *r,
                               uint32_t w, uint32_t h) {
    if (!r || !r->w || !r->h) return 0;
    if (r->x > w || r->y > h) return 0;
    if (r->w > w - r->x || r->h > h - r->y) return 0;
    return 1;
}

static int alt_load_hmi_layout(char *view, size_t view_cap,
                               char *layout, size_t layout_cap,
                               int *small_dx, int *small_dy) {
    FILE *f;
    char line[256];
    int have_view = 0, have_layout = 0;
    int have_dx = 0, have_dy = 0;
    if (!view || !view_cap || !layout || !layout_cap ||
        !small_dx || !small_dy) return 0;
    view[0] = 0;
    layout[0] = 0;
    *small_dx = 0;
    *small_dy = 0;
    f = fopen("/tmp/mmi-mirror-hmi.state", "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        alt_trim_line(line);
        if (alt_kv_text(line, "view", view, view_cap))
            have_view = 1;
        else if (alt_kv_text(line, "layout_name", layout, layout_cap))
            have_layout = 1;
        else if (alt_kv_i32(line, "small_stage_dx", small_dx))
            have_dx = 1;
        else if (alt_kv_i32(line, "small_stage_dy", small_dy))
            have_dy = 1;
    }
    fclose(f);

    /*
     * The vehicle-tested B9Sport layout has the stock map-plane SMALL offset
     * (-476,0). Prefer the live Layout values from Java, but keep this verified
     * fallback so a temporarily older state writer cannot silently center the
     * Sport SMALL map.
     */
    if (have_layout && strstr(layout, "LayoutMIB2HighB9Sport")) {
        if (!have_dx) *small_dx = -476;
        if (!have_dy) *small_dy = 0;
    }
    return have_view && have_layout;
}

static int alt_is_measured_b9_canvas(uint32_t display_w,
                                     uint32_t display_h) {
    /*
     * Vehicle evidence spans three related vertical extents:
     *  - 542: negotiated private111 Screen/display canvas,
     *  - 540: OEM cluster screen model,
     *  - 455: effective map/displayable3 plane.
     * The measured FULL/SMALL safe rectangles fit all three. Keep the guard
     * narrow to these observed B9 geometries instead of accepting arbitrary
     * 1440-wide displays.
     */
    return display_w == 1440u &&
           (display_h == 542u || display_h == 540u || display_h == 455u);
}

/*
 * V3.3 keeps the OEM-measured horizontal safeArea geometry and tunes only the
 * vertical insets after the first V3.2/V3.3 vehicle observation.
 *
 * The private111 coded canvas remains 1440x542 and the V3.1 renderer remains
 * 1:1 into the 1440x455 displayable3 viewport. CarPlay now receives a vertical
 * safe region from y=68 through bottom=450:
 *
 *   FULL  X/W stays 370/700, Y/H becomes 68/382
 *   SMALL X/W stays 490/460, Y/H becomes 68/382
 *
 * Relative to V3.1, the top edge moves down by 19 px so compass/top controls
 * have more clearance. Relative to V3.2, the bottom edge moves up by only 5 px
 * so ETA remains low while no longer using the absolute bottom edge.
 * The OEM terminal-space map-plane Y=26 remains destination metadata only.
 */
static int alt_load_measured_k1004_safe_area(uint32_t display_w,
                                             uint32_t display_h,
                                             const char *view,
                                             const char *layout,
                                             int small_dx,
                                             int small_dy,
                                             struct alt_safe_rect *out) {
    struct alt_safe_rect r;
    int64_t physical_x, physical_y;
    if (!view || !layout || !out) return 0;
    if (!alt_is_measured_b9_canvas(display_w, display_h)) return 0;
    if (!strstr(layout, "LayoutMIB2HighB9") &&
        !strstr(layout, "LayoutMIB2HighQ7")) return 0;

    memset(&r, 0, sizeof(r));

    /*
     * ListModel176 remains the source for the OEM horizontal bounds. V3.5
     * preserves X/W and applies the vehicle-tuned vertical safe region
     * y=72,h=378 (bottom=450). The Audi layout may translate the whole map
     * plane afterwards; that compositor operation remains separate and must
     * not be cancelled here.
     */
    if (!strcmp(view, "SMALL")) {
        r.x = 490u;
        r.y = 72u;
        r.w = 460u;
        r.h = 378u;
        r.physical_w = 460u;
        r.physical_h = 378u;
        r.renderer_dx = small_dx;
        r.renderer_dy = small_dy;
    } else if (!strcmp(view, "FULL")) {
        r.x = 370u;
        r.y = 72u;
        r.w = 700u;
        r.h = 378u;
        r.physical_w = 700u;
        r.physical_h = 378u;
        r.renderer_dx = 0;
        r.renderer_dy = 0;
    } else {
        return 0;
    }
    if (!r.h) return 0;

    /*
     * Horizontal safe bounds retain the measured OEM map-local geometry.
     * Vertically, V3.5 uses the tuned 72..450 visible-map range. The whole-map
     * renderer translation is still applied afterwards (Sport SMALL contributes
     * -476 on X and normally 0 on Y).
     */
    physical_x = (int64_t)(!strcmp(view, "SMALL") ? 490u : 370u) +
                 (int64_t)r.renderer_dx;
    physical_y = 72 + (int64_t)r.renderer_dy;
    if (physical_x < -8192 || physical_x > 8192 ||
        physical_y < -8192 || physical_y > 8192) {
        return 0;
    }
    r.physical_x = (int)physical_x;
    r.physical_y = (int)physical_y;

    strncpy(r.view, view, sizeof(r.view) - 1u);
    r.view[sizeof(r.view) - 1u] = 0;
    strncpy(r.layout, layout, sizeof(r.layout) - 1u);
    r.layout[sizeof(r.layout) - 1u] = 0;
    strncpy(r.source, "k1004-oem-x-vertical-72-450-v35", sizeof(r.source) - 1u);
    r.source[sizeof(r.source) - 1u] = 0;
    *out = r;
    return 1;
}

static void alt_resolve_cluster_safe_area(uint32_t display_w,
                                          uint32_t display_h,
                                          struct alt_safe_rect *out) {
    char view[16] = "";
    char layout[128] = "";
    int small_dx = 0, small_dy = 0;
    struct alt_safe_rect r;
    int have_hmi;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->renderer_dx = 0;
    out->renderer_dy = 0;
    strncpy(out->view, "FULL", sizeof(out->view) - 1u);
    out->view[sizeof(out->view) - 1u] = 0;
    strncpy(out->layout, "UNKNOWN", sizeof(out->layout) - 1u);
    out->layout[sizeof(out->layout) - 1u] = 0;

    /*
     * /info can be requested before Java has published the first HMI snapshot.
     * For the measured B9 cluster canvas, predeclare the known FULL safe region
     * rather than falling back to a one-area/full-bleed shape that would make
     * same-session FULL<->SMALL switching impossible later.
     */
    if (alt_is_measured_b9_canvas(display_w, display_h)) {
        out->x = 370u;
        out->y = 72u;
        out->w = 700u;
        out->h = 378u;
        out->physical_x = 370;
        out->physical_y = 75;
        out->physical_w = 700u;
        out->physical_h = 378u;
        strncpy(out->source, "k1004-default-v35-72-450-before-hmi",
                sizeof(out->source) - 1u);
    } else {
        out->w = display_w;
        out->h = display_h;
        out->physical_x = 0;
        out->physical_y = 0;
        out->physical_w = display_w;
        out->physical_h = display_h;
        strncpy(out->source, "fullscreen-fallback", sizeof(out->source) - 1u);
    }
    out->source[sizeof(out->source) - 1u] = 0;

    have_hmi = alt_load_hmi_layout(
        view, sizeof(view), layout, sizeof(layout), &small_dx, &small_dy);

    if (have_hmi && alt_load_measured_k1004_safe_area(
            display_w, display_h, view, layout,
            small_dx, small_dy, &r)) {
        *out = r;
        return;
    }
}

static cf_obj make_view_areas_with_safe(uint32_t w, uint32_t h,
                                        const struct alt_safe_rect *safe_rect) {
    cf_obj view = NULL, safe = NULL, areas = NULL;
    uint32_t sx = 0u, sy = 0u, sw = w, sh = h;
    if (!cf_array_create_mutable || !cf_array_append || !cf_dict_set) return NULL;
    if (safe_rect && alt_safe_rect_valid(safe_rect, w, h)) {
        sx = safe_rect->x;
        sy = safe_rect->y;
        sw = safe_rect->w;
        sh = safe_rect->h;
    }
    view = rect_dict(w, h, 0, 0);
    safe = rect_dict(sw, sh, sx, sy);
    if (!view || !safe) goto fail;
    if (!cf_dict_set_cstr_obj(view, "safeArea", safe)) goto fail;
    cf_release_safe(safe); safe = NULL;
    areas = cf_array_create_mutable(NULL, 0, NULL, NULL);
    if (!areas) goto fail;
    cf_array_append(areas, view);
    cf_release_safe(view); view = NULL;
    return areas;
fail:
    cf_release_safe(view);
    cf_release_safe(safe);
    cf_release_safe(areas);
    return NULL;
}

static cf_obj make_view_areas(uint32_t w, uint32_t h) {
    return make_view_areas_with_safe(w, h, NULL);
}


/*
 * Type111 may declare multiple candidate viewAreas.  Apple cluster templates
 * do this as well; unlike type110, ALT entries do not carry
 * viewAreaTransitionControl/viewAreaStatusBarEdge flags. The outer viewArea
 * keeps the runtime coded canvas (observed 1440x542 on the private111 path);
 * only the nested safeArea changes. The displayable3 sink remains 1440x455.
 *
 * index 0 = FULL  (370,72,700x378)
 * index 1 = SMALL (490,72,460x378)
 *
 * V3.5 preserves the V3.1/V3.2 horizontal OEM constraints while using the
 * vehicle-tuned vertical safe region top=72, bottom=450.
 */
static cf_obj make_cluster_layout_view_areas(uint32_t w, uint32_t h,
                                             int enable_two_areas) {
    struct alt_safe_rect full, small;
    cf_obj areas = NULL, v = NULL, safe = NULL;

    if (!cf_array_create_mutable || !cf_array_append || !cf_dict_set)
        return NULL;

    areas = cf_array_create_mutable(NULL, 0, NULL, NULL);
    if (!areas) return NULL;

    memset(&full, 0, sizeof(full));
    full.x = 370u;
    full.y = 72u;
    full.w = 700u;
    full.h = 378u;
    if (!full.h || !alt_safe_rect_valid(&full, w, h)) goto fail;

    v = rect_dict(w, h, 0u, 0u);
    safe = rect_dict(full.w, full.h, full.x, full.y);
    if (!v || !safe || !cf_dict_set_cstr_obj(v, "safeArea", safe)) goto fail;
    cf_release_safe(safe); safe = NULL;
    cf_array_append(areas, v);
    cf_release_safe(v); v = NULL;

    if (!enable_two_areas) return areas;

    memset(&small, 0, sizeof(small));
    small.x = 490u;
    small.y = 72u;
    small.w = 460u;
    small.h = 378u;
    if (!small.h || !alt_safe_rect_valid(&small, w, h)) goto fail;

    v = rect_dict(w, h, 0u, 0u);
    safe = rect_dict(small.w, small.h, small.x, small.y);
    if (!v || !safe || !cf_dict_set_cstr_obj(v, "safeArea", safe)) goto fail;
    cf_release_safe(safe); safe = NULL;
    cf_array_append(areas, v);
    cf_release_safe(v); v = NULL;
    return areas;

fail:
    cf_release_safe(safe);
    cf_release_safe(v);
    cf_release_safe(areas);
    return NULL;
}

static cf_obj make_i64_array_one(int64_t value) {
    cf_obj a = NULL, n = NULL;
    if (!cf_array_create_mutable || !cf_array_append || !cf_num_new)
        return NULL;
    a = cf_array_create_mutable(NULL, 0, NULL, NULL);
    n = cf_num_new(NULL, value);
    if (!a || !n) {
        cf_release_safe(n);
        cf_release_safe(a);
        return NULL;
    }
    cf_array_append(a, n);
    cf_release_safe(n);
    return a;
}

void *alt_build_cluster_display(void) {
    const struct altscreen_display *d;
    uint32_t width = 0, height = 0;
    uint32_t physical_width, physical_height;
    struct alt_safe_rect cluster_safe;
    cf_obj dict = NULL, areas = NULL, adjacent = NULL, vs = NULL;
    int layout_known = 0;
    int two_area_capable = 0;
    int initial_view_area = 0;
    int geometry_provisional = 0;
    /*
     * V3.5 cold-start policy: /info must not permanently miss AltScreen just
     * because Screen display-1 appears a few hundred milliseconds later than
     * the first CarPlay capability transaction. Prefer live measured geometry;
     * if it is not queryable yet, advertise the already vehicle-proven
     * 1440x542 B9 Type111 canvas as a negotiation-only bootstrap contract.
     * The private renderer later re-queries Screen and refuses activation on
     * any mismatch, so this is not a silent renderer-size fallback.
     */
    if (!p1404_cockpit_native_refresh_geometry()) {
        if (!p1404_cockpit_native_get_geometry(&width, &height)) {
            width = ALT111_BOOTSTRAP_WIDTH;
            height = ALT111_BOOTSTRAP_HEIGHT;
            geometry_provisional = 1;
            (void)altscreen_set_cluster_geometry(width, height);
            altscreen_log("PHASE=ALT111_NEGOTIATION_GEOMETRY source=BOOTSTRAP size=%ux%u provisional=1 renderer_verified=0",
                          width, height);
        } else {
            altscreen_log("PHASE=ALT111_NEGOTIATION_GEOMETRY source=CACHED_SCREEN size=%ux%u provisional=0",
                          width, height);
        }
    } else if (!p1404_cockpit_native_get_geometry(&width, &height)) {
        altscreen_log("ERROR ALTINFO refreshed display geometry was not published type111_not_advertised=1");
        return NULL;
    } else {
        altscreen_log("PHASE=ALT111_NEGOTIATION_GEOMETRY source=LIVE_SCREEN size=%ux%u provisional=0",
                      width, height);
    }
    d = altscreen_cluster_display();
    if (!d || !d->width_pixels || !d->height_pixels) {
        altscreen_log("ERROR ALTINFO target display geometry unpublished type111_not_advertised=1");
        return NULL;
    }
    __sync_lock_test_and_set(&g_alt_advertised_width, d->width_pixels);
    __sync_lock_test_and_set(&g_alt_advertised_height, d->height_pixels);
    __sync_lock_test_and_set(&g_alt_advertised_geometry_provisional,
                             geometry_provisional ? 1 : 0);
    __sync_synchronize();
    altscreen_log("PHASE=ALT111_INFO_GEOMETRY_CONTRACT advertised=%ux%u provisional=%d live_geometry=%d renderer_must_match=1",
                  d->width_pixels, d->height_pixels, geometry_provisional,
                  geometry_provisional ? 0 : 1);
    /* Match LIVI getInfo.displayEntry when physical panel size is unavailable. */
    physical_width = d->width_mm ? d->width_mm : 200u;
    physical_height = d->height_mm;
    if (!physical_height) {
        uint32_t scaled = physical_width * d->height_pixels;
        physical_height = alt_div_u32(scaled + (d->width_pixels >> 1),
                                      d->width_pixels);
        if (!physical_height) physical_height = 1u;
    }
    if (!cf_dict_create_mutable || !cf_str_new || !cf_num_new || !cf_dict_set || !cf_release)
        return NULL;
    dict = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    if (!dict) {
        altscreen_log("ERROR ALTINFO dict create failed");
        return NULL;
    }
    if (!set_i64(dict, "type", (int64_t)d->stream_type) ||
        !set_i64(dict, "maxFPS", (int64_t)d->max_fps) ||
        !set_i64(dict, "widthPixels", (int64_t)d->width_pixels) ||
        !set_i64(dict, "heightPixels", (int64_t)d->height_pixels) ||
        !set_i64(dict, "widthPhysical", (int64_t)physical_width) ||
        !set_i64(dict, "heightPhysical", (int64_t)physical_height) ||
        !set_i64(dict, "features", DISPLAY_FEATURE_KNOBS |
                                           DISPLAY_FEATURE_HIGH_FIDELITY_TOUCH) ||
        !set_i64(dict, "primaryInputDevice", PRIMARY_INPUT_KNOBS)) goto fail;

    vs = cf_str_new(d->uuid, -1);
    if (!vs || !cf_dict_set_cstr_obj(dict, "uuid", vs)) goto fail;
    cf_release_safe(vs); vs = NULL;

    alt_resolve_cluster_safe_area(
        d->width_pixels, d->height_pixels, &cluster_safe);

    two_area_capable =
        alt_is_measured_b9_canvas(d->width_pixels, d->height_pixels);
    layout_known =
        two_area_capable &&
        (strstr(cluster_safe.layout, "LayoutMIB2HighB9") != NULL ||
         strstr(cluster_safe.layout, "LayoutMIB2HighQ7") != NULL);
    initial_view_area =
        layout_known && !strcmp(cluster_safe.view, "SMALL") ? 1 : 0;

    /*
     * Declare BOTH Audi FULL/SMALL candidates up front on the measured B9
     * target canvas even if Java's first HMI-state file is a few hundred ms
     * late. updateViewArea can only select an index that /info already
     * declared; waiting for the HMI file here would re-introduce a reconnect
     * requirement on cold start.
     */
    areas = make_cluster_layout_view_areas(
        d->width_pixels, d->height_pixels, two_area_capable);
    if (!areas) {
        altscreen_log("ERROR ALTAREA failed to build cluster viewAreas");
        goto fail;
    }
    if (two_area_capable) {
        adjacent = make_i64_array_one(initial_view_area ? 0 : 1);
        if (!adjacent) goto fail;
    } else {
        adjacent = cf_array_create_mutable ?
            cf_array_create_mutable(NULL, 0, NULL, NULL) : NULL;
        if (!adjacent) goto fail;
    }

    if (!cf_dict_set_cstr_obj(dict, "viewAreas", areas) ||
        !set_i64(dict, "initialViewArea", initial_view_area) ||
        !cf_dict_set_cstr_obj(dict, "adjacentViewAreas", adjacent)) {
        goto fail;
    }
    cf_release_safe(adjacent); adjacent = NULL;
    cf_release_safe(areas); areas = NULL;

    altscreen_log(
        "ALTAREA_LAYOUT_SAFE_V3 schema=viewAreas[array] viewAreaCount=%d "
        "initialViewArea=%d adjacent=%d predeclared_even_if_hmi_late=%d "
        "view=full:%ux%u safe_source=%u,%u,%ux%u "
        "safe_physical=%d,%d,%ux%u renderer_offset=%d,%d "
        "safe_yh_mapping=vertical_inset_top72_bottom450 "
        "safearea_revision=V35_OEM_X_VERTICAL_72_450 "
        "renderer_geometry_revision=V31_ONE_TO_ONE_CLIP "
        "map_plane_terminal_y=26 map_plane_terminal_y_policy=metadata_only_not_renderer_offset "
        "mode=%s layout=%s source=%s renderer_scale=0 "
        "runtime_switch=updateViewArea type111_transition_flags=omitted",
        two_area_capable ? 2 : 1,
        initial_view_area,
        two_area_capable ? (initial_view_area ? 0 : 1) : -1,
        two_area_capable ? 1 : 0,
        d->width_pixels, d->height_pixels,
        cluster_safe.x, cluster_safe.y,
        cluster_safe.w, cluster_safe.h,
        cluster_safe.physical_x, cluster_safe.physical_y,
        cluster_safe.physical_w, cluster_safe.physical_h,
        cluster_safe.renderer_dx, cluster_safe.renderer_dy,
        cluster_safe.view, cluster_safe.layout,
        cluster_safe.source);
    altscreen_log("ALTINFO built type=%u %ux%u@%u uuid=%s features=0x%x input=%d fields=%u",
                  d->stream_type, d->width_pixels, d->height_pixels, d->max_fps,
                  d->uuid, DISPLAY_FEATURE_KNOBS | DISPLAY_FEATURE_HIGH_FIDELITY_TOUCH,
                  PRIMARY_INPUT_KNOBS, cf_dict_count ? cf_dict_count(dict) : 0u);
    return dict;
fail:
    cf_release_safe(vs);
    cf_release_safe(adjacent);
    cf_release_safe(areas);
    cf_release_safe(dict);
    return NULL;
}

typedef int (*airplay_send_command_fn)(void *, void *,
                                        void (*)(int, void *, void *), void *);
struct alt111_event_context {
    void *receiver;
    void *stream;
    uint32_t generation;
    int event_kind;
    int event_value;
    uint32_t event_seq;
    volatile int refs;
    volatile int callback_called;
};

static void alt111_event_context_release(struct alt111_event_context *ctx) {
    if (ctx && __sync_sub_and_fetch(&ctx->refs, 1) == 0) free(ctx);
}

static void alt111_event_response(int status, void *response, void *opaque) {
    struct alt111_event_context *ctx = (struct alt111_event_context *)opaque;
    if (!ctx) return;
    /* Publish callback ownership before any logging/state work. SendCommand may
     * invoke completion on another thread before returning a synchronous error;
     * the submit path must not release both references while callback code is
     * already using this context. */
    __sync_lock_test_and_set(&ctx->callback_called, 1);
    altscreen_log("PHASE=ALT111_EVENT_RESPONSE receiver=%p stream=%p generation=%u event=%d value=%d seq=%u status=%d response=%p",
                  ctx->receiver, ctx->stream, ctx->generation,
                  ctx->event_kind, ctx->event_value, ctx->event_seq,
                  status, response);
    if (ctx->stream && ctx->generation) {
        if (ctx->event_kind == ALT111_EVENT_UPDATE_VIEW_AREA)
            p1404_cockpit_native_view_area_result(
                ctx->receiver, ctx->stream, ctx->generation,
                ctx->event_seq, ctx->event_value,
                status, response != NULL);
        else if (ctx->event_kind == ALT111_EVENT_ZOOM)
            p1404_cockpit_native_zoom_result(
                ctx->receiver, ctx->stream, ctx->generation,
                ctx->event_seq, ctx->event_value,
                status, response != NULL);
        else if (ctx->event_kind == ALT111_EVENT_RECOVERY_KEYFRAME)
            p1404_cockpit_native_recovery_result(
                ctx->receiver, ctx->stream, ctx->generation,
                ctx->event_seq, status, response != NULL);
        else
            p1404_cockpit_native_event_result(ctx->receiver, ctx->stream,
                                          ctx->generation, ctx->event_kind,
                                          status, response != NULL);
    }
    alt111_event_context_release(ctx);
}

static int alt_send_cluster_event_impl(void *receiver, void *stream,
                           uint32_t generation, int event_kind, int bootstrap,
                           uint32_t event_seq) {
    airplay_send_command_fn send_command;
    struct alt111_event_context *ctx = NULL;
    const struct altscreen_display *display = altscreen_cluster_display();
    const char *type_name;
    cf_obj command = NULL, params = NULL, type = NULL, uuid = NULL, url = NULL;
    int rc = -1;

    if (!receiver || (!bootstrap && (!stream || !generation)) ||
        !display || !display->uuid)
        return -1;
    if (event_kind == ALT111_EVENT_SHOW_UI) type_name = "showUI";
    else if (event_kind == ALT111_EVENT_FORCE_KEYFRAME ||
             event_kind == ALT111_EVENT_RECOVERY_KEYFRAME) type_name = "forceKeyFrame";
    else if (event_kind == ALT111_EVENT_STOP_UI) type_name = "stopUI";
    else return -1;

    /* CF bindings were published by the one-shot initialization worker before
     * READY. Do not rewrite shared function pointers from concurrent per-stream
     * monitor threads. */
    send_command = (airplay_send_command_fn)
        p1404_stock_symbol_named("AirPlayReceiverSessionSendCommand");
    if (!send_command || !cf_dict_create_mutable || !cf_str_new ||
        !cf_dict_set || !cf_release) goto done;
    command = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    params = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    type = cf_str_new(type_name, -1);
    uuid = cf_str_new(display->uuid, -1);
    if (!command || !params || !type || !uuid) goto done;
    if (!cf_dict_set_cstr_obj(command, "type", type) ||
        !cf_dict_set_cstr_obj(params, "uuid", uuid)) goto done;
    if (event_kind == ALT111_EVENT_SHOW_UI) {
        url = cf_str_new(CP_ALT_CLUSTER_MAP_URL, -1);
        if (!url || !cf_dict_set_cstr_obj(params, "url", url)) goto done;
    }
    if (!cf_dict_set_cstr_obj(command, "params", params)) goto done;
    ctx = (struct alt111_event_context *)calloc(1u, sizeof(*ctx));
    if (!ctx) goto done;
    ctx->receiver = receiver;
    ctx->stream = stream;
    ctx->generation = generation;
    ctx->event_kind = event_kind;
    ctx->event_value = -1;
    ctx->event_seq = event_seq;
    ctx->refs = 2; /* submit path plus callback path */
    rc = send_command(receiver, command, alt111_event_response, ctx);
    altscreen_log("PHASE=ALT111_EVENT_SUBMIT receiver=%p stream=%p generation=%u event=%s uuid=%s url=%s rc=%d response_callback=1",
                  receiver, stream, generation, type_name, display->uuid,
                  event_kind == ALT111_EVENT_SHOW_UI ? CP_ALT_CLUSTER_MAP_URL : "-", rc);
    if (rc == 0) {
        alt111_event_context_release(ctx); /* callback owns the remaining ref */
        ctx = NULL;
    } else if (__sync_fetch_and_add(&ctx->callback_called, 0)) {
        alt111_event_context_release(ctx); /* synchronous callback already ran */
        ctx = NULL;
    }
done:
    cf_release_safe(url);
    cf_release_safe(uuid);
    cf_release_safe(type);
    cf_release_safe(params);
    cf_release_safe(command);
    if (ctx) {
        /* A nonzero submit result promises no callback; release both owners. */
        alt111_event_context_release(ctx);
        alt111_event_context_release(ctx);
    }
    return rc;
}

int alt_send_cluster_event(void *receiver, void *stream, uint32_t generation,
                           int event_kind) {
    return alt_send_cluster_event_impl(receiver, stream, generation, event_kind, 0, 0);
}


int alt_send_cluster_recovery(void *receiver, void *stream,
                               uint32_t generation, uint32_t event_seq) {
    if (!event_seq) return -1;
    return alt_send_cluster_event_impl(receiver, stream, generation,
        ALT111_EVENT_RECOVERY_KEYFRAME, 0, event_seq);
}

int alt_send_cluster_view_area(void *receiver, void *stream,
                               uint32_t generation, uint32_t event_seq,
                               int view_area_index) {
    airplay_send_command_fn send_command;
    struct alt111_event_context *ctx = NULL;
    const struct altscreen_display *display = altscreen_cluster_display();
    cf_obj command = NULL, params = NULL, type = NULL, uuid = NULL;
    cf_obj adjacent = NULL, adjacent_num = NULL;
    int rc = -1;
    const int adjacent_index = view_area_index == 0 ? 1 : 0;

    if (!receiver || !stream || !generation || !event_seq ||
        !display || !display->uuid ||
        (view_area_index != 0 && view_area_index != 1))
        return -1;

    send_command = (airplay_send_command_fn)
        p1404_stock_symbol_named("AirPlayReceiverSessionSendCommand");
    if (!send_command || !cf_dict_create_mutable || !cf_array_create_mutable ||
        !cf_array_append || !cf_str_new || !cf_num_new ||
        !cf_dict_set || !cf_release)
        goto done;

    command = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    params = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    type = cf_str_new("updateViewArea", -1);
    uuid = cf_str_new(display->uuid, -1);
    adjacent = cf_array_create_mutable(NULL, 0, NULL, NULL);
    adjacent_num = cf_num_new(NULL, (int64_t)adjacent_index);
    if (!command || !params || !type || !uuid || !adjacent || !adjacent_num)
        goto done;

    cf_array_append(adjacent, adjacent_num);
    if (!cf_dict_set_cstr_obj(command, "type", type) ||
        !cf_dict_set_cstr_obj(params, "uuid", uuid) ||
        !set_i64(params, "viewAreaIndex", (int64_t)view_area_index) ||
        !set_i64(params, "animationDurationMillis", 0) ||
        !cf_dict_set_cstr_obj(params, "adjacentViewAreas", adjacent) ||
        !cf_dict_set_cstr_obj(command, "params", params))
        goto done;

    ctx = (struct alt111_event_context *)calloc(1u, sizeof(*ctx));
    if (!ctx) goto done;
    ctx->receiver = receiver;
    ctx->stream = stream;
    ctx->generation = generation;
    ctx->event_kind = ALT111_EVENT_UPDATE_VIEW_AREA;
    ctx->event_value = view_area_index;
    ctx->event_seq = event_seq;
    ctx->refs = 2;

    rc = send_command(receiver, command, alt111_event_response, ctx);
    altscreen_log(
        "PHASE=ALT111_VIEWAREA_SUBMIT receiver=%p stream=%p generation=%u "
        "request_seq=%u uuid=%s viewAreaIndex=%d animationDurationMillis=0 "
        "adjacentViewAreas=[%d] rc=%d same_session=1",
        receiver, stream, generation, event_seq, display->uuid,
        view_area_index, adjacent_index, rc);

    if (rc == 0) {
        alt111_event_context_release(ctx);
        ctx = NULL;
    } else if (__sync_fetch_and_add(&ctx->callback_called, 0)) {
        alt111_event_context_release(ctx);
        ctx = NULL;
    }

done:
    cf_release_safe(adjacent_num);
    cf_release_safe(adjacent);
    cf_release_safe(uuid);
    cf_release_safe(type);
    cf_release_safe(params);
    cf_release_safe(command);
    if (ctx) {
        alt111_event_context_release(ctx);
        alt111_event_context_release(ctx);
    }
    return rc;
}

int alt_send_cluster_zoom(void *receiver, void *stream,
                          uint32_t generation, uint32_t event_seq,
                          int direction) {
    airplay_send_command_fn send_command;
    struct alt111_event_context *ctx = NULL;
    const struct altscreen_display *display = altscreen_cluster_display();
    cf_obj command = NULL, params = NULL, type = NULL, uuid = NULL;
    int rc = -1;

    if (!receiver || !stream || !generation || !event_seq ||
        !display || !display->uuid || (direction != 0 && direction != 1))
        return -1;

    send_command = (airplay_send_command_fn)
        p1404_stock_symbol_named("AirPlayReceiverSessionSendCommand");
    if (!send_command || !cf_dict_create_mutable || !cf_str_new ||
        !cf_num_new || !cf_dict_set || !cf_release)
        goto done;

    command = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    params = cf_dict_create_mutable(NULL, 0, NULL, NULL, NULL);
    type = cf_str_new("changeMapZoomLevel", -1);
    uuid = cf_str_new(display->uuid, -1);
    if (!command || !params || !type || !uuid) goto done;

    if (!cf_dict_set_cstr_obj(command, "type", type) ||
        !cf_dict_set_cstr_obj(params, "uuid", uuid) ||
        !set_i64(params, "zoomDirection", (int64_t)direction) ||
        !cf_dict_set_cstr_obj(command, "params", params))
        goto done;

    ctx = (struct alt111_event_context *)calloc(1u, sizeof(*ctx));
    if (!ctx) goto done;
    ctx->receiver = receiver;
    ctx->stream = stream;
    ctx->generation = generation;
    ctx->event_kind = ALT111_EVENT_ZOOM;
    ctx->event_value = direction;
    ctx->event_seq = event_seq;
    ctx->refs = 2;

    rc = send_command(receiver, command, alt111_event_response, ctx);
    altscreen_log(
        "PHASE=CLUSTER_ZOOM_SUBMIT receiver=%p stream=%p generation=%u "
        "seq=%u type=changeMapZoomLevel uuid=%s direction=%d action=%s rc=%d "
        "response_callback=1",
        receiver, stream, generation, event_seq, display->uuid, direction,
        direction == 0 ? "ZOOM_IN" : "ZOOM_OUT", rc);

    if (rc == 0) {
        alt111_event_context_release(ctx);
        ctx = NULL;
    } else if (__sync_fetch_and_add(&ctx->callback_called, 0)) {
        alt111_event_context_release(ctx);
        ctx = NULL;
    }

done:
    cf_release_safe(uuid);
    cf_release_safe(type);
    cf_release_safe(params);
    cf_release_safe(command);
    if (ctx) {
        alt111_event_context_release(ctx);
        alt111_event_context_release(ctx);
    }
    return rc;
}


/* LIVI requests the Alt UUID when Main110 becomes ready, before the phone has
 * necessarily requested type111. This request has no private stream yet and
 * must not publish renderer/visibility success from its callback. */
int alt_request_cluster_after_main(void *receiver) {
    int show_rc = alt_send_cluster_event_impl(receiver, NULL, 0,
                                             ALT111_EVENT_SHOW_UI, 1, 0);
    int key_rc = -1;
    if (show_rc == 0)
        key_rc = alt_send_cluster_event_impl(receiver, NULL, 0,
                                             ALT111_EVENT_FORCE_KEYFRAME, 1, 0);
    altscreen_log("PHASE=ALT111_REQUEST_AFTER_MAIN receiver=%p show_rc=%d keyframe_rc=%d private_stream_required=0",
                  receiver, show_rc, key_rc);
    return show_rc ? show_rc : key_rc;
}

static int ensure_display_view_area(cf_obj display, unsigned index) {
    cf_obj areas = NULL, check;
    int64_t width = 0, height = 0, initial = -1;
    if (!display || !alt_cf_is_dict(display)) return 0;
    check = cf_dict_get_cstr(display, "widthPixels");
    if (!check || !alt_cf_int64(check, &width)) return 0;
    check = cf_dict_get_cstr(display, "heightPixels");
    if (!check || !alt_cf_int64(check, &height)) return 0;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return 0;
    areas = make_view_areas((uint32_t)width, (uint32_t)height);
    if (!areas || !cf_dict_set_cstr_obj(display, "viewAreas", areas) ||
        !set_i64(display, "initialViewArea", 0)) {
        cf_release_safe(areas);
        return 0;
    }
    cf_release_safe(areas);
    check = cf_dict_get_cstr(display, "viewAreas");
    if (!check || !alt_cf_is_array(check) || !cf_array_count ||
        cf_array_count(check) == 0u) return 0;
    check = cf_dict_get_cstr(display, "initialViewArea");
    if (!check || !alt_cf_int64(check, &initial) || initial != 0) return 0;
    altscreen_log("ALTAREA existing_display=%u full=%lldx%lld safeArea=nested initialViewArea=0",
                  index, (long long)width, (long long)height);
    return 1;
}

void *alt_info_add_cluster_display(void *container) {
    cf_obj alt;
    unsigned before = 0, after;
    if (!container) return NULL;
    if (!alt_cf_is_array(container)) {
        altscreen_log("ALTINFO container not a CFArray typeid=%u left_untouched=1",
                      cf_get_typeid ? (unsigned)cf_get_typeid(container) : 0u);
        return container;
    }
    if (cf_array_count) {
        int i;
        before = cf_array_count(container);
        /* enabledFeatures=viewAreas applies to every advertised display, not
         * only type111. K1004's stock Main110 dictionary predates this feature,
         * so complete its full-screen geometry before appending AltScreen. */
        for (i = 0; i < (int)before; ++i) {
            cf_obj item = (cf_obj)cf_array_at(container, (unsigned)i);
            if (!ensure_display_view_area(item, (unsigned)i)) {
                altscreen_log("ERROR ALTAREA existing display index=%d invalid; type111_not_advertised=1 viewAreas_not_negotiable=1",
                              i);
                return container;
            }
        }
        for (i = 0; i < (int)before; ++i) {
            int64_t t = -1;
            cf_obj item = (cf_obj)cf_array_at(container, (unsigned)i);
            cf_obj tv;
            if (!item || !cf_dict_get) continue;
            tv = cf_dict_get_cstr(item, "type");
            if (tv && alt_cf_int64(tv, &t) && t == CP_STREAM_ALT_SCREEN) {
                altscreen_log("ALTINFO already has type=111 at=%d not_duplicated=1", i);
                altscreen_mark_info_alt_display();
                return container;
            }
        }
    }
    alt = alt_build_cluster_display();
    if (!alt || !cf_array_append) return container;
    cf_array_append(container, alt);
    after = cf_array_count ? cf_array_count(container) : before + 1;
    if (after > before) {
        cf_release_safe(alt);
        altscreen_log("ALTINFO displays %u -> %u creator_ref_released_after_retaining_append=1", before, after);
        altscreen_mark_info_alt_display();
    } else {
        cf_release_safe(alt);
        altscreen_log("ERROR ALTINFO append type=111 did not increase display count");
    }
    return container;
}

void *alt_advertise_features(void *value) {
    int64_t v = 0;
    cf_obj nv;
    if (!value) return NULL;
    if (!alt_cf_int64(value, &v)) {
        altscreen_log("FEATURES value is not a CFNumber left_untouched=1");
        return value;
    }
    if (v & (int64_t)CP_FEATURE_ALTSCREEN) {
        altscreen_log("FEATURES altScreen already set in 0x%llx", (unsigned long long)v);
        altscreen_mark_info_alt_feature_advertised();
        return value;
    }
    if (!cf_num_new) return value;
    nv = cf_num_new(NULL, v | (int64_t)CP_FEATURE_ALTSCREEN);
    if (!nv) return value;
    altscreen_log("FEATURES 0x%llx -> 0x%llx altScreen_bit26=1",
                  (unsigned long long)v,
                  (unsigned long long)(v | (int64_t)CP_FEATURE_ALTSCREEN));
    altscreen_mark_info_alt_feature_advertised();
    return nv;
}

void alt_note_stream_instance(void *stream, int created) {
    static void *seen[16];
    static unsigned long calls[16];
    static int live;
    int i, slot = -1;
    if (!stream) return;
    for (i = 0; i < 16; ++i) if (seen[i] == stream) { slot = i; break; }
    if (slot < 0) {
        for (i = 0; i < 16; ++i) if (!seen[i]) { slot = i; break; }
        if (slot < 0) { altscreen_log("STREAM table full"); return; }
        seen[slot] = stream;
        if (created) {
            ++live;
            altscreen_log("STREAM create slot=%d stream=%p live=%d/%d", slot, stream,
                          live, SCREEN_STREAM_MAX_INSTANCES);
        }
    }
    if (!created && calls[slot]++ == 0)
        altscreen_log("STREAM first-data slot=%d stream=%p", slot, stream);
}

static int name_is(cf_obj o, const char *want, char *scratch, size_t cap) {
    const char *s;
    if (!o || !want) return 0;
    s = alt_cf_cString(o, scratch, cap);
    return s && *s && !strcmp(s, want);
}

static const char *const kProbeKeys[] = {
    "streams", "stream", "features", "enabledFeatures", "streamType", "audioType", "type",
    "maxFPS", "widthPixels", "heightPixels", "widthPhysical", "heightPhysical",
    "uuid", "displays", "dataPort", "eventPort", "timingPort", "streamConnectionID",
    "streamToHost", "streamToReceiver", "controlType", "primaryInputDevice",
    "videoConfig", "videoLatencyMs", "videoMinLatencyMs", "videoMaxBitrate",
    "supportsDynamicBitrate", "altScreen", "viewAreas", "initialViewArea", "safeArea",
    "codec", "profile", "level", "bitrate", "latencyMs", "width", "height", "fps",
    NULL
};

static void probe_container(const char *tag, cf_obj obj, int depth);
static void probe_one(const char *tag, cf_obj dict, const char *key, int depth);

static void probe_data_value(const char *tag, const char *key, cf_obj v, int depth) {
    long n;
    if (!cf_data_len) {
        altscreen_log("OBSALL %s d=%d key=%s type=data api=missing payload=metadata-only",
                      tag, depth, key);
        return;
    }
    n = cf_data_len(v);
    altscreen_log("OBSALL %s d=%d key=%s type=data len=%ld payload=metadata-only",
                  tag, depth, key, n);
}

static int probe_ascii_contains_ci(const char *text, const char *needle) {
    const char *p;
    if (!text || !needle || !*needle) return 0;
    for (; *text; ++text) {
        p = text;
        {
            const char *n = needle;
            while (*p && *n) {
                unsigned char a = (unsigned char)*p++;
                unsigned char b = (unsigned char)*n++;
                if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
                if (a != b) break;
            }
            if (!*n) return 1;
        }
    }
    return 0;
}

static int probe_key_is_sensitive(const char *key) {
    return probe_ascii_contains_ci(key, "key") ||
           probe_ascii_contains_ci(key, "iv") ||
           probe_ascii_contains_ci(key, "secret") ||
           probe_ascii_contains_ci(key, "salt") ||
           probe_ascii_contains_ci(key, "certificate") ||
           probe_ascii_contains_ci(key, "challenge") ||
           probe_ascii_contains_ci(key, "auth") ||
           probe_ascii_contains_ci(key, "token") ||
           probe_ascii_contains_ci(key, "nonce") ||
           probe_ascii_contains_ci(key, "signature") ||
           probe_ascii_contains_ci(key, "password") ||
           probe_ascii_contains_ci(key, "pin");
}

static int probe_key_is_allowlisted(const char *key) {
    int i;
    if (!key) return 0;
    if (!strcmp(key, "[]") || !strcmp(key, "$")) return 1;
    for (i = 0; kProbeKeys[i]; ++i)
        if (!strcmp(key, kProbeKeys[i])) return 1;
    return 0;
}

static void probe_value(const char *tag, const char *key, cf_obj v, int depth) {
    int64_t num = 0;
    char scratch[160];
    const char *s;
    if (!v || depth > PROBE_DEPTH_MAX) return;
    /* Generic dictionary enumeration must never expose negotiated key/IV bytes,
     * strings, integers or nested values. The field name is enough to diagnose
     * shape while preserving the no-secret logging contract. */
    if (probe_key_is_sensitive(key)) {
        altscreen_log("OBSALL %s d=%d key=%s type=redacted value=REDACTED",
                      tag, depth, key);
        return;
    }
    if (!probe_key_is_allowlisted(key)) {
        if (alt_cf_is_data(v)) probe_data_value(tag, key, v, depth);
        else if (alt_cf_is_array(v))
            altscreen_log("OBSALL %s d=%d key=%s type=array count=%u payload=metadata-only",
                          tag, depth, key, cf_array_count ? cf_array_count(v) : 0u);
        else if (alt_cf_is_dict(v))
            altscreen_log("OBSALL %s d=%d key=%s type=dict count=%u payload=metadata-only",
                          tag, depth, key, cf_dict_count ? cf_dict_count(v) : 0u);
        else
            altscreen_log("OBSALL %s d=%d key=%s type=scalar payload=metadata-only",
                          tag, depth, key);
        return;
    }
    if (alt_cf_int64(v, &num)) {
        altscreen_log("OBSALL %s d=%d key=%s type=int value=%lld", tag, depth, key, (long long)num);
        if ((!strcmp(key, "streamType") || !strcmp(key, "type")) &&
            (num == CP_STREAM_MAIN_SCREEN || num == CP_STREAM_ALT_SCREEN)) {
            altscreen_log("OBS STREAMTYPE tag=%s seen=%lld main=%d alt=%d", tag,
                          (long long)num, num == CP_STREAM_MAIN_SCREEN,
                          num == CP_STREAM_ALT_SCREEN);
            if (num == CP_STREAM_ALT_SCREEN && !strcmp(tag, "setup-request")) {
                altscreen_log("PHASE=PHONE_REQUEST_111 STREAM_111_REQUESTED=YES");
                altscreen_mark_phone_requested();
            }
        }
        if ((!strcmp(key, "features") || !strcmp(key, "enabledFeatures")) &&
            (num & (int64_t)CP_FEATURE_ALTSCREEN) && !strcmp(tag, "setup-request")) {
            altscreen_log("PHONE_REQUESTED altScreen bit in SETUP request features=0x%llx",
                          (unsigned long long)num);
            altscreen_mark_phone_requested();
        }
        return;
    }
    if (alt_cf_is_array(v)) {
        altscreen_log("OBSALL %s d=%d key=%s type=array count=%u", tag, depth, key,
                      cf_array_count ? cf_array_count(v) : 0u);
        probe_container(tag, v, depth + 1);
        return;
    }
    if (alt_cf_is_dict(v)) {
        altscreen_log("OBSALL %s d=%d key=%s type=dict count=%u", tag, depth, key,
                      cf_dict_count ? cf_dict_count(v) : 0u);
        probe_container(tag, v, depth + 1);
        return;
    }
    if (alt_cf_is_data(v)) {
        probe_data_value(tag, key, v, depth);
        return;
    }
    if (alt_cf_is_string(v)) {
        s = alt_cf_cString(v, scratch, sizeof(scratch));
        altscreen_log("OBSALL %s d=%d key=%s type=string value=%s", tag, depth, key,
                      s && *s ? s : "");
        return;
    }
    altscreen_log("OBSALL %s d=%d key=%s type=unknown object=%p typeid=%u", tag, depth,
                  key, v, cf_get_typeid ? (unsigned)cf_get_typeid(v) : 0u);
}

static void probe_container(const char *tag, cf_obj obj, int depth) {
    unsigned n, i;
    if (!obj || depth > PROBE_DEPTH_MAX) return;
    if (alt_cf_is_array(obj)) {
        n = cf_array_count ? cf_array_count(obj) : 0u;
        altscreen_log("OBS %s array depth=%d count=%u", tag, depth, n);
        if (n > PROBE_ARRAY_MAX) {
            altscreen_log("OBS %s array depth=%d truncated=%u/%u", tag, depth,
                          PROBE_ARRAY_MAX, n);
            n = PROBE_ARRAY_MAX;
        }
        for (i = 0; i < n; ++i)
            if (cf_array_at) probe_value(tag, "[]", (cf_obj)cf_array_at(obj, i), depth + 1);
        return;
    }
    if (!alt_cf_is_dict(obj)) {
        probe_value(tag, "$", obj, depth);
        return;
    }
    n = cf_dict_count ? cf_dict_count(obj) : 0u;
    altscreen_log("OBS %s dict depth=%d count=%u", tag, depth, n);
    if (cf_dict_get_keys_values && cf_string_typeid && n <= PROBE_DICT_MAX) {
        const void *keys[PROBE_DICT_MAX];
        const void *vals[PROBE_DICT_MAX];
        memset(keys, 0, sizeof(keys));
        memset(vals, 0, sizeof(vals));
        cf_dict_get_keys_values(obj, keys, vals);
        for (i = 0; i < n; ++i) {
            char keybuf[128];
            const char *key;
            if (!keys[i] || !alt_cf_is_string(keys[i])) {
                altscreen_log("OBSALL %s d=%d key_index=%u non_string_key=%p typeid=%u", tag,
                              depth, i, keys[i],
                              keys[i] && cf_get_typeid ? (unsigned)cf_get_typeid(keys[i]) : 0u);
                continue;
            }
            key = alt_cf_cString((cf_obj)keys[i], keybuf, sizeof(keybuf));
            if (!key || !*key) key = "<empty-key>";
            probe_value(tag, key, (cf_obj)vals[i], depth);
        }
        return;
    }
    if (n > PROBE_DICT_MAX)
        altscreen_log("OBS %s dict depth=%d generic_enumeration_skipped count=%u limit=%u",
                      tag, depth, n, PROBE_DICT_MAX);
    else
        altscreen_log("OBS %s dict depth=%d generic_enumeration_unavailable=1", tag, depth);
    {
        int k;
        for (k = 0; kProbeKeys[k]; ++k) probe_one(tag, obj, kProbeKeys[k], depth);
    }
}

static void probe_one(const char *tag, cf_obj dict, const char *key, int depth) {
    cf_obj v;
    if (!cf_dict_get) return;
    v = cf_dict_get_cstr(dict, key);
    if (v) probe_value(tag, key, v, depth);
}

static int cf_matches(const void *obj, const char *want) {
    char scratch[64];
    const char *s = obj ? alt_cf_cString((cf_obj)obj, scratch, sizeof(scratch)) : NULL;
    return s && *s && !strcmp(s, want);
}

static int array_has_string(cf_obj a, const char *want) {
    unsigned i, n;
    if (!alt_cf_is_array(a) || !cf_array_count || !cf_array_at) return 0;
    n = cf_array_count(a);
    if (n > 64) n = 64;
    for (i = 0; i < n; ++i)
        if (cf_matches(cf_array_at(a, i), want)) return 1;
    return 0;
}

static cf_obj find_stream_descriptor_inner(cf_obj obj, uint32_t target, int depth) {
    cf_obj v;
    int64_t num = 0;
    unsigned i, n;
    if (!obj || depth > PROBE_DEPTH_MAX) return NULL;
    if (alt_cf_is_array(obj)) {
        n = cf_array_count ? cf_array_count(obj) : 0u;
        if (n > 64) n = 64;
        for (i = 0; i < n; ++i) {
            cf_obj hit = cf_array_at ? find_stream_descriptor_inner((cf_obj)cf_array_at(obj, i), target, depth + 1) : NULL;
            if (hit) return hit;
        }
        return NULL;
    }
    if (!alt_cf_is_dict(obj) || !cf_dict_get) return NULL;
    v = cf_dict_get_cstr(obj, "streamType");
    if (v && alt_cf_int64(v, &num) && num == (int64_t)target) return obj;
    v = cf_dict_get_cstr(obj, "type");
    if (v && alt_cf_int64(v, &num) && num == (int64_t)target) return obj;

    n = cf_dict_count ? cf_dict_count(obj) : 0u;
    if (cf_dict_get_keys_values && n > 0 && n <= PROBE_DICT_MAX) {
        const void *keys[PROBE_DICT_MAX];
        const void *vals[PROBE_DICT_MAX];
        memset(keys, 0, sizeof(keys));
        memset(vals, 0, sizeof(vals));
        cf_dict_get_keys_values(obj, keys, vals);
        for (i = 0; i < n; ++i) {
            cf_obj hit = find_stream_descriptor_inner((cf_obj)vals[i], target, depth + 1);
            if (hit) return hit;
        }
        return NULL;
    }
    {
        static const char *const recurse_keys[] = { "streams", "stream", "videoConfig", NULL };
        int k;
        for (k = 0; recurse_keys[k]; ++k) {
            v = cf_dict_get_cstr(obj, recurse_keys[k]);
            if (v) {
                cf_obj hit = find_stream_descriptor_inner(v, target, depth + 1);
                if (hit) return hit;
            }
        }
    }
    return NULL;
}

void *alt_find_stream_descriptor(void *obj, uint32_t target_stream_type) {
    return find_stream_descriptor_inner((cf_obj)obj, target_stream_type, 0);
}

static int object_requests_alt(cf_obj obj, int depth) {
    cf_obj v;
    int64_t num = 0;
    if (!obj || depth > PROBE_DEPTH_MAX) return 0;
    if (find_stream_descriptor_inner(obj, CP_STREAM_ALT_SCREEN, depth)) return 1;
    if (!alt_cf_is_dict(obj) || !cf_dict_get) return 0;
    v = cf_dict_get_cstr(obj, "features");
    if (v) {
        if (alt_cf_int64(v, &num) && (num & (int64_t)CP_FEATURE_ALTSCREEN)) return 1;
        if (array_has_string(v, "altScreen")) return 1;
    }
    v = cf_dict_get_cstr(obj, "enabledFeatures");
    if (v) {
        if (alt_cf_int64(v, &num) && (num & (int64_t)CP_FEATURE_ALTSCREEN)) return 1;
        if (array_has_string(v, "altScreen")) return 1;
    }
    return 0;
}

static int response_offers_alt(cf_obj resp) {
    cf_obj ef;
    int64_t num = 0;
    if (!resp || !cf_dict_get || !alt_cf_is_dict(resp)) return 0;
    /* Stream SETUP responses are not initial-session responses: stock K1004
     * returns streams[] without repeating enabledFeatures.  A successfully
     * merged explicit type-111 descriptor is therefore sufficient proof for
     * this response; requiring enabledFeatures here made every independent
     * AltScreen SETUP look unaccepted after the merge. */
    if (find_stream_descriptor_inner(resp, CP_STREAM_ALT_SCREEN, 0)) {
        altscreen_log("NEGOTIATED probe streams contains explicit type111=1");
        return 1;
    }
    ef = cf_dict_get_cstr(resp, "enabledFeatures");
    if (!ef) return 0;
    if (alt_cf_int64(ef, &num)) {
        altscreen_log("NEGOTIATED probe enabledFeatures int=0x%llx bit26=%d",
                      (unsigned long long)num,
                      (num & (int64_t)CP_FEATURE_ALTSCREEN) != 0);
        return (num & (int64_t)CP_FEATURE_ALTSCREEN) != 0;
    }
    if (alt_cf_is_array(ef)) {
        unsigned n = cf_array_count ? cf_array_count(ef) : 0;
        altscreen_log("NEGOTIATED probe enabledFeatures array count=%u altScreen=%d viewAreas=%d",
                      n, array_has_string(ef, "altScreen"), array_has_string(ef, "viewAreas"));
        return array_has_string(ef, "altScreen");
    }
    return 0;
}

static int append_feature_unique(cf_obj a, const char *name) {
    cf_obj s;
    int ok;
    if (!a || !name || !cf_array_append || !cf_str_new) return 0;
    if (array_has_string(a, name)) return 1;
    s = cf_str_new(name, -1);
    if (!s) return 0;
    cf_array_append(a, s);
    /* This helper can receive a stock-owned enabledFeatures array. Hold the +1
     * until exact P1404 stock-array callbacks are proven; our own arrays will
     * safely release all other temporary children through their callback tables. */
    ok = array_has_string(a, name);
    if (!ok) cf_release_safe(s);
    return ok;
}

static int accept_alt_in_setup_response(cf_obj resp) {
    cf_obj ef;
    int64_t num = 0;
    if (!resp || !alt_cf_is_dict(resp) || !cf_dict_get || !cf_dict_set) return 0;
    ef = cf_dict_get_cstr(resp, "enabledFeatures");
    if (ef && alt_cf_is_array(ef)) {
        cf_obj fresh;
        unsigned i, before = cf_array_count ? cf_array_count(ef) : 0;
        int ok_alt, ok_view, set_ok = 0;
        if (!cf_array_count || !cf_array_at || !cf_array_create_mutable ||
            !cf_array_append || before > 64u) return 0;
        fresh = cf_array_create_mutable(NULL, 0, NULL, NULL);
        if (!fresh) return 0;
        for (i = 0; i < before; ++i) {
            const void *item = cf_array_at(ef, i);
            if (!item) { cf_release_safe(fresh); return 0; }
            cf_array_append(fresh, item);
        }
        ok_alt = append_feature_unique(fresh, "altScreen");
        ok_view = append_feature_unique(fresh, "viewAreas");
        if (ok_alt && ok_view)
            set_ok = cf_dict_set_cstr_obj(resp, "enabledFeatures", fresh) &&
                     cf_dict_get_cstr(resp, "enabledFeatures") == fresh;
        altscreen_log("PHASE=SETUP_RESPONSE_PATCH enabledFeatures=array-copy before=%u after=%u alt=%d viewAreas=%d set=%d stock_array_untouched=1",
                      before, cf_array_count(fresh), ok_alt, ok_view, set_ok);
        cf_release_safe(fresh);
        return ok_alt && ok_view && set_ok;
    }
    if (ef && alt_cf_int64(ef, &num)) {
        cf_obj nv = cf_num_new ? cf_num_new(NULL, num | (int64_t)CP_FEATURE_ALTSCREEN) : NULL;
        int ok;
        if (!nv) return 0;
        ok = cf_dict_set_cstr_obj(resp, "enabledFeatures", nv);
        cf_release_safe(nv); /* stock SETUP response callback ownership already proved */
        if (!ok) return 0;
        altscreen_log("PHASE=SETUP_RESPONSE_PATCH enabledFeatures=int 0x%llx->0x%llx",
                      (unsigned long long)num,
                      (unsigned long long)(num | (int64_t)CP_FEATURE_ALTSCREEN));
        return 1;
    }
    if (!ef && cf_array_create_mutable) {
        cf_obj a = cf_array_create_mutable(NULL, 0, NULL, NULL);
        int ok_alt, ok_view, set_ok = 0;
        if (!a) {
            altscreen_log("ERROR SETUP enabledFeatures array create failed");
            return 0;
        }
        ok_alt = append_feature_unique(a, "altScreen");
        ok_view = append_feature_unique(a, "viewAreas");
        if (ok_alt) set_ok = cf_dict_set_cstr_obj(resp, "enabledFeatures", a);
        altscreen_log("PHASE=SETUP_RESPONSE_PATCH enabledFeatures=created alt=%d viewAreas=%d count=%u set=%d",
                      ok_alt, ok_view, cf_array_count ? cf_array_count(a) : 0u, set_ok);
        cf_release_safe(a);
        return ok_alt && ok_view && set_ok;
    }
    altscreen_log("ERROR SETUP enabledFeatures unsupported object=%p", ef);
    return 0;
}

void *AirPlayReceiverServerPlatformCopyProperty(void *a0, unsigned a1, void *a2,
                                                void *a3, int *a4) {
    void *r;
    void *lr = __builtin_return_address(0);
    char scratch[48];
    altscreen_runtime_ensure_initialized();
    if (!real_server_copy) p1404_airplay_bind_lazy();
    if (!real_server_copy) return NULL;
    r = real_server_copy(a0, a1, a2, a3, a4);
    if (!p1404_identity_ok) return r;
    obs_call("ServerPlatformCopyProperty", a0, lr, (long)r);
    if (name_is((cf_obj)a2, "features", scratch, sizeof(scratch))) {
        int64_t stock = 0;
        if (alt_cf_int64(r, &stock))
            altscreen_log("PHASE=AIRPLAY_INFO_STOCK INFO_STOCK features=0x%llx self=%p",
                          (unsigned long long)stock, a0);
        else
            altscreen_log("INFO_STOCK features not an int64 object=%p", r);
        if (!alt_runtime_negotiation_ready("server-features") ||
            !p1404_armed || !p1404_mutate_armed || !alt_flag_feature) return r;
        {
            void *patched = alt_advertise_features(r);
            if (patched && patched != r) {
                int64_t now = 0;
                if (alt_cf_int64(patched, &now))
                    altscreen_log("PHASE=AIRPLAY_INFO_FEATURE INFO_FINAL features=0x%llx added_altScreen_bit=%d self=%p",
                                  (unsigned long long)now,
                                  (now & (int64_t)CP_FEATURE_ALTSCREEN) != 0, a0);
                if (a4) *a4 = 0;
                return patched;
            }
        }
    }
    return r;
}

void *AirPlayReceiverSessionPlatformCopyProperty(void *a0, unsigned a1, void *a2,
                                                 void *a3, int *a4) {
    void *r;
    void *lr = __builtin_return_address(0);
    char scratch[48];
    altscreen_runtime_ensure_initialized();
    if (!real_session_copy) p1404_airplay_bind_lazy();
    if (!real_session_copy) return NULL;
    r = real_session_copy(a0, a1, a2, a3, a4);
    if (!p1404_identity_ok) return r;
    obs_call("SessionPlatformCopyProperty", a0, lr, (long)r);
    if (name_is((cf_obj)a2, "displays", scratch, sizeof(scratch)) && r) {
        alt_note_displays_container("info-stock", r);
        if (alt_runtime_negotiation_ready("session-displays") &&
            p1404_armed && p1404_mutate_armed && alt_flag_info) {
            r = alt_info_add_cluster_display(r);
            alt_note_displays_container("info-final", r);
        }
    }
    return r;
}

void *AirPlayReceiverSessionScreen_CopyDisplaysInfo(void *a0, int *a1) {
    void *r;
    void *lr = __builtin_return_address(0);
    altscreen_runtime_ensure_initialized();
    if (!real_copy_displays) p1404_airplay_bind_lazy();
    if (!real_copy_displays) return NULL;
    r = real_copy_displays(a0, a1);
    if (!p1404_identity_ok) return r;
    obs_call("Screen_CopyDisplaysInfo", a0, lr, (long)r);
    alt_note_displays_container("stock", r);
    if (alt_runtime_negotiation_ready("screen-displays") &&
        p1404_armed && p1404_mutate_armed && alt_flag_info && r) {
        r = alt_info_add_cluster_display(r);
        alt_note_displays_container("final", r);
        if (a1) *a1 = 0;
    }
    return r;
}

/* Legacy PlatformControl body. p1404_airplay_fullchain.c macro-renames this
 * symbol and exports the canonical six-argument wrapper that owns SETUP split /
 * private111 merge. Parameter mapping is exact: params=a4, outParams=a5. */
int AirPlayReceiverSessionPlatformControl(void *a0, unsigned a1, void *a2,
                                          const void *a3, const void *a4,
                                          void **a5) {
    int r, is_setup = 0, wants_alt = 0;
    void *lr = __builtin_return_address(0);
    char scratch[64];
    const char *cmd_name;
    cf_obj alt_desc = NULL;
    altscreen_runtime_ensure_initialized();
    if (!real_session_control) p1404_airplay_bind_lazy();
    if (!real_session_control) return -1;
    if (!p1404_identity_ok)
        return real_session_control(a0, a1, a2, a3, a4, a5);
    cmd_name = alt_cf_cString((cf_obj)a2, scratch, sizeof(scratch));
    if (cmd_name && !strcmp(cmd_name, "setUpStreams")) {
        is_setup = 1;
        alt_desc = (cf_obj)alt_find_stream_descriptor((void *)a4, CP_STREAM_ALT_SCREEN);
        wants_alt = alt_desc != NULL || object_requests_alt((cf_obj)a4, 0);
        altscreen_log("PHASE=SETUP_REQUEST_ENTER session=%p flags=%u wants_alt=%d alt_descriptor=%p params=%p legacy=1",
                      a0, a1, wants_alt, alt_desc, a4);
        probe_container("setup-request", (cf_obj)a4, 0);
        if (alt_desc)
            altscreen_log("PHASE=SETUP_111_DESCRIPTOR session=%p descriptor=%p create111_armed=%d",
                          a0, alt_desc, alt_flag_create111);
        if (wants_alt) {
            altscreen_mark_phone_requested();
            altscreen_log("PHASE=SETUP_111_REQUEST create111_armed=%d descriptor=%p",
                          alt_flag_create111, alt_desc);
        }
    }
    alt_note_control_command(a0, a2, (void *)a4);
    r = real_session_control(a0, a1, a2, a3, a4, a5);
    obs_call("SessionPlatformControl", a0, lr, (long)r);
    if (is_setup) {
        altscreen_log("PHASE=SETUP_STOCK_RETURN rc=%d response=%p wants_alt=%d alt_descriptor=%p legacy=1",
                      r, (a5 ? *a5 : NULL), wants_alt, alt_desc);
        if (a5 && *a5) probe_container("setup-response-stock", (cf_obj)*a5, 0);
        if (r == 0 && wants_alt && a5 && *a5 &&
            alt_runtime_negotiation_ready("legacy-setup-response") &&
            p1404_armed && p1404_mutate_armed &&
            alt_flag_feature) {
            if (!accept_alt_in_setup_response((cf_obj)*a5))
                altscreen_log("ERROR SETUP response AltScreen acceptance patch failed");
        }
        if (a5 && *a5) {
            probe_container("setup-response-final", (cf_obj)*a5, 0);
            if (response_offers_alt((cf_obj)*a5))
                altscreen_mark_alt_feature_negotiated();
        }
        if (wants_alt && r != 0)
            altscreen_log("ERROR PHASE=SETUP_111_STOCK_REJECT rc=%d descriptor=%p private_dispatch_required=%d",
                          r, alt_desc, alt_flag_create111);
        if (wants_alt && alt_flag_create111 && !alt_state_lookup(a0))
            altscreen_log("ERROR PHASE=STREAM_111_PRIVATE_PATH_NOT_BOUND session=%p descriptor=%p",
                          a0, alt_desc);
    } else if (a5 && *a5 && response_offers_alt((cf_obj)*a5)) {
        altscreen_mark_alt_feature_negotiated();
    }
    return r;
}

void *alt_bootstrap_server_create(void) {
    void *target = p1404_direct_stock_symbol_named ?
        p1404_direct_stock_symbol_named("AirPlayReceiverServerCreate") : NULL;
    /* Preserve the stock target before launching any asynchronous work. */
    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    return target;
}

typedef int (*screen_stream_set_property_fn)(
    const void *, unsigned, void *, const void *, const void *);
int _ScreenStreamSetProperty(const void *stream, unsigned flags, void *property,
                             const void *qualifier, const void *value);
static screen_stream_set_property_fn real_stream_set_property;

static screen_stream_set_property_fn stock_stream_set_property_target(void) {
    void *target = (void *)real_stream_set_property;
    if (target) return real_stream_set_property;
    target = p1404_stock_symbol_named("_ScreenStreamSetProperty");
    if (target == (void *)&_ScreenStreamSetProperty) target = NULL;
    if (target) real_stream_set_property = (screen_stream_set_property_fn)target;
    return real_stream_set_property;
}

/*
 * Stock CarPlay supplies AVCDecoderConfigurationRecord through the "avcc"
 * ScreenStream property separately from ProcessData.  Mirror fifthBro's
 * codec-config/frame separation, but keep this observer fail-open: config is
 * merely cached by stream pointer and cannot create SHM or mark a stream
 * private.  Only the existing private111 ProcessData identity gate later emits
 * the cached config into /carplay111_h264.
 */
int _ScreenStreamSetProperty(const void *stream, unsigned flags, void *property,
                             const void *qualifier, const void *value) {
    screen_stream_set_property_fn target = stock_stream_set_property_target();
    const uint8_t *bytes_ptr = NULL;
    size_t bytes_len = 0u;
    char keybuf[64];
    const char *key;

    if (!target) return -1;

    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if ((!altscreen_runtime_is_ready || altscreen_runtime_is_ready()) &&
        p1404_identity_ok && p1404_armed && stream && property && value) {
        if (!cf_get_typeid) bind_cf();
        key = alt_cf_cString(property, keybuf, sizeof(keybuf));
        if (key && !strcmp(key, "avcc") &&
            alt_cf_data_bytes(value, &bytes_ptr, &bytes_len)) {
            p111_h264_tap_note_avcc((void *)stream, bytes_ptr, bytes_len);
        }
    }

    return target(stream, flags, property, qualifier, value);
}

void *alt_observe_proc(void *a0, void *a1, void *a2, void *a3, void *caller);
void *alt_observe_create(void *a0, void *a1, void *a2, void *a3, void *caller);

void *alt_observe_proc(void *a0, void *a1, void *a2, void *a3, void *caller) {
    size_t len = (size_t)(unsigned long)a2;
    void *target = stock_stream_target(0);
    (void)a3;
    if (!target) return NULL;
    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if ((!altscreen_runtime_is_ready || altscreen_runtime_is_ready()) &&
        p1404_identity_ok && p1404_armed) {
        obs_stream_see(a0, caller, 0);
        if (a1 && len) {
            const int private_video = alt_state_feed_private_video(a0, a1, len);
            if (private_video) p111_h264_tap_write(a0, a1, len);
            obs_stream_data(a0, a1, len);
        }
    }
    return target;
}

void *alt_observe_create(void *a0, void *a1, void *a2, void *a3, void *caller) {
    void *target = stock_stream_target(1);
    (void)a1; (void)a2; (void)a3;
    if (!target) return NULL;
    if (altscreen_runtime_ensure_initialized)
        altscreen_runtime_ensure_initialized();
    if ((!altscreen_runtime_is_ready || altscreen_runtime_is_ready()) &&
        p1404_identity_ok && p1404_armed) obs_stream_see(a0, caller, 1);
    return target;
}

void alt_note_displays_container(const char *tag, void *container) {
    unsigned n, i;
    if (!container) { altscreen_log("DISPLAYS %s returned NULL", tag); return; }
    n = (alt_cf_is_array(container) && cf_array_count) ? cf_array_count(container) : 0u;
    altscreen_log("DISPLAYS %s container=%p is_array=%d count=%u", tag, container,
                  alt_cf_is_array(container), n);
    for (i = 0; i < n && i < 16; ++i) {
        cf_obj it, tv, uv;
        int64_t v = -1;
        char uuid[96];
        const char *us = "-";
        if (!cf_array_at || !cf_dict_get) break;
        it = (cf_obj)cf_array_at(container, i);
        if (!it) continue;
        tv = cf_dict_get_cstr(it, "type");
        uv = cf_dict_get_cstr(it, "uuid");
        if (uv) us = alt_cf_cString(uv, uuid, sizeof(uuid));
        if (tv && alt_cf_int64(tv, &v))
            altscreen_log("DISPLAYS %s[%u] type=%lld is_dict=%d uuid=%s", tag, i,
                          (long long)v, alt_cf_is_dict(it), us && *us ? us : "-");
    }
}

void alt_note_control_command(void *session, void *cmd, void *arg) {
    char scratch[64];
    const char *name;
    cf_obj alt_desc = NULL;
    struct altscreen_ctx snap;
    int wants_alt;
    if ((altscreen_runtime_is_ready && !altscreen_runtime_is_ready()) ||
        !p1404_armed || !session) return;
    memset(&snap, 0, sizeof(snap));
    name = alt_cf_cString((cf_obj)cmd, scratch, sizeof(scratch));
    if (!name || !*name) return;
    if (!strcmp(name, "setUpStreams")) {
        alt_desc = (cf_obj)alt_find_stream_descriptor(arg, CP_STREAM_ALT_SCREEN);
        wants_alt = alt_desc != NULL || object_requests_alt((cf_obj)arg, 0);
        altscreen_log("CONTROL setUpStreams session=%p arg=%p thread=%lu wants_alt=%d alt_descriptor=%p",
                      session, arg, obs_thread_id(), wants_alt, alt_desc);
        if (alt_desc) {
            if (alt_state_register(session) && alt_state_snapshot(session, 0, &snap))
                altscreen_log("CONTROL alt ctx id=%u generation=%u registered descriptor=%p stock110_untouched=1",
                              snap.id, snap.generation, alt_desc);
        } else if (wants_alt) {
            altscreen_log("CONTROL AltScreen feature requested without explicit 111 descriptor session=%p",
                          session);
        }
    } else if (!strcmp(name, "tearDownStreams") || !strcmp(name, "stopServer")) {
        (void)alt_state_snapshot(session, 1, &snap);
        altscreen_log("PHASE=TEARDOWN CONTROL=%s session=%p alt_ctx_id=%u generation=%u private_session=%p private_stream=%p",
                      name, session, snap.id, snap.generation,
                      snap.alt_screen_session, snap.alt_screen_stream);
        alt_state_unregister(session);
        obs_streams_report(name);
    } else if (!strcmp(name, "requestUI") || !strcmp(name, "changeModes") ||
               !strcmp(name, "modesChanged") || !strcmp(name, "forceKeyFrame") ||
               !strcmp(name, "showUI")) {
        altscreen_log("PHASE=UI_CONTROL CONTROL=%s session=%p arg=%p", name, session, arg);
        probe_container("ui-control", (cf_obj)arg, 0);
    } else {
        altscreen_log("CONTROL %s session=%p arg=%p", name, session, arg);
    }
}
