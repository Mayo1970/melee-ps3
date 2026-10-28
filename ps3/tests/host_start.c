// The host game build's start into a match, with -Wl,--wrap on the two calls below. MELEE_HOST_MATCH=
// "ckind[:color],.../stkind[/item_freq]": after the boot, the debug VS mode with these fighters, player 0 human.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>

void __real_gm_ChangeGameModeAfterCurrentScene(int pending_mode);
void __real_gm_LoadAnnouncer(void);

static bool booted;

static const char* match(void)
{
    return getenv("MELEE_HOST_MATCH");
}

// The first mode change is the boot's, to the title or the memory card scene.
void __wrap_gm_ChangeGameModeAfterCurrentScene(int pending_mode)
{
    if (!booted && match() != NULL) {
        OSReport("host_start: mode %d becomes debug VS (%d)\n", pending_mode, GM_DEBUG_VS);
        pending_mode = GM_DEBUG_VS;
    }
    booted = true;
    __real_gm_ChangeGameModeAfterCurrentScene(pending_mode);
}

// onEnterDebugVs calls this last, after it fills gmVsMelee_StartData.
void __wrap_gm_LoadAnnouncer(void)
{
    StartMeleeData* start = &gmVsMelee_StartData;
    char buf[256];
    char* save = NULL;
    char* fighters;
    char* stage;
    char* items;
    char* c;
    int i;

    __real_gm_LoadAnnouncer();
    if (match() == NULL || gm_GetCurrentGameMode() != GM_DEBUG_VS) {
        return;
    }
    snprintf(buf, sizeof(buf), "%s", match());
    fighters = strtok_r(buf, "/", &save);
    stage = strtok_r(NULL, "/", &save);
    items = strtok_r(NULL, "/", &save);
    for (i = 0; i < 4; i++) {
        start->players[i].slot_type = Gm_PKind_NA;
    }
    for (i = 0, c = strtok_r(fighters, ",", &save); c != NULL && i < 4; c = strtok_r(NULL, ",", &save), i++) {
        start->players[i].ckind = (s8) atoi(c);
        // "ckind:color" picks a costume.
        start->players[i].color = strchr(c, ':') != NULL ? (u8) atoi(strchr(c, ':') + 1) : 0;
        // A trailing 'h' makes a human (idle, without a pad script) of the others too.
        start->players[i].slot_type = i == 0 || strchr(c, 'h') != NULL ? Gm_PKind_Human : Gm_PKind_Cpu;
        // "kN": CPU kind N (training mode's CPU is 0); "lN": CPU level N.
        if (strchr(c, 'k') != NULL) {
            start->players[i].cpu_kind = atoi(strchr(c, 'k') + 1);
        }
        start->players[i].cpu_level = strchr(c, 'l') != NULL ? atoi(strchr(c, 'l') + 1) : 9;
    }
    if (stage != NULL) {
        start->rules.stkind = (u16) atoi(stage);
    }
    if (items != NULL) {
        start->rules.item_freq = (s8) atoi(items);
    }
    OSReport("host_start: %d fighters (%d %d %d %d), stage %d, items %d\n", i, start->players[0].ckind,
             start->players[1].ckind, start->players[2].ckind, start->players[3].ckind, start->rules.stkind,
             start->rules.item_freq);
}
