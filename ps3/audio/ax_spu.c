// The audio SPU program: the AX DSP microcode of CRC 4e8a8b21 (Melee's) and the AI, after Dolphin's AX
// HLE. Each AI period sends the AI buffer to the audio port, then runs that frame's command list.
#include <string.h>

#include "ax_shared.h"
#include "spu_io.h"

#define MAIL_CMDLIST 0xBABE0000u
#define CMDLIST_WORDS 384
#define BUF_SAMPLES (AX_FRAME_SAMPLES)
#define MS_SAMPLES 32
#define MAX_UPDATES 128
#define CACHE_LINES 32
#define CACHE_LINE 128
// Port fill, in frames: the target absorbs a late PPU; below the floor, silence replaces stale data.
#define TARGET_AHEAD (8 * AX_PORT_BLOCK)
#define SILENCE_FLOOR (2 * AX_PORT_BLOCK)

enum {
    CMD_SETUP = 0x00,
    CMD_DL_AND_VOL_MIX = 0x01,
    CMD_PB_ADDR = 0x02,
    CMD_PROCESS = 0x03,
    CMD_MIX_AUXA = 0x04,
    CMD_MIX_AUXB = 0x05,
    CMD_UPLOAD_LRS = 0x06,
    CMD_SET_LR = 0x07,
    CMD_UNK_08 = 0x08,
    CMD_MIX_AUXB_NOWRITE = 0x09,
    CMD_COMPRESSOR_TABLE_ADDR = 0x0A,
    CMD_UNK_0B = 0x0B,
    CMD_UNK_0C = 0x0C,
    CMD_MORE = 0x0D,
    CMD_OUTPUT = 0x0E,
    CMD_END = 0x0F,
    CMD_MIX_AUXB_LR = 0x10,
    CMD_SET_OPPOSITE_LR = 0x11,
    CMD_UNK_12 = 0x12,
    CMD_SEND_AUX_AND_MIX = 0x13,
};

enum {
    MIX_L = 1 << 0,
    MIX_L_RAMP = 1 << 1,
    MIX_R = 1 << 2,
    MIX_R_RAMP = 1 << 3,
    MIX_S = 1 << 4,
    MIX_S_RAMP = 1 << 5,
    MIX_AUXA_L = 1 << 6,
    MIX_AUXA_L_RAMP = 1 << 7,
    MIX_AUXA_R = 1 << 8,
    MIX_AUXA_R_RAMP = 1 << 9,
    MIX_AUXA_S = 1 << 10,
    MIX_AUXA_S_RAMP = 1 << 11,
    MIX_AUXB_L = 1 << 12,
    MIX_AUXB_L_RAMP = 1 << 13,
    MIX_AUXB_R = 1 << 14,
    MIX_AUXB_R_RAMP = 1 << 15,
    MIX_AUXB_S = 1 << 16,
    MIX_AUXB_S_RAMP = 1 << 17,
};

enum { FORMAT_ADPCM = 0x00, FORMAT_PCM16 = 0x0A, FORMAT_PCM8 = 0x19 };
enum { SRC_POLYPHASE = 0, SRC_LINEAR = 1, SRC_NEAREST = 2 };

// The GameCube AXPB, 0xC0 bytes, with Dolphin's field names.
typedef struct Pb {
    uint16_t next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo;
    uint16_t src_type, coef_select, mixer_control, running, is_stream;
    struct {
        uint16_t left, left_delta, right, right_delta;
        uint16_t auxA_left, auxA_left_delta, auxA_right, auxA_right_delta;
        uint16_t auxB_left, auxB_left_delta, auxB_right, auxB_right_delta;
        uint16_t auxB_surround, auxB_surround_delta, surround, surround_delta;
        uint16_t auxA_surround, auxA_surround_delta;
    } mixer;
    uint16_t itd[7];
    struct {
        uint16_t num_updates[5];
        uint16_t data_hi, data_lo;
    } updates;
    struct {
        int16_t left, auxA_left, auxB_left, right, auxA_right, auxB_right;
        int16_t surround, auxA_surround, auxB_surround;
    } dpop;
    struct {
        uint16_t cur_volume;
        int16_t cur_volume_delta;
    } vol_env;
    uint16_t fir[3];
    struct {
        uint16_t looping, sample_format;
        uint16_t loop_addr_hi, loop_addr_lo, end_addr_hi, end_addr_lo, cur_addr_hi, cur_addr_lo;
    } audio_addr;
    struct {
        int16_t coefs[16];
        uint16_t gain, pred_scale;
        int16_t yn1, yn2;
    } adpcm;
    struct {
        uint16_t ratio_hi, ratio_lo, cur_addr_frac;
        int16_t last_samples[4];
    } src;
    struct {
        uint16_t pred_scale;
        int16_t yn1, yn2;
    } adpcm_loop;
    uint16_t pad[3];
} __attribute__((aligned(16))) Pb;

_Static_assert(sizeof(Pb) == 0xC0, "AXPB is 0xC0 bytes");

#define HILO(name) (((uint32_t) name##_hi << 16) | name##_lo)

static AxShared shared __attribute__((aligned(128)));
static uint64_t shared_ea;

// The DSP's mixing buffers for one 5 ms frame: main, AUX A and AUX B, each left, right, surround.
static int32_t buf[9][BUF_SAMPLES] __attribute__((aligned(16)));
#define MAIN_L buf[0]
#define MAIN_R buf[1]
#define MAIN_S buf[2]
#define AUXA_L buf[3]
#define AUXA_R buf[4]
#define AUXA_S buf[5]
#define AUXB_L buf[6]
#define AUXB_R buf[7]
#define AUXB_S buf[8]

static uint16_t cmdlist[CMDLIST_WORDS] __attribute__((aligned(16)));
static uint8_t cache[CACHE_LINES][CACHE_LINE] __attribute__((aligned(128)));
static uint32_t cache_tag[CACHE_LINES];

static uint32_t ring_frames;
static uint32_t write_pos;
static int16_t ai_last[2];

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// DMA for any alignment: through a window of whole quadwords.
static void get_any(void* dst, uint64_t ea, uint32_t size)
{
    static uint8_t window[512 + 32] __attribute__((aligned(16)));
    uint8_t* out = dst;

    while (size != 0) {
        uint32_t lead = (uint32_t) (ea & 15);
        uint32_t n = size < 512 ? size : 512;
        io_get(window, ea - lead, (lead + n + 15) & ~15u);
        memcpy(out, window + lead, n);
        out += n;
        ea += n;
        size -= n;
    }
}

static void put_stats(void)
{
    io_put(&shared.frames, shared_ea + 64, 16);
}

static uint8_t aram_byte(uint32_t addr)
{
    uint32_t line;
    uint32_t slot;

    // ARAM accesses wrap at 16 MB, as on the GameCube.
    addr &= AX_ARAM_SIZE - 1;
    line = addr / CACHE_LINE;
    slot = line % CACHE_LINES;
    if (cache_tag[slot] != line) {
        io_get(cache[slot], shared.aram_ea + (uint64_t) line * CACHE_LINE, CACHE_LINE);
        cache_tag[slot] = line;
    }
    return cache[slot][addr % CACHE_LINE];
}

// The DSP accelerator: decodes the next sample and handles the loop and end addresses.
static uint32_t acc_loop_addr, acc_end_addr;
static uint32_t* acc_cur_addr;
static Pb* acc_pb;

static int16_t acc_sample(void)
{
    int16_t ret;
    uint32_t step = 0;

    switch (acc_pb->audio_addr.sample_format) {
    case FORMAT_ADPCM: {
        int scale;
        int coef_idx;
        int temp;
        int val;

        if ((*acc_cur_addr & 15) == 0) {
            acc_pb->adpcm.pred_scale = aram_byte((*acc_cur_addr & ~15u) >> 1);
            *acc_cur_addr += 2;
        }
        switch (acc_end_addr & 15) {
        case 0:
            step = 1;
            break;
        case 1:
            step = 0;
            break;
        default:
            step = 2;
            break;
        }
        scale = 1 << (acc_pb->adpcm.pred_scale & 0xF);
        coef_idx = (acc_pb->adpcm.pred_scale >> 4) & 7;
        temp = (*acc_cur_addr & 1) ? (aram_byte(*acc_cur_addr >> 1) & 0xF)
                                   : (aram_byte(*acc_cur_addr >> 1) >> 4);
        if (temp >= 8) {
            temp -= 16;
        }
        val = scale * temp + ((0x400 + acc_pb->adpcm.coefs[coef_idx * 2] * acc_pb->adpcm.yn1 +
                               acc_pb->adpcm.coefs[coef_idx * 2 + 1] * acc_pb->adpcm.yn2) >>
                              11);
        val = clamp(val, -0x7FFF, 0x7FFF);
        acc_pb->adpcm.yn2 = acc_pb->adpcm.yn1;
        acc_pb->adpcm.yn1 = (int16_t) val;
        *acc_cur_addr += 1;
        ret = (int16_t) val;
        break;
    }
    case FORMAT_PCM16:
        ret = (int16_t) ((aram_byte(*acc_cur_addr * 2) << 8) | aram_byte(*acc_cur_addr * 2 + 1));
        acc_pb->adpcm.yn2 = acc_pb->adpcm.yn1;
        acc_pb->adpcm.yn1 = ret;
        step = 2;
        *acc_cur_addr += 1;
        break;
    case FORMAT_PCM8:
        ret = (int16_t) (aram_byte(*acc_cur_addr) << 8);
        acc_pb->adpcm.yn2 = acc_pb->adpcm.yn1;
        acc_pb->adpcm.yn1 = ret;
        step = 2;
        *acc_cur_addr += 1;
        break;
    default:
        return 0;
    }
    // The end address raised an exception that the microcode handled like this.
    if (*acc_cur_addr == acc_end_addr + step - 1) {
        *acc_cur_addr = acc_loop_addr;
        if (acc_pb->audio_addr.looping) {
            acc_pb->adpcm.pred_scale = acc_pb->adpcm_loop.pred_scale;
            // The microcode keeps the history of a one-shot voice over its loop.
            if (!acc_pb->is_stream) {
                acc_pb->adpcm.yn1 = acc_pb->adpcm_loop.yn1;
                acc_pb->adpcm.yn2 = acc_pb->adpcm_loop.yn2;
            }
        } else {
            acc_pb->running = 0;
        }
    }
    return ret;
}

// Linear interpolation, as Dolphin's AX HLE also does for the polyphase type.
static void get_input_samples(Pb* pb, int16_t* out, uint32_t count)
{
    uint32_t cur_addr = HILO(pb->audio_addr.cur_addr);
    uint32_t pos = pb->src.cur_addr_frac;
    uint32_t ratio = HILO(pb->src.ratio);
    int16_t temp[4];
    uint32_t idx = 0;
    uint32_t i;

    acc_pb = pb;
    acc_loop_addr = HILO(pb->audio_addr.loop_addr);
    acc_end_addr = HILO(pb->audio_addr.end_addr);
    acc_cur_addr = &cur_addr;

    if (pb->src_type == SRC_NEAREST) {
        for (i = 0; i < count; i++) {
            out[i] = acc_sample();
        }
        memcpy(pb->src.last_samples, out + count - 4, sizeof(pb->src.last_samples));
    } else {
        for (i = 0; i < 4; i++) {
            temp[idx++ & 3] = pb->src.last_samples[i];
        }
        for (i = 0; i < count; i++) {
            uint16_t frac;

            pos += ratio;
            while (pos >= 0x10000) {
                temp[idx++ & 3] = acc_sample();
                pos -= 0x10000;
            }
            frac = (uint16_t) pos;
            if (frac != 0) {
                int32_t s0 = temp[idx++ & 3];
                int32_t s1 = temp[idx++ & 3];
                out[i] = (int16_t) ((s0 * (uint16_t) -frac + s1 * frac) >> 16);
                idx += 2;
            } else {
                out[i] = temp[idx++ & 3];
                idx += 3;
            }
        }
        for (i = 4; i-- > 0;) {
            pb->src.last_samples[i] = temp[--idx & 3];
        }
        pb->src.cur_addr_frac = (uint16_t) pos;
    }
    pb->audio_addr.cur_addr_hi = (uint16_t) (cur_addr >> 16);
    pb->audio_addr.cur_addr_lo = (uint16_t) cur_addr;
}

static void mix_add(int32_t* out, const int16_t* in, uint32_t count, uint16_t* vol, int16_t* dpop,
                    int ramp)
{
    uint16_t delta = ramp ? vol[1] : 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        int32_t s = clamp((int32_t) (((int64_t) in[i] * vol[0]) >> 15), -32767, 32767);
        out[i] += (int16_t) s;
        vol[0] += delta;
        *dpop = (int16_t) s;
    }
}

// Mixer control bits of this microcode version, as Dolphin maps them.
static uint32_t mixer_control(uint16_t mc)
{
    uint32_t ret = MIX_L | MIX_R;

    if (mc & 1) {
        ret |= MIX_AUXA_L | MIX_AUXA_R;
    }
    if (mc & 2) {
        ret |= MIX_AUXB_L | MIX_AUXB_R;
    }
    if (mc & 4) {
        ret |= MIX_S;
        if (ret & MIX_AUXA_L) {
            ret |= MIX_AUXA_S;
        }
        if (ret & MIX_AUXB_L) {
            ret |= MIX_AUXB_S;
        }
    }
    if (mc & 8) {
        ret |= MIX_L_RAMP | MIX_R_RAMP;
        if (ret & MIX_AUXA_L) {
            ret |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
        }
        if (ret & MIX_AUXB_L) {
            ret |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
        }
        if (ret & MIX_AUXA_S) {
            ret |= MIX_AUXA_S_RAMP;
        }
        if (ret & MIX_AUXB_S) {
            ret |= MIX_AUXB_S_RAMP;
        }
    }
    return ret;
}

// One millisecond of one voice; off is the sample offset in the frame buffers.
static void process_voice(Pb* pb, uint32_t off, uint32_t mc)
{
    int16_t s[MS_SAMPLES];
    uint32_t i;

    if (!pb->running) {
        return;
    }
    get_input_samples(pb, s, MS_SAMPLES);
    for (i = 0; i < MS_SAMPLES; i++) {
        s[i] = (int16_t) clamp((s[i] * (int32_t) pb->vol_env.cur_volume) >> 15, -32767, 32767);
        pb->vol_env.cur_volume += pb->vol_env.cur_volume_delta;
    }
#define MIX(on, ramp, out, vol, last)                                                              \
    if (mc & (on)) {                                                                               \
        mix_add(out + off, s, MS_SAMPLES, &pb->mixer.vol, &pb->dpop.last, (mc & (ramp)) != 0);     \
    }
    MIX(MIX_L, MIX_L_RAMP, MAIN_L, left, left)
    MIX(MIX_R, MIX_R_RAMP, MAIN_R, right, right)
    MIX(MIX_S, MIX_S_RAMP, MAIN_S, surround, surround)
    MIX(MIX_AUXA_L, MIX_AUXA_L_RAMP, AUXA_L, auxA_left, auxA_left)
    MIX(MIX_AUXA_R, MIX_AUXA_R_RAMP, AUXA_R, auxA_right, auxA_right)
    MIX(MIX_AUXA_S, MIX_AUXA_S_RAMP, AUXA_S, auxA_surround, auxA_surround)
    MIX(MIX_AUXB_L, MIX_AUXB_L_RAMP, AUXB_L, auxB_left, auxB_left)
    MIX(MIX_AUXB_R, MIX_AUXB_R_RAMP, AUXB_R, auxB_right, auxB_right)
    MIX(MIX_AUXB_S, MIX_AUXB_S_RAMP, AUXB_S, auxB_surround, auxB_surround)
#undef MIX
}

static void process_pb_list(uint32_t addr)
{
    static Pb pb;
    static uint16_t updates[MAX_UPDATES * 2];

    while (addr != 0) {
        uint32_t mc;
        uint32_t total = 0;
        uint32_t start = 0;
        uint32_t ms;
        uint32_t i;

        io_get(&pb, addr, sizeof(pb));
        for (ms = 0; ms < 5; ms++) {
            total += pb.updates.num_updates[ms];
        }
        if (total > MAX_UPDATES) {
            total = MAX_UPDATES;
        }
        if (total != 0) {
            get_any(updates, HILO(pb.updates.data), total * 4);
        }
        for (ms = 0; ms < 5; ms++) {
            // Updates name a PB word and its new value, for this millisecond.
            for (i = start; i < start + pb.updates.num_updates[ms] && i < total; i++) {
                if (updates[2 * i] < sizeof(pb) / 2) {
                    ((uint16_t*) &pb)[updates[2 * i]] = updates[2 * i + 1];
                }
            }
            start += pb.updates.num_updates[ms];
            mc = mixer_control(pb.mixer_control);
            process_voice(&pb, ms * MS_SAMPLES, mc);
        }
        io_put(&pb, addr, sizeof(pb));
        addr = HILO(pb.next_pb);
    }
}

static void setup(uint32_t addr)
{
    uint16_t init[0x20];
    uint32_t b;

    get_any(init, addr, sizeof(init));
    for (b = 0; b < 9; b++) {
        int32_t val = (int32_t) (((uint32_t) init[3 * b] << 16) | init[3 * b + 1]);
        // A zero start value clears the buffer whatever the delta.
        int32_t delta = val != 0 ? (int16_t) init[3 * b + 2] : 0;
        uint32_t i;
        for (i = 0; i < BUF_SAMPLES; i++) {
            buf[b][i] = val;
            val += delta;
        }
    }
}

// Reads three 160-sample buffers of 32-bit samples from ea and adds them to dst.
static void download_add(int32_t* const dst[3], uint32_t ea)
{
    static int32_t in[BUF_SAMPLES] __attribute__((aligned(16)));
    int b;
    int i;

    for (b = 0; b < 3; b++) {
        if (dst[b] == NULL) {
            continue;
        }
        get_any(in, ea + b * sizeof(in), sizeof(in));
        for (i = 0; i < BUF_SAMPLES; i++) {
            dst[b][i] += in[i];
        }
    }
}

static void upload(int32_t* const src[], int count, uint32_t ea)
{
    int b;

    for (b = 0; b < count; b++) {
        io_put(src[b], ea + b * sizeof(buf[0]), sizeof(buf[0]));
    }
}

static void mix_aux(int aux, uint32_t write_addr, uint32_t read_addr)
{
    int32_t* const auxa[3] = { AUXA_L, AUXA_R, AUXA_S };
    int32_t* const auxb[3] = { AUXB_L, AUXB_R, AUXB_S };
    int32_t* const main[3] = { MAIN_L, MAIN_R, MAIN_S };

    if (write_addr != 0) {
        upload(aux == 0 ? auxa : auxb, 3, write_addr);
    }
    download_add(main, read_addr);
}

static void download_volume(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb)
{
    static int32_t in[BUF_SAMPLES] __attribute__((aligned(16)));
    const uint16_t vols[3] = { vol_main, vol_auxa, vol_auxb };
    int group;

    // Each group reads the same three buffers at addr, as Dolphin's AX HLE does.
    for (group = 0; group < 3; group++) {
        int b;
        for (b = 0; b < 3; b++) {
            int i;
            get_any(in, addr + b * sizeof(in), sizeof(in));
            for (i = 0; i < BUF_SAMPLES; i++) {
                buf[group * 3 + b][i] += (int32_t) (((int64_t) in[i] * vols[group]) >> 15);
            }
        }
    }
}

static void set_lr(uint32_t addr, int opposite)
{
    static int32_t in[BUF_SAMPLES] __attribute__((aligned(16)));
    int i;

    get_any(in, addr, sizeof(in));
    for (i = 0; i < BUF_SAMPLES; i++) {
        MAIN_L[i] = opposite ? -in[i] : in[i];
        MAIN_R[i] = in[i];
        MAIN_S[i] = 0;
    }
}

static void output(uint32_t surround_addr, uint32_t lr_addr)
{
    static int16_t lr[BUF_SAMPLES * 2] __attribute__((aligned(16)));
    int i;

    io_put(MAIN_S, surround_addr, sizeof(buf[0]));
    // The AI takes right, then left.
    for (i = 0; i < BUF_SAMPLES; i++) {
        lr[2 * i] = (int16_t) clamp(MAIN_R[i], -32767, 32767);
        lr[2 * i + 1] = (int16_t) clamp(MAIN_L[i], -32767, 32767);
    }
    io_put(lr, lr_addr, sizeof(lr));
}

static void mix_auxb_lr(uint32_t up_addr, uint32_t down_addr)
{
    int32_t* const auxb[2] = { AUXB_L, AUXB_R };
    static int32_t in[BUF_SAMPLES] __attribute__((aligned(16)));
    int b;
    int i;

    upload(auxb, 2, up_addr);
    for (b = 0; b < 2; b++) {
        get_any(in, down_addr + b * sizeof(in), sizeof(in));
        for (i = 0; i < BUF_SAMPLES; i++) {
            auxb[b][i] = in[i];
            buf[b][i] += in[i];
        }
    }
}

static void send_aux_and_mix(uint32_t auxa_up, uint32_t auxb_s_up, const uint32_t down[4])
{
    int32_t* const auxa[3] = { AUXA_L, AUXA_R, AUXA_S };
    int32_t* const auxb_s[1] = { AUXB_S };
    int32_t* const dst[4] = { MAIN_L, MAIN_R, AUXB_L, AUXB_R };
    int i;

    upload(auxa, 3, auxa_up);
    upload(auxb_s, 1, auxb_s_up);
    for (i = 0; i < 4; i++) {
        int32_t* const one[3] = { dst[i], NULL, NULL };
        download_add(one, down[i]);
    }
}

static uint32_t read_cmdlist(uint32_t addr, uint32_t words)
{
    if (words > CMDLIST_WORDS) {
        words = CMDLIST_WORDS;
    }
    get_any(cmdlist, addr, words * 2);
    return words;
}

#define ARG32(i) (((uint32_t) cmdlist[(i)] << 16) | cmdlist[(i) + 1])

static void run_cmdlist(uint32_t addr, uint32_t words)
{
    uint32_t size = read_cmdlist(addr, words);
    uint32_t i = 0;
    uint32_t pb_addr = 0;
    int line;

    // ARAM may have changed since the last frame.
    for (line = 0; line < CACHE_LINES; line++) {
        cache_tag[line] = 0xFFFFFFFF;
    }
    while (i < size) {
        uint16_t cmd = cmdlist[i++];
        switch (cmd) {
        case CMD_SETUP:
            setup(ARG32(i));
            i += 2;
            break;
        case CMD_DL_AND_VOL_MIX:
            download_volume(ARG32(i), cmdlist[i + 2], cmdlist[i + 3], cmdlist[i + 4]);
            i += 5;
            break;
        case CMD_PB_ADDR:
            pb_addr = ARG32(i);
            i += 2;
            break;
        case CMD_PROCESS:
            process_pb_list(pb_addr);
            break;
        case CMD_MIX_AUXA:
        case CMD_MIX_AUXB:
            mix_aux(cmd - CMD_MIX_AUXA, ARG32(i), ARG32(i + 2));
            i += 4;
            break;
        case CMD_UPLOAD_LRS: {
            int32_t* const lrs[3] = { MAIN_L, MAIN_R, MAIN_S };
            upload(lrs, 3, ARG32(i));
            i += 2;
            break;
        }
        case CMD_SET_LR:
            set_lr(ARG32(i), 0);
            i += 2;
            break;
        case CMD_UNK_08:
            i += 10;
            break;
        case CMD_MIX_AUXB_NOWRITE:
            mix_aux(1, 0, ARG32(i));
            i += 2;
            break;
        case CMD_COMPRESSOR_TABLE_ADDR:
            i += 2;
            break;
        case CMD_UNK_0B:
        case CMD_UNK_0C:
            break;
        case CMD_MORE:
            size = read_cmdlist(ARG32(i), cmdlist[i + 2]);
            i = 0;
            break;
        case CMD_OUTPUT:
            output(ARG32(i), ARG32(i + 2));
            i += 4;
            break;
        case CMD_END:
            shared.frames++;
            return;
        case CMD_MIX_AUXB_LR:
            mix_auxb_lr(ARG32(i), ARG32(i + 2));
            i += 4;
            break;
        case CMD_SET_OPPOSITE_LR:
            set_lr(ARG32(i), 1);
            i += 2;
            break;
        case CMD_UNK_12:
            i += 4;
            break;
        case CMD_SEND_AUX_AND_MIX: {
            const uint32_t down[4] = { ARG32(i + 4), ARG32(i + 6), ARG32(i + 8), ARG32(i + 10) };
            send_aux_and_mix(ARG32(i), ARG32(i + 2), down);
            i += 12;
            break;
        }
        default:
            shared.bad_commands++;
            return;
        }
    }
    shared.bad_commands++;
}

// The ring position of the block the port plays.
static uint32_t port_read_pos(void)
{
    uint64_t index;

    get_any(&index, shared.port_read_ea, sizeof(index));
    return (uint32_t) (index % shared.port_blocks) * AX_PORT_BLOCK;
}

static uint32_t frames_ahead(void)
{
    uint32_t read = port_read_pos();
    uint32_t ahead = (write_pos + ring_frames - read) % ring_frames;

    // Further ahead than the ring allows means the port overtook us; write after its block.
    if (ahead > ring_frames - AX_PORT_BLOCK) {
        write_pos = (read + AX_PORT_BLOCK) % ring_frames;
        ahead = AX_PORT_BLOCK;
    }
    return ahead;
}

static void write_port(const float* frames)
{
    uint32_t first = ring_frames - write_pos;

    if (first > AX_OUT_FRAMES) {
        first = AX_OUT_FRAMES;
    }
    io_put(frames, shared.port_data_ea + (uint64_t) write_pos * 8, first * 8);
    if (first < AX_OUT_FRAMES) {
        io_put(frames + 2 * first, shared.port_data_ea, (AX_OUT_FRAMES - first) * 8);
    }
    write_pos = (write_pos + AX_OUT_FRAMES) % ring_frames;
}

// One AI period: the latched AI buffer, 32 kHz R/L pairs, to the port as 48 kHz L/R floats.
static void ai_tick(int running)
{
    static int16_t in[AX_FRAME_SAMPLES * 2] __attribute__((aligned(16)));
    static float out[AX_OUT_FRAMES * 2] __attribute__((aligned(16)));
    uint32_t m;

    if (running && shared.ai_addr != 0 && shared.ai_length >= sizeof(in)) {
        get_any(in, shared.ai_addr, sizeof(in));
    } else {
        memset(in, 0, sizeof(in));
    }
    // Output frame m sits at input 2m/3, one input frame late, so its right neighbour is known.
    for (m = 0; m < AX_OUT_FRAMES; m++) {
        uint32_t k = 2 * m / 3;
        int32_t f = (int32_t) (2 * m % 3);
        int c;
        for (c = 0; c < 2; c++) {
            int32_t a = k == 0 ? ai_last[c] : in[2 * (k - 1) + 1 - c];
            int32_t b = in[2 * k + 1 - c];
            out[2 * m + c] = (float) (a * 3 + (b - a) * f) * (1.0f / (3.0f * 32768.0f));
        }
    }
    ai_last[0] = in[2 * (AX_FRAME_SAMPLES - 1) + 1];
    ai_last[1] = in[2 * (AX_FRAME_SAMPLES - 1)];
    write_port(out);
    shared.ticks++;
}

static void refresh_control(void)
{
    io_get(&shared, shared_ea, 64);
}

// Returns -1 for the quit mail, 1 after a command list ran, 0 otherwise.
static int handle_mail(void)
{
    uint32_t mail;

    if (io_mail_count() == 0) {
        return 0;
    }
    mail = io_mail_read();
    if (mail == AX_MAIL_QUIT) {
        return -1;
    }
    if ((mail & 0xFFFF0000) == MAIL_CMDLIST) {
        run_cmdlist(io_mail_read(), mail & 0xFFFF);
        put_stats();
        io_event(AX_EV_DSP);
        return 1;
    }
    // Task mails (0xCDD1xxxx) need no answer from this program.
    return 0;
}

// Runs mails until this period's command list ran. Writes silence when the PPU is late.
static int wait_for_cmdlist(void)
{
    for (;;) {
        int r = handle_mail();
        if (r != 0) {
            return r > 0;
        }
        refresh_control();
        if (!shared.ai_running) {
            return 1;
        }
        if (frames_ahead() < SILENCE_FLOOR) {
            ai_tick(0);
            shared.silent_ticks++;
            put_stats();
        }
        io_wait_us(100);
    }
}

int ax_spu_main(uint64_t arg)
{
    static float zero[AX_OUT_FRAMES * 2] __attribute__((aligned(16)));
    uint32_t i;

    shared_ea = arg;
    refresh_control();
    ring_frames = shared.port_blocks * AX_PORT_BLOCK;
    for (i = 0; i < CACHE_LINES; i++) {
        cache_tag[i] = 0xFFFFFFFF;
    }
    for (i = 0; i < ring_frames; i += AX_OUT_FRAMES) {
        io_put(zero, shared.port_data_ea + (uint64_t) i * 8,
               (ring_frames - i < AX_OUT_FRAMES ? ring_frames - i : AX_OUT_FRAMES) * 8);
    }
    // Over the silent ring, start writing one block after the one the port plays.
    write_pos = (port_read_pos() + AX_PORT_BLOCK) % ring_frames;
    io_event(AX_EV_INIT);

    for (;;) {
        int running;

        while (frames_ahead() + AX_OUT_FRAMES > TARGET_AHEAD) {
            if (handle_mail() < 0) {
                return 0;
            }
            io_wait_us(200);
        }
        refresh_control();
        running = shared.ai_running != 0;
        ai_tick(running);
        put_stats();
        if (running) {
            io_event(AX_EV_AI);
            if (!wait_for_cmdlist()) {
                return 0;
            }
        }
    }
}
