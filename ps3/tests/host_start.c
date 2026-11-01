// The host build's start (--wrap on the calls below). MELEE_HOST_MATCH="ckind[:color],.../stkind[/item_freq[/mask]]":
// debug VS, player 0 human. MELEE_HOST_ADV="ckind/scene": Adventure from that scene (0x38: F-Zero), no CSS.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmregclear.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <ps3_netplay.h>
#include <sysdolphin/baselib/random.h>

void __real_gm_ChangeGameModeAfterCurrentScene(int pending_mode);
void __real_gm_LoadAnnouncer(void);
void __real_gm_SetGameModeStateId(u8 id);

static bool booted;

static const char* match(void)
{
    return getenv("MELEE_HOST_MATCH");
}

static const char* adventure(void)
{
    return getenv("MELEE_HOST_ADV");
}

// The first mode change is the boot's, to the title or the memory card scene.
void __wrap_gm_ChangeGameModeAfterCurrentScene(int pending_mode)
{
    if (!booted && match() != NULL) {
        OSReport("host_start: mode %d becomes debug VS (%d)\n", pending_mode, GM_DEBUG_VS);
        pending_mode = GM_DEBUG_VS;
    } else if (!booted && adventure() != NULL) {
        OSReport("host_start: mode %d becomes Adventure (%d)\n", pending_mode, GM_ADVENTURE);
        pending_mode = GM_ADVENTURE;
    }
    // MELEE_HOST_NET_HOST, or MELEE_HOST_NET_JOIN=<code>: the boot hosts or joins, so the match is a netplay one.
    if (!booted && getenv("MELEE_HOST_NET_HOST") != NULL) {
        ps3_net_host();
    } else if (!booted && getenv("MELEE_HOST_NET_JOIN") != NULL && strlen(getenv("MELEE_HOST_NET_JOIN")) == 8) {
        ps3_net_join(getenv("MELEE_HOST_NET_JOIN"));
    }
    booted = true;
    __real_gm_ChangeGameModeAfterCurrentScene(pending_mode);
}

// Adventure's on_load starts at its CSS (0x70); fill in what the CSS's exit would, and start at the scene.
void __wrap_gm_SetGameModeStateId(u8 id)
{
    static bool done;
    struct gmm_x0_528_t* css = gmMainLib_8015CDD4();
    UnkAdventureData* data = gm_GetAdventureData();
    char buf[64];
    char* save = NULL;
    char* scene;

    if (done || adventure() == NULL || id != 0x70) {
        __real_gm_SetGameModeStateId(id);
        return;
    }
    done = true;
    snprintf(buf, sizeof(buf), "%s", adventure());
    css->c_kind = (s8) atoi(strtok_r(buf, "/", &save));
    scene = strtok_r(NULL, "/", &save);
    id = scene != NULL ? (u8) strtoul(scene, NULL, 0) : 0;
    data->x0.x0.ckind = css->c_kind;
    data->x0.x0.color = css->color;
    data->x0.x0.cpu_level = css->cpu_level;
    data->x0.x0.stocks = css->stocks;
    data->x0.x0.nametag = css->nametag;
    gm_80168F88();
    if (getenv("MELEE_HOST_SEED") != NULL) {
        *HSD_RandSeedPtr = (u32) strtoul(getenv("MELEE_HOST_SEED"), NULL, 0);
    }
    OSReport("host_start: Adventure as ckind %d from scene %#x, RNG seed %#x\n", css->c_kind, id, *HSD_RandSeedPtr);
    __real_gm_SetGameModeStateId(id);
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
    char* mask;
    char* c;
    int i;

    __real_gm_LoadAnnouncer();
    if (match() == NULL || gm_GetCurrentGameMode() != GM_DEBUG_VS) {
        return;
    }
    // The boot seeds the RNG from the clock; MELEE_HOST_SEED repeats a match.
    if (getenv("MELEE_HOST_SEED") != NULL) {
        *HSD_RandSeedPtr = (u32) strtoul(getenv("MELEE_HOST_SEED"), NULL, 0);
    }
    OSReport("host_start: RNG seed %#x\n", *HSD_RandSeedPtr);
    snprintf(buf, sizeof(buf), "%s", match());
    fighters = strtok_r(buf, "/", &save);
    stage = strtok_r(NULL, "/", &save);
    items = strtok_r(NULL, "/", &save);
    mask = strtok_r(NULL, "/", &save);
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
    // A bit per item kind, e.g. 0x400000000 for Poke Balls only (kind 0x22).
    if (mask != NULL) {
        start->rules.x20 = strtoull(mask, NULL, 0);
    }
    OSReport("host_start: %d fighters (%d %d %d %d), stage %d, items %d\n", i, start->players[0].ckind,
             start->players[1].ckind, start->players[2].ckind, start->players[3].ckind, start->rules.stkind,
             start->rules.item_freq);
    // As gmVsMelee_EnterVs's hook; its announcer call would come back here.
    if (ps3_net_handshake(start)) {
        gmVsMelee_NetplayLoads(start);
    }
}
