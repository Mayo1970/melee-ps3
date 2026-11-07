// The host build's start (--wrap on the calls below). MELEE_HOST_MATCH="ckind[:color],.../stkind[/item_freq[/mask]]":
// debug VS, player 0 human. MELEE_HOST_ADV="ckind/scene": Adventure from that scene (0x38: F-Zero), no CSS.
// MELEE_HOST_EVENT="level/ckind[:color]": the event of that internal level (gm_803DF918 gives its menu number), no CSS.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmevent.h>
#include <melee/gm/gmregclear.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/it/forward.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <ps3_netplay.h>
#include <sysdolphin/baselib/random.h>

void __real_gm_ChangeGameModeAfterCurrentScene(int pending_mode);
void __real_gm_LoadAnnouncer(void);
void __real_gm_SetGameModeStateId(u8 id);
void __real_gm_Mode_Event_OnLoad(void);
void __real_gm_80168F88(void);
void __real_itEvYoshiEgg_Spawn(Vec3* pos);
ItemKind* __real_gm_801BEC54(void);

static bool booted;

static const char* match(void)
{
    return getenv("MELEE_HOST_MATCH");
}

static const char* adventure(void)
{
    return getenv("MELEE_HOST_ADV");
}

static const char* event(void)
{
    return getenv("MELEE_HOST_EVENT");
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
    } else if (!booted && event() != NULL) {
        OSReport("host_start: mode %d becomes Event (%d)\n", pending_mode, GM_EVENT);
        pending_mode = GM_EVENT;
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

// The event's on_load picks the CSS (state 0) for a free fighter; the CSS's exit would fill x2 and x3.
void __wrap_gm_Mode_Event_OnLoad(void)
{
    static bool done;
    struct EventData* ev = &gmMainLib_804D3EE0->vs.unk_530;
    char buf[64];
    char* save = NULL;
    char* c;

    if (done || event() == NULL) {
        __real_gm_Mode_Event_OnLoad();
        return;
    }
    done = true;
    snprintf(buf, sizeof(buf), "%s", event());
    ev->unk_535 = (u8) atoi(strtok_r(buf, "/", &save));
    c = strtok_r(NULL, "/", &save);
    ev->x2 = c != NULL ? (s8) atoi(c) : CKind_Mario;
    ev->x3 = c != NULL && strchr(c, ':') != NULL ? (u8) atoi(strchr(c, ':') + 1) : 0;
    if (getenv("MELEE_HOST_SEED") != NULL) {
        *HSD_RandSeedPtr = (u32) strtoul(getenv("MELEE_HOST_SEED"), NULL, 0);
    }
    OSReport("host_start: event level %d as ckind %d, RNG seed %#x\n", ev->unk_535, ev->x2, *HSD_RandSeedPtr);
    __real_gm_Mode_Event_OnLoad();
    // A free fighter (x44 != 0): the CSS preloads it in cache entry 0 every frame (mnCharSel_Scene_OnFrame).
    if (ev->x44 != 0) {
        struct GameCache* cache = &lbDvd_GetPreloadCacheScene()->game_cache;

        cache->entries[0].char_id = ev->x2;
        cache->entries[0].color = ev->x3;
        lbDvd_80018254();
    }
    __real_gm_SetGameModeStateId(1);
}

// onEnterVs calls this last: the fighters the event chose.
void __wrap_gm_80168F88(void)
{
    struct EventData* ev = &gmMainLib_804D3EE0->vs.unk_530;

    if (event() != NULL && gm_GetCurrentGameMode() == GM_EVENT) {
        OSReport("host_start: event level %d fighters %d %d %d %d\n", ev->unk_535, ev->x4C[0], ev->x4C[1],
                 ev->x4C[2], ev->x4C[3]);
    }
    __real_gm_80168F88();
}

void __wrap_itEvYoshiEgg_Spawn(Vec3* pos)
{
    OSReport("host_start: event egg at %.2f %.2f %.2f\n", pos->x, pos->y, pos->z);
    __real_itEvYoshiEgg_Spawn(pos);
}

ItemKind* __wrap_gm_801BEC54(void)
{
    ItemKind* kinds = __real_gm_801BEC54();
    char line[256];
    int n = 0;
    int i;

    if (kinds == NULL) {
        return kinds;
    }
    for (i = 0; i < It_PKind_Items_Size && n < (int) sizeof(line) - 12; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %#x", kinds[i]);
        if (kinds[i] == It_PKind_Terminate) {
            break;
        }
    }
    OSReport("host_start: event Pokemon list%s\n", line);
    return kinds;
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
