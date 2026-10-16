#ifndef STARTUP_FRAME_PROGRESS_H
#define STARTUP_FRAME_PROGRESS_H
#include <stdint.h>

/* Progress belongs to one producer session, never to two different sessions. */
struct StartupFrameProgress {
    uint32_t writer, generation, cookie;
    unsigned frames;
    /**
     * Initialize an empty decoded-frame progress record.
     * Parameters: none.
     * Returns: a record with no producer identity or validated frames.
     */
    StartupFrameProgress() : writer(0), generation(0), cookie(0), frames(0) { }

    /**
     * Clear the producer identity and validated-frame count.
     * Parameters: none.
     * Returns: nothing.
     */
    void reset() { writer = generation = cookie = 0; frames = 0; }

    /**
     * Record one frame for a producer session, resetting progress on identity change.
     * Parameters: pid is the producer PID, gen is its generation, and stream is its cookie.
     * Returns: nothing.
     */
    void observe(uint32_t pid, uint32_t gen, uint32_t stream) {
        if (writer != pid || generation != gen || cookie != stream) {
            writer = pid; generation = gen; cookie = stream; frames = 0;
        }
        ++frames;
    }
};
#endif
