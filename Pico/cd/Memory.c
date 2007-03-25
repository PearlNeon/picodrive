// This is part of Pico Library

// (c) Copyright 2004 Dave, All rights reserved.
// (c) Copyright 2007 notaz, All rights reserved.
// Free for non-commercial use.

// For commercial use, separate licencing terms must be obtained.

// A68K no longer supported here

//#define __debug_io

#include "../PicoInt.h"

#include "../sound/sound.h"
#include "../sound/ym2612.h"
#include "../sound/sn76496.h"

#include "gfx_cd.h"
#include "pcm.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

//#define __debug_io
//#define __debug_io2

//#define rdprintf dprintf
#define rdprintf(...)
//#define wrdprintf dprintf
#define wrdprintf(...)

// -----------------------------------------------------------------

// poller detection
#define POLL_LIMIT 16
#define POLL_CYCLES 124
// int m68k_poll_addr, m68k_poll_cnt;
unsigned int s68k_poll_adclk, s68k_poll_cnt;

#ifndef _ASM_CD_MEMORY_C
static u32 m68k_reg_read16(u32 a)
{
  u32 d=0;
  a &= 0x3e;
  // dprintf("m68k_regs r%2i: [%02x] @%06x", realsize&~1, a+(realsize&1), SekPc);

  switch (a) {
    case 0:
      d = ((Pico_mcd->s68k_regs[0x33]<<13)&0x8000) | Pico_mcd->m.busreq; // here IFL2 is always 0, just like in Gens
      goto end;
    case 2:
      d = (Pico_mcd->s68k_regs[a]<<8) | (Pico_mcd->s68k_regs[a+1]&0xc7);
      // the DMNA delay must only be visible on s68k side (Lunar2, Silpheed)
      if (Pico_mcd->m.state_flags&2) { d &= ~1; d |= 2; }
      //printf("m68k_regs r3: %02x @%06x\n", (u8)d, SekPc);
      goto end;
    case 4:
      d = Pico_mcd->s68k_regs[4]<<8;
      goto end;
    case 6:
      d = *(u16 *)(Pico_mcd->bios + 0x72);
      goto end;
    case 8:
      d = Read_CDC_Host(0);
      goto end;
    case 0xA:
      dprintf("m68k FIXME: reserved read");
      goto end;
    case 0xC:
      d = Pico_mcd->m.timer_stopwatch >> 16;
      dprintf("m68k stopwatch timer read (%04x)", d);
      goto end;
  }

  if (a < 0x30) {
    // comm flag/cmd/status (0xE-0x2F)
    d = (Pico_mcd->s68k_regs[a]<<8) | Pico_mcd->s68k_regs[a+1];
    goto end;
  }

  dprintf("m68k_regs FIXME invalid read @ %02x", a);

end:

  // dprintf("ret = %04x", d);
  return d;
}
#endif

#ifndef _ASM_CD_MEMORY_C
static
#endif
void m68k_reg_write8(u32 a, u32 d)
{
  a &= 0x3f;
  // dprintf("m68k_regs w%2i: [%02x] %02x @%06x", realsize, a, d, SekPc);

  switch (a) {
    case 0:
      d &= 1;
      if ((d&1) && (Pico_mcd->s68k_regs[0x33]&(1<<2))) { dprintf("m68k: s68k irq 2"); SekInterruptS68k(2); }
      return;
    case 1:
      d &= 3;
      if (!(d&1)) Pico_mcd->m.state_flags |= 1; // reset pending, needed to be sure we fetch the right vectors on reset
      if ( (Pico_mcd->m.busreq&1) != (d&1)) dprintf("m68k: s68k reset %i", !(d&1));
      if ( (Pico_mcd->m.busreq&2) != (d&2)) dprintf("m68k: s68k brq %i", (d&2)>>1);
      if ((Pico_mcd->m.state_flags&1) && (d&3)==1) {
        SekResetS68k(); // S68k comes out of RESET or BRQ state
        Pico_mcd->m.state_flags&=~1;
        dprintf("m68k: resetting s68k, cycles=%i", SekCyclesLeft);
      }
      Pico_mcd->m.busreq = d;
      return;
    case 2:
      Pico_mcd->s68k_regs[2] = d; // really use s68k side register
      return;
    case 3: {
      u32 dold = Pico_mcd->s68k_regs[3]&0x1f;
      //printf("m68k_regs w3: %02x @%06x\n", (u8)d, SekPc);
      d &= 0xc2;
      if ((dold>>6) != ((d>>6)&3))
        dprintf("m68k: prg bank: %i -> %i", (Pico_mcd->s68k_regs[a]>>6), ((d>>6)&3));
      //if ((Pico_mcd->s68k_regs[3]&4) != (d&4)) dprintf("m68k: ram mode %i mbit", (d&4) ? 1 : 2);
      //if ((Pico_mcd->s68k_regs[3]&2) != (d&2)) dprintf("m68k: %s", (d&4) ? ((d&2) ? "word swap req" : "noop?") :
      //                                             ((d&2) ? "word ram to s68k" : "word ram to m68k"));
      if (dold & 4) {
        d ^= 2;                // writing 0 to DMNA actually sets it, 1 does nothing
      } else {
        //dold &= ~2; // ??
#if 1
	if ((d & 2) && !(dold & 2)) {
          Pico_mcd->m.state_flags |= 2; // we must delay setting DMNA bit (needed for Silpheed)
          d &= ~2;
	}
#else
        if (d & 2) dold &= ~1; // return word RAM to s68k in 2M mode
#endif
      }
      Pico_mcd->s68k_regs[3] = d | dold; // really use s68k side register
#ifdef USE_POLL_DETECT
      if ((s68k_poll_adclk&0xfe) == 2 && s68k_poll_cnt > POLL_LIMIT) {
        SekSetStopS68k(0); s68k_poll_adclk = 0;
        //printf("%05i:%03i: s68k poll release, a=%02x\n", Pico.m.frame_count, Pico.m.scanline, a);
      }
#endif
      return;
    }
    case 6:
      Pico_mcd->bios[0x72 + 1] = d; // simple hint vector changer
      return;
    case 7:
      Pico_mcd->bios[0x72] = d;
      dprintf("hint vector set to %08x", PicoRead32(0x70));
      return;
    case 0xf:
      d = (d << 1) | ((d >> 7) & 1); // rol8 1 (special case)
    case 0xe:
      //dprintf("m68k: comm flag: %02x", d);
      Pico_mcd->s68k_regs[0xe] = d;
#ifdef USE_POLL_DETECT
      if ((s68k_poll_adclk&0xfe) == 0xe && s68k_poll_cnt > POLL_LIMIT) {
        SekSetStopS68k(0); s68k_poll_adclk = 0;
        //printf("%05i:%03i: s68k poll release, a=%02x\n", Pico.m.frame_count, Pico.m.scanline, a);
      }
#endif
      return;
  }

  if ((a&0xf0) == 0x10) {
      Pico_mcd->s68k_regs[a] = d;
#ifdef USE_POLL_DETECT
      if ((a&0xfe) == (s68k_poll_adclk&0xfe) && s68k_poll_cnt > POLL_LIMIT) {
        SekSetStopS68k(0); s68k_poll_adclk = 0;
        //printf("%05i:%03i: s68k poll release, a=%02x\n", Pico.m.frame_count, Pico.m.scanline, a);
      }
#endif
      return;
  }

  dprintf("m68k FIXME: invalid write? [%02x] %02x", a, d);
}


#define READ_FONT_DATA(basemask) \
{ \
      unsigned int fnt = *(unsigned int *)(Pico_mcd->s68k_regs + 0x4c); \
      unsigned int col0 = (fnt >> 8) & 0x0f, col1 = (fnt >> 12) & 0x0f;   \
      if (fnt & (basemask << 0)) d  = col1      ; else d  = col0;       \
      if (fnt & (basemask << 1)) d |= col1 <<  4; else d |= col0 <<  4; \
      if (fnt & (basemask << 2)) d |= col1 <<  8; else d |= col0 <<  8; \
      if (fnt & (basemask << 3)) d |= col1 << 12; else d |= col0 << 12; \
}


#ifndef _ASM_CD_MEMORY_C
static
#endif
u32 s68k_reg_read16(u32 a)
{
  u32 d=0;

  // dprintf("s68k_regs r%2i: [%02x] @ %06x", realsize&~1, a+(realsize&1), SekPcS68k);

  switch (a) {
    case 0:
      return ((Pico_mcd->s68k_regs[0]&3)<<8) | 1; // ver = 0, not in reset state
    case 2:
      d = (Pico_mcd->s68k_regs[a]<<8) | (Pico_mcd->s68k_regs[a+1]&0x1f);
      //printf("s68k_regs r3: %02x @%06x\n", (u8)d, SekPcS68k);
      goto poll_detect;
    case 6:
      return CDC_Read_Reg();
    case 8:
      return Read_CDC_Host(1); // Gens returns 0 here on byte reads
    case 0xC:
      d = Pico_mcd->m.timer_stopwatch >> 16;
      dprintf("s68k stopwatch timer read (%04x)", d);
      return d;
    case 0x30:
      dprintf("s68k int3 timer read (%02x)", Pico_mcd->s68k_regs[31]);
      return Pico_mcd->s68k_regs[31];
    case 0x34: // fader
      return 0; // no busy bit
    case 0x50: // font data (check: Lunar 2, Silpheed)
      READ_FONT_DATA(0x00100000);
      return d;
    case 0x52:
      READ_FONT_DATA(0x00010000);
      return d;
    case 0x54:
      READ_FONT_DATA(0x10000000);
      return d;
    case 0x56:
      READ_FONT_DATA(0x01000000);
      return d;
  }

  d = (Pico_mcd->s68k_regs[a]<<8) | Pico_mcd->s68k_regs[a+1];

  if (a >= 0x0e && a < 0x30) goto poll_detect;

  return d;

poll_detect:
#ifdef USE_POLL_DETECT
  // polling detection
  if (a == (s68k_poll_adclk&0xfe)) {
    unsigned int clkdiff = SekCyclesDoneS68k() - (s68k_poll_adclk>>8);
    if (clkdiff <= POLL_CYCLES) {
      s68k_poll_cnt++;
      //printf("-- diff: %u, cnt = %i\n", clkdiff, s68k_poll_cnt);
      if (s68k_poll_cnt > POLL_LIMIT) {
        SekSetStopS68k(1);
        //printf("%05i:%03i: s68k poll detected @ %06x, a=%02x\n", Pico.m.frame_count, Pico.m.scanline, SekPcS68k, a);
      }
      s68k_poll_adclk = (SekCyclesDoneS68k() << 8) | a;
      return d;
    }
  }
  s68k_poll_adclk = (SekCyclesDoneS68k() << 8) | a;
  s68k_poll_cnt = 0;

#endif
  return d;
}

#ifndef _ASM_CD_MEMORY_C
static
#endif
void s68k_reg_write8(u32 a, u32 d)
{
  //dprintf("s68k_regs w%2i: [%02x] %02x @ %06x", realsize, a, d, SekPcS68k);

  // TODO: review against Gens
  // Warning: d might have upper bits set
  switch (a) {
    case 2:
      return; // only m68k can change WP
    case 3: {
      int dold = Pico_mcd->s68k_regs[3];
      //printf("s68k_regs w3: %02x @%06x\n", (u8)d, SekPcS68k);
      d &= 0x1d;
      d |= dold&0xc2;
      if (d&4) {
        if ((d ^ dold) & 5) {
          d &= ~2; // in case of mode or bank change we clear DMNA (m68k req) bit
#ifdef _ASM_CD_MEMORY_C
          PicoMemResetCD(d);
#endif
        }
#ifdef _ASM_CD_MEMORY_C
        if ((d ^ dold) & 0x1d)
          PicoMemResetCDdecode(d);
#endif
        if (!(dold & 4)) {
          dprintf("wram mode 2M->1M");
          wram_2M_to_1M(Pico_mcd->word_ram2M);
        }
      } else {
        if (dold & 4) {
          dprintf("wram mode 1M->2M");
          if (!(d&1)) { // it didn't set the ret bit, which means it doesn't want to give WRAM to m68k
            d &= ~3;
            d |= (dold&1) ? 2 : 1; // then give it to the one which had bank0 in 1M mode
          }
          wram_1M_to_2M(Pico_mcd->word_ram2M);
#ifdef _ASM_CD_MEMORY_C
          PicoMemResetCD(d);
#endif
        }
        else
          d |= dold&1;
        if (d&1) d &= ~2; // return word RAM to m68k in 2M mode
      }
      break;
    }
    case 4:
      dprintf("s68k CDC dest: %x", d&7);
      Pico_mcd->s68k_regs[4] = (Pico_mcd->s68k_regs[4]&0xC0) | (d&7); // CDC mode
      return;
    case 5:
      //dprintf("s68k CDC reg addr: %x", d&0xf);
      break;
    case 7:
      CDC_Write_Reg(d);
      return;
    case 0xa:
      dprintf("s68k set CDC dma addr");
      break;
    case 0xc:
    case 0xd:
      dprintf("s68k set stopwatch timer");
      Pico_mcd->m.timer_stopwatch = 0;
      return;
    case 0xe:
      Pico_mcd->s68k_regs[0xf] = (d>>1) | (d<<7); // ror8 1, Gens note: Dragons lair
      return;
    case 0x31:
      dprintf("s68k set int3 timer: %02x", d);
      Pico_mcd->m.timer_int3 = (d & 0xff) << 16;
      break;
    case 0x33: // IRQ mask
      dprintf("s68k irq mask: %02x", d);
      if ((d&(1<<4)) && (Pico_mcd->s68k_regs[0x37]&4) && !(Pico_mcd->s68k_regs[0x33]&(1<<4))) {
        CDD_Export_Status();
      }
      break;
    case 0x34: // fader
      Pico_mcd->s68k_regs[a] = (u8) d & 0x7f;
      return;
    case 0x36:
      return; // d/m bit is unsetable
    case 0x37: {
      u32 d_old = Pico_mcd->s68k_regs[0x37];
      Pico_mcd->s68k_regs[0x37] = d&7;
      if ((d&4) && !(d_old&4)) {
        CDD_Export_Status();
      }
      return;
    }
    case 0x4b:
      Pico_mcd->s68k_regs[a] = (u8) d;
      CDD_Import_Command();
      return;
  }

  if ((a&0x1f0) == 0x10 || (a >= 0x38 && a < 0x42))
  {
    dprintf("s68k FIXME: invalid write @ %02x?", a);
    return;
  }

  Pico_mcd->s68k_regs[a] = (u8) d;
}


#ifndef _ASM_CD_MEMORY_C
static u32 OtherRead16End(u32 a, int realsize)
{
  u32 d=0;

  if ((a&0xffffc0)==0xa12000) {
    d=m68k_reg_read16(a);
    goto end;
  }

  dprintf("m68k FIXME: unusual r%i: %06x @%06x", realsize&~1, (a&0xfffffe)+(realsize&1), SekPc);

end:
  return d;
}


static void OtherWrite8End(u32 a, u32 d, int realsize)
{
  if ((a&0xffffc0)==0xa12000) { m68k_reg_write8(a, d); return; }

  dprintf("m68k FIXME: strange w%i: %06x, %08x @%06x", realsize, a&0xffffff, d, SekPc);
}


#undef _ASM_MEMORY_C
#include "../MemoryCmn.c"
#include "cell_map.c"
#endif // !def _ASM_CD_MEMORY_C

// -----------------------------------------------------------------
//                     Read Rom and read Ram

//u8 PicoReadM68k8_(u32 a);
#ifdef _ASM_CD_MEMORY_C
u8 PicoReadM68k8(u32 a);
#else
static u8 PicoReadM68k8(u32 a)
{
  u32 d=0;

  if ((a&0xe00000)==0xe00000) { d = *(u8 *)(Pico.ram+((a^1)&0xffff)); goto end; } // Ram

  a&=0xffffff;

  if (a < 0x20000) { d = *(u8 *)(Pico_mcd->bios+(a^1)); goto end; } // bios

  // prg RAM
  if ((a&0xfe0000)==0x020000 && (Pico_mcd->m.busreq&2)) {
    u8 *prg_bank = Pico_mcd->prg_ram_b[Pico_mcd->s68k_regs[3]>>6];
    d = *(prg_bank+((a^1)&0x1ffff));
    goto end;
  }

  // word RAM
  if ((a&0xfc0000)==0x200000) {
    wrdprintf("m68k_wram r8: [%06x] @%06x", a, SekPc);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M mode?
      int bank = Pico_mcd->s68k_regs[3]&1;
      if (a >= 0x220000)
           a = (a&3) | (cell_map(a >> 2) << 2); // cell arranged
      else a &= 0x1ffff;
      d = Pico_mcd->word_ram1M[bank][a^1];
    } else {
      // allow access in any mode, like Gens does
      d = Pico_mcd->word_ram2M[(a^1)&0x3ffff];
    }
    wrdprintf("ret = %02x", (u8)d);
    goto end;
  }

  if ((a&0xff4000)==0xa00000) { d=z80Read8(a); goto end; } // Z80 Ram

  if ((a&0xffffc0)==0xa12000)
    rdprintf("m68k_regs r8: [%02x] @%06x", a&0x3f, SekPc);

  d=OtherRead16(a&~1, 8|(a&1)); if ((a&1)==0) d>>=8;

  if ((a&0xffffc0)==0xa12000)
    rdprintf("ret = %02x", (u8)d);

  end:

#ifdef __debug_io
  dprintf("r8 : %06x,   %02x @%06x", a&0xffffff, (u8)d, SekPc);
#endif
  return (u8)d;
}
#endif


#ifdef _ASM_CD_MEMORY_C
u16 PicoReadM68k16(u32 a);
#else
static u16 PicoReadM68k16(u32 a)
{
  u16 d=0;

  if ((a&0xe00000)==0xe00000) { d=*(u16 *)(Pico.ram+(a&0xfffe)); goto end; } // Ram

  a&=0xfffffe;

  if (a < 0x20000) { d = *(u16 *)(Pico_mcd->bios+a); goto end; } // bios

  // prg RAM
  if ((a&0xfe0000)==0x020000 && (Pico_mcd->m.busreq&2)) {
    u8 *prg_bank = Pico_mcd->prg_ram_b[Pico_mcd->s68k_regs[3]>>6];
    wrdprintf("m68k_prgram r16: [%i,%06x] @%06x", Pico_mcd->s68k_regs[3]>>6, a, SekPc);
    d = *(u16 *)(prg_bank+(a&0x1fffe));
    wrdprintf("ret = %04x", d);
    goto end;
  }

  // word RAM
  if ((a&0xfc0000)==0x200000) {
    wrdprintf("m68k_wram r16: [%06x] @%06x", a, SekPc);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M mode?
      int bank = Pico_mcd->s68k_regs[3]&1;
      if (a >= 0x220000)
           a = (a&2) | (cell_map(a >> 2) << 2); // cell arranged
      else a &= 0x1fffe;
      d = *(u16 *)(Pico_mcd->word_ram1M[bank]+a);
    } else {
      // allow access in any mode, like Gens does
      d = *(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe));
    }
    wrdprintf("ret = %04x", d);
    goto end;
  }

  if ((a&0xffffc0)==0xa12000)
    rdprintf("m68k_regs r16: [%02x] @%06x", a&0x3f, SekPc);

  d = (u16)OtherRead16(a, 16);

  if ((a&0xffffc0)==0xa12000)
    rdprintf("ret = %04x", d);

  end:

#ifdef __debug_io
  dprintf("r16: %06x, %04x  @%06x", a&0xffffff, d, SekPc);
#endif
  return d;
}
#endif


#ifdef _ASM_CD_MEMORY_C
u32 PicoReadM68k32(u32 a);
#else
static u32 PicoReadM68k32(u32 a)
{
  u32 d=0;

  if ((a&0xe00000)==0xe00000) { u16 *pm=(u16 *)(Pico.ram+(a&0xfffe)); d = (pm[0]<<16)|pm[1]; goto end; } // Ram

  a&=0xfffffe;

  if (a < 0x20000) { u16 *pm=(u16 *)(Pico_mcd->bios+a); d = (pm[0]<<16)|pm[1]; goto end; } // bios

  // prg RAM
  if ((a&0xfe0000)==0x020000 && (Pico_mcd->m.busreq&2)) {
    u8 *prg_bank = Pico_mcd->prg_ram_b[Pico_mcd->s68k_regs[3]>>6];
    u16 *pm=(u16 *)(prg_bank+(a&0x1fffe));
    d = (pm[0]<<16)|pm[1];
    goto end;
  }

  // word RAM
  if ((a&0xfc0000)==0x200000) {
    wrdprintf("m68k_wram r32: [%06x] @%06x", a, SekPc);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M mode?
      int bank = Pico_mcd->s68k_regs[3]&1;
      if (a >= 0x220000) { // cell arranged
        u32 a1, a2;
        a1 = (a&2) | (cell_map(a >> 2) << 2);
        if (a&2) a2 = cell_map((a+2) >> 2) << 2;
        else     a2 = a1 + 2;
        d  = *(u16 *)(Pico_mcd->word_ram1M[bank]+a1) << 16;
        d |= *(u16 *)(Pico_mcd->word_ram1M[bank]+a2);
      } else {
        u16 *pm=(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe)); d = (pm[0]<<16)|pm[1];
      }
    } else {
      // allow access in any mode, like Gens does
      u16 *pm=(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe)); d = (pm[0]<<16)|pm[1];
    }
    wrdprintf("ret = %08x", d);
    goto end;
  }

  if ((a&0xffffc0)==0xa12000)
    rdprintf("m68k_regs r32: [%02x] @%06x", a&0x3f, SekPc);

  d = (OtherRead16(a, 32)<<16)|OtherRead16(a+2, 32);

  if ((a&0xffffc0)==0xa12000)
    rdprintf("ret = %08x", d);

  end:
#ifdef __debug_io
  dprintf("r32: %06x, %08x @%06x", a&0xffffff, d, SekPc);
#endif
  return d;
}
#endif


// -----------------------------------------------------------------
//                            Write Ram

#ifdef _ASM_CD_MEMORY_C
void PicoWriteM68k8(u32 a,u8 d);
#else
static void PicoWriteM68k8(u32 a,u8 d)
{
#ifdef __debug_io
  dprintf("w8 : %06x,   %02x @%06x", a&0xffffff, d, SekPc);
#endif
  //if ((a&0xe0ffff)==0xe0a9ba+0x69c)
  //  dprintf("w8 : %06x,   %02x @%06x", a&0xffffff, d, SekPc);


  if ((a&0xe00000)==0xe00000) { // Ram
    *(u8 *)(Pico.ram+((a^1)&0xffff)) = d;
    return;
  }

  a&=0xffffff;

  // prg RAM
  if ((a&0xfe0000)==0x020000 && (Pico_mcd->m.busreq&2)) {
    u8 *prg_bank = Pico_mcd->prg_ram_b[Pico_mcd->s68k_regs[3]>>6];
    *(u8 *)(prg_bank+((a^1)&0x1ffff))=d;
    return;
  }

  // word RAM
  if ((a&0xfc0000)==0x200000) {
    wrdprintf("m68k_wram w8: [%06x] %02x @%06x", a, d, SekPc);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M mode?
      int bank = Pico_mcd->s68k_regs[3]&1;
      if (a >= 0x220000)
           a = (a&3) | (cell_map(a >> 2) << 2); // cell arranged
      else a &= 0x1ffff;
      *(u8 *)(Pico_mcd->word_ram1M[bank]+(a^1))=d;
    } else {
      // allow access in any mode, like Gens does
      *(u8 *)(Pico_mcd->word_ram2M+((a^1)&0x3ffff))=d;
    }
    return;
  }

  if ((a&0xffffc0)==0xa12000)
    rdprintf("m68k_regs w8: [%02x] %02x @%06x", a&0x3f, d, SekPc);

  OtherWrite8(a,d,8);
}
#endif


#ifdef _ASM_CD_MEMORY_C
void PicoWriteM68k16(u32 a,u16 d);
#else
static void PicoWriteM68k16(u32 a,u16 d)
{
#ifdef __debug_io
  dprintf("w16: %06x, %04x", a&0xffffff, d);
#endif
  //  dprintf("w16: %06x, %04x  @%06x", a&0xffffff, d, SekPc);

  if ((a&0xe00000)==0xe00000) { // Ram
    *(u16 *)(Pico.ram+(a&0xfffe))=d;
    return;
  }

  a&=0xfffffe;

  // prg RAM
  if ((a&0xfe0000)==0x020000 && (Pico_mcd->m.busreq&2)) {
    u8 *prg_bank = Pico_mcd->prg_ram_b[Pico_mcd->s68k_regs[3]>>6];
    wrdprintf("m68k_prgram w16: [%i,%06x] %04x @%06x", Pico_mcd->s68k_regs[3]>>6, a, d, SekPc);
    *(u16 *)(prg_bank+(a&0x1fffe))=d;
    return;
  }

  // word RAM
  if ((a&0xfc0000)==0x200000) {
    wrdprintf("m68k_wram w16: [%06x] %04x @%06x", a, d, SekPc);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M mode?
      int bank = Pico_mcd->s68k_regs[3]&1;
      if (a >= 0x220000)
           a = (a&2) | (cell_map(a >> 2) << 2); // cell arranged
      else a &= 0x1fffe;
      *(u16 *)(Pico_mcd->word_ram1M[bank]+a)=d;
    } else {
      // allow access in any mode, like Gens does
      *(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe))=d;
    }
    return;
  }

  // regs
  if ((a&0xffffc0)==0xa12000) {
    rdprintf("m68k_regs w16: [%02x] %04x @%06x", a&0x3f, d, SekPc);
    if (a == 0xe) { // special case, 2 byte writes would be handled differently
      Pico_mcd->s68k_regs[0xe] = d >> 8;
#ifdef USE_POLL_DETECT
      if ((s68k_poll_adclk&0xfe) == 0xe && s68k_poll_cnt > POLL_LIMIT) {
        SekSetStopS68k(0); s68k_poll_adclk = -1;
        //printf("%05i:%03i: s68k poll release, a=%02x\n", Pico.m.frame_count, Pico.m.scanline, a);
      }
#endif
      return;
    }
    m68k_reg_write8(a,  d>>8);
    m68k_reg_write8(a+1,d&0xff);
    return;
  }

  OtherWrite16(a,d);
}
#endif


#ifdef _ASM_CD_MEMORY_C
void PicoWriteM68k32(u32 a,u32 d);
#else
static void PicoWriteM68k32(u32 a,u32 d)
{
#ifdef __debug_io
  dprintf("w32: %06x, %08x", a&0xffffff, d);
#endif

  if ((a&0xe00000)==0xe00000)
  {
    // Ram:
    u16 *pm=(u16 *)(Pico.ram+(a&0xfffe));
    pm[0]=(u16)(d>>16); pm[1]=(u16)d;
    return;
  }

  a&=0xfffffe;

  // prg RAM
  if ((a&0xfe0000)==0x020000 && (Pico_mcd->m.busreq&2)) {
    u8 *prg_bank = Pico_mcd->prg_ram_b[Pico_mcd->s68k_regs[3]>>6];
    u16 *pm=(u16 *)(prg_bank+(a&0x1fffe));
    pm[0]=(u16)(d>>16); pm[1]=(u16)d;
    return;
  }

  // word RAM
  if ((a&0xfc0000)==0x200000) {
    if (d != 0) // don't log clears
      wrdprintf("m68k_wram w32: [%06x] %08x @%06x", a, d, SekPc);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M mode?
      int bank = Pico_mcd->s68k_regs[3]&1;
      if (a >= 0x220000) { // cell arranged
        u32 a1, a2;
        a1 = (a&2) | (cell_map(a >> 2) << 2);
        if (a&2) a2 = cell_map((a+2) >> 2) << 2;
        else     a2 = a1 + 2;
        *(u16 *)(Pico_mcd->word_ram1M[bank]+a1) = d >> 16;
        *(u16 *)(Pico_mcd->word_ram1M[bank]+a2) = d;
      } else {
        u16 *pm=(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe));
        pm[0]=(u16)(d>>16); pm[1]=(u16)d;
      }
    } else {
      // allow access in any mode, like Gens does
      u16 *pm=(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe));
      pm[0]=(u16)(d>>16); pm[1]=(u16)d;
    }
    return;
  }

  if ((a&0xffffc0)==0xa12000)
    rdprintf("m68k_regs w32: [%02x] %08x @%06x", a&0x3f, d, SekPc);

  OtherWrite16(a,  (u16)(d>>16));
  OtherWrite16(a+2,(u16)d);
}
#endif


// -----------------------------------------------------------------

#ifdef _ASM_CD_MEMORY_C
u8 PicoReadS68k8(u32 a);
#else
static u8 PicoReadS68k8(u32 a)
{
  u32 d=0;

  a&=0xffffff;

  // prg RAM
  if (a < 0x80000) {
    d = *(Pico_mcd->prg_ram+(a^1));
    goto end;
  }

  // regs
  if ((a&0xfffe00) == 0xff8000) {
    a &= 0x1ff;
    rdprintf("s68k_regs r8: [%02x] @ %06x", a, SekPcS68k);
    if (a >= 0x58 && a < 0x68)
         d = gfx_cd_read(a&~1);
    else d = s68k_reg_read16(a&~1);
    if ((a&1)==0) d>>=8;
    rdprintf("ret = %02x", (u8)d);
    goto end;
  }

  // word RAM (2M area)
  if ((a&0xfc0000)==0x080000) { // 080000-0bffff
    // test: batman returns
    wrdprintf("s68k_wram2M r8: [%06x] @%06x", a, SekPcS68k);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M decode mode?
      int bank = !(Pico_mcd->s68k_regs[3]&1);
      d = Pico_mcd->word_ram1M[bank][((a>>1)^1)&0x1ffff];
      if (a&1) d &= 0x0f;
      else d >>= 4;
      dprintf("FIXME: decode");
    } else {
      // allow access in any mode, like Gens does
      d = Pico_mcd->word_ram2M[(a^1)&0x3ffff];
    }
    wrdprintf("ret = %02x", (u8)d);
    goto end;
  }

  // word RAM (1M area)
  if ((a&0xfe0000)==0x0c0000 && (Pico_mcd->s68k_regs[3]&4)) { // 0c0000-0dffff
    int bank;
    wrdprintf("s68k_wram1M r8: [%06x] @%06x", a, SekPcS68k);
//    if (!(Pico_mcd->s68k_regs[3]&4))
//      dprintf("s68k_wram1M FIXME: wrong mode");
    bank = !(Pico_mcd->s68k_regs[3]&1);
    d = Pico_mcd->word_ram1M[bank][(a^1)&0x1ffff];
    wrdprintf("ret = %02x", (u8)d);
    goto end;
  }

  // PCM
  if ((a&0xff8000)==0xff0000) {
    dprintf("s68k_pcm r8: [%06x] @%06x", a, SekPcS68k);
    a &= 0x7fff;
    if (a >= 0x2000)
      d = Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][(a>>1)&0xfff];
    else if (a >= 0x20) {
      a &= 0x1e;
      d = Pico_mcd->pcm.ch[a>>2].addr >> PCM_STEP_SHIFT;
      if (a & 2) d >>= 8;
    }
    dprintf("ret = %02x", (u8)d);
    goto end;
  }

  // bram
  if ((a&0xff0000)==0xfe0000) {
    d = Pico_mcd->bram[(a>>1)&0x1fff];
    goto end;
  }

  dprintf("s68k r8 : %06x,   %02x @%06x", a&0xffffff, (u8)d, SekPcS68k);

  end:

#ifdef __debug_io2
  dprintf("s68k r8 : %06x,   %02x @%06x", a&0xffffff, (u8)d, SekPcS68k);
#endif
  return (u8)d;
}
#endif


//u16 PicoReadS68k16_(u32 a);
#ifdef _ASM_CD_MEMORY_C
u16 PicoReadS68k16(u32 a);
#else
static u16 PicoReadS68k16(u32 a)
{
  u32 d=0;

  a&=0xfffffe;

  // prg RAM
  if (a < 0x80000) {
    wrdprintf("s68k_prgram r16: [%06x] @%06x", a, SekPcS68k);
    d = *(u16 *)(Pico_mcd->prg_ram+a);
    wrdprintf("ret = %04x", d);
    goto end;
  }

  // regs
  if ((a&0xfffe00) == 0xff8000) {
    a &= 0x1fe;
    rdprintf("s68k_regs r16: [%02x] @ %06x", a, SekPcS68k);
    if (a >= 0x58 && a < 0x68)
         d = gfx_cd_read(a);
    else d = s68k_reg_read16(a);
    rdprintf("ret = %04x", d);
    goto end;
  }

  // word RAM (2M area)
  if ((a&0xfc0000)==0x080000) { // 080000-0bffff
    wrdprintf("s68k_wram2M r16: [%06x] @%06x", a, SekPcS68k);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M decode mode?
      int bank = !(Pico_mcd->s68k_regs[3]&1);
      d = Pico_mcd->word_ram1M[bank][((a>>1)^1)&0x1ffff];
      d |= d << 4; d &= ~0xf0;
      dprintf("FIXME: decode");
    } else {
      // allow access in any mode, like Gens does
      d = *(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe));
    }
    wrdprintf("ret = %04x", d);
    goto end;
  }

  // word RAM (1M area)
  if ((a&0xfe0000)==0x0c0000 && (Pico_mcd->s68k_regs[3]&4)) { // 0c0000-0dffff
    int bank;
    wrdprintf("s68k_wram1M r16: [%06x] @%06x", a, SekPcS68k);
//    if (!(Pico_mcd->s68k_regs[3]&4))
//      dprintf("s68k_wram1M FIXME: wrong mode");
    bank = !(Pico_mcd->s68k_regs[3]&1);
    d = *(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe));
    wrdprintf("ret = %04x", d);
    goto end;
  }

  // bram
  if ((a&0xff0000)==0xfe0000) {
    dprintf("FIXME: s68k_bram r16: [%06x] @%06x", a, SekPcS68k);
    a = (a>>1)&0x1fff;
    d = Pico_mcd->bram[a++];		// Gens does little endian here, and so do we..
    d|= Pico_mcd->bram[a++] << 8;	// This is most likely wrong
    dprintf("ret = %04x", d);
    goto end;
  }

  // PCM
  if ((a&0xff8000)==0xff0000) {
    dprintf("FIXME: s68k_pcm r16: [%06x] @%06x", a, SekPcS68k);
    a &= 0x7fff;
    if (a >= 0x2000)
      d = Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][(a>>1)&0xfff];
    else if (a >= 0x20) {
      a &= 0x1e;
      d = Pico_mcd->pcm.ch[a>>2].addr >> PCM_STEP_SHIFT;
      if (a & 2) d >>= 8;
    }
    dprintf("ret = %04x", d);
    goto end;
  }

  dprintf("s68k r16: %06x, %04x  @%06x", a&0xffffff, d, SekPcS68k);

  end:

#ifdef __debug_io2
  dprintf("s68k r16: %06x, %04x  @%06x", a&0xffffff, d, SekPcS68k);
#endif
  return d;
}
#endif


#ifdef _ASM_CD_MEMORY_C
u32 PicoReadS68k32(u32 a);
#else
static u32 PicoReadS68k32(u32 a)
{
  u32 d=0;

  a&=0xfffffe;

  // prg RAM
  if (a < 0x80000) {
    u16 *pm=(u16 *)(Pico_mcd->prg_ram+a);
    d = (pm[0]<<16)|pm[1];
    goto end;
  }

  // regs
  if ((a&0xfffe00) == 0xff8000) {
    a &= 0x1fe;
    rdprintf("s68k_regs r32: [%02x] @ %06x", a, SekPcS68k);
    if (a >= 0x58 && a < 0x68)
         d = (gfx_cd_read(a)<<16)|gfx_cd_read(a+2);
    else d = (s68k_reg_read16(a)<<16)|s68k_reg_read16(a+2);
    rdprintf("ret = %08x", d);
    goto end;
  }

  // word RAM (2M area)
  if ((a&0xfc0000)==0x080000) { // 080000-0bffff
    wrdprintf("s68k_wram2M r32: [%06x] @%06x", a, SekPcS68k);
    if (Pico_mcd->s68k_regs[3]&4) { // 1M decode mode?
      int bank = !(Pico_mcd->s68k_regs[3]&1);
      a >>= 1;
      d  = Pico_mcd->word_ram1M[bank][((a+0)^1)&0x1ffff] << 16;
      d |= Pico_mcd->word_ram1M[bank][((a+1)^1)&0x1ffff];
      d |= d << 4; d &= 0x0f0f0f0f;
    } else {
      // allow access in any mode, like Gens does
      u16 *pm=(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe)); d = (pm[0]<<16)|pm[1];
    }
    wrdprintf("ret = %08x", d);
    goto end;
  }

  // word RAM (1M area)
  if ((a&0xfe0000)==0x0c0000 && (Pico_mcd->s68k_regs[3]&4)) { // 0c0000-0dffff
    int bank;
    wrdprintf("s68k_wram1M r32: [%06x] @%06x", a, SekPcS68k);
//    if (!(Pico_mcd->s68k_regs[3]&4))
//      dprintf("s68k_wram1M FIXME: wrong mode");
    bank = !(Pico_mcd->s68k_regs[3]&1);
    u16 *pm=(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe)); d = (pm[0]<<16)|pm[1];
    wrdprintf("ret = %08x", d);
    goto end;
  }

  // PCM
  if ((a&0xff8000)==0xff0000) {
    dprintf("FIXME: s68k_pcm r32: [%06x] @%06x", a, SekPcS68k);
    a &= 0x7fff;
    if (a >= 0x2000) {
      a >>= 1;
      d  = Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][a&0xfff] << 16;
      d |= Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][(a+1)&0xfff];
    } else if (a >= 0x20) {
      a &= 0x1e;
      if (a & 2) {
        a >>= 2;
        d  = (Pico_mcd->pcm.ch[a].addr >> (PCM_STEP_SHIFT-8)) & 0xff0000;
        d |= (Pico_mcd->pcm.ch[(a+1)&7].addr >> PCM_STEP_SHIFT)   & 0xff;
      } else {
        d = Pico_mcd->pcm.ch[a>>2].addr >> PCM_STEP_SHIFT;
        d = ((d<<16)&0xff0000) | ((d>>8)&0xff); // PCM chip is LE
      }
    }
    dprintf("ret = %08x", d);
    goto end;
  }

  // bram
  if ((a&0xff0000)==0xfe0000) {
    dprintf("FIXME: s68k_bram r32: [%06x] @%06x", a, SekPcS68k);
    a = (a>>1)&0x1fff;
    d = Pico_mcd->bram[a++] << 16;		// middle endian? TODO: verify against Fusion..
    d|= Pico_mcd->bram[a++] << 24;
    d|= Pico_mcd->bram[a++];
    d|= Pico_mcd->bram[a++] << 8;
    dprintf("ret = %08x", d);
    goto end;
  }

  dprintf("s68k r32: %06x, %08x @%06x", a&0xffffff, d, SekPcS68k);

  end:

#ifdef __debug_io2
  dprintf("s68k r32: %06x, %08x @%06x", a&0xffffff, d, SekPcS68k);
#endif
  return d;
}
#endif


#ifndef _ASM_CD_MEMORY_C
/* check: jaguar xj 220 (draws entire world using decode) */
static void decode_write8(u32 a, u8 d, int r3)
{
  u8 *pd = Pico_mcd->word_ram1M[!(r3 & 1)] + (((a>>1)^1)&0x1ffff);
  u8 oldmask = (a&1) ? 0xf0 : 0x0f;

  r3 &= 0x18;
  d  &= 0x0f;
  if (!(a&1)) d <<= 4;

  if (r3 == 8) {
    if ((!(*pd & (~oldmask))) && d) goto do_it;
  } else if (r3 > 8) {
    if (d) goto do_it;
  } else {
    goto do_it;
  }

  return;
do_it:
  *pd = d | (*pd & oldmask);
}


static void decode_write16(u32 a, u16 d, int r3)
{
  u8 *pd = Pico_mcd->word_ram1M[!(r3 & 1)] + (((a>>1)^1)&0x1ffff);

  //if ((a & 0x3ffff) < 0x28000) return;

  r3 &= 0x18;
  d  &= 0x0f0f;
  d  |= d >> 4;

  if (r3 == 8) {
    u8 dold = *pd;
    if (!(dold & 0xf0)) dold |= d & 0xf0;
    if (!(dold & 0x0f)) dold |= d & 0x0f;
    *pd = dold;
  } else if (r3 > 8) {
    u8 dold = *pd;
    if (!(d & 0xf0)) d |= dold & 0xf0;
    if (!(d & 0x0f)) d |= dold & 0x0f;
    *pd = d;
  } else {
    *pd = d;
  }
}
#endif

// -----------------------------------------------------------------

#ifdef _ASM_CD_MEMORY_C
void PicoWriteS68k8(u32 a,u8 d);
#else
static void PicoWriteS68k8(u32 a,u8 d)
{
#ifdef __debug_io2
  dprintf("s68k w8 : %06x,   %02x @%06x", a&0xffffff, d, SekPcS68k);
#endif

  a&=0xffffff;

  // prg RAM
  if (a < 0x80000) {
    u8 *pm=(u8 *)(Pico_mcd->prg_ram+(a^1));
    *pm=d;
    return;
  }

  // regs
  if ((a&0xfffe00) == 0xff8000) {
    a &= 0x1ff;
    rdprintf("s68k_regs w8: [%02x] %02x @ %06x", a, d, SekPcS68k);
    if (a >= 0x58 && a < 0x68)
         gfx_cd_write16(a&~1, (d<<8)|d);
    else s68k_reg_write8(a,d);
    return;
  }

  // word RAM (2M area)
  if ((a&0xfc0000)==0x080000) { // 080000-0bffff
    int r3 = Pico_mcd->s68k_regs[3];
    wrdprintf("s68k_wram2M w8: [%06x] %02x @%06x", a, d, SekPcS68k);
    if (r3 & 4) { // 1M decode mode?
      decode_write8(a, d, r3);
    } else {
      // allow access in any mode, like Gens does
      *(u8 *)(Pico_mcd->word_ram2M+((a^1)&0x3ffff))=d;
    }
    return;
  }

  // word RAM (1M area)
  if ((a&0xfe0000)==0x0c0000 && (Pico_mcd->s68k_regs[3]&4)) { // 0c0000-0dffff
    // Wing Commander tries to write here in wrong mode
    int bank;
    if (d)
      wrdprintf("s68k_wram1M w8: [%06x] %02x @%06x", a, d, SekPcS68k);
//    if (!(Pico_mcd->s68k_regs[3]&4))
//      dprintf("s68k_wram1M FIXME: wrong mode");
    bank = !(Pico_mcd->s68k_regs[3]&1);
    *(u8 *)(Pico_mcd->word_ram1M[bank]+((a^1)&0x1ffff))=d;
    return;
  }

  // PCM
  if ((a&0xff8000)==0xff0000) {
    a &= 0x7fff;
    if (a >= 0x2000)
      Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][(a>>1)&0xfff] = d;
    else if (a < 0x12)
      pcm_write(a>>1, d);
    return;
  }

  // bram
  if ((a&0xff0000)==0xfe0000) {
    Pico_mcd->bram[(a>>1)&0x1fff] = d;
    SRam.changed = 1;
    return;
  }

  dprintf("s68k w8 : %06x,   %02x @%06x", a&0xffffff, d, SekPcS68k);
}
#endif


#ifdef _ASM_CD_MEMORY_C
void PicoWriteS68k16(u32 a,u16 d);
#else
static void PicoWriteS68k16(u32 a,u16 d)
{
#ifdef __debug_io2
  dprintf("s68k w16: %06x, %04x @%06x", a&0xffffff, d, SekPcS68k);
#endif

  a&=0xfffffe;

  // prg RAM
  if (a < 0x80000) {
    wrdprintf("s68k_prgram w16: [%06x] %04x @%06x", a, d, SekPcS68k);
    *(u16 *)(Pico_mcd->prg_ram+a)=d;
    return;
  }

  // regs
  if ((a&0xfffe00) == 0xff8000) {
    a &= 0x1fe;
    rdprintf("s68k_regs w16: [%02x] %04x @ %06x", a, d, SekPcS68k);
    if (a >= 0x58 && a < 0x68)
      gfx_cd_write16(a, d);
    else {
      if (a == 0xe) { // special case, 2 byte writes would be handled differently
        Pico_mcd->s68k_regs[0xf] = d;
        return;
      }
      s68k_reg_write8(a,  d>>8);
      s68k_reg_write8(a+1,d&0xff);
    }
    return;
  }

  // word RAM (2M area)
  if ((a&0xfc0000)==0x080000) { // 080000-0bffff
    int r3 = Pico_mcd->s68k_regs[3];
    wrdprintf("s68k_wram2M w16: [%06x] %04x @%06x", a, d, SekPcS68k);
    if (r3 & 4) { // 1M decode mode?
      decode_write16(a, d, r3);
    } else {
      // allow access in any mode, like Gens does
      *(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe))=d;
    }
    return;
  }

  // word RAM (1M area)
  if ((a&0xfe0000)==0x0c0000 && (Pico_mcd->s68k_regs[3]&4)) { // 0c0000-0dffff
    int bank;
    if (d)
      wrdprintf("s68k_wram1M w16: [%06x] %04x @%06x", a, d, SekPcS68k);
//    if (!(Pico_mcd->s68k_regs[3]&4))
//      dprintf("s68k_wram1M FIXME: wrong mode");
    bank = !(Pico_mcd->s68k_regs[3]&1);
    *(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe))=d;
    return;
  }

  // PCM
  if ((a&0xff8000)==0xff0000) {
    a &= 0x7fff;
    if (a >= 0x2000)
      Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][(a>>1)&0xfff] = d;
    else if (a < 0x12)
      pcm_write(a>>1, d & 0xff);
    return;
  }

  // bram
  if ((a&0xff0000)==0xfe0000) {
    dprintf("s68k_bram w16: [%06x] %04x @%06x", a, d, SekPcS68k);
    a = (a>>1)&0x1fff;
    Pico_mcd->bram[a++] = d;		// Gens does little endian here, an so do we..
    Pico_mcd->bram[a++] = d >> 8;
    SRam.changed = 1;
    return;
  }

  dprintf("s68k w16: %06x, %04x @%06x", a&0xffffff, d, SekPcS68k);
}
#endif


#ifdef _ASM_CD_MEMORY_C
void PicoWriteS68k32(u32 a,u32 d);
#else
static void PicoWriteS68k32(u32 a,u32 d)
{
#ifdef __debug_io2
  dprintf("s68k w32: %06x, %08x @%06x", a&0xffffff, d, SekPcS68k);
#endif

  a&=0xfffffe;

  // prg RAM
  if (a < 0x80000) {
    u16 *pm=(u16 *)(Pico_mcd->prg_ram+a);
    pm[0]=(u16)(d>>16); pm[1]=(u16)d;
    return;
  }

  // regs
  if ((a&0xfffe00) == 0xff8000) {
    a &= 0x1fe;
    rdprintf("s68k_regs w32: [%02x] %08x @ %06x", a, d, SekPcS68k);
    if (a >= 0x58 && a < 0x68) {
      gfx_cd_write16(a,   d>>16);
      gfx_cd_write16(a+2, d&0xffff);
    } else {
      s68k_reg_write8(a,   d>>24);
      s68k_reg_write8(a+1,(d>>16)&0xff);
      s68k_reg_write8(a+2,(d>>8) &0xff);
      s68k_reg_write8(a+3, d     &0xff);
    }
    return;
  }

  // word RAM (2M area)
  if ((a&0xfc0000)==0x080000) { // 080000-0bffff
    int r3 = Pico_mcd->s68k_regs[3];
    wrdprintf("s68k_wram2M w32: [%06x] %08x @%06x", a, d, SekPcS68k);
    if (r3 & 4) { // 1M decode mode?
      decode_write16(a  , d >> 16, r3);
      decode_write16(a+2, d      , r3);
    } else {
      // allow access in any mode, like Gens does
      u16 *pm=(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe));
      pm[0]=(u16)(d>>16); pm[1]=(u16)d;
    }
    return;
  }

  // word RAM (1M area)
  if ((a&0xfe0000)==0x0c0000 && (Pico_mcd->s68k_regs[3]&4)) { // 0c0000-0dffff
    int bank;
    u16 *pm;
    if (d)
      wrdprintf("s68k_wram1M w32: [%06x] %08x @%06x", a, d, SekPcS68k);
//    if (!(Pico_mcd->s68k_regs[3]&4))
//      dprintf("s68k_wram1M FIXME: wrong mode");
    bank = !(Pico_mcd->s68k_regs[3]&1);
    pm=(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe));
    pm[0]=(u16)(d>>16); pm[1]=(u16)d;
    return;
  }

  // PCM
  if ((a&0xff8000)==0xff0000) {
    a &= 0x7fff;
    if (a >= 0x2000) {
      a >>= 1;
      Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][a&0xfff] = (d >> 16);
      Pico_mcd->pcm_ram_b[Pico_mcd->pcm.bank][(a+1)&0xfff] = d;
    } else if (a < 0x12) {
      a >>= 1;
      pcm_write(a,  (d>>16) & 0xff);
      pcm_write(a+1, d & 0xff);
    }
    return;
  }

  // bram
  if ((a&0xff0000)==0xfe0000) {
    dprintf("s68k_bram w32: [%06x] %08x @%06x", a, d, SekPcS68k);
    a = (a>>1)&0x1fff;
    Pico_mcd->bram[a++] = d >> 16;		// middle endian? verify?
    Pico_mcd->bram[a++] = d >> 24;
    Pico_mcd->bram[a++] = d;
    Pico_mcd->bram[a++] = d >> 8;
    SRam.changed = 1;
    return;
  }

  dprintf("s68k w32: %06x, %08x @%06x", a&0xffffff, d, SekPcS68k);
}
#endif


// -----------------------------------------------------------------


#if defined(EMU_C68K)
static __inline int PicoMemBaseM68k(u32 pc)
{
  if ((pc&0xe00000)==0xe00000)
    return (int)Pico.ram-(pc&0xff0000); // Program Counter in Ram

  if (pc < 0x20000)
    return (int)Pico_mcd->bios; // Program Counter in BIOS

  if ((pc&0xfc0000)==0x200000)
  {
    if (!(Pico_mcd->s68k_regs[3]&4))
      return (int)Pico_mcd->word_ram2M - 0x200000; // Program Counter in Word Ram
    if (pc < 0x220000) {
      int bank = (Pico_mcd->s68k_regs[3]&1);
      return (int)Pico_mcd->word_ram1M[bank] - 0x200000;
    }
  }

  // Error - Program Counter is invalid
  dprintf("m68k FIXME: unhandled jump to %06x", pc);

  return (int)Pico_mcd->bios;
}


static u32 PicoCheckPcM68k(u32 pc)
{
  pc-=PicoCpu.membase; // Get real pc
  pc&=0xfffffe;

  PicoCpu.membase=PicoMemBaseM68k(pc);

  return PicoCpu.membase+pc;
}


static __inline int PicoMemBaseS68k(u32 pc)
{
  if (pc < 0x80000)                     // PRG RAM
    return (int)Pico_mcd->prg_ram;

  if ((pc&0xfc0000)==0x080000)          // WORD RAM 2M area (assume we are in the right mode..)
    return (int)Pico_mcd->word_ram2M - 0x080000;

  if ((pc&0xfe0000)==0x0c0000) {        // word RAM 1M area
    int bank = !(Pico_mcd->s68k_regs[3]&1);
    return (int)Pico_mcd->word_ram1M[bank] - 0x0c0000;
  }

  // Error - Program Counter is invalid
  dprintf("s68k FIXME: unhandled jump to %06x", pc);

  return (int)Pico_mcd->prg_ram;
}


static u32 PicoCheckPcS68k(u32 pc)
{
  pc-=PicoCpuS68k.membase; // Get real pc
  pc&=0xfffffe;

  PicoCpuS68k.membase=PicoMemBaseS68k(pc);

  return PicoCpuS68k.membase+pc;
}
#endif


void PicoMemSetupCD()
{
  dprintf("PicoMemSetupCD()");
#ifdef EMU_C68K
  // Setup m68k memory callbacks:
  PicoCpu.checkpc=PicoCheckPcM68k;
  PicoCpu.fetch8 =PicoCpu.read8 =PicoReadM68k8;
  PicoCpu.fetch16=PicoCpu.read16=PicoReadM68k16;
  PicoCpu.fetch32=PicoCpu.read32=PicoReadM68k32;
  PicoCpu.write8 =PicoWriteM68k8;
  PicoCpu.write16=PicoWriteM68k16;
  PicoCpu.write32=PicoWriteM68k32;
  // s68k
  PicoCpuS68k.checkpc=PicoCheckPcS68k;
  PicoCpuS68k.fetch8 =PicoCpuS68k.read8 =PicoReadS68k8;
  PicoCpuS68k.fetch16=PicoCpuS68k.read16=PicoReadS68k16;
  PicoCpuS68k.fetch32=PicoCpuS68k.read32=PicoReadS68k32;
  PicoCpuS68k.write8 =PicoWriteS68k8;
  PicoCpuS68k.write16=PicoWriteS68k16;
  PicoCpuS68k.write32=PicoWriteS68k32;
#endif
  // m68k_poll_addr = m68k_poll_cnt = 0;
  s68k_poll_adclk = s68k_poll_cnt = 0;
}


#ifdef EMU_M68K
unsigned char  PicoReadCD8w (unsigned int a) {
	return m68ki_cpu_p == &PicoS68kCPU ? PicoReadS68k8(a) : PicoReadM68k8(a);
}
unsigned short PicoReadCD16w(unsigned int a) {
	return m68ki_cpu_p == &PicoS68kCPU ? PicoReadS68k16(a) : PicoReadM68k16(a);
}
unsigned int   PicoReadCD32w(unsigned int a) {
	return m68ki_cpu_p == &PicoS68kCPU ? PicoReadS68k32(a) : PicoReadM68k32(a);
}
void PicoWriteCD8w (unsigned int a, unsigned char d) {
	if (m68ki_cpu_p == &PicoS68kCPU) PicoWriteS68k8(a, d); else PicoWriteM68k8(a, d);
}
void PicoWriteCD16w(unsigned int a, unsigned short d) {
	if (m68ki_cpu_p == &PicoS68kCPU) PicoWriteS68k16(a, d); else PicoWriteM68k16(a, d);
}
void PicoWriteCD32w(unsigned int a, unsigned int d) {
	if (m68ki_cpu_p == &PicoS68kCPU) PicoWriteS68k32(a, d); else PicoWriteM68k32(a, d);
}

// these are allowed to access RAM
unsigned int  m68k_read_pcrelative_CD8 (unsigned int a) {
  a&=0xffffff;
  if(m68ki_cpu_p == &PicoS68kCPU) {
    if (a < 0x80000) return *(u8 *)(Pico_mcd->prg_ram+(a^1)); // PRG Ram
    if ((a&0xfc0000)==0x080000 && !(Pico_mcd->s68k_regs[3]&4)) // word RAM (2M area: 080000-0bffff)
      return *(u8 *)(Pico_mcd->word_ram2M+((a^1)&0x3ffff));
    if ((a&0xfe0000)==0x0c0000 &&  (Pico_mcd->s68k_regs[3]&4)) { // word RAM (1M area: 0c0000-0dffff)
      int bank = !(Pico_mcd->s68k_regs[3]&1);
      return *(u8 *)(Pico_mcd->word_ram1M[bank]+((a^1)&0x1ffff));
    }
    dprintf("s68k_read_pcrelative_CD8 FIXME: can't handle %06x", a);
  } else {
    if((a&0xe00000)==0xe00000) return *(u8 *)(Pico.ram+((a^1)&0xffff)); // Ram
    if(a<0x20000)              return *(u8 *)(Pico.rom+(a^1)); // Bios
    if((a&0xfc0000)==0x200000) { // word RAM
      if(!(Pico_mcd->s68k_regs[3]&4)) // 2M?
        return *(u8 *)(Pico_mcd->word_ram2M+((a^1)&0x3ffff));
      else if (a < 0x220000) {
        int bank = Pico_mcd->s68k_regs[3]&1;
        return *(u8 *)(Pico_mcd->word_ram1M[bank]+((a^1)&0x1ffff));
      }
    }
    dprintf("m68k_read_pcrelative_CD8 FIXME: can't handle %06x", a);
  }
  return 0;//(u8)  lastread_d;
}
unsigned int  m68k_read_pcrelative_CD16(unsigned int a) {
  a&=0xffffff;
  if(m68ki_cpu_p == &PicoS68kCPU) {
    if (a < 0x80000) return *(u16 *)(Pico_mcd->prg_ram+(a&~1)); // PRG Ram
    if ((a&0xfc0000)==0x080000 && !(Pico_mcd->s68k_regs[3]&4)) // word RAM (2M area: 080000-0bffff)
      return *(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe));
    if ((a&0xfe0000)==0x0c0000 &&  (Pico_mcd->s68k_regs[3]&4)) { // word RAM (1M area: 0c0000-0dffff)
      int bank = !(Pico_mcd->s68k_regs[3]&1);
      return *(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe));
    }
    dprintf("s68k_read_pcrelative_CD16 FIXME: can't handle %06x", a);
  } else {
    if((a&0xe00000)==0xe00000) return *(u16 *)(Pico.ram+(a&0xfffe)); // Ram
    if(a<0x20000)              return *(u16 *)(Pico.rom+(a&~1)); // Bios
    if((a&0xfc0000)==0x200000) { // word RAM
      if(!(Pico_mcd->s68k_regs[3]&4)) // 2M?
        return *(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe));
      else if (a < 0x220000) {
        int bank = Pico_mcd->s68k_regs[3]&1;
        return *(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe));
      }
    }
    dprintf("m68k_read_pcrelative_CD16 FIXME: can't handle %06x", a);
  }
  return 0;
}
unsigned int  m68k_read_pcrelative_CD32(unsigned int a) {
  u16 *pm;
  a&=0xffffff;
  if(m68ki_cpu_p == &PicoS68kCPU) {
    if (a < 0x80000) { u16 *pm=(u16 *)(Pico_mcd->prg_ram+(a&~1)); return (pm[0]<<16)|pm[1]; } // PRG Ram
    if ((a&0xfc0000)==0x080000 && !(Pico_mcd->s68k_regs[3]&4)) // word RAM (2M area: 080000-0bffff)
      { pm=(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe)); return (pm[0]<<16)|pm[1]; }
    if ((a&0xfe0000)==0x0c0000 &&  (Pico_mcd->s68k_regs[3]&4)) { // word RAM (1M area: 0c0000-0dffff)
      int bank = !(Pico_mcd->s68k_regs[3]&1);
      pm=(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe));
      return (pm[0]<<16)|pm[1];
    }
    dprintf("s68k_read_pcrelative_CD32 FIXME: can't handle %06x", a);
  } else {
    if((a&0xe00000)==0xe00000) { u16 *pm=(u16 *)(Pico.ram+(a&0xfffe)); return (pm[0]<<16)|pm[1]; } // Ram
    if(a<0x20000)              { u16 *pm=(u16 *)(Pico.rom+(a&~1));     return (pm[0]<<16)|pm[1]; }
    if((a&0xfc0000)==0x200000) { // word RAM
      if(!(Pico_mcd->s68k_regs[3]&4)) // 2M?
        { pm=(u16 *)(Pico_mcd->word_ram2M+(a&0x3fffe)); return (pm[0]<<16)|pm[1]; }
      else if (a < 0x220000) {
        int bank = Pico_mcd->s68k_regs[3]&1;
        pm=(u16 *)(Pico_mcd->word_ram1M[bank]+(a&0x1fffe));
        return (pm[0]<<16)|pm[1];
      }
    }
    dprintf("m68k_read_pcrelative_CD32 FIXME: can't handle %06x", a);
  }
  return 0;
}
#endif // EMU_M68K

