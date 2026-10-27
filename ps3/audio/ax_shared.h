#ifndef PS3_AUDIO_AX_SHARED_H
#define PS3_AUDIO_AX_SHARED_H

// Shared by the PPU and the audio SPU program, which plays the GameCube DSP and AI. Both are big-endian.
#include <stdint.h>

#define AX_ARAM_SIZE 0x1000000
#define AX_FRAME_SAMPLES 160 // 5 ms at 32 kHz, one AX frame
#define AX_OUT_FRAMES 240    // the same 5 ms at the port's 48 kHz
#define AX_PORT_BLOCK 256    // frames per audio port block

// SPU to PPU events.
enum {
    AX_EV_INIT = 1, // the DSP task started
    AX_EV_DSP = 2,  // a command list ran; the GameCube DSP yielded here
    AX_EV_AI = 3,   // the AI latched the next DMA buffer
};

// The SPU event port the PPU's queue is connected to; port 1 belongs to spu_printf.
#define AX_EVENT_PORT 16

// Besides AX's own mails, the PPU sends this one to stop the program.
#define AX_MAIL_QUIT 0xDEAD0000u

// The PPU writes bytes 0-63, the SPU writes bytes 64-79, so neither DMA overwrites the other's fields.
typedef struct AxShared {
    uint64_t aram_ea;
    uint64_t port_data_ea;
    uint64_t port_read_ea; // u64 index of the block the port plays, 0 to port_blocks - 1
    uint32_t port_blocks;
    uint32_t ai_running;
    uint32_t ai_addr; // AIInitDMA: the buffer the AI plays next
    uint32_t ai_length;
    uint32_t ppu_pad[6];

    uint32_t frames;        // command lists run
    uint32_t ticks;         // AI periods output
    uint32_t silent_ticks;  // silence written while a command list was late
    uint32_t bad_commands;  // command lists cut short by an unknown command
    uint32_t spu_pad[12];
} __attribute__((aligned(128))) AxShared;

#endif
