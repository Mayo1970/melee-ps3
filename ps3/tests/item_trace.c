// For a host trace build only (EXTRA_SRCS, with -Wl,--wrap on these calls): logs the Poke Balls and the Pokemon
// they release, with their states, to see where a Pokemon leaves its GameCube behaviour.
#include <dolphin/os.h>
#include <melee/it/forward.h>
#include <melee/it/inlines.h>
#include <melee/it/item.h>
#include <melee/it/itemattrs.h>
#include <melee/it/types.h>

Item_GObj* __real_Item_80268B18(SpawnItem* spawn);
Item_GObj* __real_Item_80268B5C(SpawnItem* spawn);
void __real_Item_80268E5C(HSD_GObj* gobj, enum_t msid, Item_StateChangeFlags flags);

static bool traced(int kind)
{
    return kind == It_Kind_M_Ball || (kind >= It_PKind_Start && kind < It_PKind_Related_End);
}

static Item_GObj* spawned(const char* how, SpawnItem* spawn, Item_GObj* gobj)
{
    if (spawn->kind < It_Kind_Common_End || traced(spawn->kind)) {
        OSReport("itrace: %s kind %d at (%.1f, %.1f) -> %p\n", how, spawn->kind, spawn->pos.x, spawn->pos.y,
                 (void*) gobj);
    }
    return gobj;
}

Item_GObj* __wrap_Item_80268B18(SpawnItem* spawn)
{
    return spawned("spawn", spawn, __real_Item_80268B18(spawn));
}

Item_GObj* __wrap_Item_80268B5C(SpawnItem* spawn)
{
    return spawned("spawn held", spawn, __real_Item_80268B5C(spawn));
}

void __wrap_Item_80268E5C(HSD_GObj* gobj, enum_t msid, Item_StateChangeFlags flags)
{
    Item* ip = GET_ITEM(gobj);

    if (traced(ip->kind)) {
        OSReport("itrace: %p kind %d state %d at (%.1f, %.1f)\n", (void*) gobj, ip->kind, msid, ip->pos.x,
                 ip->pos.y);
    }
    if (ip->kind == It_Kind_M_Ball && msid == 5) {
        const itPokemonSpawn_DatAttrs* a = &ip->xC4_article_data->x4_specialAttributes->pokemon_spawn;
        const s32* w = a->pokemon_spawn_weights;

        OSReport("itrace: ball attrs %p: x0 %g x4 %g x8 %g xC %g x10 %g x14 %g x18 %g x34 %g x38 %d; weights %d %d %d "
                 "%d %d %d %d %d %d %d %d %d %d %d %d\n",
                 (const void*) a, a->x0, a->x4, a->x8, a->xC, a->x10, a->x14, a->x18, a->x34, a->x38, w[0], w[1],
                 w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14]);
    }
    __real_Item_80268E5C(gobj, msid, flags);
}
