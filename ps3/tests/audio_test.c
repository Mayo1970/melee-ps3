// Host test: the SDK's AX library over ps3/audio, the audio program on a thread. Tones in turn: PCM16 left, PCM8
// through the rate converter right, ADPCM, PCM16 through AUX A (side, pitch, level), then AUX A with each AXFX effect.
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/axfx.h>
#include <dolphin/os.h>

#include "../os/intr.h"
#include "audio_host.h"

void ps3_audio_stats(u32* frames, u32* ticks, u32* silent_ticks, u32* bad_commands);

#define PI 3.14159265358979
#define AMPLITUDE 0.9
#define VOLUME 0x4000 // 0.5
#define TONE_FRAMES 100
#define GAP_FRAMES 30
#define SEGMENTS 4
#define TIMEOUT_SECONDS 600
#define FX_COUNT 4
#define FX_TONE_FRAMES 100
#define FX_PERIOD 200
#define FX_START (GAP_FRAMES + SEGMENTS * (TONE_FRAMES + GAP_FRAMES))
#define FX_TAIL_MIN 16 // an output peak above this, with no input, is a tail frame
#define AUX_SAMPLES 160 // per AUX channel and AX frame

void OSReport(char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

OSTime OSGetTime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (OSTime) ts.tv_sec * 40500000 + ts.tv_nsec / 25;
}

void DCFlushRange(void* addr, u32 nBytes) {}
void DCFlushRangeNoSync(void* addr, u32 nBytes) {}
void DCInvalidateRange(void* addr, u32 nBytes) {}

// AXFX's own allocator, which AXFXSetHooks replaces as axdriver.c does.
volatile OSHeapHandle __OSCurrHeap = -1;

void* OSAllocFromHeap(int heap, unsigned long size)
{
    fprintf(stderr, "AXFX allocated %lu bytes without the hooks\n", size);
    abort();
}

void OSFreeToHeap(int heap, void* ptr)
{
    fprintf(stderr, "AXFX freed without the hooks\n");
    abort();
}

static u8 pcm16[1600 * 2] __attribute__((aligned(32)));
static u8 pcm8[1600] __attribute__((aligned(32)));
static u8 adpcm[256] __attribute__((aligned(32)));
static u8 aux_pcm16[1600 * 2] __attribute__((aligned(32)));
static u32 ar_stack[16];
static AXVPB* voices[SEGMENTS];
static AXVPB* fx_voices[FX_COUNT];
static volatile int frame;

static const struct {
    double freq;
    int left, right;
} expect[SEGMENTS] = { { 440, 1, 0 }, { 330, 0, 1 }, { 500, 1, 1 }, { 220, 1, 1 } };

static s16 tone(double freq, double rate, int i)
{
    return (s16) lround(AMPLITUDE * 32767 * sin(2 * PI * freq * i / rate));
}

static u32 upload(void* src, u32 size)
{
    ARQRequest req;
    u32 aram = ARAlloc((size + 31) & ~31u);
    ARQPostRequest(&req, 0, ARQ_TYPE_MRAM_TO_ARAM, ARQ_PRIORITY_HIGH, (u32) (uintptr_t) src, aram,
                   (size + 31) & ~31u, NULL);
    return aram;
}

// DSP-ADPCM with one predictor (an exact sine recurrence), choosing each frame's scale by its error.
static void encode_adpcm(double freq, AXPBADPCM* info, AXPBADPCMLOOP* loop)
{
    double w = 2 * PI * freq / 32000;
    int c1 = (int) lround(2 * cos(w) * 2048);
    int c2 = -2048;
    int yn1 = tone(freq, 32000, -1);
    int yn2 = tone(freq, 32000, -2);
    int f;

    memset(info, 0, sizeof(*info));
    info->a[0][0] = (u16) c1;
    info->a[0][1] = (u16) c2;
    info->yn1 = (u16) yn1;
    info->yn2 = (u16) yn2;
    for (f = 0; f < 32; f++) {
        int best_scale = 0;
        double best_err = -1;
        int s;
        for (s = 0; s < 12; s++) {
            int h1 = yn1, h2 = yn2, i;
            double err = 0;
            for (i = 0; i < 14; i++) {
                int x = tone(freq, 32000, f * 14 + i);
                int pred = (0x400 + c1 * h1 + c2 * h2) >> 11;
                int q = (int) lround((double) (x - pred) / (1 << s));
                int v;
                q = q < -8 ? -8 : q > 7 ? 7 : q;
                v = q * (1 << s) + pred;
                v = v < -0x7FFF ? -0x7FFF : v > 0x7FFF ? 0x7FFF : v;
                err += (double) (x - v) * (x - v);
                h2 = h1;
                h1 = v;
            }
            if (best_err < 0 || err < best_err) {
                best_err = err;
                best_scale = s;
            }
        }
        adpcm[f * 8] = (u8) best_scale;
        {
            int i;
            for (i = 0; i < 14; i++) {
                int x = tone(freq, 32000, f * 14 + i);
                int pred = (0x400 + c1 * yn1 + c2 * yn2) >> 11;
                int q = (int) lround((double) (x - pred) / (1 << best_scale));
                int v;
                q = q < -8 ? -8 : q > 7 ? 7 : q;
                v = q * (1 << best_scale) + pred;
                v = v < -0x7FFF ? -0x7FFF : v > 0x7FFF ? 0x7FFF : v;
                adpcm[f * 8 + 1 + i / 2] |= (u8) ((q & 15) << (i & 1 ? 0 : 4));
                yn2 = yn1;
                yn1 = v;
            }
        }
    }
    info->pred_scale = adpcm[0];
    loop->loop_pred_scale = adpcm[0];
    loop->loop_yn1 = (u16) yn1;
    loop->loop_yn2 = (u16) yn2;
}

static AXVPB* make_voice(u16 format, u32 start, u32 end, u32 ratio, u16 l, u16 r, u16 aux)
{
    AXVPB* v = AXAcquireVoice(15, NULL, 0);
    AXPBADDR addr = { 1, format, (u16) (start >> 16), (u16) start, (u16) (end >> 16), (u16) end,
                      (u16) (start >> 16), (u16) start };
    AXPBMIX mix;
    AXPBVE ve = { 0x7FFF, 0 };

    memset(&mix, 0, sizeof(mix));
    mix.vL = l;
    mix.vR = r;
    mix.vAuxAL = aux;
    mix.vAuxAR = aux;
    AXSetVoiceAddr(v, &addr);
    AXSetVoiceSrcType(v, AX_SRC_TYPE_LINEAR);
    AXSetVoiceSrcRatio(v, ratio / 65536.0f);
    AXSetVoiceMix(v, &mix);
    AXSetVoiceVe(v, &ve);
    return v;
}

static struct AXFX_REVERBHI fx_hi;
static struct AXFX_REVERBSTD fx_std;
static struct AXFX_CHORUS fx_chorus;
static struct AXFX_DELAY fx_delay;
static int fx_allocs, fx_frees;
// The effect on AUX A, or -1 to return its input untouched, two frames later.
static volatile int fx_current = -1;

// What each effect got and gave on AUX A.
static struct {
    const char* name;
    int tail_wanted;
    int frames, tone_frames, tail_frames;
    int layout_errors; // input surround not zero, or left not equal to right
    int saturated;     // outputs at INT_MIN or INT_MAX, as a NaN or an overflow gives
    s64 in_peak, out_peak;
} fx[FX_COUNT] = { { "reverb_hi", 1 }, { "reverb_std", 1 }, { "chorus", 0 }, { "delay", 1 } };

static void* fx_alloc(unsigned long size)
{
    fx_allocs++;
    return calloc(1, size);
}

static void fx_free(void* ptr)
{
    fx_frees++;
    free(ptr);
}

// The game's parameters (axdriver.c, lbaudio_ax.c); the chorus varies its delay, so both resamplers run.
static int fx_init(void)
{
    AXFXSetHooks(fx_alloc, fx_free);
    fx_hi.time = 3.0f;
    fx_hi.preDelay = 0.1f;
    fx_hi.damping = 0.5f;
    fx_hi.coloration = 0.5f;
    fx_hi.crosstalk = 0.3f;
    fx_hi.mix = 0.5f;
    fx_std.time = 1.88f;
    fx_std.preDelay = 0.002f;
    fx_std.damping = 0.64f;
    fx_std.coloration = 0.5f;
    fx_std.mix = 1.0f;
    fx_chorus.baseDelay = 15;
    fx_chorus.variation = 4;
    fx_chorus.period = 500;
    fx_delay.delay[0] = 260;
    fx_delay.delay[1] = 310;
    fx_delay.delay[2] = 6;
    fx_delay.feedback[0] = fx_delay.feedback[1] = 24;
    fx_delay.output[0] = fx_delay.output[1] = 35;
    return AXFXReverbHiInit(&fx_hi) == 1 && AXFXReverbStdInit(&fx_std) == 1 &&
           AXFXChorusInit(&fx_chorus) == 1 && AXFXDelayInit(&fx_delay) == 1;
}

static s64 peak(const s32* x)
{
    s64 p = 0;
    int i;
    for (i = 0; i < AUX_SAMPLES; i++) {
        s64 v = x[i] < 0 ? -(s64) x[i] : x[i];
        p = v > p ? v : p;
    }
    return p;
}

static s64 max64(s64 a, s64 b)
{
    return a > b ? a : b;
}

static int saturated(const s32* x)
{
    int n = 0;
    int i;
    for (i = 0; i < AUX_SAMPLES; i++) {
        n += x[i] == 0x7FFFFFFF || x[i] == -0x7FFFFFFF - 1;
    }
    return n;
}

static void aux_callback(void* data, void* context)
{
    struct AXFX_BUFFERUPDATE* b = data;
    int e = fx_current;
    s64 in_peak;
    s64 out_peak;
    int i;

    if (e < 0) {
        return;
    }
    for (i = 0; i < AUX_SAMPLES; i++) {
        fx[e].layout_errors += b->left[i] != b->right[i] || b->surround[i] != 0;
    }
    in_peak = peak(b->left);
    switch (e) {
    case 0:
        AXFXReverbHiCallback(b, &fx_hi);
        break;
    case 1:
        AXFXReverbStdCallback(b, &fx_std);
        break;
    case 2:
        AXFXChorusCallback(b, &fx_chorus);
        break;
    case 3:
        AXFXDelayCallback(b, &fx_delay);
        break;
    }
    out_peak = max64(peak(b->left), max64(peak(b->right), peak(b->surround)));
    fx[e].saturated += saturated(b->left) + saturated(b->right) + saturated(b->surround);
    fx[e].frames++;
    if (in_peak != 0) {
        fx[e].tone_frames++;
    } else if (fx[e].tone_frames != 0 && out_peak > FX_TAIL_MIN) {
        fx[e].tail_frames++;
    }
    fx[e].in_peak = max64(fx[e].in_peak, in_peak);
    fx[e].out_peak = max64(fx[e].out_peak, out_peak);
}

static void on_frame(void)
{
    int f = frame++;
    int seg = (f - GAP_FRAMES) / (TONE_FRAMES + GAP_FRAMES);
    int at = (f - GAP_FRAMES) % (TONE_FRAMES + GAP_FRAMES);

    if (f >= GAP_FRAMES && seg < SEGMENTS) {
        if (at == 0) {
            AXSetVoiceState(voices[seg], 1);
        } else if (at == TONE_FRAMES) {
            AXSetVoiceState(voices[seg], 0);
        }
    }
    if (f >= FX_START && (f - FX_START) / FX_PERIOD < FX_COUNT) {
        int e = (f - FX_START) / FX_PERIOD;
        if ((f - FX_START) % FX_PERIOD == 0) {
            fx_current = e;
            AXSetVoiceState(fx_voices[e], 1);
        } else if ((f - FX_START) % FX_PERIOD == FX_TONE_FRAMES) {
            AXSetVoiceState(fx_voices[e], 0);
        }
    }
}

// qemu-ppc64 has no debugger here, so a fault prints its address and code address.
static void on_fault(int sig, siginfo_t* info, void* context)
{
    ucontext_t* uc = context;
    fprintf(stderr, "fault: signal %d at %p, pc %lx, lr %lx\n", sig, info->si_addr,
            (unsigned long) uc->uc_mcontext.gp_regs[32], (unsigned long) uc->uc_mcontext.gp_regs[36]);
    _exit(3);
}

typedef struct Span {
    u32 start, end;
} Span;

static double level(const float* x, u32 n)
{
    double sum = 0;
    u32 i;
    for (i = 0; i < n; i++) {
        sum += x[2 * i] * x[2 * i];
    }
    return sqrt(sum / n);
}

static double frequency(const float* x, u32 n)
{
    u32 crossings = 0;
    u32 i;
    for (i = 1; i < n; i++) {
        if ((x[2 * i - 2] < 0) != (x[2 * i] < 0)) {
            crossings++;
        }
    }
    return crossings / 2.0 / (n / 48000.0);
}

int main(void)
{
    AXPBADPCM info;
    AXPBADPCMLOOP loop;
    u32 tones_end = (u32) ((GAP_FRAMES + SEGMENTS * (TONE_FRAMES + GAP_FRAMES)) * 240.0);
    u32 needed = (FX_START + FX_COUNT * FX_PERIOD + 10) * 240;
    Span spans[16];
    int nspans = 0;
    int failures = 0;
    time_t start = time(NULL);
    u32 frames, ticks, silent, bad;
    int callbacks;
    u32 i;
    int s;
    int e;
    int old;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    for (i = 0; i < 1600; i++) {
        s16 a = tone(440, 32000, (int) i % 800);
        s16 b = tone(220, 32000, (int) i);
        pcm16[2 * i] = (u8) (a >> 8);
        pcm16[2 * i + 1] = (u8) a;
        aux_pcm16[2 * i] = (u8) (b >> 8);
        aux_pcm16[2 * i + 1] = (u8) b;
        pcm8[i] = (u8) (tone(330, 16000, (int) i) >> 8);
    }

    ps3_intr_init();
    ARInit(ar_stack, 16);
    ARQInit();
    AIInit(NULL);
    AXInit();

    {
        u32 a16 = upload(pcm16, 800 * 2) / 2;
        u32 a8 = upload(pcm8, 1600);
        u32 aa = upload(aux_pcm16, 1600 * 2) / 2;
        u32 ad;
        encode_adpcm(500, &info, &loop);
        ad = upload(adpcm, sizeof(adpcm)) * 2;
        voices[0] = make_voice(0x0A, a16, a16 + 799, 0x10000, VOLUME, 0, 0);
        voices[1] = make_voice(0x19, a8, a8 + 1599, 0x8000, 0, VOLUME, 0);
        voices[2] = make_voice(0x00, ad + 2, ad + 32 * 16 - 1, 0x10000, VOLUME, VOLUME, 0);
        AXSetVoiceAdpcm(voices[2], &info);
        AXSetVoiceAdpcmLoop(voices[2], &loop);
        voices[3] = make_voice(0x0A, aa, aa + 1599, 0x10000, 0, 0, VOLUME);
        for (e = 0; e < FX_COUNT; e++) {
            fx_voices[e] = make_voice(0x0A, aa, aa + 1599, 0x10000, 0, 0, VOLUME);
        }
    }
    if (!fx_init()) {
        fprintf(stderr, "an AXFX effect failed to start\n");
        return 1;
    }
    AXRegisterAuxACallback(aux_callback, NULL);
    AXRegisterCallback(on_frame);

    while (host_captured < needed) {
        if (time(NULL) - start > TIMEOUT_SECONDS) {
            fprintf(stderr, "timeout: %u of %u frames played\n", host_captured, needed);
            return 1;
        }
        usleep(10000);
    }
    // Both counts at one moment: the program keeps running while the test checks.
    callbacks = frame;
    ps3_audio_stats(&frames, &ticks, &silent, &bad);
    old = OSDisableInterrupts();
    AXRegisterAuxACallback(NULL, NULL);
    OSRestoreInterrupts(old);

    // Tones, as runs of frames where either side is above -40 dB.
    for (i = 0; i < tones_end && i < host_captured && nspans < 16; i++) {
        int on = fabsf(host_capture[2 * i]) > 0.01f || fabsf(host_capture[2 * i + 1]) > 0.01f;
        if (on && (nspans == 0 || spans[nspans - 1].end + 480 < i)) {
            spans[nspans].start = i;
            spans[nspans++].end = i;
        } else if (on) {
            spans[nspans - 1].end = i;
        }
    }
    if (nspans != SEGMENTS) {
        fprintf(stderr, "%d tones found, %d expected\n", nspans, SEGMENTS);
        failures++;
    }
    for (s = 0; s < nspans && s < SEGMENTS; s++) {
        // Skip 10 ms at each edge, where the voice starts and stops.
        u32 a = spans[s].start + 480;
        u32 n = spans[s].end - 480 - a;
        double want_level = AMPLITUDE * VOLUME / 32768.0 / sqrt(2.0);
        int c;
        for (c = 0; c < 2; c++) {
            int want = c == 0 ? expect[s].left : expect[s].right;
            double lv = level(host_capture + 2 * a + c, n);
            double fr = frequency(host_capture + 2 * a + c, n);
            int ok = want ? fabs(fr - expect[s].freq) < expect[s].freq * 0.01 &&
                                fabs(lv - want_level) < want_level * 0.1
                          : lv < 0.001;
            printf("tone %d %s: %.1f Hz, level %.4f (want %s)%s\n", s, c == 0 ? "L" : "R", fr, lv,
                   want ? "tone" : "silence", ok ? "" : "  FAIL");
            failures += !ok;
        }
    }
    for (e = 0; e < FX_COUNT; e++) {
        // On AUX A: the input's layout (left equals right, no surround), saturation, range and tail.
        int ok = fx[e].layout_errors == 0 && fx[e].saturated == 0 && fx[e].out_peak > 0 &&
                 fx[e].out_peak <= 4 * fx[e].in_peak && abs(fx[e].tone_frames - FX_TONE_FRAMES) <= 2 &&
                 (fx[e].tail_wanted ? fx[e].tail_frames >= 20 : fx[e].tail_frames <= 5);
        printf("%s: %d frames, %d with input, %d of tail after it; input peak %lld, output peak %lld; "
               "layout errors %d, saturated %d%s\n",
               fx[e].name, fx[e].frames, fx[e].tone_frames, fx[e].tail_frames, (long long) fx[e].in_peak,
               (long long) fx[e].out_peak, fx[e].layout_errors, fx[e].saturated, ok ? "" : "  FAIL");
        failures += !ok;
    }
    // In the output: the effect's return starts two frames after a direct tone would (tone 0 at GAP_FRAMES).
    for (e = 0; e < FX_COUNT && nspans != 0; e++) {
        u32 at = spans[0].start + (u32) (FX_START + e * FX_PERIOD - GAP_FRAMES + 2) * 240;
        u32 bad_samples = 0;
        int c;
        for (i = at; i < at + FX_PERIOD * 240; i++) {
            bad_samples += !isfinite(host_capture[2 * i]) || !isfinite(host_capture[2 * i + 1]) ||
                           fabsf(host_capture[2 * i]) > 0.999f || fabsf(host_capture[2 * i + 1]) > 0.999f;
        }
        for (c = 0; c < 2; c++) {
            double tone_lv = level(host_capture + 2 * (at + 10 * 240) + c, 85 * 240);
            double tail_lv = level(host_capture + 2 * (at + 105 * 240) + c, 45 * 240);
            int ok = bad_samples == 0 && tone_lv > 0.01 && (fx[e].tail_wanted ? tail_lv > 0.001 : tail_lv < 0.001);
            printf("%s %s: level %.4f with the tone, %.4f after it; %u samples not finite or clipped%s\n",
                   fx[e].name, c == 0 ? "L" : "R", tone_lv, tail_lv, bad_samples, ok ? "" : "  FAIL");
            failures += !ok;
        }
    }
    AXFXReverbHiShutdown(&fx_hi);
    AXFXReverbStdShutdown(&fx_std);
    AXFXChorusShutdown(&fx_chorus);
    AXFXDelayShutdown(&fx_delay);
    printf("AXFX hooks: %d allocations, %d frees%s\n", fx_allocs, fx_frees,
           fx_allocs != 0 && fx_allocs == fx_frees ? "" : "  FAIL");
    failures += fx_allocs == 0 || fx_allocs != fx_frees;
    printf("AX frame callbacks %d, command lists run %u, AI periods %u, silent periods %u, "
           "bad command lists %u\n",
           callbacks, frames, ticks, silent, bad);
    // The last command list may still be on its way to the program.
    failures += silent != 0 || bad != 0 || frames + 2 < (u32) callbacks;
    printf("%d failures\n", failures);
    return failures != 0;
}
