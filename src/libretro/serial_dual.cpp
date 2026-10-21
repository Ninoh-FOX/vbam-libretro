/* One end of the in-memory link cable (Cable Link over netplay, see dual.h).
 * This file is part of every core instance: each GBA has its own end.
 * Ported from the Dual Cable Link of gpSP plus (by way of VBA Next).
 *
 * The cable is an AGB-005 Game Link Cable (GBATEK), for 2 to 4 GBAs:
 *  - The parent's SI is grounded, each child's SI is the SO of the one
 *    before it (slot - 1). SC and SD are common wires.
 *  - Multiplayer: only the parent starts. SIOMULTI0-3 read FFFF during the
 *    transfer and the word of each GBA at the end (FFFF where there is
 *    none), on every end.
 *  - Normal mode is for two: between the parent and the first child. The
 *    child gets the parent's bits, the parent always gets zero (its SI is
 *    grounded).
 *  - General purpose: the pins as wired above.
 *
 * Each end counts session cycles; every GBA starts the session at the same
 * time (0). Whatever an end does is stamped with its time, and the others
 * apply it when their own clock gets there. Ends only ever stop (wait for
 * another one) in dualport_sync, which changes nothing for the game, so the
 * machines are the same whatever the threads do. */

#ifdef HAVE_DUAL

#include <string.h>

#include "../common/Port.h"
#include "../gba/Globals.h"
#include "dual.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define R_SIOMULTI0    0x120
#define R_SIODATA32_L  0x120
#define R_SIODATA32_H  0x122
#define R_SIOCNT       0x128
#define R_SIOMLT_SEND  0x12A
#define R_SIODATA8     0x12A
#define R_RCNT         0x134

#define DMODE_NORMAL   0
#define DMODE_MULTI    1
#define DMODE_UART     2
#define DMODE_GPIO     3
#define DMODE_JOYBUS   4

/* Wire state word (DEV_WIRES.v0) */
#define DW_SO          0x01   /* SO level this end drives */
#define DW_SC_DRIVE    0x02   /* GPIO: drives SC */
#define DW_SC          0x04
#define DW_SD_DRIVE    0x08   /* GPIO: drives SD */
#define DW_SD          0x10
#define DW_MODE_SH     8      /* bits 8..10: DMODE_* */

/* Transfer in progress on this end */
#define DX_NONE        0
#define DX_MP_PARENT   1
#define DX_MP_CHILD    2
#define DX_N_MASTER    3
#define DX_N_SLAVE     4

/* Transfer times (multiplayer ones are per player) */
#define DCYC_256K_8      524
#define DCYC_256K_32    2097
#define DCYC_2M_8         67
#define DCYC_2M_32       268
static const u32 dual_mp_cycles[4] = { 31457, 7864, 5242, 2621 };

int dualport_active = 0;

static struct {
   u32 slot;
   u32 n;                /* GBAs on the cable */
   u64 clock;            /* Session cycles */
   u64 promised;         /* Nothing we do is stamped earlier than this */
   u64 target;           /* The current frame ends here */
   u32 keys;             /* Buttons for the current frame */
   u32 frames;           /* Frames run since dualport_begin */
   bool frame_over;      /* The current frame ended */
   int64_t rtc_epoch;    /* RTC seconds at dualport_begin */

   u32 wires_sent;       /* Last wire word posted (~0: none) */
   u32 so_out;           /* Normal mode SO level (latched while busy) */
   u32 p_wires[DUAL_MAX_PLAYERS]; /* The other ends, as last heard */
   u32 p_known;          /* (one bit per end) */

   u32 xfer;             /* DX_* */
   u64 t_done;
   u32 seq;
   u32 bits;             /* Normal mode: 8 or 32 */
   u32 word_rx;          /* Normal mode: what this end receives */
   u16 word_parent;      /* Multiplayer: the parent's word... */
   u16 word_own;         /* ...and ours (child) */
} dp;

#define DP_KNOWN(s)      ((dp.p_known >> (s)) & 1)

static inline u16 rd(u32 a) { return READ16LE(&ioMem[a]); }
static inline void wr(u32 a, u16 v) { WRITE16LE(&ioMem[a], v); }

/* The other end of a normal mode cable: the parent and the first child. */
static int dual_partner(void)
{
   return (dp.slot < 2 && dp.n >= 2) ? (int)(dp.slot ^ 1) : -1;
}

static u32 dual_mode(void)
{
   const u16 siocnt = rd(R_SIOCNT);
   const u16 rcnt = rd(R_RCNT);
   if (rcnt & 0x8000)
      return (rcnt & 0x4000) ? DMODE_JOYBUS : DMODE_GPIO;
   if (siocnt & 0x2000)
      return (siocnt & 0x1000) ? DMODE_UART : DMODE_MULTI;
   return DMODE_NORMAL;
}

/* Time stamp for something the game does now: its time, but never at or
 * before what we already published (the others may have got there). */
static u64 dual_io_time(int ticks)
{
   return MAX(dp.clock + (u64)MAX(ticks, 1), dp.promised + 1);
}

/* What this end drives on the cable. */
static u32 dual_wires(void)
{
   const u16 rcnt = rd(R_RCNT);
   const u32 mode = dual_mode();
   u32 w = mode << DW_MODE_SH;
   switch (mode)
   {
      case DMODE_NORMAL:
         w |= dp.so_out ? DW_SO : 0;
         break;
      case DMODE_GPIO:
         /* An input pin is not driven: the others see the pull-up. */
         w |= (rcnt & 0x80) ? ((rcnt & 0x08) ? DW_SO : 0) : DW_SO;
         if (rcnt & 0x10)
            w |= DW_SC_DRIVE | ((rcnt & 0x01) ? DW_SC : 0);
         if (rcnt & 0x20)
            w |= DW_SD_DRIVE | ((rcnt & 0x02) ? DW_SD : 0);
         break;
      default:
         w |= DW_SO;     /* Idle high */
         break;
   }
   return w;
}

static void dual_post_all(const dual_event *ev)
{
   u32 s;
   for (s = 0; s < dp.n; s++)
      if (s != dp.slot)
         dualbus_post(dp.slot, s, ev);
}

static void dual_post_wires(u64 t)
{
   const u32 w = dual_wires();
   if (w != dp.wires_sent)
   {
      dual_event ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind = DEV_WIRES;
      ev.t = t;
      ev.v0 = w;
      dual_post_all(&ev);
      dp.wires_sent = w;
   }
}

/* Our SI: grounded on the parent end, the SO of the one before on a child. */
static u32 dual_si(void)
{
   if (!dp.slot)
      return 0;
   return DP_KNOWN(dp.slot - 1) ? ((dp.p_wires[dp.slot - 1] & DW_SO) != 0) : 1;
}

/* SD in multiplayer: high once every other GBA is in multiplayer mode. */
static int dual_all_multi(void)
{
   u32 s;
   for (s = 0; s < dp.n; s++)
      if (s != dp.slot && (!DP_KNOWN(s) ||
               ((dp.p_wires[s] >> DW_MODE_SH) & 7) != DMODE_MULTI))
         return false;
   return dp.n > 1;
}

/* A common wire (SC or SD) in general purpose mode: any GBA driving it low
 * pulls it low, otherwise the pull-up keeps it high. */
static u32 dual_common_wire(u32 drive, u32 level)
{
   u32 s, v = 1;
   for (s = 0; s < dp.n; s++)
      if (s != dp.slot && DP_KNOWN(s) && (dp.p_wires[s] & drive) &&
            !(dp.p_wires[s] & level))
         v = 0;
   return v;
}

/* Refresh the bits driven by the other ends (SIOCNT 2/3, RCNT 0-3). Returns
 * true if a general purpose SI falling edge asks for an IRQ. */
static int dual_refresh_inputs(void)
{
   u16 siocnt = rd(R_SIOCNT);
   u16 rcnt = rd(R_RCNT);
   const u32 si = dual_si();
   int irq = 0;

   switch (dual_mode())
   {
      case DMODE_NORMAL:
         siocnt = (siocnt & ~0x0004) | (si ? 0x0004 : 0);
         wr(R_SIOCNT, siocnt);
         break;
      case DMODE_MULTI:
         {
            const bool sd = dual_all_multi();
            const bool busy = (siocnt & 0x0080) != 0;
            siocnt = (siocnt & ~0x000C) | (si ? 0x0004 : 0) | (sd ? 0x0008 : 0);
            wr(R_SIOCNT, siocnt);
            /* RCNT 0-3 read the pins (games test SC, low while busy) */
            rcnt = (rcnt & ~0x000F) | (busy ? 0 : 0x1) | (sd ? 0x2 : 0) |
               (si ? 0x4 : 0) | 0x8;
            wr(R_RCNT, rcnt);
         }
         break;
      case DMODE_GPIO:
         {
            const u32 old_si = (rcnt >> 2) & 1;
            u32 v = rcnt & 0x0F;
            if (!(rcnt & 0x10))   /* SC input */
               v = (v & ~1u) | dual_common_wire(DW_SC_DRIVE, DW_SC);
            if (!(rcnt & 0x20))   /* SD input */
               v = (v & ~2u) | (dual_common_wire(DW_SD_DRIVE, DW_SD) << 1);
            if (!(rcnt & 0x40))   /* SI input */
               v = (v & ~4u) | (si ? 4 : 0);
            if (!(rcnt & 0x80))   /* SO input: nothing drives it on this cable */
               v = (v & ~8u) | 8;
            if (old_si && !(v & 4) && (rcnt & 0x100))
               irq = true;
            wr(R_RCNT, (rcnt & ~0x000F) | v);
         }
         break;
      default:
         break;
   }
   return irq;
}

static void dual_mp_start_parent(u64 t)
{
   dual_event ev;
   const u16 siocnt = rd(R_SIOCNT);
   dp.xfer = DX_MP_PARENT;
   dp.t_done = t + (u64)dual_mp_cycles[siocnt & 3] * dp.n;
   dp.seq++;
   dp.word_parent = rd(R_SIOMLT_SEND);
   wr(R_SIOMULTI0 + 0, 0xFFFF);
   wr(R_SIOMULTI0 + 2, 0xFFFF);
   wr(R_SIOMULTI0 + 4, 0xFFFF);
   wr(R_SIOMULTI0 + 6, 0xFFFF);

   memset(&ev, 0, sizeof(ev));
   ev.kind = DEV_MP_START;
   ev.t = t;
   ev.t_done = dp.t_done;
   ev.v0 = dp.word_parent;
   ev.seq = dp.seq;
   dual_post_all(&ev);
}

static void dual_n_start_master(u64 t)
{
   dual_event ev;
   const u16 siocnt = rd(R_SIOCNT);
   const bool is32 = (siocnt & 0x1000) != 0;
   dp.xfer = DX_N_MASTER;
   dp.bits = is32 ? 32 : 8;
   dp.t_done = t + ((siocnt & 0x2) ? (is32 ? DCYC_2M_32 : DCYC_2M_8)
         : (is32 ? DCYC_256K_32 : DCYC_256K_8));

   memset(&ev, 0, sizeof(ev));
   ev.kind = DEV_N_START;
   ev.t = t;
   ev.t_done = dp.t_done;
   ev.v0 = is32 ? (rd(R_SIODATA32_L) | ((u32)rd(R_SIODATA32_H) << 16))
      : (rd(R_SIODATA8) & 0xFF);
   ev.a = dp.bits;
   if (dual_partner() >= 0)
      dualbus_post(dp.slot, dual_partner(), &ev);
}

/* Something from another end is due now. */
static int dual_apply(const dual_event *ev)
{
   const u16 siocnt = rd(R_SIOCNT);
   const u32 mode = dual_mode();

   switch (ev->kind)
   {
      case DEV_WIRES:
         dp.p_wires[ev->from] = ev->v0;
         dp.p_known |= 1u << ev->from;
         return dual_refresh_inputs();

      case DEV_MP_START:
         /* Only the children get these. One takes part if it is in
          * multiplayer mode and not busy; either way everybody gets an
          * answer. */
         if (dp.slot && mode == DMODE_MULTI && dp.xfer == DX_NONE)
         {
            dp.xfer = DX_MP_CHILD;
            dp.seq = ev->seq;
            dp.t_done = ev->t_done;
            dp.word_parent = ev->v0;
            dp.word_own = rd(R_SIOMLT_SEND);
            wr(R_SIOMULTI0 + 0, 0xFFFF);
            wr(R_SIOMULTI0 + 2, 0xFFFF);
            wr(R_SIOMULTI0 + 4, 0xFFFF);
            wr(R_SIOMULTI0 + 6, 0xFFFF);
            wr(R_SIOCNT, siocnt | 0x0080);     /* Busy */
            dualbus_reply(dp.slot, ev->seq, dp.word_own, 1);
            dual_refresh_inputs();
         }
         else
            dualbus_reply(dp.slot, ev->seq, 0xFFFF, 0);
         return false;

      case DEV_N_START:
         /* The other end clocks a transfer: we get its bits if we are
          * waiting for an external clock with the same word size. The parent
          * always gets zero. */
         if (mode == DMODE_NORMAL && (siocnt & 0x0081) == 0x0080 &&
               dp.xfer == DX_NONE && ((siocnt & 0x1000) ? 32 : 8) == ev->a)
         {
            dp.xfer = DX_N_SLAVE;
            dp.bits = ev->a;
            dp.word_rx = dp.slot ? ev->v0 : 0;
            /* Our view of it is late by the lookahead when it comes from a
             * child. */
            dp.t_done = MAX(ev->t_done + (ev->from ? DUAL_LOOKAHEAD : 0), dp.clock);
         }
         return false;
   }
   return false;
}

/* The transfer time is over. */
static int dual_complete(void)
{
   u16 siocnt = rd(R_SIOCNT);
   const bool irq = (siocnt & 0x4000) != 0;

   switch (dp.xfer)
   {
      case DX_MP_PARENT:
      case DX_MP_CHILD:
         {
            /* Each child answered this transfer when it got to its start:
             * wait for the answers, whenever they get here they are the same
             * ones. */
            u32 s;
            wr(R_SIOMULTI0, dp.word_parent);
            for (s = 1; s < 4; s++)
            {
               u32 w = 0xFFFF;
               if (s == dp.slot)
                  w = dp.word_own;
               else if (s < dp.n && !dualbus_wait_reply(dp.slot, s, dp.seq, &w))
                  w = 0xFFFF;
               wr(R_SIOMULTI0 + s * 2, w & 0xFFFF);
            }
            siocnt &= ~(0x0080 | 0x0030 | 0x0040);
            siocnt |= (dp.slot & 3) << 4;                 /* ID */
            wr(R_SIOCNT, siocnt);
         }
         break;

      case DX_N_MASTER:
      case DX_N_SLAVE:
         {
            u32 rx;
            if (dp.xfer == DX_N_SLAVE)
               rx = dp.word_rx;
            else if (!dp.slot)
               rx = 0;                                     /* SI grounded */
            else                                          /* The idle SO before us */
               rx = dual_si() ? 0xFFFFFFFF : 0;
            if (dp.bits == 32)
            {
               wr(R_SIODATA32_L, rx & 0xFFFF);
               wr(R_SIODATA32_H, rx >> 16);
            }
            else
               wr(R_SIODATA8, (rd(R_SIODATA8) & 0xFF00) | (rx & 0xFF));
            siocnt &= ~0x0080;
            wr(R_SIOCNT, siocnt);
            dp.so_out = (siocnt & 0x0008) ? 1 : 0;        /* SO back to bit 3 */
         }
         break;

      default:
         return false;
   }

   dp.xfer = DX_NONE;
   dual_refresh_inputs();
   /* (Our current time is already published: what we do now is later.) */
   dual_post_wires(MAX(dp.clock, dp.promised) + 1);
   return irq;
}

int dualport_write_siocnt(uint16_t value, int ticks)
{
   const u16 oldval = rd(R_SIOCNT);
   const u64 t = dual_io_time(ticks);
   u16 newval;

   wr(R_SIOCNT, value);   /* So that the mode reads right */
   switch (dual_mode())
   {
      case DMODE_MULTI:
         /* Baud rate, start (parent only), mode and IRQ are writable. */
         newval = (value & 0x7F03) | (oldval & 0x007C);
         if (dp.slot || dp.xfer != DX_NONE)
            newval = (newval & ~0x0080) | (oldval & 0x0080);
         else
            newval |= value & 0x0080;
         wr(R_SIOCNT, newval);
         if ((newval & 0x0080) && !(oldval & 0x0080) && !dp.slot &&
               dp.xfer == DX_NONE)
            dual_mp_start_parent(t);
         break;

      case DMODE_NORMAL:
         newval = (value & 0x7F8B) | (oldval & 0x0004);
         if (dp.xfer != DX_NONE)
            newval |= 0x0080;               /* Start stays set until done */
         wr(R_SIOCNT, newval);
         if (!(newval & 0x0080))
            dp.so_out = (newval & 0x0008) ? 1 : 0;
         /* A transfer runs whenever start is set with the internal clock:
          * games arm the port first and switch the clock source with start
          * still set. */
         if ((newval & 0x0080) && (newval & 0x0001) && dp.xfer == DX_NONE &&
               (!(oldval & 0x0080) || !(oldval & 0x0001)))
            dual_n_start_master(t);
         break;

      default:
         break;      /* UART / GPIO / JOY BUS: just stored */
   }

   dual_refresh_inputs();
   dual_post_wires(t);
   return false;
}

int dualport_write_rcnt(uint16_t value, int ticks)
{
   const u64 t = dual_io_time(ticks);
   int irq;
   wr(R_RCNT, value);
   irq = dual_refresh_inputs();
   dual_post_wires(t);
   return irq;
}

/* Called at the top of every scheduler iteration, with the cycles that were
 * run since the last one: our clock moves on, we wait (if need be) until the
 * others are where the rules say they must be, and what they did that is due
 * now is applied. */
int dualport_sync(int cycles)
{
   dual_event ev;
   int irq = 0;

   dp.clock += (u64)MAX(cycles, 0);
   dualbus_sync(dp.slot, MAX(dp.clock, dp.promised), dp.clock);

   if (dualbus_pending(dp.slot))
      while (dualbus_pop(dp.slot, dp.clock, &ev))
         irq |= dual_apply(&ev);

   if (dp.xfer != DX_NONE && dp.clock >= dp.t_done)
      irq |= dual_complete();

   return irq;
}

/* Our own transfer end and the end of the frame are events; what comes from
 * the others is not (it is applied at our next event): when it shows up
 * depends on the other threads, an event for it would make our machine
 * depend on them too. */
int dualport_next_event(void)
{
   u64 next = dp.target > dp.clock ? dp.target - dp.clock : 1;
   if (dp.xfer != DX_NONE)
      next = MIN(next, dp.t_done > dp.clock ? dp.t_done - dp.clock : 1);
   return (int)MIN(next, 0x7FFFFFFF);
}

/* A linked frame starts: the others may have raised our promise while we
 * were stopped (they needed us further on, and we did nothing meanwhile). */
void dualport_frame_begin(void)
{
   dp.promised = MAX(dp.promised, dualbus_frame_start(dp.slot));
   dp.frames++;
   dp.frame_over = false;
}

/* GBA.cpp: the frame must end now. */
int dualport_frame_due(void)
{
   return dp.clock >= dp.target;
}

/* libretro.c: the frame ended. */
int dualport_frame_over(void)
{
   return dp.frame_over;
}

/* Our frame is over: we stop until the next one. We promise that nothing we
 * do from now on is stamped before a scanline later, so that the others can
 * finish their own frame (each one overshoots the end a little) without
 * waiting for our next one. */
void dualport_frame_end(void)
{
   while (dp.target <= dp.clock)
      dp.target += DUAL_FRAME_CYCLES;
   dp.promised = MAX(dp.promised, dp.clock + DUAL_FRAME_SLACK);
   dp.frame_over = true;
   dualbus_frame_done(dp.slot, dp.promised);
}

void dualport_begin(unsigned slot, int64_t rtc_epoch)
{
   memset(&dp, 0, sizeof(dp));
   dualport_active = 1;
   dp.slot = slot;
   dp.n = dualbus_players();
   dp.rtc_epoch = rtc_epoch;
   /* Every GBA was just reset (or loaded from the same reset): the first
    * frame ends at its first VBlank, the same time on every GBA. */
   dp.target = (u64)MAX(CPUCyclesToVblank(), 1);
   dp.wires_sent = ~0U;
   dp.so_out = (rd(R_SIOCNT) & 0x0008) ? 1 : 0;
   /* Anything that was going on is lost, the cable was just plugged in. */
   wr(R_SIOCNT, rd(R_SIOCNT) & ~0x0080);
   dual_refresh_inputs();
   dual_post_wires(0);
}

void dualport_end(void)
{
   dualport_active = 0;
}

void dualport_set_keys(uint32_t keys) { dp.keys = keys; }
uint32_t dualport_get_keys(void) { return dp.keys; }

int64_t dualport_rtc_seconds(void)
{
   return dp.rtc_epoch + (int64_t)(((u64)dp.frames * DUAL_FRAME_CYCLES) >> 24);
}

#endif /* HAVE_DUAL */
