/* p1404_control_fence.c - explicit serialization/cancellation for private111. */
#include "p1404_control_fence.h"
#include "p1404_lock_wait.h"
#include <unistd.h>

static volatile int g_fence_guard;
static uint32_t g_fence_generation;

static void fence_lock(void) {
    /* Control-plane only: never used on the video hot path. A held lock means a
     * bounded private SETUP commit/teardown is in progress, so waiting preserves
     * a strict total order instead of guessing the stock thread model. */
    while (__sync_lock_test_and_set(&g_fence_guard, 1)) p1404_lock_wait_yield();
}

static uint32_t next_generation_locked(void) {
    ++g_fence_generation;
    if (g_fence_generation == 0u) ++g_fence_generation; /* zero is never a token */
    return g_fence_generation;
}

uint32_t alt_control_fence_begin(void) {
    uint32_t g;
    fence_lock();
    g = next_generation_locked();
    __sync_lock_release(&g_fence_guard);
    return g;
}

int alt_control_fence_is_current(uint32_t generation) {
    return generation != 0u && generation == g_fence_generation;
}

int alt_control_fence_lock_current(uint32_t generation) {
    fence_lock();
    if (!generation || generation != g_fence_generation) {
        __sync_lock_release(&g_fence_guard);
        return 0;
    }
    return 1;
}

uint32_t alt_control_fence_cancel_and_lock(void) {
    fence_lock();
    return next_generation_locked();
}

void alt_control_fence_unlock(void) {
    __sync_lock_release(&g_fence_guard);
}

void alt_control_fence_reset(void) {
    g_fence_generation = 0u;
    g_fence_guard = 0;
}
