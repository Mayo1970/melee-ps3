// What the game and the port need from the rollback (ps3/net/rollback.c), under TARGET_PS3.
#ifndef PS3_ROLLBACK_H
#define PS3_ROLLBACK_H

#include <stddef.h>

#include <dolphin/pad.h>

// One logic frame of the scene loop (gmscene.c); true when the scene ends.
typedef bool (*Ps3RbFrame)(void* arg);

// fn_8016E730 has set up a match: its logic frames from here on are saved and may run again.
void ps3_rb_match_start(void);
// The scene loop's exit.
void ps3_rb_scene_end(void);
// The scene loop's logic frame for one pad queue entry, run through frame(arg) with the saves and re-runs around
// it. True when the scene ends.
bool ps3_rb_frame(Ps3RbFrame frame, void* arg);
// HSD_PadRenewMasterStatus: have is true when stat holds the pad queue's entry. Fills stat with the channels the
// frame uses and keeps them for a re-run; false when the frame renews no pad.
bool ps3_rb_pad(PADStatus stat[4], bool have);
// True while a logic frame runs again: the pad queue and the sound stay as they are.
bool ps3_rb_rerun(void);
// HSD_AudioSFXStartParam in a frame that runs again: the voice the frame's last run started for the same sound, or
// -1 (no new sound). The voices a re-run does not take again stop.
int ps3_rb_sound_rerun(int id);
// HSD_AudioSFXStartParam started voice vid for sound id: the frame's re-runs take it again.
void ps3_rb_sound_started(int id, int vid);

// The port: true from the first saved frame to the scene's end.
bool ps3_rb_session(void);
// A buffer outside the HSD heap that the logic writes (16-byte aligned): the store saves it until the scene ends.
void ps3_rb_cover(void* p, size_t n);
// For traces: the frame running, whether it runs again, whether a logic frame runs now (not the render), and the
// number of restores so far.
void ps3_rb_trace_state(int* frame, bool* rerun, bool* inside, unsigned* pass);
// The heartbeat's rollback line (vi.c), while a session runs or after one ended.
void ps3_rb_log(unsigned n);
// Logs a frame's gameplay values, as the netplay's desync check sends them (ps3_net_frame_done).
void ps3_rb_log_view(const char* whose, int frame, const void* data, int size);

#endif
