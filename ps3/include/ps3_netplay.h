// Netplay (ps3/net/netplay.c), under TARGET_PS3: a console hosts, another joins with its code; the host (player 1)
// sets up each VS match, and the rollback (ps3_rollback.h) plays it with the inputs exchanged here.
#ifndef PS3_NETPLAY_H
#define PS3_NETPLAY_H

#include <stdbool.h>

#include <dolphin/pad.h>
#include <melee/mn/forward.h>

enum { PS3_NET_OFF, PS3_NET_HOST, PS3_NET_CONNECT, PS3_NET_PAIRED };

// The PS3 menu (pad thread): host (UPnP opens the port; the code is the address), join a host by its 8 hexadecimal
// digits, or leave.
void ps3_net_host(void);
void ps3_net_join(const char code[8]);
void ps3_net_cancel(void);
// The pairing's state, the player number once paired (1 or 2) and a status line for the menu.
int ps3_net_state(int* player, const char** status);
// A host's code, "" until UPnP has answered.
const char* ps3_net_code(void);
// Changes with anything ps3_net_state gives.
unsigned ps3_net_serial(void);

// The end of gmVsMelee_EnterVs and gmVsMelee_EnterSuddenDeath, or the host's test start: when paired, player 2
// sends its own fighter, player 1 makes the 1v1 of both, player 2 takes it whole. True for a netplay match.
bool ps3_net_handshake(StartMeleeData* start);
// fn_8016E730's start: a netplay match takes the agreed RNG seed and each player's options.
void ps3_net_match_begin(void);
// True from that start to the session's end: the game must not read local pads outside the netplay's inputs.
bool ps3_net_playing(void);

typedef struct Ps3NetTick {
    bool stall;        // run no frame this tick
    int rollback_from; // the first frame to run again; the current frame when none
    int confirmed;     // the newest frame whose remote input is known, as of this tick
} Ps3NetTick;

// The rollback (main thread). ps3_rb_match_start: a netplay match is set up.
bool ps3_net_session(void);
void ps3_net_session_begin(void);
void ps3_net_session_end(int frame);
// Before each tick's frames: what arrived, the frames to run again, and whether this tick runs a frame.
void ps3_net_tick(int frame, Ps3NetTick* t);
// Blocks until the remote inputs up to frame are known (the scene ended there); false when the peer is lost.
bool ps3_net_wait(int frame, Ps3NetTick* t);
// HSD_PadRenewMasterStatus in a session: the frame's pads. sample: its first run, stat holds the pad queue's entry,
// which becomes the local input of frame + the input delay.
void ps3_net_frame_inputs(int frame, bool sample, PADStatus stat[PAD_MAX_CONTROLLERS]);
// After each run of a frame: its gameplay values (ps3_rb_view), for the desync check.
void ps3_net_frame_done(int frame, unsigned hash, const void* view, int size);
// PADControlMotor: the channel that rumbles for chan, or -1 (the other console's player).
int ps3_net_motor_channel(int chan);
// The heartbeat's netplay line (vi.c).
void ps3_net_log(unsigned n);

// pad.c: the first channel with a pad at the last read (-1 for none), and the buttons held on any pad.
int ps3_pad_first_channel(void);
unsigned ps3_pad_buttons(void);

#endif
