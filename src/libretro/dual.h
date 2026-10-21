/* Cable Link over netplay (Dual Cable Link, as in gpSP plus, mGBA plus and
 * VBA Next).
 *
 * All the GBAs of a link session (2 to 4) run on every console, joined by a
 * link cable in memory, and only the buttons travel over the network
 * (libretro netpacket interface). See README-dual.md.
 *
 * The core is built several times into the same library. Instance A is the
 * core the frontend talks to; the hidden ones (B, C, D) are the very same
 * code built with -DVBA_INSTANCE_B and every global renamed (gbab_*,
 * gbac_*, gbad_*), so each one is another, independent GBA. Each instance
 * has its own end of the cable (serial_dual.cpp). The cable itself (the
 * "bus") and the session logic live once, in A (dual.cpp). What the hidden
 * instances are called by is declared extern "C", so that the renamed names
 * are the plain ones with the prefix.
 *
 * Slot 0 is the parent end of the cable (the netplay host's GBA), slots 1-3
 * the children (the clients' GBAs, in the order the host gave them). On each
 * console A is that console's slot and the hidden GBAs are the other ones.
 * Every console computes the same machines, so everything the cable does
 * depends on emulated time only, never on how fast the threads run.
 *
 * TIME. Each end counts session cycles from 0 (when the session started, or
 * every GBA was reset). A linked frame does not end at the GBA's own
 * VBlank but at a fixed session time (the first VBlank after the start,
 * then every 280896 cycles), the same for every GBA: a game can move its own
 * VBlank a little, and GBAs whose frames ended at different times could wait
 * for each other forever. The player's GBA shows the picture of its last
 * VBlank. */

#ifndef VBAM_DUAL_H
#define VBAM_DUAL_H

#include <stdint.h>
#include <stddef.h>

#define DUAL_MAX_PLAYERS 4

/* Cycles in a frame. */
#define DUAL_FRAME_CYCLES 280896

#ifdef HAVE_DUAL

#ifdef __cplusplus
extern "C" {
#endif

/* Events from one end of the cable to the others. */
#define DEV_WIRES       1   /* v0: the sender's mode and pin levels */
#define DEV_MP_START    2   /* multiplayer transfer started (parent) */
#define DEV_N_START     3   /* normal mode transfer started (internal clock) */

typedef struct {
   uint64_t t;              /* When it happened (sender's session cycles) */
   uint64_t t_done;         /* *_START: when the transfer ends */
   uint32_t v0;             /* Data word */
   uint32_t seq;            /* *_START: transfer number */
   uint8_t kind;
   uint8_t a;               /* N_START: 8 or 32 bits */
   uint8_t from;            /* Slot of the sender */
} dual_event;

/* How far (cycles) the parent may run ahead of the slowest child. A child
 * never runs ahead of the parent, so whatever the parent does reaches the
 * children on time; what a child does (other than answering a transfer)
 * reaches the others this much later. Half a frame. */
#define DUAL_LOOKAHEAD   140448

/* What a GBA that finished its frame promises the others (cycles). */
#define DUAL_FRAME_SLACK 1232

/* ---- the bus (dual.cpp, instance A only). The ends call these. ---- */
unsigned dualbus_players(void);
void dualbus_sync(unsigned slot, uint64_t publish, uint64_t clock);
uint64_t dualbus_frame_start(unsigned slot);
void dualbus_frame_done(unsigned slot, uint64_t publish);
void dualbus_post(unsigned from_slot, unsigned to_slot, const dual_event *ev);
int dualbus_pending(unsigned slot);
int dualbus_pop(unsigned slot, uint64_t now, dual_event *ev);
void dualbus_reply(unsigned from_slot, uint32_t seq, uint32_t word, int active);
int dualbus_wait_reply(unsigned slot, unsigned from_slot, uint32_t seq,
                       uint32_t *word);
/* The game, for the hidden GBAs (they read the player's GBA's copy). */
uint8_t *dual_shared_rom(int *size);

/* ---- one end of the cable (serial_dual.cpp, in every instance) ---- */
extern int dualport_active;
void dualport_begin(unsigned slot, int64_t rtc_epoch);
void dualport_end(void);
/* GBA.cpp: time moves on (cycles since the last call); what the other ends
 * did is applied. Returns nonzero for a serial IRQ. */
int dualport_sync(int cycles);
/* GBA.cpp: cycles to the next event of this end (transfer end, end of frame). */
int dualport_next_event(void);
/* GBA.cpp: the game writes SIOCNT / RCNT. `ticks`: cycles run since the
 * last dualport_sync. Returns nonzero for a serial IRQ. */
int dualport_write_siocnt(uint16_t value, int ticks);
int dualport_write_rcnt(uint16_t value, int ticks);
/* libretro.cpp: a linked frame of this GBA. */
void dualport_frame_begin(void);
int dualport_frame_due(void);
int dualport_frame_over(void);
void dualport_frame_end(void);
void dualport_set_keys(uint32_t keys);
uint32_t dualport_get_keys(void);
/* RTC.cpp: the RTC of a linked GBA (seconds, the host's local time). */
int64_t dualport_rtc_seconds(void);

/* ---- GBA.cpp, in every instance ---- */
uint32_t CPUDualHash(void);
void CPUDualHashParts(uint32_t *out);
int CPUCyclesToVblank(void);

/* ---- libretro.cpp, in every instance: a hidden GBA becomes the same
 * machine as the player's one (BIOS, RTC). ---- */
void dualinst_same_machine(const uint8_t *bios_data, int use_bios, int rtc);

#ifdef __cplusplus
}
#endif

/* ---- the session (dual.cpp, instance A only), called by libretro.cpp ---- */
#if defined(__LIBRETRO__) && !defined(VBA_INSTANCE_B)
#include <libretro.h>

/* What retro_run does with a frame (dual_frame_begin). */
enum {
   DUAL_FRAME_WAIT = 0, /* Someone's buttons are not here yet: show the last frame again */
   DUAL_FRAME_RUN,      /* Run it (dual_frame_run) */
   DUAL_FRAME_HOLD      /* Ahead of the other consoles: a frame of silence */
};

typedef struct {
   retro_environment_t environ_cb;
   void (*message)(const char *text, unsigned ms);
   /* A frame of the player's GBA, with the buttons dualport_get_keys gives */
   void (*run_frame)(void);
   /* The player's GBA starts over (as the reset button does) */
   void (*reset)(void);
   /* Settings put aside while linked (cheats) */
   void (*link_begin)(void);
   /* Settings that change the machine (RTC, save type, ...) */
   uint32_t (*machine_cfg)(void);
   /* The BIOS the player's GBA runs, and whether it is the official one */
   const uint8_t *(*bios)(int *use_bios);
   int (*rtc_enabled)(void);
   /* Savestates of the player's GBA (with its save memory: SRAM, flash,
    * EEPROM) */
   size_t (*serialize_size)(void);
   int (*serialize)(void *data, size_t size);
   int (*unserialize)(const void *data, size_t size);
} dual_frontend;

void dual_set_frontend(const dual_frontend *fe);
void dual_game_loaded(const char *path, const void *data, size_t size);
void dual_game_unloaded(void);
void dual_set_players(unsigned players);
void dual_set_net(retro_netpacket_send_t send, retro_netpacket_poll_receive_t poll);

void dual_session_start(unsigned client_id);
bool dual_peer_connected(unsigned client_id);
void dual_peer_disconnected(unsigned client_id);
void dual_session_stop(void);
bool dual_net_receive(const void *buf, size_t len, uint16_t client_id);

bool dual_setting_up(void);   /* true: do not run the game this frame */
bool dual_running(void);
bool dual_linked(void);       /* Setting up or running: no savestates, no cheats */
void dual_request_reset(void);

int dual_frame_begin(uint32_t local_keys);
void dual_frame_run(void);
#endif

#endif /* HAVE_DUAL */

#endif
