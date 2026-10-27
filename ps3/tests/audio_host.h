#ifndef PS3_TESTS_AUDIO_HOST_H
#define PS3_TESTS_AUDIO_HOST_H

#include <stdint.h>

// What the host port played: 48 kHz L/R float frames.
#define HOST_CAPTURE_FRAMES (48000 * 12)
extern float host_capture[HOST_CAPTURE_FRAMES * 2];
extern volatile uint32_t host_captured;

#endif
