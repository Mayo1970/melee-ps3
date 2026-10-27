#ifndef PS3_AUDIO_BACKEND_H
#define PS3_AUDIO_BACKEND_H

// What sdk_audio.c needs from the platform: the audio port and the audio program.
#include <stdbool.h>
#include <stdint.h>

#include "ax_shared.h"

// Opens the audio port, fills the port fields of shared, and starts the audio program with it.
bool ps3_audio_backend_start(AxShared* shared);
void ps3_audio_backend_mail(uint32_t mail);

// The backend calls this for each event the audio program sends.
void ps3_audio_spu_event(uint32_t code);

#endif
