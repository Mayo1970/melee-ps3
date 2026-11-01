// Netplay (ps3_netplay.h): pairing by code (LAN broadcast, tools/matchmaker.py), handshake, a session's inputs, time
// sync and desync check. One UDP socket: a thread receives and resends, the main thread sends each frame's inputs.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <melee/gm/forward.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <sysdolphin/baselib/random.h>

#include <ps3_netplay.h>
#include <ps3_options.h>
#include <ps3_rollback.h>

#include "../os/sync.h"
#include "../os/sys.h"
#include "link.h"
#include "rb_shared.h"

#ifndef PS3_NET_SERVER
#define PS3_NET_SERVER ""
#endif

// The consoles' port, also the LAN broadcasts'; the matchmaker's default port.
#define NET_PORT 51000
#define SERVER_PORT 51001
#define NET_VERSION 1
#define HEAD 8
#define FRAMES 128
// Frames whose remote input may be a prediction, as Slippi's rollback window.
#define MAX_AHEAD 7
#define MAX_SEND 32
#define CHECK_EVERY 30
#define CHECKS 32
#define VIEWS 8
#define VIEW_MAX 1024
// Desyncs logged with their gameplay values.
#define DETAILS 4
#define SEARCH_MS 1000
#define HELLO_MS 100
#define KEEP_MS 500
#define RESEND_MS 50
#define PUNCH_TIMEOUT_MS 15000
#define PAIR_TIMEOUT_MS 15000
#define SESSION_TIMEOUT_MS 5000
#define PAIR_WAIT_MS 30000
#define SETUP_WAIT_MS 120000
// The other console's match load may take this much longer than ours.
#define START_WAIT_MS 60000
#define LINGER_MS 3000

enum {
    PK_REG = 1,  // console to matchmaker: code, nonce, LAN address, build
    PK_WAIT,     // matchmaker: nonce; no other console on the code yet
    PK_PAIR,     // matchmaker: nonce, session, role, the other console's public and LAN addresses
    PK_LAN,      // broadcast: code, nonce, search age, build
    PK_HELLO,    // to the peer while connecting: session, nonce, build, role, heard, code
    PK_KEEP,     // paired, between matches: session
    PK_SETUP,    // player 1: session, match, seed, options, start data
    PK_ACK,      // player 2: session, match, options, accepted
    PK_INPUT,    // in a session (see send_input)
    PK_VIEW,     // a desync's gameplay values: session, match, frame, size, bytes
    PK_BYE,      // leaving: session
};

// One frame of a player's pad, as sent. err is not sent: the netplay ports are always connected.
typedef struct NetIn {
    u16 button;
    s8 sx, sy, cx, cy;
    u8 l, r;
} NetIn;

typedef struct Check {
    s32 frame;
    u32 hash;
    bool compared;
} Check;

typedef struct View {
    s32 frame;
    int size;
    u8 data[VIEW_MAX];
} View;

typedef struct Stats {
    u32 rollbacks, rerun_frames, deepest, stalls_sync, stalls_wait, checks, desyncs, pk_in, pk_out;
} Stats;

extern char __executable_start[], etext[];

static PsMutex lock = PS_MUTEX_INIT;
static PsCond wake = PS_COND_INIT;
// The thread runs; a search waits for it to resolve the matchmaker.
static bool running, search_request;

// The menu reads these without the lock.
static volatile int state = PS3_NET_OFF;
static volatile int role;
static const char* volatile status = "";
static volatile unsigned serial = 1;

static char code[8];
static u32 nonce, peer_nonce, session, build;
static u32 search_ms, punch_ms, last_rx_ms, last_tx_ms, last_search_ms, last_hello_ms;
static Ps3NetAddr server, peer, cand[2];
static int ncand;
static bool have_server, heard, acked;

// The handshake. match_no counts the matches of this pairing; a session's packets carry it.
static u32 match_no, seed;
static u8 opts_local, opts_remote;
static bool pending, armed, setup_wanted, setup_acked, setup_refused, setup_got;
static u8 setup_pkt[1024], ack_pkt[32];
static int setup_len, ack_len;
static bool kept_tap[2], kept_cstick[2], kept;

// The session. Frames are the rollback's (rollback.c), from 0 at the match start.
static bool in_session, started, peer_started, lost, lost_logged;
static int delay, local_chan, last_frame;
static NetIn local_in[FRAMES], remote_in[FRAMES], used_in[FRAMES];
static s32 remote_at[FRAMES];
static s32 local_newest, confirmed, peer_ack, checked, peer_frame, last_stall;
static u32 session_ms, peer_frame_ms, rtt_ms, ping_echo, ping_echo_ms, linger_until;
static s16 peer_adv;
static float adv_avg, peer_adv_avg;
static u8 input_pkt[HEAD + 44 + MAX_SEND * 8];
static int input_len;
static u32 run_hash[FRAMES];
static View views[VIEWS];
static Check local_checks[CHECKS], remote_checks[CHECKS];
static s32 final_upto, check_frame;
static u32 check_hash;
static Stats st, st_last;

static u32 now_ms(void)
{
    return (u32) (ps3_sys_timebase() / (ps3_sys_timebase_freq() / 1000));
}

static void wr16(u8* p, u16 v)
{
    p[0] = (u8) (v >> 8);
    p[1] = (u8) v;
}

static void wr32(u8* p, u32 v)
{
    p[0] = (u8) (v >> 24);
    p[1] = (u8) (v >> 16);
    p[2] = (u8) (v >> 8);
    p[3] = (u8) v;
}

static u16 rd16(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}

static u32 rd32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static void head(u8* p, int type)
{
    memcpy(p, "MLNP", 4);
    p[4] = (u8) type;
    p[5] = NET_VERSION;
    p[6] = p[7] = 0;
}

static void set_status(const char* s)
{
    status = s;
    serial++;
}

static void set_state(int s)
{
    state = s;
    serial++;
}

static void send_to(const Ps3NetAddr* to, const u8* p, int len)
{
    ps3_link_send(to, p, len);
    if (to == &peer) {
        last_tx_ms = now_ms();
    }
}

// Same code, same build: the build is a hash of the code, which a different EBOOT changes.
static u32 code_hash(void)
{
    return rb_hash((const u8*) __executable_start, (u32) (etext - __executable_start) & ~15u);
}

static void send_simple(int type)
{
    u8 p[HEAD + 4];

    head(p, type);
    wr32(p + 8, session);
    send_to(&peer, p, sizeof(p));
}

static void send_reg(u32 now)
{
    u8 p[HEAD + 24];
    Ps3NetAddr local = { 0, 0 };

    ps3_link_local(&local);
    head(p, PK_REG);
    memcpy(p + 8, code, 8);
    wr32(p + 16, nonce);
    wr32(p + 20, local.ip);
    wr16(p + 24, local.port);
    wr16(p + 26, 0);
    wr32(p + 28, build);
    ps3_link_send(&server, p, sizeof(p));
}

static void send_lan(u32 now)
{
    static const Ps3NetAddr everyone = { 0xFFFFFFFFu, NET_PORT };
    u8 p[HEAD + 20];

#ifdef __linux__
    // The host test of the matchmaker's path.
    if (getenv("MELEE_HOST_NET_NOLAN") != NULL) {
        return;
    }
#endif
    head(p, PK_LAN);
    memcpy(p + 8, code, 8);
    wr32(p + 16, nonce);
    wr32(p + 20, now - search_ms);
    wr32(p + 24, build);
    ps3_link_send(&everyone, p, sizeof(p));
}

static void send_hello(const Ps3NetAddr* to)
{
    u8 p[HEAD + 24];

    head(p, PK_HELLO);
    wr32(p + 8, session);
    wr32(p + 12, nonce);
    wr32(p + 16, build);
    p[20] = (u8) role;
    p[21] = heard;
    wr16(p + 22, 0);
    memcpy(p + 24, code, 8);
    send_to(to, p, sizeof(p));
}

static void connect_to(int r, u32 s, u32 now)
{
    role = r;
    session = s;
    heard = acked = false;
    punch_ms = now;
    last_hello_ms = 0;
    set_state(PS3_NET_CONNECT);
}

static void unpair(const char* why)
{
    set_state(PS3_NET_OFF);
    set_status(why);
    lost = true;
    setup_wanted = false;
    ps_cond_broadcast(&wake);
}

static void handle_pairing(const Ps3NetAddr* from, const u8* p, int n, u32 now)
{
    int type = p[4];

    if (type == PK_WAIT && n >= HEAD + 4 && state == PS3_NET_SEARCH && rd32(p + 8) == nonce) {
        if (status[0] != 'W') {
            set_status("WAITING FOR THE OTHER CONSOLE");
        }
    } else if (type == PK_PAIR && n >= HEAD + 28 && state == PS3_NET_SEARCH && rd32(p + 8) == nonce) {
        ncand = 0;
        cand[ncand].ip = rd32(p + 20);
        cand[ncand++].port = rd16(p + 24);
        // The other console's LAN address answers when both are behind the same router.
        if (rd32(p + 28) != 0 && (rd32(p + 28) != cand[0].ip || rd16(p + 32) != cand[0].port)) {
            cand[ncand].ip = rd32(p + 28);
            cand[ncand++].port = rd16(p + 32);
        }
        OSReport("netplay: the matchmaker pairs this console as player %d (session %08x)\n", p[16], rd32(p + 12));
        connect_to(p[16], rd32(p + 12), now);
        set_status("CONNECTING");
    } else if (type == PK_LAN && n >= HEAD + 20 && state == PS3_NET_SEARCH && memcmp(p + 8, code, 8) == 0 &&
               rd32(p + 16) != nonce)
    {
        if (rd32(p + 24) != build) {
            set_status("THE OTHER CONSOLE HAS ANOTHER BUILD");
            return;
        }
        // The console that searched longer is player 1; the HELLOs settle a tie.
        peer_nonce = rd32(p + 16);
        ncand = 1;
        cand[0] = *from;
        OSReport("netplay: found on the LAN at %08x:%u\n", from->ip, from->port);
        connect_to(now - search_ms >= rd32(p + 20) ? 1 : 2, nonce ^ peer_nonce, now);
        set_status("CONNECTING (LAN)");
    } else if (type == PK_HELLO && n >= HEAD + 24) {
        u32 s = rd32(p + 8);
        int their = p[20];

        if (state == PS3_NET_SEARCH && memcmp(p + 24, code, 8) == 0 && rd32(p + 12) != nonce) {
            // Its broadcast or the matchmaker's answer did not reach us; its HELLO did.
            ncand = 1;
            cand[0] = *from;
            connect_to(their == 1 ? 2 : 1, s, now);
        }
        if ((state != PS3_NET_CONNECT && state != PS3_NET_PAIRED) || s != session) {
            return;
        }
        if (rd32(p + 16) != build) {
            send_simple(PK_BYE);
            unpair("THE OTHER CONSOLE HAS ANOTHER BUILD");
            return;
        }
        if (their == role) {
            role = nonce < rd32(p + 12) ? 1 : 2;
        }
        peer = *from;
        last_rx_ms = now;
        heard = true;
        acked |= p[21] != 0;
        if (state == PS3_NET_CONNECT && acked) {
            set_state(PS3_NET_PAIRED);
            set_status(role == 1 ? "PAIRED: SET UP A VS MATCH" : "PAIRED: START ANY VS MATCH");
            OSReport("netplay: paired as player %d with %08x:%u\n", role, peer.ip, peer.port);
        }
        // Each HELLO gets an answer: the other console stops only once ours says we heard it.
        send_hello(from);
    }
}

static void store_check(Check* ring, s32 frame, u32 hash)
{
    Check* c = &ring[(frame / CHECK_EVERY) % CHECKS];

    if (c->frame != frame) {
        c->frame = frame;
        c->hash = hash;
        c->compared = false;
    }
}

static void handle_input(const u8* p, int n, u32 now)
{
    s32 first = (s32) rd32(p + 24), frame = (s32) rd32(p + 16), ack = (s32) rd32(p + 20);
    int count = p[28], i;

    if (n < HEAD + 44 + count * 8) {
        return;
    }
    st.pk_in++;
    peer_started = true;
    for (i = 0; i < count; i++) {
        s32 f = first + i;
        const u8* q = p + HEAD + 44 + i * 8;

        if (f > confirmed && f < confirmed + FRAMES / 2 && remote_at[f % FRAMES] != f) {
            NetIn* in = &remote_in[f % FRAMES];

            in->button = rd16(q);
            in->sx = (s8) q[2];
            in->sy = (s8) q[3];
            in->cx = (s8) q[4];
            in->cy = (s8) q[5];
            in->l = q[6];
            in->r = q[7];
            remote_at[f % FRAMES] = f;
        }
    }
    while (remote_at[(confirmed + 1) % FRAMES] == confirmed + 1) {
        confirmed++;
    }
    if (ack > peer_ack) {
        peer_ack = ack;
    }
    if (frame >= peer_frame) {
        peer_frame = frame;
        peer_frame_ms = now;
        peer_adv = (s16) rd16(p + 30);
    }
    if (rd32(p + 36) != 0) {
        u32 r = now - rd32(p + 36) - rd32(p + 40);

        if (r < 2000) {
            rtt_ms = rtt_ms == 0 ? r : (rtt_ms * 7 + r) / 8;
        }
    }
    ping_echo = rd32(p + 32);
    ping_echo_ms = now;
    if ((s32) rd32(p + 44) >= 0) {
        store_check(remote_checks, (s32) rd32(p + 44), rd32(p + 48));
    }
}

static void handle(const Ps3NetAddr* from, const u8* p, int n, u32 now)
{
    int type = p[4];

    if (type <= PK_HELLO) {
        handle_pairing(from, p, n, now);
        return;
    }
    if (n < HEAD + 4 || rd32(p + 8) != session || state != PS3_NET_PAIRED) {
        return;
    }
    last_rx_ms = now;
    switch (type) {
    case PK_SETUP:
        if (role == 2 && n >= HEAD + 16 && n <= (int) sizeof(setup_pkt) && rd32(p + 12) > match_no) {
            memcpy(setup_pkt, p, (size_t) n);
            setup_len = n;
            setup_got = true;
        } else if (role == 2 && ack_len != 0 && rd32(p + 12) == match_no) {
            // Our answer was lost.
            ps3_link_send(&peer, ack_pkt, ack_len);
        }
        break;
    case PK_ACK:
        if (role == 1 && n >= HEAD + 10 && setup_wanted && rd32(p + 12) == match_no) {
            opts_remote = p[16];
            setup_refused = p[17] == 0;
            setup_acked = true;
        }
        break;
    case PK_INPUT:
        if (n >= HEAD + 44 && rd32(p + 12) == match_no && (pending || armed || in_session)) {
            handle_input(p, n, now);
        }
        break;
    case PK_VIEW:
        if (n >= HEAD + 16 && rd32(p + 12) == match_no && rd16(p + 20) <= n - HEAD - 16) {
            ps3_rb_log_view("the other console", (s32) rd32(p + 16), p + HEAD + 16, rd16(p + 20));
        }
        break;
    case PK_BYE:
        OSReport("netplay: the other console left\n");
        unpair("THE OTHER CONSOLE LEFT");
        break;
    }
    ps_cond_broadcast(&wake);
}

// The thread's resends, by state.
static void periodic(u32 now)
{
    int i;

    switch (state) {
    case PS3_NET_SEARCH:
        if (!search_request && now - last_search_ms >= SEARCH_MS) {
            last_search_ms = now;
            if (have_server) {
                send_reg(now);
            }
            send_lan(now);
        }
        break;
    case PS3_NET_CONNECT:
        if (now - punch_ms > PUNCH_TIMEOUT_MS) {
            OSReport("netplay: no answer from the other console in %u s\n", PUNCH_TIMEOUT_MS / 1000);
            unpair("CANNOT REACH THE OTHER CONSOLE");
        } else if (now - last_hello_ms >= HELLO_MS) {
            last_hello_ms = now;
            for (i = 0; i < ncand; i++) {
                send_hello(&cand[i]);
            }
            if (heard) {
                send_hello(&peer);
            }
        }
        break;
    case PS3_NET_PAIRED:
        if (now - last_rx_ms > (in_session ? SESSION_TIMEOUT_MS : PAIR_TIMEOUT_MS)) {
            if (in_session) {
                // The main thread notices in ps3_net_tick and logs it.
                lost = true;
                break;
            }
            OSReport("netplay: nothing from the other console for %u s\n", PAIR_TIMEOUT_MS / 1000);
            unpair("THE OTHER CONSOLE IS GONE");
            break;
        }
        if (setup_wanted && !setup_acked && now - last_tx_ms >= HELLO_MS) {
            send_to(&peer, setup_pkt, setup_len);
        } else if ((in_session || (s32) (linger_until - now) > 0) && input_len != 0) {
            // While the main thread blocks, and after the session, for the other console's last frames.
            if (now - last_tx_ms >= RESEND_MS) {
                send_to(&peer, input_pkt, input_len);
            }
        } else if (now - last_tx_ms >= KEEP_MS) {
            send_simple(PK_KEEP);
        }
        break;
    }
}

static const char* server_name(void)
{
#ifdef __linux__
    if (getenv("MELEE_HOST_NET_SERVER") != NULL) {
        return getenv("MELEE_HOST_NET_SERVER");
    }
#endif
    return ps3_options.netplay_server[0] != '\0' ? ps3_options.netplay_server : PS3_NET_SERVER;
}

// The search the menu asked for; the name lookup runs outside the lock, as it may take seconds.
static void begin_search(void)
{
    const char* name = server_name();
    Ps3NetAddr addr = { 0, 0 };
    bool have = false;
    char buf[80];

    if (name[0] != '\0') {
        // A name without a port takes the matchmaker's default one.
        snprintf(buf, sizeof(buf), strchr(name, ':') != NULL ? "%s" : "%s:%d", name, SERVER_PORT);
        have = ps3_link_resolve(buf, &addr);
    }
    ps_mutex_lock(&lock);
    if (state == PS3_NET_SEARCH) {
        server = addr;
        have_server = have;
        nonce = (u32) ps3_sys_timebase() * 2654435761u ^ (u32) (ps3_sys_timebase() >> 32);
        search_ms = now_ms();
        last_search_ms = 0;
        match_no = 0;
        set_status(name[0] == '\0' ? "NO MATCHMAKER SET: LAN ONLY"
                   : have           ? "SEARCHING"
                                    : "MATCHMAKER NOT FOUND: LAN ONLY");
        OSReport("netplay: searching for code %.8s; matchmaker %s%s, build %08x\n", code,
                 name[0] != '\0' ? name : "none", name[0] != '\0' && !have ? " (does not resolve)" : "", build);
    }
    ps_mutex_unlock(&lock);
}

static void net_main(void* arg)
{
    static u8 buf[1500];
    Ps3NetAddr from;
    bool search;
    int n;

    if (!ps3_link_open(NET_PORT)) {
        OSReport("netplay: cannot open UDP port %u\n", NET_PORT);
        ps_mutex_lock(&lock);
        running = false;
        unpair("NETWORK ERROR");
        ps_mutex_unlock(&lock);
        return;
    }
    build = code_hash();
    for (;;) {
        ps_mutex_lock(&lock);
        search = search_request;
        search_request = false;
        ps_mutex_unlock(&lock);
        if (search) {
            begin_search();
        }
        n = ps3_link_recv(&from, buf, sizeof(buf), 10000);
        ps_mutex_lock(&lock);
        if (n >= HEAD && memcmp(buf, "MLNP", 4) == 0 && buf[5] == NET_VERSION) {
            handle(&from, buf, n, now_ms());
        }
        periodic(now_ms());
        ps_mutex_unlock(&lock);
    }
}

// The menu calls it from the pad alarm: the thread does the work.
void ps3_net_search(const char digits[8])
{
    ps_mutex_lock(&lock);
    if (state == PS3_NET_PAIRED || state == PS3_NET_CONNECT) {
        send_simple(PK_BYE);
    }
    memcpy(code, digits, 8);
    search_request = true;
    set_state(PS3_NET_SEARCH);
    set_status("STARTING");
    if (!running) {
        running = true;
        ps_thread_start(net_main, NULL, "netplay");
    }
    ps_mutex_unlock(&lock);
}

void ps3_net_cancel(void)
{
    ps_mutex_lock(&lock);
    if (state == PS3_NET_PAIRED || state == PS3_NET_CONNECT) {
        send_simple(PK_BYE);
    }
    search_request = false;
    unpair("");
    ps_mutex_unlock(&lock);
}

int ps3_net_state(int* player, const char** why)
{
    *player = role;
    *why = status;
    return state;
}

unsigned ps3_net_serial(void)
{
    return serial;
}

// Waits on the thread's packets for done(); false on a timeout, a lost pairing, or B on a pad (cancel).
static bool wait_for(bool (*done)(void), u32 timeout_ms, bool paired)
{
    u32 t0 = now_ms();

    while (!done()) {
        if ((paired && state != PS3_NET_PAIRED) || state == PS3_NET_OFF || now_ms() - t0 > timeout_ms ||
            (ps3_pad_buttons() & PAD_BUTTON_B) != 0)
        {
            return false;
        }
        ps_cond_timedwait(&wake, &lock, 20000);
    }
    return true;
}

static bool is_paired(void)
{
    return state == PS3_NET_PAIRED;
}

static bool is_acked(void)
{
    return setup_acked;
}

static bool is_setup(void)
{
    return setup_got;
}

// The rules up to their callbacks; the callbacks only compare, as both consoles run the same EBOOT.
#define RULES_DATA offsetof(StartMeleeRules, on_unpause_override)
#define RULES_FUNCS 7
#define PLAYERS_DATA (sizeof(PlayerInitData) * GM_MAX_PLAYERS)
#define SETUP_SIZE (HEAD + 16 + RULES_DATA + RULES_FUNCS * 8 + PLAYERS_DATA)

static void rules_funcs(const StartMeleeRules* r, u64 out[RULES_FUNCS])
{
    out[0] = (u64) (uintptr_t) r->on_unpause_override;
    out[1] = (u64) (uintptr_t) r->on_pause_override;
    out[2] = (u64) (uintptr_t) r->check_for_pauser_override;
    out[3] = (u64) (uintptr_t) r->on_match_start;
    out[4] = (u64) (uintptr_t) r->on_frame_start;
    out[5] = (u64) (uintptr_t) r->on_frame_end;
    out[6] = (u64) (uintptr_t) r->on_match_end;
}

static void reset_inputs(void)
{
    int i;

    memset(local_in, 0, sizeof(local_in));
    memset(used_in, 0, sizeof(used_in));
    for (i = 0; i < FRAMES; i++) {
        remote_at[i] = -1;
    }
    for (i = 0; i < CHECKS; i++) {
        local_checks[i].frame = remote_checks[i].frame = -1;
    }
    for (i = 0; i < VIEWS; i++) {
        views[i].frame = -1;
    }
    confirmed = peer_ack = checked = peer_frame = final_upto = check_frame = -1;
    peer_started = lost = lost_logged = false;
    peer_adv = 0;
    adv_avg = peer_adv_avg = 0;
    rtt_ms = ping_echo = 0;
    input_len = 0;
}

static u8 local_opts(void)
{
    local_chan = ps3_pad_first_channel();
    if (local_chan < 0) {
        local_chan = 0;
    }
    return (u8) (ps3_options.tap_jump[local_chan] | ps3_options.cstick_tilts[local_chan] << 1);
}

static bool handshake_p1(StartMeleeData* start)
{
    u64 funcs[RULES_FUNCS];
    u8* p = setup_pkt;
    int i;

    match_no++;
    seed = (u32) ps3_sys_timebase() ^ *HSD_RandSeedPtr;
    start->players[0].slot_type = Gm_PKind_Human;
    start->players[1].slot_type = Gm_PKind_Human;
    head(p, PK_SETUP);
    wr32(p + 8, session);
    wr32(p + 12, match_no);
    wr32(p + 16, seed);
    p[20] = opts_local;
    p[21] = p[22] = p[23] = 0;
    memcpy(p + HEAD + 16, &start->rules, RULES_DATA);
    rules_funcs(&start->rules, funcs);
    for (i = 0; i < RULES_FUNCS; i++) {
        wr32(p + HEAD + 16 + RULES_DATA + i * 8, (u32) (funcs[i] >> 32));
        wr32(p + HEAD + 16 + RULES_DATA + i * 8 + 4, (u32) funcs[i]);
    }
    memcpy(p + HEAD + 16 + RULES_DATA + RULES_FUNCS * 8, start->players, PLAYERS_DATA);
    setup_len = SETUP_SIZE;
    setup_acked = setup_refused = false;
    setup_wanted = true;
    send_to(&peer, setup_pkt, setup_len);
    OSReport("netplay: match %u set up (stage %d, fighters %d and %d), waiting for player 2\n", match_no,
             start->rules.stkind, start->players[0].ckind, start->players[1].ckind);
    if (!wait_for(is_acked, SETUP_WAIT_MS, true)) {
        setup_wanted = false;
        OSReport("netplay: player 2 did not start a VS match; this one is local\n");
        set_status("PLAYER 2 DID NOT START: LOCAL MATCH");
        return false;
    }
    setup_wanted = false;
    if (setup_refused) {
        OSReport("netplay: player 2 is in another VS mode; this match is local\n");
        set_status("START THE SAME VS MODE");
        return false;
    }
    return true;
}

static bool handshake_p2(StartMeleeData* start)
{
    const u8* p = setup_pkt;
    u64 ours[RULES_FUNCS];
    bool same = true;
    int i;

    setup_got = false;
    OSReport("netplay: waiting for player 1's match\n");
    if (!wait_for(is_setup, SETUP_WAIT_MS, true)) {
        OSReport("netplay: player 1 did not start a VS match; this one is local\n");
        set_status("PLAYER 1 DID NOT START: LOCAL MATCH");
        return false;
    }
    setup_got = false;
    if (setup_len < (int) SETUP_SIZE) {
        return false;
    }
    rules_funcs(&start->rules, ours);
    for (i = 0; i < RULES_FUNCS; i++) {
        u64 theirs = (u64) rd32(p + HEAD + 16 + RULES_DATA + i * 8) << 32 | rd32(p + HEAD + 16 + RULES_DATA + i * 8 + 4);

        same &= theirs == ours[i];
    }
    match_no = rd32(p + 12);
    head(ack_pkt, PK_ACK);
    wr32(ack_pkt + 8, session);
    wr32(ack_pkt + 12, match_no);
    ack_pkt[16] = opts_local;
    ack_pkt[17] = same;
    ack_len = HEAD + 10;
    send_to(&peer, ack_pkt, ack_len);
    if (!same) {
        OSReport("netplay: player 1 is in another VS mode; this match is local\n");
        set_status("START THE SAME VS MODE");
        return false;
    }
    seed = rd32(p + 16);
    opts_remote = p[20];
    memcpy(&start->rules, p + HEAD + 16, RULES_DATA);
    memcpy(start->players, p + HEAD + 16 + RULES_DATA + RULES_FUNCS * 8, PLAYERS_DATA);
    OSReport("netplay: match %u from player 1 (stage %d, fighters %d and %d)\n", match_no, start->rules.stkind,
             start->players[0].ckind, start->players[1].ckind);
    return true;
}

bool ps3_net_handshake(StartMeleeData* start, bool* taken)
{
    bool ok;

    *taken = false;
    ps_mutex_lock(&lock);
    if (state == PS3_NET_SEARCH || state == PS3_NET_CONNECT) {
        OSReport("netplay: a VS match starts while pairing; waiting up to %u s\n", PAIR_WAIT_MS / 1000);
        wait_for(is_paired, PAIR_WAIT_MS, false);
    }
    if (state != PS3_NET_PAIRED) {
        ps_mutex_unlock(&lock);
        return false;
    }
    pending = armed = false;
    ack_len = 0;
    opts_local = local_opts();
    ok = role == 1 ? handshake_p1(start) : handshake_p2(start);
    if (ok) {
        reset_inputs();
        pending = true;
        *taken = role == 2;
        set_status(role == 1 ? "NETPLAY MATCH: YOU ARE PORT 1" : "NETPLAY MATCH: YOU ARE PORT 2");
    }
    ps_mutex_unlock(&lock);
    return ok;
}

void ps3_net_match_begin(void)
{
    int me, other;

    ps_mutex_lock(&lock);
    if (pending) {
        pending = false;
        armed = true;
        *HSD_RandSeedPtr = seed;
        me = role - 1;
        other = 1 - me;
        // The options are per port: each player keeps their own on the port they play.
        memcpy(kept_tap, ps3_options.tap_jump, sizeof(kept_tap));
        memcpy(kept_cstick, ps3_options.cstick_tilts, sizeof(kept_cstick));
        kept = true;
        ps3_options.tap_jump[me] = opts_local & 1;
        ps3_options.cstick_tilts[me] = (opts_local >> 1) & 1;
        ps3_options.tap_jump[other] = opts_remote & 1;
        ps3_options.cstick_tilts[other] = (opts_remote >> 1) & 1;
        OSReport("netplay: match %u starts, seed %08x, this console is port %d (pad channel %d)\n", match_no, seed,
                 role, local_chan);
    }
    ps_mutex_unlock(&lock);
}

bool ps3_net_session(void)
{
    return armed;
}

void ps3_net_session_begin(void)
{
    int f;

    ps_mutex_lock(&lock);
    armed = false;
    in_session = true;
    started = false;
#ifdef __linux__
    delay = getenv("MELEE_HOST_NET_DELAY") != NULL ? atoi(getenv("MELEE_HOST_NET_DELAY")) : ps3_options.netplay_delay;
#else
    delay = ps3_options.netplay_delay;
#endif
    delay = delay < 0 ? 0 : delay > MAX_AHEAD ? MAX_AHEAD : delay;
    // The first frames have no local input from a tick: neutral ones, sent as any other.
    for (f = 0; f < delay; f++) {
        memset(&local_in[f], 0, sizeof(local_in[f]));
    }
    local_newest = delay - 1;
    last_stall = 0;
    session_ms = now_ms();
    memset(&st, 0, sizeof(st));
    memset(&st_last, 0, sizeof(st_last));
    OSReport("netplay: session of match %u, input delay %d, player %d\n", match_no, delay, role);
    ps_mutex_unlock(&lock);
}

static void send_input(s32 frame);

void ps3_net_session_end(int frame)
{
    ps_mutex_lock(&lock);
    if (in_session) {
        in_session = false;
        linger_until = now_ms() + LINGER_MS;
        send_input(frame);
        OSReport("netplay: session end at frame %d: %u rollbacks (%u frames run again, deepest %u), %u stalls for "
                 "the time sync, %u waits for the other console, %u desyncs in %u checks\n",
                 frame, st.rollbacks, st.rerun_frames, st.deepest, st.stalls_sync, st.stalls_wait, st.desyncs,
                 st.checks);
    }
    armed = pending = false;
    if (kept) {
        kept = false;
        memcpy(ps3_options.tap_jump, kept_tap, sizeof(kept_tap));
        memcpy(ps3_options.cstick_tilts, kept_cstick, sizeof(kept_cstick));
    }
    if (state == PS3_NET_PAIRED) {
        set_status(role == 1 ? "PAIRED: SET UP A VS MATCH" : "PAIRED: START ANY VS MATCH");
    }
    ps_mutex_unlock(&lock);
}

// The local inputs the other console has not acknowledged, at most MAX_SEND, with the newest check.
static void send_input(s32 frame)
{
    u8* p = input_pkt;
    u32 now = now_ms();
    s32 first = peer_ack + 1, f;
    int count, i;

    if (state != PS3_NET_PAIRED) {
        return;
    }
    if (first < local_newest - MAX_SEND + 1) {
        first = local_newest - MAX_SEND + 1;
    }
    if (first < 0) {
        first = 0;
    }
    count = local_newest >= first ? local_newest - first + 1 : 0;
    head(p, PK_INPUT);
    wr32(p + 8, session);
    wr32(p + 12, match_no);
    wr32(p + 16, (u32) frame);
    wr32(p + 20, (u32) confirmed);
    wr32(p + 24, (u32) first);
    p[28] = (u8) count;
    p[29] = 0;
    wr16(p + 30, (u16) (s16) (adv_avg * 8));
    // A 0 echo means none yet.
    wr32(p + 32, now | 1);
    wr32(p + 36, ping_echo);
    wr32(p + 40, now - ping_echo_ms);
    wr32(p + 44, (u32) check_frame);
    wr32(p + 48, check_hash);
    for (i = 0, f = first; i < count; i++, f++) {
        const NetIn* in = &local_in[f % FRAMES];
        u8* q = p + HEAD + 44 + i * 8;

        wr16(q, in->button);
        q[2] = (u8) in->sx;
        q[3] = (u8) in->sy;
        q[4] = (u8) in->cx;
        q[5] = (u8) in->cy;
        q[6] = in->l;
        q[7] = in->r;
    }
    input_len = HEAD + 44 + count * 8;
    send_to(&peer, input_pkt, input_len);
    st.pk_out++;
}

static void log_desync(const Check* remote, const Check* local)
{
    const View* v = &views[(local->frame / CHECK_EVERY) % VIEWS];
    u8 p[HEAD + 16 + VIEW_MAX];

    // Once apart, every later check differs too: the heartbeat counts them.
    if (st.desyncs++ >= DETAILS) {
        return;
    }
    OSReport("netplay: desync at frame %d: this console %08x, the other %08x\n", local->frame, local->hash,
             remote->hash);
    if (v->frame != local->frame) {
        return;
    }
    ps3_rb_log_view("this console", v->frame, v->data, v->size);
    head(p, PK_VIEW);
    wr32(p + 8, session);
    wr32(p + 12, match_no);
    wr32(p + 16, (u32) v->frame);
    wr16(p + 20, (u16) v->size);
    wr16(p + 22, 0);
    memcpy(p + HEAD + 16, v->data, (size_t) v->size);
    send_to(&peer, p, HEAD + 16 + v->size);
}

// Frames up to upto ran with their real inputs: their checks are final, and compared with the other console's.
static void finalize(s32 upto)
{
    s32 k;
    int i;

    for (k = final_upto < 0 ? 0 : (final_upto / CHECK_EVERY + 1) * CHECK_EVERY; k <= upto; k += CHECK_EVERY) {
        store_check(local_checks, k, run_hash[k % FRAMES]);
        check_frame = k;
        check_hash = run_hash[k % FRAMES];
    }
    if (upto > final_upto) {
        final_upto = upto;
    }
    for (i = 0; i < CHECKS; i++) {
        Check* r = &remote_checks[i];
        Check* l = &local_checks[i];

        if (r->frame >= 0 && !r->compared && r->frame <= final_upto && l->frame == r->frame) {
            r->compared = true;
            st.checks++;
            if (r->hash != l->hash) {
                log_desync(r, l);
            }
        }
    }
}

// Frames from checked + 1 to upto: the first whose run used another remote input than the real one.
static s32 first_wrong(s32 upto, s32 none)
{
    s32 k;

    for (k = checked + 1; k <= upto; k++) {
        if (memcmp(&used_in[k % FRAMES], &remote_in[k % FRAMES], sizeof(NetIn)) != 0) {
            return k;
        }
    }
    return none;
}

static void note_rollback(s32 from, s32 to)
{
    u32 depth = (u32) (to - from);

    st.rollbacks++;
    st.rerun_frames += depth;
    st.deepest = depth > st.deepest ? depth : st.deepest;
}

void ps3_net_tick(int frame, Ps3NetTick* t)
{
    u32 now;
    s32 upto;

    ps_mutex_lock(&lock);
    now = now_ms();
    last_frame = frame;
    if (!lost && (state != PS3_NET_PAIRED || now - last_rx_ms > SESSION_TIMEOUT_MS ||
                  (!started && !peer_started && now - session_ms > START_WAIT_MS)))
    {
        lost = true;
    }
    if (lost && !lost_logged) {
        OSReport("netplay: lost the other console at frame %d; its port stays idle\n", frame);
        lost_logged = true;
    }
    t->confirmed = confirmed;
    upto = confirmed < frame - 1 ? confirmed : frame - 1;
    t->rollback_from = first_wrong(upto, frame);
    if (t->rollback_from < frame) {
        note_rollback(t->rollback_from, frame);
    }
    if (upto > checked) {
        checked = upto;
    }
    finalize(t->rollback_from - 1 < checked ? t->rollback_from - 1 : checked);
    t->stall = false;
    if (!lost) {
        // Both consoles start frame 0 together: the first waits for the other's packets.
        started |= peer_started;
        if (started) {
            // Where the other console is now: its last frame, the time since, and half the round trip.
            float est = (float) peer_frame + (float) (now - peer_frame_ms + rtt_ms / 2) * 0.06f;

            adv_avg += ((float) frame - est - adv_avg) / 16;
            peer_adv_avg += ((float) peer_adv / 8 - peer_adv_avg) / 16;
        }
        if (!started) {
            t->stall = true;
        } else if (frame - confirmed > MAX_AHEAD) {
            t->stall = true;
            st.stalls_wait++;
        } else if ((adv_avg - peer_adv_avg) / 2 > 1.0f && frame - last_stall > 30) {
            t->stall = true;
            last_stall = frame;
            st.stalls_sync++;
        }
    }
    if (t->stall) {
        send_input(frame);
    }
    ps_mutex_unlock(&lock);
}

bool ps3_net_wait(int frame, Ps3NetTick* t)
{
    u32 t0;
    bool ok = true;

    ps_mutex_lock(&lock);
    t0 = now_ms();
    while (confirmed < frame && !lost) {
        if (state != PS3_NET_PAIRED || now_ms() - last_rx_ms > SESSION_TIMEOUT_MS || now_ms() - t0 > 2 * SESSION_TIMEOUT_MS) {
            lost = true;
            break;
        }
        ps_cond_timedwait(&wake, &lock, 20000);
    }
    if (lost) {
        OSReport("netplay: lost the other console while frame %d waited for its input\n", frame);
        ok = false;
    }
    t->stall = false;
    t->confirmed = confirmed;
    t->rollback_from = ok ? first_wrong(frame, frame + 1) : frame + 1;
    if (t->rollback_from <= frame) {
        note_rollback(t->rollback_from, frame + 1);
    }
    if (ok && frame > checked) {
        checked = frame;
    }
    ps_mutex_unlock(&lock);
    return ok;
}

static void to_net(NetIn* in, const PADStatus* s)
{
    in->button = s->button;
    in->sx = s->stickX;
    in->sy = s->stickY;
    in->cx = s->substickX;
    in->cy = s->substickY;
    in->l = s->triggerLeft;
    in->r = s->triggerRight;
}

static void to_pad(PADStatus* s, const NetIn* in)
{
    memset(s, 0, sizeof(*s));
    s->button = in->button;
    s->stickX = in->sx;
    s->stickY = in->sy;
    s->substickX = in->cx;
    s->substickY = in->cy;
    s->triggerLeft = in->l;
    s->triggerRight = in->r;
    s->err = PAD_ERR_NONE;
}

void ps3_net_frame_inputs(int frame, bool sample, PADStatus stat[PAD_MAX_CONTROLLERS])
{
    static const NetIn none;
    NetIn remote;
    int me = role - 1, c;

    ps_mutex_lock(&lock);
    if (sample) {
        NetIn in = none;
        s32 f = frame + delay;

        if (stat[local_chan].err == PAD_ERR_NONE) {
            to_net(&in, &stat[local_chan]);
        }
        // One sample per first run; a skipped tick repeats the last input.
        while (local_newest < f) {
            local_newest++;
            local_in[local_newest % FRAMES] = local_newest == f ? in : local_in[(local_newest + FRAMES - 1) % FRAMES];
        }
        send_input(frame);
    }
    if (frame <= confirmed) {
        remote = remote_in[frame % FRAMES];
    } else if (lost || confirmed < 0) {
        remote = none;
    } else {
        // The prediction: the other player holds what they held last.
        remote = remote_in[confirmed % FRAMES];
    }
    used_in[frame % FRAMES] = remote;
    for (c = 0; c < PAD_MAX_CONTROLLERS; c++) {
        memset(&stat[c], 0, sizeof(stat[c]));
        stat[c].err = PAD_ERR_NO_CONTROLLER;
    }
    to_pad(&stat[me], &local_in[frame % FRAMES]);
    to_pad(&stat[1 - me], &remote);
    ps_mutex_unlock(&lock);
}

void ps3_net_frame_done(int frame, unsigned hash, const void* view, int size)
{
    ps_mutex_lock(&lock);
    run_hash[frame % FRAMES] = hash;
    if (frame % CHECK_EVERY == 0 && size <= VIEW_MAX) {
        View* v = &views[(frame / CHECK_EVERY) % VIEWS];

        v->frame = frame;
        v->size = size;
        memcpy(v->data, view, (size_t) size);
    }
    ps_mutex_unlock(&lock);
}

int ps3_net_motor_channel(int chan)
{
    if (!in_session && !armed) {
        return chan;
    }
    return chan == role - 1 ? local_chan : -1;
}

void ps3_net_log(unsigned n)
{
    static const char* const names[] = { "off", "searching", "connecting", "paired" };
    Stats d;

    if (state == PS3_NET_OFF && !in_session) {
        return;
    }
    ps_mutex_lock(&lock);
    d.rollbacks = st.rollbacks - st_last.rollbacks;
    d.rerun_frames = st.rerun_frames - st_last.rerun_frames;
    d.stalls_sync = st.stalls_sync - st_last.stalls_sync;
    d.stalls_wait = st.stalls_wait - st_last.stalls_wait;
    d.checks = st.checks - st_last.checks;
    d.desyncs = st.desyncs - st_last.desyncs;
    d.pk_in = st.pk_in - st_last.pk_in;
    d.pk_out = st.pk_out - st_last.pk_out;
    st_last = st;
    ps_mutex_unlock(&lock);
    if (!in_session) {
        OSReport("heartbeat %u: netplay: %s, player %d\n", n, names[state], role);
        return;
    }
    OSReport("heartbeat %u: netplay: player %d, frame %d, confirmed %d, other console at %d; rtt %u ms; advantage "
             "%.1f, other %.1f; %u rollbacks (%u frames), %u + %u stalls; packets %u in, %u out; %u desyncs in %u "
             "checks%s\n",
             n, role, last_frame, confirmed, peer_frame, rtt_ms, (double) adv_avg, (double) peer_adv_avg, d.rollbacks,
             d.rerun_frames, d.stalls_sync, d.stalls_wait, d.pk_in, d.pk_out, d.desyncs, d.checks,
             lost ? "; lost" : "");
}
