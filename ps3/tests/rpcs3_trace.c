// For the RPCS3 build only (EXTRA_SRCS, with -Wl,--wrap on these calls): logs the sound calls after the automatic
// A press (PS3_PAD_AUTOPRESS), to find where the first sound effect faults.
#include <dolphin/ax.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "../os/udplog.h"

#ifndef PS3_PAD_AUTOPRESS
#define PS3_PAD_AUTOPRESS 0
#endif

static int tracing(void)
{
    return VIGetRetraceCount() >= PS3_PAD_AUTOPRESS;
}

int __real_HSD_SynthSFXPlayWithGroup(int sfx_id, u8 vol, u8 vol2, u8 pan, int priority, int itd_flag, int group,
                                     f32 pitch1, f32 pitch2, f32 mix_main, f32 mix_auxA, f32 mix_auxB);
AXVPB* __real_AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext);
void __real_AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr);
void __real_AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm);
void __real_AXSetVoiceSrc(AXVPB* p, AXPBSRC* src);
void __real_AXSetVoiceState(AXVPB* p, u16 state);

int __wrap_HSD_SynthSFXPlayWithGroup(int sfx_id, u8 vol, u8 vol2, u8 pan, int priority, int itd_flag, int group,
                                     f32 pitch1, f32 pitch2, f32 mix_main, f32 mix_auxA, f32 mix_auxB)
{
    int r;

    if (tracing()) {
        ps3_log("trace: SFX play %d group %d\n", sfx_id, group);
    }
    r = __real_HSD_SynthSFXPlayWithGroup(sfx_id, vol, vol2, pan, priority, itd_flag, group, pitch1, pitch2,
                                         mix_main, mix_auxA, mix_auxB);
    if (tracing()) {
        ps3_log("trace: SFX play %d -> %d\n", sfx_id, r);
    }
    return r;
}

AXVPB* __wrap_AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext)
{
    AXVPB* v = __real_AXAcquireVoice(priority, callback, userContext);

    if (tracing()) {
        ps3_log("trace: AXAcquireVoice(%u, %p, %u) -> %p\n", priority, (void*) callback, userContext, (void*) v);
    }
    return v;
}

void __wrap_AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
    if (tracing()) {
        ps3_log("trace: AXSetVoiceAddr(%p, %p) loop %u fmt %u loop %08x end %08x cur %08x\n", (void*) p, (void*) addr,
                addr->loopFlag, addr->format, ((u32) addr->loopAddressHi << 16) | addr->loopAddressLo,
                ((u32) addr->endAddressHi << 16) | addr->endAddressLo,
                ((u32) addr->currentAddressHi << 16) | addr->currentAddressLo);
    }
    __real_AXSetVoiceAddr(p, addr);
}

void __wrap_AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm)
{
    if (tracing()) {
        ps3_log("trace: AXSetVoiceAdpcm(%p, %p)\n", (void*) p, (void*) adpcm);
    }
    __real_AXSetVoiceAdpcm(p, adpcm);
}

void __wrap_AXSetVoiceSrc(AXVPB* p, AXPBSRC* src)
{
    if (tracing()) {
        ps3_log("trace: AXSetVoiceSrc(%p, %p)\n", (void*) p, (void*) src);
    }
    __real_AXSetVoiceSrc(p, src);
}

void __wrap_AXSetVoiceState(AXVPB* p, u16 state)
{
    if (tracing()) {
        ps3_log("trace: AXSetVoiceState(%p, %u)\n", (void*) p, state);
    }
    __real_AXSetVoiceState(p, state);
}

int __real_HSD_SynthSFXLoad(const char* filename, int bankID, void (*cb)(int, int), int mode);
void __real_HSD_SynthSFXGroupDataRemove(int sfx_id);
void __real_HSD_SynthSFXBankDeflag(int bank_id);
void __real_HSD_SynthSFXUnloadBank(int bank_id);

int __wrap_HSD_SynthSFXLoad(const char* filename, int bankID, void (*cb)(int, int), int mode)
{
    int r = __real_HSD_SynthSFXLoad(filename, bankID, cb, mode);

    ps3_log("trace: SFX load %s bank %d -> %d\n", filename, bankID, r);
    return r;
}

void __wrap_HSD_SynthSFXGroupDataRemove(int sfx_id)
{
    ps3_log("trace: SFX group remove %d\n", sfx_id);
    __real_HSD_SynthSFXGroupDataRemove(sfx_id);
}

void __wrap_HSD_SynthSFXBankDeflag(int bank_id)
{
    ps3_log("trace: SFX bank deflag %d\n", bank_id);
    __real_HSD_SynthSFXBankDeflag(bank_id);
}

void __wrap_HSD_SynthSFXUnloadBank(int bank_id)
{
    ps3_log("trace: SFX bank unload %d\n", bank_id);
    __real_HSD_SynthSFXUnloadBank(bank_id);
}
