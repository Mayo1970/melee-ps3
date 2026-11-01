/* INCLUDEd in .data (melee.ld, host.ld): the game's statics, which a rollback restores, lie in [__rb_data_start,
   __rb_data_end). Outside: state that threads change, timings, and the values HSD keeps of what GX holds. */
*/game/src/sysdolphin/baselib/state.o(.data .data.*)
*/game/src/sysdolphin/baselib/perf.o(.data .data.*)
*/game/src/sysdolphin/baselib/tev.o(.data.prev_ch)
*/game/src/sysdolphin/baselib/synth.o(.data .data.*)
*/game/src/sysdolphin/baselib/axdriver.o(.data .data.*)
*/game/src/sysdolphin/baselib/video.o(.data .data.*)
*/game/src/sysdolphin/baselib/devcom.o(.data .data.*)
*/game/src/sysdolphin/baselib/rumble.o(.data .data.*)
*/game/src/sysdolphin/baselib/card.o(.data .data.*)
*/game/src/sysdolphin/baselib/debugconsole_main.o(.data .data.*)
*/game/src/melee/lb/lbcardgame.o(.data .data.*)
*/game/src/melee/lb/lbcardnew.o(.data .data.*)
*/game/src/melee/lb/lbsnap.o(.data .data.*)
*/game/src/melee/lb/lbmthp.o(.data .data.*)
*/game/src/melee/lb/lbarq.o(.data .data.*)
. = ALIGN(128);
__rb_data_start = .;
*/game/src/*(.data .data.*)
. = ALIGN(128);
__rb_data_end = .;
