/* ka10_cpu.c: PDP-10 CPU simulator

   Copyright (c) 2013, Richard Cornwell

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Richard Cornwell shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   cpu          KA10 central processor


   The 36b system family had six different implementions: PDP-6, KA10, KI10,
   L10, KL10 extended, and KS10.

   The register state for the KS10 is:

   AC[16]                       accumulators
   PC                           program counter
   flags<0:11>                  state flags
   pi_enb<1:7>                  enabled PI levels
   pi_act<1:7>                  active PI levels
   pi_prq<1:7>                  program PI requests
   apr_enb<0:7>                 enabled system flags
   apr_flg<0:7>                 system flags

   The PDP-10 had just two instruction formats: memory reference
   and I/O.

    000000000 0111 1 1111 112222222222333333
    012345678 9012 3 4567 890123456789012345
   +---------+----+-+----+------------------+
   |  opcode | ac |i| idx|     address      | memory reference
   +---------+----+-+----+------------------+

    000 0000000 111 1 1111 112222222222333333
    012 3456789 012 3 4567 890123456789012345
   +---+-------+---+-+----+------------------+
   |111|device |iop|i| idx|     address      | I/O
   +---+-------+---+-+----+------------------+

   This routine is the instruction decode routine for the PDP-10.
   It is called from the simulator control program to execute
   instructions in simulated memory, starting at the simulated PC.
   It runs until an abort occurs.

   General notes:

   1. Reasons to stop.  The simulator can be stopped by:

        HALT instruction
        MUUO instruction in executive mode
        pager error in interrupt sequence
        invalid vector table in interrupt sequence
        illegal instruction in interrupt sequence
        breakpoint encountered
        nested indirects exceeding limit
        nested XCT's exceeding limit
        I/O error in I/O simulator

   2. Interrupts.  PDP-10's have a seven level priority interrupt
      system.  Interrupt requests can come from internal sources,
      such as APR program requests, or external sources, such as
      I/O devices.  The requests are stored in pi_prq for program
      requests, pi_apr for other internal flags, and pi_ioq for
      I/O device flags.  Internal and device (but not program)
      interrupts must be enabled on a level by level basis.  When
      an interrupt is granted on a level, interrupts at that level
      and below are masked until the interrupt is dismissed.

      The I/O device interrupt system is taken from the PDP-11.
      int_req stores the interrupt requests for Unibus I/O devices.
      Routines in the Unibus adapter map requests in int_req to
      PDP-10 levels.  The Unibus adapter also calculates which
      device to get a vector from when a PDP-10 interrupt is granted.

   3. Arithmetic.  The PDP-10 is a 2's complement system.

   4. Adding I/O devices.  These modules must be modified:

        pdp10_defs.h    add device address and interrupt definitions
        pdp10_sys.c     add sim_devices table entry

   A note on ITS 1-proceed.  The simulator follows the implementation
   on the KS10, keeping 1-proceed as a side flag (its_1pr) rather than
   as flags<8>.  This simplifies the flag saving instructions, which
   don't have to clear flags<8> before saving it.  Instead, the page
   fail and interrupt code must restore flags<8> from its_1pr.  Unlike
   the KS10, the simulator will not lose the 1-proceed trap if the
   1-proceeded instructions clears 1-proceed.
*/

#include "ka10_defs.h"
#include "sim_timer.h"
#include <time.h>

#define HIST_PC         0x40000000
#define HIST_MIN        64
#define HIST_MAX        65536
#define TMR_RTC         1

#define UNIT_V_MSIZE    (UNIT_V_UF + 0)
#if KI10
#define UNIT_MSIZE      (0177 << UNIT_V_MSIZE)
#else
#define UNIT_MSIZE      (017 << UNIT_V_MSIZE)
#endif
#define UNIT_V_TWOSEG   (UNIT_V_MSIZE + 8)
#define UNIT_TWOSEG     (1 << UNIT_V_TWOSEG)


uint64  M[MAXMEMSIZE];                        /* Memory */
#if KI
uint64  FM[64];                               /* Fast memory register */
#else
uint64  FM[16];                               /* Fast memory register */
#endif
uint64  AR;                                   /* Primary work register */
uint64  MQ;                                   /* Extension to AR */
uint64  BR;                                   /* Secondary operand */
uint64  AD;                                   /* Address Data */
uint64  MB;                                   /* Memory Bufer Register */
uint32  AB;                                   /* Memory address buffer */
uint32  PC;                                   /* Program counter */
uint32  IR;                                   /* Instruction register */
uint32  FLAGS;                                /* Flags */
uint32  AC;                                   /* Operand accumulator */
int     BYF5;                                 /* Second half of LDB/DPB instruction */
int     uuo_cycle;                            /* Uuo cycle in progress */
int     sac_inh;                              /* Don't store AR in AC */
int     SC;                                   /* Shift count */
int     SCAD;                                 /* Shift count extension */
int     FE;                                   /* Exponent */
#if !KI
int     Pl, Ph, Rl, Rh, Pflag;                /* Protection registers */
#endif
char    push_ovf;                             /* Push stack overflow */
char    mem_prot;                             /* Memory protection flag */
char    nxm_flag;                             /* Non-existant memory flag */
char    clk_flg;                              /* Clock flag */
char    ov_irq;                               /* Trap overflow */
char    fov_irq;                              /* Trap floating overflow */
char    PIR;                                  /* Current priority level */
char    PIH;                                  /* Highest priority */
char    PIE;                                  /* Priority enable mask */
char    pi_enable;                            /* Interrupts enabled */
char    parity_irq;                           /* Parity interupt */
char    pi_pending;                           /* Interrupt pending. */
int     pi_req;                               /* Current interrupt request */
int     pi_enc;                               /* Flag for pi */
int     apr_irq;                              /* Apr Irq level */
char    clk_en;                               /* Enable clock interrupts */
int     clk_irq;                              /* Clock interrupt */
char    pi_restore;                           /* Restore previous level */
char    pi_hold;                              /* Hold onto interrupt */
#if KI
uint64  ARX;                                  /* Extension to AR */
uint64  BRX;                                  /* Extension to BR */
uint64  ADX;                                  /* Extension to AD */
uint32  ub_ptr;                               /* User base pointer */
uint32  eb_ptr;                               /* Executive base pointer */
uint8   fm_sel;                               /* User fast memory block */
char    small_user;                           /* Small user flag */
char    user_addr_cmp;                        /* User address compare flag */
char    page_enable;                          /* Enable paging */
char    xct_flag;                             /* XCT flags */
uint32  ac_stack;                             /* Register stack pointer */
uint32  pag_reload;                           /* Page reload pointer */
char    inout_fail;                           /* In out fail flag */
int     modify;                               /* Modify cycle */
#endif

char    dev_irq[128];                         /* Pending irq by device */
t_stat  (*dev_tab[128])(uint32 dev, uint64 *data);
t_stat  rtc_srv(UNIT * uptr);
int32   rtc_tps = 60;
int32   tmxr_poll = 10000;

typedef struct {
    uint32      pc;
    uint32      ea;
    uint64      ir;
    uint64      ac;
    uint32      flags;
    uint64      mb;
    uint64      fmb;
    } InstHistory;

int32 hst_p = 0;                                        /* history pointer */
int32 hst_lnt = 0;                                      /* history length */
InstHistory *hst = NULL;                                /* instruction history */

/* Forward and external declarations */

t_stat cpu_ex (t_value *vptr, t_addr addr, UNIT *uptr, int32 sw);
t_stat cpu_dep (t_value val, t_addr addr, UNIT *uptr, int32 sw);
t_stat cpu_reset (DEVICE *dptr);
t_stat cpu_set_size (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat cpu_set_hist (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat cpu_show_hist (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat cpu_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag,
                     const char *cptr);
const char          *cpu_description (DEVICE *dptr);
void set_ac_display (uint64 *acbase);

t_bool build_dev_tab (void);

/* CPU data structures

   cpu_dev      CPU device descriptor
   cpu_unit     CPU unit
   cpu_reg      CPU register list
   cpu_mod      CPU modifier list
*/

UNIT cpu_unit = { UDATA (&rtc_srv, UNIT_FIX|UNIT_TWOSEG, MAXMEMSIZE) };

REG cpu_reg[] = {
    { ORDATA (PC, PC, 18) },
    { ORDATA (FLAGS, FLAGS, 18) },
    { ORDATA (FM0, FM[00], 36) },                       /* addr in memory */
    { ORDATA (FM1, FM[01], 36) },                       /* modified at exit */
    { ORDATA (FM2, FM[02], 36) },                       /* to SCP */
    { ORDATA (FM3, FM[03], 36) },
    { ORDATA (FM4, FM[04], 36) },
    { ORDATA (FM5, FM[05], 36) },
    { ORDATA (FM6, FM[06], 36) },
    { ORDATA (FM7, FM[07], 36) },
    { ORDATA (FM10, FM[010], 36) },
    { ORDATA (FM11, FM[011], 36) },
    { ORDATA (FM12, FM[012], 36) },
    { ORDATA (FM13, FM[013], 36) },
    { ORDATA (FM14, FM[014], 36) },
    { ORDATA (FM15, FM[015], 36) },
    { ORDATA (FM16, FM[016], 36) },
    { ORDATA (FM17, FM[017], 36) },
    { ORDATA (PIENB, pi_enable, 7) },
    { BRDATA (REG, FM, 8, 36, 017) },
    { NULL }
    };

MTAB cpu_mod[] = {
    { MTAB_XTD|MTAB_VDV, 0, "IDLE", "IDLE", &sim_set_idle, &sim_show_idle },
    { MTAB_XTD|MTAB_VDV, 0, NULL, "NOIDLE", &sim_clr_idle, NULL },
    { UNIT_MSIZE, 1, "16K", "16K", &cpu_set_size },
    { UNIT_MSIZE, 2, "32K", "32K", &cpu_set_size },
    { UNIT_MSIZE, 4, "64K", "64K", &cpu_set_size },
    { UNIT_MSIZE, 8, "128K", "128K", &cpu_set_size },
    { UNIT_MSIZE, 12, "196K", "196K", &cpu_set_size },
    { UNIT_MSIZE, 16, "256K", "256K", &cpu_set_size },
#if KI
    { UNIT_MSIZE, 32, "512K", "512K", &cpu_set_size },
    { UNIT_MSIZE, 64, "1024K", "1024K", &cpu_set_size },
    { UNIT_MSIZE, 128, "2048K", "2048K", &cpu_set_size },
#endif
#if !KI
    { UNIT_TWOSEG, 0, "ONESEG", "ONESEG", NULL, NULL, NULL},
    { UNIT_TWOSEG, UNIT_TWOSEG, "TWOSEG", "TWOSEG", NULL, NULL, NULL},
#endif
    { MTAB_XTD|MTAB_VDV|MTAB_NMO|MTAB_SHP, 0, "HISTORY", "HISTORY",
      &cpu_set_hist, &cpu_show_hist },
    { 0 }
    };

/* Simulator debug controls */
DEBTAB              cpu_debug[] = {
    {"IRQ", DEBUG_IRQ, "Debug IRQ requests"},
    {0, 0}
};

DEVICE cpu_dev = {
    "CPU", &cpu_unit, cpu_reg, cpu_mod,
    1, 8, 18, 1, 8, 36,
    &cpu_ex, &cpu_dep, &cpu_reset,
    NULL, NULL, NULL, NULL, DEV_DEBUG, 0, cpu_debug,
    NULL, NULL, &cpu_help, NULL, NULL, &cpu_description
    };

/* Data arrays */
#define FCE     000001   /* Fetch memory into AR */
#define FCEPSE  000002   /* Fetch and store memory into AR */
#define SCE     000004   /* Save AR into memory */
#define FAC     000010   /* Fetch AC into AR */
#define FAC2    000020   /* Fetch AC+1 into MQ */
#define SAC     000040   /* Save AC into AR */
#define SACZ    000100   /* Save AC into AR if AC not 0 */
#define SAC2    000200   /* Save MQ into AC+1 */
#define MBR     000400   /* Load Mem to BR, AC to AR */
#define SWAR    001000   /* Swap AR */
#define FBR     002000   /* Load AC into BR */
#define FMB     004000   /* Load MB into BR */

int opflags[] = {
        /* UUO00 */       /* LUUO01 */      /* LUUO02 */    /* LUUO03 */
        0,                0,                0,              0,
        /* LUUO04 */      /* LUUO05 */      /* LUUO06 */    /* LUUO07 */
        0,                0,                0,              0,
        /* LUUO10 */      /* LUUO11 */      /* LUUO12 */    /* LUUO13 */
        0,                0,                0,              0,
        /* LUUO14 */      /* LUUO15 */      /* LUUO16 */    /* LUUO17 */
        0,                0,                0,              0,
        /* LUUO20 */      /* LUUO21 */      /* LUUO22 */    /* LUUO23 */
        0,                0,                0,              0,
        /* LUUO24 */      /* LUUO25 */      /* LUUO26 */    /* LUUO27 */
        0,                0,                0,              0,
        /* LUUO30 */      /* LUUO31 */      /* LUUO32 */    /* LUUO33 */
        0,                0,                0,              0,
        /* LUUO34 */      /* LUUO35 */      /* LUUO36 */    /* LUUO37 */
        0,                0,                0,              0,
        /* MUUO40 */      /* MUUO41 */      /* MUUO42 */    /* MUUO43 */
        0,                0,                0,              0,
        /* MUUO44 */      /* MUUO45 */      /* MUUO46 */    /* MUUO47 */
        0,                0,                0,              0,
        /* MUUO50 */      /* MUUO51 */      /* MUUO52 */    /* MUUO53 */
        0,                0,                0,              0,
        /* MUUO54 */      /* MUUO55 */      /* MUUO56 */    /* MUUO57 */
        0,                0,                0,              0,
        /* MUUO60 */      /* MUUO61 */      /* MUUO62 */    /* MUUO63 */
        0,                0,                0,              0,
        /* MUUO64 */      /* MUUO65 */      /* MUUO66 */    /* MUUO67 */
        0,                0,                0,              0,
        /* MUUO70 */      /* MUUO71 */      /* MUUO72 */    /* MUUO73 */
        0,                0,                0,              0,
        /* MUUO74 */      /* MUUO75 */      /* MUUO76 */    /* MUUO77 */
        0,                0,                0,              0,
        /* UJEN */        /* UUO101 */      /* GFAD */      /* GFSB */
        0,                0,                0,              0,
        /* JSYS */        /* ADJSP */       /* GFMP */      /*GFDV */
        0,                0,                0,              0,
#if KI
        /* DFAD */        /* DFSB */        /* DFMP */      /* DFDV */
        FCE|FAC|FAC2|SAC|SAC2, FCE|FAC|FAC2|SAC|SAC2,
                                FCE|FAC|FAC2|SAC|SAC2, FCE|FAC|FAC2|SAC|SAC2,
        /* DADD */        /* DSUB */        /* DMUL */      /* DDIV */
        0,                0,                0,              0,
        /* DMOVE */       /* DMOVN */       /* FIX */       /* EXTEND */
        FCE|SAC|SAC2,     FCE|SAC|SAC2,     FCE|SAC,        0,
        /* DMOVEM */      /* DMOVNM */      /* FIXR */      /* FLTR */
        FAC|FAC2,         FAC|FAC2,         FCE|SAC,        FCE|SAC,
#else
        /* DFAD */        /* DFSB */        /* DFMP */      /* DFDV */
        0,                0,                0,              0,
        /* DADD */        /* DSUB */        /* DMUL */      /* DDIV */
        0,                0,                0,              0,
        /* DMOVE */       /* DMOVN */       /* FIX */       /* EXTEND */
        0,                0,                0,              0,
        /* DMOVEM */      /* DMOVNM */      /* FIXR */      /* FLTR */
        0,                0,                0,              0,
#endif
        /* UFA */         /* DFN */         /* FSC */       /* IBP */
        FCE|FBR,          FCE|FAC,          FAC|SAC,        FCEPSE,
        /* ILDB */        /* LDB */         /* IDPB */      /* DPB */
        FCEPSE,           FCE,              FCEPSE,         FCE,
        /* FAD */         /* FADL */        /* FADM */      /* FADB */
        SAC|FBR|FCE,      SAC|SAC2|FBR|FCE, FCEPSE|FBR,     SAC|FBR|FCEPSE,
        /* FADR */        /* FADRI */       /* FADRM */     /* FADRB */
        SAC|FBR|FCE,      SAC|FBR|SWAR,     FCEPSE|FBR,     SAC|FBR|FCEPSE,
        /* FSB */         /* FSBL */        /* FSBM */      /* FSBB */
        SAC|FBR|FCE,      SAC|SAC2|FBR|FCE, FCEPSE|FBR,     SAC|FBR|FCEPSE,
        /* FSBR */        /* FSBRI */       /* FSBRM */     /* FSBRB */
        SAC|FBR|FCE,      SAC|FBR|SWAR,     FCEPSE|FBR,     SAC|FBR|FCEPSE,
        /* FMP */         /* FMPL */        /* FMPM */      /* FMPB */
        SAC|FBR|FCE,      SAC|SAC2|FBR|FCE, FCEPSE|FBR,     SAC|FBR|FCEPSE,
        /* FMPR */        /* FMPRI */       /* FMPRM */     /* FMPRB */
        SAC|FBR|FCE,      SAC|FBR|SWAR,     FCEPSE|FBR,     SAC|FBR|FCEPSE,
        /* FDV */         /* FDVL */        /* FDVM */      /* FDVB */
        SAC|FBR|FCE,      FAC2|SAC2|SAC|FBR|FCE, FCEPSE|FBR, SAC|FBR|FCEPSE,
        /* FDVR */        /* FDVRI */       /* FDVRM */     /* FDVRB */
        SAC|FBR|FCE,      SAC|FBR|SWAR,     FCEPSE|FBR,     SAC|FBR|FCEPSE,

        /* MOVE */        /* MOVEI */       /* MOVEM */     /* MOVES */
        SAC|FCE,          SAC,              FAC|SCE,        SACZ|FCEPSE,
        /* MOVS */        /* MOVSI */       /* MOVSM */     /* MOVSS */
        SWAR|SAC|FCE,     SWAR|SAC,         SWAR|FAC|SCE,   SWAR|SACZ|FCEPSE,
        /* MOVN */        /* MOVNI */       /* MOVNM */     /* MOVNS */
        SAC|FCE,          SAC,              FAC|SCE,        SACZ|FCEPSE,
        /* MOVM */        /* MOVMI */       /* MOVMM */     /* MOVMS */
        SAC|FCE,          SAC,              FAC|SCE,        SACZ|FCEPSE,
        /* IMUL */        /* IMULI */       /* IMULM */     /* IMULB */
        SAC|FCE|FBR,      SAC|FBR,          FCEPSE|FBR,     SAC|FCEPSE|FBR,
        /* MUL */         /* MULI */        /* MULM */      /* MULB */
        SAC2|SAC|FCE|FBR, SAC2|SAC|FBR,     FCEPSE|FBR,     SAC2|SAC|FCEPSE|FBR,
        /* IDIV */        /* IDIVI */       /* IDIVM */     /* IDIVB */
        SAC2|SAC|FCE|FAC, SAC2|SAC|FAC,     FCEPSE|FAC,     SAC2|SAC|FCEPSE|FAC,
        /* DIV */         /* DIVI */        /* DIVM */      /* DIVB */
        SAC2|SAC|FCE|FAC, SAC2|SAC|FAC,     FCEPSE|FAC,     SAC2|SAC|FCEPSE|FAC,
        /* ASH */         /* ROT */         /* LSH */       /* JFFO */
        FAC|SAC,          FAC|SAC,          FAC|SAC,        FAC,
        /* ASHC */        /* ROTC */        /* LSHC */      /* UUO247 */
        FAC|SAC|SAC2|FAC2, FAC|SAC|SAC2|FAC2, FAC|SAC|SAC2|FAC2,  0,

        /* EXCH */        /* BLT */         /* AOBJP */     /* AOBJN */
        FAC|FCEPSE,       FAC,              FAC|SAC,        FAC|SAC,
        /* JRST */        /* JFCL */        /* XCT */       /* MAP */
        0,                0,                0,              SAC,
        /* PUSHJ */       /* PUSH */        /* POP */       /* POPJ */
        FAC|SAC,          FAC|FCE|SAC,      FAC|SAC,        FAC|SAC,
        /* JSR */         /* JSP */         /* JSA */       /* JRA */
        SCE,              SAC,              FBR|SCE,        0,
        /* ADD */         /* ADDI */        /* ADDM */      /* ADDB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* SUB */         /* SUBI */        /* SUBM */      /* SUBB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,

        /* CAI */         /* CAIL */        /* CAIE */      /* CAILE */
        0,                0,                0,              0,
        /* CAIA */        /* CAIGE */       /* CAIN */      /* CAIG */
        0,                0,                0,              0,
        /* CAM */         /* CAML */        /* CAME */      /* CAMLE */
        FCE,              FCE,              FCE,            FCE,
        /* CAMA */        /* CAMGE */       /* CAMN */      /* CAMG */
        FCE,              FCE,              FCE,            FCE,
        /* JUMP */        /* JUMPL */       /* JUMPE */     /* JUMPLE */
        FAC,              FAC,              FAC,            FAC,
        /* JUMPA */       /* JUMPGE */      /* JUMPN */     /* JUMPG */
        FAC,              FAC,              FAC,            FAC,
        /* SKIP */        /* SKIPL */       /* SKIPE */     /* SKIPLE */
        SACZ|FCE,         SACZ|FCE,         SACZ|FCE,       SACZ|FCE,
        /* SKIPA */       /* SKIPGE */      /* SKIPN */     /* SKIPG */
        SACZ|FCE,         SACZ|FCE,         SACZ|FCE,       SACZ|FCE,
        /* AOJ */         /* AOJL */        /* AOJE */      /* AOJLE */
        SAC|FAC,          SAC|FAC,          SAC|FAC,        SAC|FAC,
        /* AOJA */        /* AOJGE */       /* AOJN */      /* AOJG */
        SAC|FAC,          SAC|FAC,          SAC|FAC,        SAC|FAC,
        /* AOS */         /* AOSL */        /* AOSE */      /* AOSLE */
        SACZ|FCEPSE,      SACZ|FCEPSE,      SACZ|FCEPSE,    SACZ|FCEPSE,
        /* AOSA */        /* AOSGE */       /* AOSN */      /* AOSG */
        SACZ|FCEPSE,      SACZ|FCEPSE,      SACZ|FCEPSE,    SACZ|FCEPSE,
        /* SOJ */         /* SOJL */        /* SOJE */      /* SOJLE */
        SAC|FAC,          SAC|FAC,          SAC|FAC,        SAC|FAC,
        /* SOJA */        /* SOJGE */       /* SOJN */      /* SOJG */
        SAC|FAC,          SAC|FAC,          SAC|FAC,        SAC|FAC,
        /* SOS */         /* SOSL */        /* SOSE */      /* SOSLE */
        SACZ|FCEPSE,      SACZ|FCEPSE,      SACZ|FCEPSE,    SACZ|FCEPSE,
        /* SOSA */        /* SOSGE */       /* SOSN */      /* SOSG */
        SACZ|FCEPSE,      SACZ|FCEPSE,      SACZ|FCEPSE,    SACZ|FCEPSE,

        /* SETZ */        /* SETZI */       /* SETZM */     /* SETZB */
        FBR|SAC,          FBR|SAC,          FBR|SCE,        FBR|SAC|SCE,
        /* AND */         /* ANDI */        /* ANDM */      /* ANDB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* ANDCA */       /* ANDCAI */      /* ANDCAM */    /* ANDCAB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* SETM */        /* SETMI */       /* SETMM */     /* SETMB */
        FBR|SAC|FCE,      FBR|SAC,          FBR,            FBR|SAC|FCE,
        /* ANDCM */       /* ANDCMI */      /* ANDCMM */    /* ANDCMB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* SETA */        /* SETAI */       /* SETAM */     /* SETAB */
        FBR|SAC,          FBR|SAC,          FBR|SCE,        FBR|SAC|SCE,
        /* XOR */         /* XORI */        /* XORM */      /* XORB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* IOR */         /* IORI */        /* IORM */      /* IORB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* ANDCB */       /* ANDCBI */      /* ANDCBM */    /* ANDCBB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* EQV */         /* EQVI */        /* EQVM */      /* EQVB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* SETCA */       /* SETCAI */      /* SETCAM */    /* SETCAB */
        FBR|SAC,          FBR|SAC,          FBR|SCE,        FBR|SAC|SCE,
        /* ORCA */        /* ORCAI */       /* ORCAM */     /* ORCAB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* SETCM */       /* SETCMI */      /* SETCMM */    /* SETCMB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* ORCM */        /* ORCMI */       /* ORCMM */     /* ORCMB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* ORCB */        /* ORCBI */       /* ORCBM */     /* ORCBB */
        FBR|SAC|FCE,      FBR|SAC,          FBR|FCEPSE,     FBR|SAC|FCEPSE,
        /* SETO */        /* SETOI */       /* SETOM */     /* SETOB */
        FBR|SAC,          FBR|SAC,          FBR|SCE,        FBR|SAC|SCE,

        /* HLL */         /* HLLI */        /* HLLM */      /* HLLS */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HRL */         /* HRLI */        /* HRLM */      /* HRLS */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,
        /* HLLZ */        /* HLLZI */       /* HLLZM */     /* HLLZS */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HRLZ */        /* HRLZI */       /* HRLZM */     /* HRLZS */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,
        /* HLLO */        /* HLLOI */       /* HLLOM */     /* HLLOS */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HRLO */        /* HRLOI */       /* HRLOM */     /* HRLOS */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,
        /* HLLE */        /* HLLEI */       /* HLLEM */     /* HLLES */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HRLE */        /* HRLEI */       /* HRLEM */     /* HRLES */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,
        /* HRR */         /* HRRI */        /* HRRM */      /* HRRS */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HLR */         /* HLRI */        /* HLRM */      /* HLRS */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,
        /* HRRZ */        /* HRRZI */       /* HRRZM */     /* HRRZS */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HLRZ */        /* HLRZI */       /* HLRZM */     /* HLRZS */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,
        /* HRRO */        /* HRROI */       /* HRROM */     /* HRROS */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HLRO */        /* HLROI */       /* HLROM */     /* HLROS */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,
        /* HRRE */        /* HRREI */       /* HRREM */     /* HRRES */
        FBR|SAC|FCE,      FBR|SAC,          FAC|FMB|FCEPSE, FMB|SACZ|FCEPSE,
        /* HLRE */        /* HLREI */       /* HLREM */     /* HLRES */
        SWAR|FBR|SAC|FCE, SWAR|FBR|SAC,     SWAR|FAC|FMB|FCEPSE, SWAR|FMB|SACZ|FCEPSE,

        /* TRN */         /* TLN */         /* TRNE */      /* TLNE */
        FBR,              FBR|SWAR,         FBR,            FBR|SWAR,
        /* TRNA */        /* TLNA */        /* TRNN */      /* TLNN */
        FBR,              FBR|SWAR,         FBR,            FBR|SWAR,
        /* TDN */         /* TSN */         /* TDNE */      /* TSNE */
        FBR|FCE,          FBR|SWAR|FCE,     FBR|FCE,        FBR|SWAR|FCE,
        /* TDNA */        /* TSNA */        /* TDNN */      /* TSNN */
        FBR|FCE,          FBR|SWAR|FCE,     FBR|FCE,        FBR|SWAR|FCE,
        /* TRZ */         /* TLZ */         /* TRZE */      /* TLZE */
        FBR|SAC,          FBR|SAC|SWAR,     FBR|SAC,        FBR|SAC|SWAR,
        /* TRZA */        /* TLZA */        /* TRZN */      /* TLZN */
        FBR|SAC,          FBR|SAC|SWAR,     FBR|SAC,        FBR|SAC|SWAR,
        /* TDZ */         /* TSZ */         /* TDZE */      /* TSZE */
        FBR|SAC|FCE,      FBR|SAC|SWAR|FCE, FBR|SAC|FCE,    FBR|SAC|SWAR|FCE,
        /* TDZA */        /* TSZA */        /* TDZN */      /* TSZN */
        FBR|SAC|FCE,      FBR|SAC|SWAR|FCE, FBR|SAC|FCE,    FBR|SAC|SWAR|FCE,
        /* TRC */         /* TLC */         /* TRCE */      /* TLCE */
        FBR|SAC,          FBR|SAC|SWAR,     FBR|SAC,        FBR|SAC|SWAR,
        /* TRCA */        /* TLCA */        /* TRCN */      /* TLCN */
        FBR|SAC,          FBR|SAC|SWAR,     FBR|SAC,        FBR|SAC|SWAR,
        /* TDC */         /* TSC */         /* TDCE */      /* TSCE */
        FBR|SAC|FCE,      FBR|SAC|SWAR|FCE, FBR|SAC|FCE,    FBR|SAC|SWAR|FCE,
        /* TDCA */        /* TSCA */        /* TDCN */      /* TSCN */
        FBR|SAC|FCE,      FBR|SAC|SWAR|FCE, FBR|SAC|FCE,    FBR|SAC|SWAR|FCE,
        /* TRO */         /* TLO */         /* TROE */      /* TLOE */
        FBR|SAC,          FBR|SAC|SWAR,     FBR|SAC,        FBR|SAC|SWAR,
        /* TROA */        /* TLOA */        /* TRON */      /* TLON */
        FBR|SAC,          FBR|SAC|SWAR,     FBR|SAC,        FBR|SAC|SWAR,
        /* TDO */         /* TSO */         /* TDOE */      /* TSOE */
        FBR|SAC|FCE,      FBR|SWAR|SAC|FCE, FBR|SAC|FCE,    FBR|SAC|SWAR|FCE,
        /* TDOA */        /* TSOA */        /* TDON */      /* TSON */
        FBR|SAC|FCE,      FBR|SWAR|SAC|FCE, FBR|SAC|FCE,    FBR|SAC|SWAR|FCE,
        /* IOT  Instructions */
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
        0,                0,                0,              0,
};

#define SWAP_AR         ((RMASK & AR) << 18) | ((AR >> 18) & RMASK)
#define SMEAR_SIGN(x)   x = ((x) & SMASK) ? (x) | EXPO : (x) & MANT
#define GET_EXPO(x)     ((((x) & SMASK) ? 0377 : 0 )  \
                                        ^ (((x) >> 27) & 0377))
#if KI
#define AOB(x)          ((x + 1) & RMASK) | ((x + 01000000LL) & (C1|LMASK))
#define SOB(x)          ((x + RMASK) & RMASK) | ((x + LMASK) & (C1|LMASK));
#else
#define AOB(x)          (x + 01000001LL)
#define SOB(x)          (x + 0777776777777LL)
#endif

void set_interrupt(int dev, int lvl) {
    lvl &= 07;
    if (lvl) {
       dev_irq[dev>>2] = 0200 >> lvl;
       pi_pending = 1;
       sim_debug(DEBUG_IRQ, &cpu_dev, "set irq %o %o\n", dev & 0774, lvl);
    }
}

void clr_interrupt(int dev) {
    dev_irq[dev>>2] = 0;
    sim_debug(DEBUG_IRQ, &cpu_dev, "clear irq %o\n", dev & 0774);
}

void check_apr_irq() {
        int flg = 0;
        clr_interrupt(0);
        clr_interrupt(4);
#if KI
        if (!page_enable) 
            return;
#endif
        if (apr_irq) {
            flg |= ((FLAGS & OVR) != 0) & ov_irq;
            flg |= ((FLAGS & FLTOVR) != 0) & fov_irq;
#if KI
            flg |= clk_flg & (clk_irq != 0);
#endif
            flg |= nxm_flag | mem_prot | push_ovf;
            if (flg)
                set_interrupt(0, apr_irq);
        }
        if (clk_flg & clk_en)
            set_interrupt(4, clk_irq);
}

int check_irq_level() {
     int i, lvl;
     int pi_ok, pi_t;

     for(i = lvl = 0; i < 128; i++)
        lvl |= dev_irq[i];
     if (lvl == 0)
        pi_pending = 0;
     PIR |= (lvl & PIE);
     /* Compute mask for pi_ok */
     pi_t = (~PIR & ~PIH) >> 1;
     pi_ok = 0100 & (PIR & ~PIH);
     if (!pi_ok) {
        /* None at level 1, check for lower level */
         lvl = 0040;
         for(i = 2; i <= 7; i++) {
            if (lvl & pi_t) {
                pi_ok |= lvl;
                lvl >>= 1;
            } else {
                break;
            }
         }
     }
     /* We have 1 bit for each non held interrupt. */
     pi_req = PIR & ~PIH & pi_ok;
     if (pi_req) {
        int pi_r = pi_req;
        for(lvl = i = 1; i<=7; i++, lvl++) {
           if (pi_r & 0100)
              break;
           pi_r <<= 1;
        }
        pi_enc = lvl;
        return 1;
     }
     return 0;
}

void restore_pi_hold() {
     int i, lvl;
     int pi_ok, pi_t;

     if (!pi_enable)
        return;
     /* Compute mask for pi_ok */
     lvl = 0100;
     /* None at level 1, check for lower level */
     for(i = 1; i <= 7; i++) {
        if (lvl & PIH) {
            PIR &= ~lvl;
            PIH &= ~lvl;
            break;
         }
         lvl >>= 1;
     }
     if (dev_irq[0])
        check_apr_irq();
     pi_pending = 1;
}

void set_pi_hold() {
     PIH |= 0200 >> pi_enc;
     PIR &= ~(0200 >> pi_enc);
}

#if KI
static int      timer_irq, timer_flg;
static uint64   fault_data;


t_stat dev_pag(uint32 dev, uint64 *data) {
    uint64 res = 0;
    switch(dev & 03) {
    case CONI:
        /* Complement of vpn */
        *data = res;
        break;

     case CONO:
        /* Set Stack AC and Page Table Reload Counter */
        ac_stack = (*data >> 9) & 0760;
        pag_reload = *data & 037;
        break;

    case DATAO:
        res = *data;
        if (res & LSIGN) {
            eb_ptr = (res & 017777) << 9;
            page_enable = (res & 020000) != 0;
        }
        if (res & SMASK) {
            ub_ptr = ((res >> 18) & 017777) << 9;
            user_addr_cmp = (res & 00020000000000LL) != 0;
            small_user =  (res & 00040000000000LL) != 0;
            fm_sel = (res & 00300000000000LL) >> 29;
       }
       break;

    case DATAI:
        res = (eb_ptr >> 9);
        if (page_enable)
            res |= 020000;
        res |= ((uint64)(ub_ptr)) << 9;
        if (user_addr_cmp)
            res |= 00020000000000LL;
        if (small_user)
            res |= 00040000000000LL;
        res |= ((uint64)(fm_sel)) << 29;
        *data = res;
        break;
    return SCPE_OK;
    }
}

#endif

t_stat dev_apr(uint32 dev, uint64 *data) {
    uint64 res = 0;
    switch(dev & 03) {
    case CONI:
        /* Read trap conditions */
#if KI
        res = clk_irq | (apr_irq << 3) | (nxm_flag << 6);
        res |= (inout_fail << 7) | (clk_flg << 9) | (clk_en << 10);
        res |= (timer_irq << 14) | (parity_irq << 15) | (timer_flg << 17);
#else
        res = apr_irq | (((FLAGS & OVR) != 0) << 3) | (ov_irq << 4) ;
        res |= (((FLAGS & FLTOVR) != 0) << 6) | (fov_irq << 7) ;
        res |= (clk_flg << 9) | (((uint64)clk_en) << 10) | (nxm_flag << 12);
        res |= (mem_prot << 13) | (((FLAGS & USERIO) != 0) << 15);
        res |= (push_ovf << 16);
#endif
        *data = res;
        break;

     case CONO:
        /* Set trap conditions */
        res = *data;
#if KI
        clk_irq = res & 07;
        apr_irq = (res >> 3) & 07;
        if (res & 0000100)
            nxm_flag = 0;
        if (res & 0000200)
            inout_fail = 0;
        if (res & 0001000) {
            clk_flg = 0;
            clr_interrupt(4);
        }
        if (res & 0002000)
            clk_en = 1;
        if (res & 0004000)
            clk_en = 0;
        if (res & 0040000)
            timer_irq = 1;
        if (res & 0100000)
            timer_irq = 0;
        if (res & 0400000)
            timer_flg = 0;
#else
        clk_irq = apr_irq = res & 07;
        if (res & 010)
            FLAGS &= ~OVR;
        if (res & 020)
            ov_irq = 1;
        if (res & 040)
            ov_irq = 0;
        if (res & 0100)
            FLAGS &= ~FLTOVR;
        if (res & 0200)
            fov_irq = 1;
        if (res & 0400)
            fov_irq = 0;
        if (res & 01000) {
            clk_flg = 0;
            clr_interrupt(4);
        }
        if (res & 02000)
            clk_en = 1;
        if (res & 04000)
            clk_en = 0;
        if (res & 010000)
            nxm_flag = 0;
        if (res & 020000)
            mem_prot = 0;
        if (res & 0200000)
            reset_all(1);
        if (res & 0400000)
            push_ovf = 0;
#endif
        check_apr_irq();
        break;

    case DATAO:
#if !KI
        /* Set protection registers */
        Rh = 0377 & (*data >> 1);
        Rl = 0377 & (*data >> 10);
        Pflag = 01 & (*data >> 18);
        Ph = 0377 & (*data >> 19);
        Pl = 0377 & (*data >> 28);
#endif
        break;

    case DATAI:
        /* Read switches */
        break;
    }
    return SCPE_OK;
}

#if KI
int page_lookup(int addr, int flag, int *loc, int wr) {
    uint64  data;
    int base;
    int page = addr >> 9;
    int pg;
    int uf = 0;
    if ((!flag && (FLAGS & USER) != 0) ||
        ((xct_flag & 1) && ((wr == 0) || modify)) ||
        ((xct_flag & 2) && wr)) {
        base = ub_ptr;
        uf = 1;
        if (small_user && (addr & 0340000) != 0) {
            fault_data = 2LL;
            fault_data = (((uint64)(page))<<18) | ((uint64)(uf) << 28);
            return 0;
        }
    } else {
        /* If paging is not enabled, address is direct */
        if (!page_enable) {
            *loc = addr;
            return 1;
        }
        /* Pages 340-377 via UBR */
        if ((addr & 0340000) == 0340000) {
            base = ub_ptr;
            page += 01000 - 0340;
        /* Pages 400-777 via EBR */
        } else if (addr & 0400000) {
            base = eb_ptr;
        } else {
            *loc = addr;
            return 1;
        }
    }
    data = M[base + (page >> 1)];
    if ((page & 1) == 0)
       data >>= 18;
    data &= RMASK;
    if ((data & LSIGN) == 0 || (wr & ((data & 0100000) != 0))) {
        fault_data = (((uint64)(page))<<18) | ((uint64)(uf) << 28) | 020LL;
        fault_data |= (data & 0100000) ? 04LL : 0LL;
        fault_data |= (data & 0040000) ? 02LL : 0LL;
        fault_data |= wr;
        return 0;
    }
    *loc = ((data & 037777) << 9) + (addr & 0777);
    return 1;
}

#else
int page_lookup(int addr, int flag, int *loc, int wr) {
      if (!flag && (FLAGS & USER) != 0) {
          if (addr <= ((Pl << 10) + 01777))
             *loc = (AB + (Rl << 10)) & RMASK;
          else if (cpu_unit.flags & UNIT_TWOSEG &&
                    (!Pflag & wr) == wr &&
                    (AB & 0400000) != 0 &&
                    (addr <= ((Ph << 10) + 01777)))
             *loc = (AB + (Rh << 10)) & RMASK;
          else {
            mem_prot = 1;
            set_interrupt(0, apr_irq);
            return 0;
          }
      } else {
         *loc = addr;
      }
      return 1;
}
#endif

t_stat dev_pi(uint32 dev, uint64 *data) {
    uint64 res = 0;
    switch(dev & 3) {
    case CONO:
        /* Set PI flags */
        res = *data;
        if (res & 010000) {
           PIR = PIH = PIE = 0;
           pi_enable = 0;
           parity_irq = 0;
        }
        if (res & 0200)
           pi_enable = 1;
        if (res & 0400)
           pi_enable = 0;
        if (res & 01000)
           PIE &= ~(*data & 0177);
        if (res & 02000)
           PIE |= (*data & 0177);
        if (res & 04000) {
           PIR |= (*data & 0177);
           pi_pending = 1;
        }
        if (res & 040000)
           parity_irq = 1;
        if (res & 0100000)
           parity_irq = 0;
        break;
     case CONI:
        res = PIE;
        res |= (pi_enable << 7);
        res |= (PIH << 8);
#if KI
        res |= ((uint64)(PIR) << 18);
#endif
        res |= (parity_irq << 15);
        *data = res;
        break;
    case DATAO:
        /* Set lights */
    case DATAI:
        break;
    }
    return SCPE_OK;
}

t_stat null_dev(uint32 dev, uint64 *data) {
    switch(dev & 3) {
    case CONI:
    case DATAI:
         *data = 0;
         break;

    case CONO:
    case DATAO:
         break;
    }
    return SCPE_OK;
}

#if KI
uint64 get_reg(int reg) {
    if (FLAGS & USER) {
       return FM[fm_sel|reg];
    } else if (xct_flag & 1) {
       if (FLAGS & USERIO) {
          if (fm_sel == 0) {
             int addr;
             if (page_lookup(reg, 0, &addr, 0)) 
                return M[addr];
          }
          return FM[fm_sel|reg];
       }
       return M[ub_ptr + ac_stack + reg];
    }
    return FM[reg];
}

void   set_reg(int reg, uint64 value, int mem) {
    if (FLAGS & USER) {
        FM[fm_sel|reg] = value;
    } else if (((xct_flag & 1) && mem && modify) || 
        ((xct_flag & 1) && !mem) || (xct_flag & 2)) {
        if (FLAGS & USERIO) {
           if (fm_sel == 0) {
              int addr;
              if (page_lookup(reg, 0, &addr, 1)) 
                 M[addr] = value;
           } else
              FM[fm_sel|reg] = value;
           return;
        }
        M[ub_ptr + ac_stack + reg] = value;
        return;
    }
    FM[reg] = value;
}

#else
#define get_reg(reg)                 FM[(reg) & 017]
#define set_reg(reg, value, mem)     FM[(reg) & 017] = value
#endif


int Mem_read(int flag) {
    if (AB < 020) {
        MB = get_reg(AB);
    } else {
        int addr;
        sim_interval--;
        if (!page_lookup(AB, flag, &addr, 0))
            return 1;
        if (addr > MEMSIZE) {
            nxm_flag = 1;
            set_interrupt(0, apr_irq);
            return 1;
        }
        MB = M[addr];
    }
    return 0;
}

int Mem_write(int flag) {
    if (AB < 020)
        set_reg(AB, MB, 1);
    else {
        int addr;
        sim_interval--;
        if (!page_lookup(AB, flag, &addr, 1))
            return 1;
        if (addr > MEMSIZE) {
            nxm_flag = 1;
            set_interrupt(0, apr_irq);
            return 1;
        }
        M[addr] = MB;
    }
    return 0;
}

/*
 * Function to determine number of leading zero bits in a work
 */
int nlzero(uint64 w) {
    int n = 0;
    if (w == 0) return 36;
    if ((w & 00777777000000LL) == 0) { n += 18; w <<= 18; }
    if ((w & 00777000000000LL) == 0) { n += 9;  w <<= 9;  }
    if ((w & 00770000000000LL) == 0) { n += 6;  w <<= 6;  }
    if ((w & 00700000000000LL) == 0) { n += 3;  w <<= 3;  }
    if ((w & 00600000000000LL) == 0) { n ++;    w <<= 1;  }
    if ((w & 00400000000000LL) == 0) { n ++; }
    return n;
}

t_stat sim_instr (void)
{
t_stat reason;
int     f;
int     i_flags;
int     pi_rq;
int     pi_ov;
int     pi_cycle;
int     ind;
int     f_load_pc;
int     f_inst_fetch;
int     f_pc_inh;
int     nrf;
int     fxu_hold_set;
int     sac_inh;
int     flag1;
int     flag3;
/* Restore register state */

if ((reason = build_dev_tab ()) != SCPE_OK)            /* build, chk dib_tab */
    return reason;


/* Main instruction fetch/decode loop: check clock queue, intr, trap, bkpt */
   f_load_pc = 1;
   f_inst_fetch = 1;
   ind = 0;
   uuo_cycle = 0;
   push_ovf = mem_prot = nxm_flag = 0;
   pi_cycle = 0;
   pi_rq = 0;
   pi_ov = 0;
   BYF5 = 0;


  while ( reason == 0) {                                /* loop until ABORT */
     if (sim_interval <= 0) {                           /* check clock queue */
          if (reason = sim_process_event () != SCPE_OK) {    /* error?  stop sim */
                if (reason != SCPE_STEP || !BYF5)
                   return reason;
          }
     }

     if (sim_brk_summ && sim_brk_test(PC, SWMASK('E'))) {
         reason = STOP_IBKPT;
         break;
    }


    /* Normal instruction */
    if (f_load_pc) {
        AB = PC;
        uuo_cycle = 0;
#if KI
        xct_flag = 0;
#endif
    }

    if (f_inst_fetch) {
fetch:
       Mem_read(pi_cycle | uuo_cycle);
       IR = (MB >> 27) & 0777;
       AC = (MB >> 23) & 017;
       i_flags = opflags[IR];
       BYF5 = 0;
    }

    /* Second half of byte instruction */
    if (BYF5) {
        i_flags = FCE;
        AB = AR & RMASK;
    }

    /* Update history */
    if (hst_lnt) {
            int i;
            hst_p = hst_p + 1;
            if (hst_p >= hst_lnt)
                    hst_p = 0;
            hst[hst_p].pc = HIST_PC | ((BYF5)? PC : AB);
            hst[hst_p].ea = AB;
            hst[hst_p].ir = MB;
            hst[hst_p].flags = (FLAGS << 4) |(clk_flg << 3) |(mem_prot << 2) |
                               (nxm_flag << 1) | (push_ovf);
            hst[hst_p].ac = get_reg(AC);
    }

    /* Handle indirection repeat until no longer indirect */
    do {
         if (pi_enable & !pi_cycle & pi_pending) {
            pi_rq = check_irq_level();
         }
         ind = (MB & 020000000) != 0;
         AR = MB;
         AB = MB & RMASK;
         if (MB &  017000000) {
             AR = MB = (AB + get_reg((MB >> 18) & 017)) & FMASK;
             AB = MB & RMASK;
         }
         if (IR != 0254)
             AR &= RMASK;
         if (ind & !pi_rq)
              Mem_read(pi_cycle | uuo_cycle);
         /* Handle events during a indirect loop */
         if (sim_interval-- <= 0) {
              if (reason = sim_process_event () != SCPE_OK) {
                 if (reason != SCPE_STEP || !BYF5)
                     return reason;
              }
         }
    } while (ind & !pi_rq);

    /* Update final address into history. */
    if (hst_lnt) {
        hst[hst_p].ea = AB;
    }

    /* If there is a interrupt handle it. */
    if (pi_rq) {
        set_pi_hold();
        pi_cycle = 1;
        pi_rq = 0;
        pi_hold = 0;
        pi_ov = 0;
        AB = 040 | (pi_enc << 1);
        goto fetch;
    }


fetch_opr:
    /* Set up to execute instruction */
    f_inst_fetch = 1;
    f_load_pc = 1;
    f_pc_inh = 0;
    nrf = 0;
    fxu_hold_set = 0;
    sac_inh = 0;
#if KI
    modify = 0;
#endif
    /* Load pseudo registers based on flags */
    if (i_flags & (FCEPSE|FCE)) {
#if KI
        modify = 1;
#endif
        if (Mem_read(0))
            goto last;
        AR = MB;
    }

    if (i_flags & FAC) {
        BR = AR;
        AR = get_reg(AC);
    }

    if (i_flags & SWAR) {
        AR = SWAP_AR;
    }

    if (i_flags & FBR) {
        BR = get_reg(AC);
    }

    if (i_flags & FMB) {
        BR = MB;
    }

    if (hst_lnt) {
        hst[hst_p].mb = AR;
    }

    if (i_flags & FAC2) {
        MQ = get_reg((AC + 1) & 017);
    } else if (!BYF5) {
        MQ = 0;
    }

    /* Process the instruction */
    switch (IR & 0770) {
    case 0040:
    case 0050:
    case 0060:
    case 0070:
muuo:
              uuo_cycle = 1;
    case 0000:      /* UUO */
              if (IR == 0)
                 uuo_cycle = 1;
    case 0010:
    case 0020:
    case 0030:
              f_pc_inh = 1;
uuo:
              MB = ((uint64)(IR) << 27) | ((uint64)(AC) << 23) | (uint64)(AB);
#if KI
              if (IR == 0 || (IR & 040) != 0) {
                  AB = ub_ptr | 0424;
                  uuo_cycle = 1;
                  Mem_write(uuo_cycle);
                  AB |= 1;
                  MB = (FLAGS << 23) | ((PC + 1) & RMASK);
                  Mem_write(uuo_cycle);
                  AB = ub_ptr | 0430;
                  if ((FLAGS & (TRP1|TRP2)) != 0)
                      AB |= 1;
                  if (FLAGS & USER)
                      AB |= 2;
                  if (FLAGS & PUBLIC)
                      AB |= 4;
                  Mem_read(uuo_cycle);
                  FLAGS |= (MB >> 23) & 017777;
                  PC = MB & RMASK;
                  f_pc_inh = 1;
                  break;
              }
              AB = ((FLAGS & USER) ? 0 : eb_ptr) | 040;
#else
              AB = 040;
#endif
              Mem_write(uuo_cycle);
              AB += 1;
              f_load_pc = 0;
              break;

#if KI
    case 0100: /* OPR */ /* MUUO */
unasign:
              goto muuo;

    case 0110:
       switch(IR & 07) {
       case 0:       /* DFAD */
       case 1:       /* DFSB */

              /* On Load AR,MQ has memory operand */
              /* AR,MQ = AC  BR,MB  = mem */
                    /* AR High */
              AB = (AB + 1) & RMASK;
              if (Mem_read(0))
                  break;
              SC = GET_EXPO(BR);
              BR = SMEAR_SIGN(BR);
              BR <<= 35;
              BR |= MB & CMASK;
              FE = GET_EXPO(AR);
              AR = SMEAR_SIGN(AR);
              AR <<= 35;
              AR |= (MQ & CMASK);
              if (IR & 01) {
                  BR = (DFMASK ^ BR) + 1;
              }
              SCAD = SC - FE;
              if (SCAD < 0) {
                  AD = AR;
                  AR = BR;
                  BR = AD;
                  SCAD = FE;
                  FE = SC;
                  SC = SCAD;
                  SCAD = SC - FE;
              }
              if (SCAD > 0) {
                  while (SCAD > 0) {
                     AR = (AR & (DSMASK|DNMASK)) | (AR >> 1);
                     SCAD--;
                  }
              }
              AD = (AR + BR);
              flag1 = 0;
              if ((AR & DSMASK) ^ (BR & DSMASK)) {
                  if (AD & DSMASK) {
                     AD = (DCMASK ^ AD) + 1;
                     flag1 = 1;
                  }
              } else {
                  if (AR & DSMASK) {
                     AD = (DCMASK ^ AD) + 1;
                     flag1 = 1;
                  }
                  if (AD & DNMASK) {
                     AD ++;
                     AD = (AD & DSMASK) | (AD >> 1);
                     SC++;
                  }
              }
              AR = AD;

              while (AR != 0 && ((AR & DXMASK) == 0)) {
                 AR <<= 1;
                 SC--;
                 fxu_hold_set = 1;
              }
dpnorm:
              if (AR == 0)
                  flag1 = 0;
              ARX = AR & CMASK;
              AR >>= 35;
              AR &= MMASK;
              if (flag1) {
                  ARX = (ARX ^ CMASK) + 1;
                  AR = (AR ^ MMASK) + ((ARX & SMASK) != 0);
                  ARX &= CMASK;
                  AR &= MMASK;
                  AR |= SMASK;
              }
              if (((SC & 0400) != 0)) {
                 FLAGS |= OVR|FLTOVR|TRP1;
                 if (fxu_hold_set) {
                     FLAGS |= FLTUND;
                 }
                 check_apr_irq();
              }
              SCAD = SC ^ ((AR & SMASK) ? 0377 : 0);
              AR &= SMASK|MMASK;
              if (AR != 0)
                  AR |= ((uint64)(SCAD & 0377)) << 27;

              MQ = ARX;
              break;
       case 2: /* DFMP */
              /* On Load AR,MQ has memory operand */
              /* AR,MQ = AC  BR,MB  = mem */
                    /* AR High */
              AB = (AB + 1) & RMASK;
              if (Mem_read(0))
                  break;
              SC = GET_EXPO(AR);
              AR = SMEAR_SIGN(AR);
              AR <<= 35;
              AR |= (MQ & CMASK);
              FE = GET_EXPO(BR);
              BR = SMEAR_SIGN(BR);
              BR <<= 35;
              BR |= MB & CMASK;
              flag1 = 0;
              if (AR & DSMASK) {
                  AR = (DFMASK ^ AR) + 1;
                  flag1 = 1;
              }
              if (BR & DSMASK) {
                  BR = (DFMASK ^ BR) + 1;
                  flag1 = !flag1;
              }
              SC = SC + FE - 0201;
              if (SC < 0)
                  fxu_hold_set = 1;
              AD = (AR >> 30) * (BR >> 30);
              AD += ((AR >> 30) * (BR & PMASK)) >> 30;
              AD += ((AR & PMASK) * (BR >> 30)) >> 30;
              AR = AD >> 1;
              if (AR & DNMASK) {
                 AR >>= 1;
                 SC++;
              }
              goto dpnorm;
       case 3: /* DFDV */
              /* On Load AR,MQ has memory operand */
              /* AR,MQ = AC  BR,MB  = mem */
                    /* AR High */
              AB = (AB + 1) & RMASK;
              if (Mem_read(0))
                  break;
              SC = GET_EXPO(AR);
              AR = SMEAR_SIGN(AR);
              AR <<= 35;
              AR |= (MQ & CMASK);
              FE = GET_EXPO(BR);
              BR = SMEAR_SIGN(BR);
              BR <<= 35;
              BR |= MB & CMASK;
              flag1 = 0;
              if (AR & DSMASK) {
                  AR = (DFMASK ^ AR) + 1;
                  flag1 = 1;
              }
              if (BR & DSMASK) {
                  BR = (DFMASK ^ BR) + 1;
                  flag1 = !flag1;
              }
              if (AR >= (BR << 1)) {
                  FLAGS |= OVR|FLTOVR|NODIV|TRP1;
                  AR = 0;      /* For clean history */
                  sac_inh = 1;
                  check_apr_irq();
                  break;
              }

              if (AR == 0)  {
                  sac_inh = 1;
                  break;
              }
              SC = SC - FE + 0201;
              if (AR < BR) {
                  AR <<= 1;
                  SC--;
              }
              if (SC < 0)
                  fxu_hold_set = 1;
              AD = 0;
              for (FE = 0; FE < 62; FE++) {
                  AD <<= 1;
                  if (AR >= BR) {
                     AR = AR - BR;
                     AD |= 1;
                  }
                  AR <<= 1;
              }
              AR = AD;
              goto dpnorm;
       case 4: /* DADD */
       case 5: /* DSUB */
       case 6: /* DMUL */
       case 7: /* DDIV */
              goto muuo;
       }
       break;

    case 0120:
       switch (IR & 07) {
       case 3: /* UUO */
              goto muuo;
       case 0: /* DMOVE */
              AB = (AB + 1) & RMASK;
              if (Mem_read(0))
                   break;
              MQ = MB;
              break;

       case 1: /* DMOVN */
              AB = (AB + 1) & RMASK;
              if (Mem_read(0))
                   break;
              MQ = ((MB & CMASK) ^ CMASK) + 1;   /* Low */
              /* High */
              AR = (CM(AR) + ((MQ & SMASK) != 0)) & FMASK;
              MQ &= CMASK;
              break;

       case 4: /* DMOVEM */
              /* Handle each half as seperate instruction */
              if ((FLAGS & BYTI) == 0 || pi_cycle) {
                  MB = AR;
                  if (Mem_write(0))
                      break;
                  if (!pi_cycle) {
                      FLAGS |= BYTI;
                      f_pc_inh = 1;
                      break;
                   }
              }
              if ((FLAGS & BYTI) || pi_cycle) {
                   if (!pi_cycle)
                      FLAGS &= ~BYTI;
                   AB = (AB + 1) & RMASK;
                   MB = MQ;
                   if (Mem_write(0))
                      break;
              }
              break;

       case 5: /* DMOVNM */
              /* Handle each half as seperate instruction */
              if ((FLAGS & BYTI) == 0 || pi_cycle) {
                  BR = AR = CM(AR);
                  BR = (BR + 1);
                  MQ = (((MQ & CMASK) ^ CMASK) + 1);
                  if (MQ & SMASK)
                     AR = BR;
                  AR &= FMASK;
                  MB = AR;
                  if (Mem_write(0))
                      break;
                  if (!pi_cycle) {
                      FLAGS |= BYTI;
                      f_pc_inh = 1;
                      break;
                   }
              }
              if ((FLAGS & BYTI) || pi_cycle) {
                   if (!pi_cycle)
                      FLAGS &= ~BYTI;
                   MQ = (CM(MQ) + 1) & CMASK;
                   AB = (AB + 1) & RMASK;
                   MB = MQ;
                   if (Mem_write(0))
                      break;
              }
              break;

       case 2: /* FIX */
       case 6: /* FIXR */
              MQ = 0;
              SC = ((((AR & SMASK) ? 0377 : 0 )
                      ^ ((AR >> 27) & 0377)) + 0600) & 0777;
              flag1 = 0;
              if ((AR & SMASK) != 0) {
                 AR ^= MMASK;
                 AR++;
                 AR &= MMASK;
                 flag1 = 1;
              } else {
                 AR &= MMASK;
              }
              SC -= 27;
              SC &= 0777;
              if (SC < 9) {
              /* 0 < N < 8 */
                  AR = (AR << SC) & FMASK;
              }  else if ((SC & 0400) != 0) {
              /* -27 < N < 0 */
                  SC = 01000 - SC;
                  MQ = (AR << (36 - SC)) - flag1 ;
                  AR = (AR >> SC);
                  if ((IR & 04) && (MQ & SMASK) != 0)
                       AR ++;
              } else if (!pi_cycle) {
                  FLAGS |= OVR|TRP1;        /* OV & T1 */
                  sac_inh = 1;
              }
              if (flag1)
                 AR = (CM(AR) + 1) & FMASK;
              break;

        case 7: /* FLTR */
              if (AR & SMASK) {
                  flag1 = 1;
                  AR = (CM(AR) + 1) & CMASK;
              } else
                  flag1 = 0;
              AR <<= 19;
              SC = 163;
              goto fnorm;
        }
        break;
#else
    case 0100: /* OPR */ /* MUUO */
    case 0110:
    case 0120:
unasign:
              MB = ((uint64)(IR) << 27) | ((uint64)(AC) << 23) | (uint64)(AB);
              AB = 060;
              uuo_cycle = 1;
              Mem_write(uuo_cycle);
              AB += 1;
              f_load_pc = 0;
              f_pc_inh = 1;
              break;
#endif
    case 0130:      /* Byte OPS */
       switch(IR & 07) {
       case 3: /* IBP/ADJBP */
       case 4: /* ILDB */
       case 6: /* IDPB */
              if ((FLAGS & BYTI) == 0) {      /* BYF6 */
                  SC = (AR >> 24) & 077;
                  SCAD = (((AR >> 30) & 077) + (0777 ^ SC) + 1) & 0777;
                  if (SCAD & 0400) {
                      SC = ((0777 ^ ((AR >> 24) & 077)) + 044 + 1) & 0777;
#if KI
                      AR = (AR & LMASK) | ((AR + 1) & RMASK);
#else
                      AR = (AR + 1) & FMASK;
#endif
                  } else
                      SC = SCAD;
                  AR &= PMASK;
                  AR |= (uint64)(SC & 077) << 30;
                  if ((IR & 04) == 0)
                      break;
              }

       case 5:/* LDB */
       case 7:/* DPB */
              if (((FLAGS & BYTI) == 0) | !BYF5) {
                  SC = (AR >> 30) & 077;
                  MQ = (uint64)(1) << ( 077 & (AR >> 24));
                  MQ -= 1;
                  SC = ((0777 ^ SC) + 1) & 0777;
                  f_load_pc = 0;
                  f_inst_fetch = 0;
                  f_pc_inh = 1;
                  FLAGS |= BYTI;      /* BYF6 */
                  BYF5 = 1;
                  break;
              }
              if ((IR & 06) == 4) {
                  AR = MB;
                  while(SC != 0) {
                      AR >>= 1;
                      SC = (SC + 1) & 0777;
                  }
                  AR &= MQ;
                  set_reg(AC, AR, 0);
              } else {
                  BR = MB;
                  AR = get_reg(AC) & MQ;
                  while(SC != 0) {
                      AR <<= 1;
                      MQ <<= 1;
                      SC = (SC + 1) & 0777;
                  }
                  BR &= CM(MQ);
                  AR &= FMASK;
                  BR |= AR & MQ;
                  MB = BR;
                  Mem_write(0);
              }
              FLAGS &= ~BYTI; /* BYF6 */
              BYF5 = 0;
              break;

       case 1:/* DFN */
              AD = (CM(BR) + 1) & FMASK;
              SC = (BR >> 27) & 0777;
              BR = AR;
              AR = AD;
              AD = (CM(BR) + ((AD & MANT) == 0)) & FMASK;
              AR &= MANT;
              AR |= ((uint64)(SC & 0777)) << 27;
              BR = AR;
              AR = AD;
              MB = BR;
              if (Mem_write(0))
                 break;
              set_reg(AC, AR, 0);
              break;

       case 2:/* FSC */
              SC = ((AB & LSIGN) ? 0400 : 0) | (AB & 0377);
              SCAD = GET_EXPO(AR);
              SC = (SCAD + SC) & 0777;

              if (AR & SMASK) {
                 AR = CM(AR) + 1;
                 flag1 = 1;
              } else {
                 flag1 = 0;
              }
              AR &= MMASK;
              if (AR != 0) {
                  if ((AR & 00000777770000LL) == 0) { SC -= 12;  AR <<= 12; }
                  if ((AR & 00000777000000LL) == 0) { SC -= 6;  AR <<= 6; }
                  if ((AR & 00000740000000LL) == 0) { SC -= 4;  AR <<= 4; }
                  if ((AR & 00000600000000LL) == 0) { SC -= 2;  AR <<= 2; }
                  if ((AR & 00000400000000LL) == 0) { SC -= 1;  AR <<= 1; }
              } else if (flag1) {
                  AR =  BIT9;
                  SC++;
              }
              if (((SC & 0400) != 0) ^ ((SC & 0200) != 0))
                  fxu_hold_set = 1;
              if (SC & 0400) {
                  FLAGS |= OVR|FLTOVR|TRP1;
                  if (!fxu_hold_set)
                      FLAGS |= FLTUND;
                  check_apr_irq();
              }
              if (flag1) {
                 AR = SMASK | ((CM(AR) + 1) & MMASK);
                 SC ^= 0377;
              } else if (AR == 0)
                 SC = 0;
              AR |= ((uint64)((SC) & 0377)) << 27;
              break;

       case 0:/* UFA */
              goto fadd;
       }
       break;

    case 0150:      /* FSB */
              AD = (CM(AR) + 1) & FMASK;
              AR = BR;
              BR = AD;

    case 0140:      /* FAD */
fadd:
              SC = ((BR >> 27) & 0777);
              if ((BR & SMASK) == (AR & SMASK)) {
                  SCAD = SC + (((AR >> 27) & 0777) ^ 0777) + 1;
              } else {
                  SCAD = SC + ((AR >> 27) & 0777);
              }
              SCAD &= 0777;
              if (((BR & SMASK) != 0) == ((SCAD & 0400) != 0)) {
                  AD = AR;
                  AR = BR;
                  BR = AD;
              }
              if ((SCAD & 0400) == 0) {
                 if ((AR & SMASK) == (BR & SMASK))
                      SCAD = ((SCAD ^ 0777) + 1) & 0777;
                 else
                      SCAD = (SCAD ^ 0777);
              } else {
                 if ((AR & SMASK) != (BR & SMASK))
                      SCAD = (SCAD + 1) & 0777;
              }

              /* Get exponent */
              SC = GET_EXPO(AR);
              /* Smear the signs */
              SMEAR_SIGN(BR);
              SMEAR_SIGN(AR);
              AR <<= 27;
              BR <<= 27;
              if (SCAD & 0400) {
                  SCAD = 01000 - SCAD;
                  if (SCAD < 28) {
                      AD = (BR & (SMASK<<27))? (FMASK<<27|MMASK) : 0;
                      BR = (BR >> SCAD) | (AD << (54 - SCAD));
                  } else {
                      BR = 0;
                  }
              }
              /* Do the addition now */
              AR = (AR + BR);

              /* Set flag1 to sign and make positive */
              if (AR & FPSMASK) {
                  AR = (AR ^ FPFMASK) + 1;
                  flag1 = 1;
              } else {
                  flag1 = 0;
              }
fnorm:
              if (AR != 0) {
fxnorm:
                  if ((AR & FPNMASK) != 0) { SC += 1;  AR >>= 1; }
                  if (((SC & 0400) != 0) ^ ((SC & 0200) != 0))
                      fxu_hold_set = 1;
                  if (IR != 0130) {   /* !UFA */
                      if ((AR & 00777777777000000000LL) == 0) { SC -= 27; AR <<= 27; }
                      if ((AR & 00777760000000000000LL) == 0) { SC -= 14; AR <<= 14; }
                      if ((AR & 00777000000000000000LL) == 0) { SC -= 9;  AR <<= 9; }
                      if ((AR & 00770000000000000000LL) == 0) { SC -= 6;  AR <<= 6; }
                      if ((AR & 00740000000000000000LL) == 0) { SC -= 4;  AR <<= 4; }
                      if ((AR & 00600000000000000000LL) == 0) { SC -= 2;  AR <<= 2; }
                      if ((AR & 00400000000000000000LL) == 0) { SC -= 1;  AR <<= 1; }
                      if (!nrf && !flag1 &&
                               ((IR & 04) != 0) && ((AR & BIT9) != 0)) {
                          AR += BIT8;
                          nrf = 1;
                          goto fxnorm;
                      }
                  }
                  if (flag1) {
                      AR = (AR ^ FPCMASK) + 1;
                  }
                  MQ = AR & MMASK;
                  AR >>= 27;
                  if (flag1) {
                      AR |= SMASK;
                      MQ |= SMASK;
                  }
              } else if (flag1) {
                 AR =  BIT9 | SMASK;
                 MQ = SMASK;
                 SC++;
              } else {
                 AR = MQ = 0;
                 SC = 0;
              }
              if (((SC & 0400) != 0)) {
                  FLAGS |= OVR|FLTOVR|TRP1;
                  if (!fxu_hold_set) {
                      FLAGS |= FLTUND;
                  }
                  check_apr_irq();
              }
              SCAD = SC ^ ((AR & SMASK) ? 0377 : 0);
              AR &= SMASK|MMASK;
              AR |= ((uint64)(SCAD & 0377)) << 27;
              /* FADL FSBL FMPL */
              if ((IR & 07) == 1) {
                  SC = (SC + (0777 ^  26)) & 0777;
                  if (MQ != 0) {
                      MQ &= MMASK;
                      SC ^= (SC & SMASK) ? 0377 : 0;
                      MQ |= ((uint64)(SC & 0377)) << 27;
                  }
              }

              /* Handle UFA */
              if (IR == 0130) {
                  set_reg((AC + 1) & 017, AR, 0);
                  break;
              }
              break;

    case 0160:      /* FMP */
              /* Compute exponent */
              SC = (((BR & SMASK) ? 0777 : 0) ^ (BR >> 27)) & 0777;
              SC += (((AR & SMASK) ? 0777 : 0) ^ (AR >> 27)) & 0777;
              SC += 0600;
              SC &= 0777;
              /* Make positive and compute result sign */
              flag1 = 0;
              if (AR & SMASK) {
                 AR = CM(AR) + 1;
                 flag1 = 1;
              }
              if (BR & SMASK) {
                 BR = CM(BR) + 1;
                 flag1 = !flag1;
              }
              AR &= MMASK;
              BR &= MMASK;
              AR = (AR * BR);
              goto fnorm;

    case 0170:      /* FDV */
              flag1 = 0;
              SC = ((BR & SMASK) ? 0777 : 0) ^ (BR >> 27);
              SC += ((AR & SMASK) ? 0 : 0777) ^ (AR >> 27);
              SC = (SC + 0201) & 0777;
              if ((IR & 7) == 1) {    /* FDVL */
                   FE = (((BR & SMASK) ? 0777 : 0) ^ (BR >> 27)) - 26;
                   if (BR & SMASK) {
                       MQ = (CM(MQ) + 1) & MMASK;
                       BR = CM(BR);
                       if (MQ == 0)
                           BR = BR + 1;
                       flag1 = 1;
                   }
                   MQ &= MMASK;
              } else {                /* not FDVL */
                   if (BR & SMASK) {
                       BR = CM(BR) + 1;
                       flag1 = 1;
                   }
              }
              if (AR & SMASK) {
                  AR = CM(AR) + 1;
                  flag1 = !flag1;
              }
              /* Clear exponents */
              AR &= MMASK;
              BR &= MMASK;
              /* Check if we need to fix things */
              if (BR >= (AR << 1)) {
                  FLAGS |= OVR|NODIV|FLTOVR|TRP1;  /* Overflow and No Divide */
                  check_apr_irq();
                  sac_inh = 1;
                  break;      /* Done */
              }
                 BR = (BR << 27) + MQ;
              MB = AR;
              if ((IR & 07) == 1) {
                 AR <<= 27;
                 AD = 0;
                 if (BR < AR) {
                    BR <<= 1;
                    SC--;
                 }
                 for (SCAD = 0; SCAD < 27; SCAD++) {
                     AD <<= 1;
                     if (BR >= AR) {
                        BR = BR - AR;
                        AD |= 1;
                     }
                     BR <<= 1;
                 }
                 MQ = BR >> 28;
                 AR = AD;
                 SC++;
              } else {
                 AR = BR / AR;
              }
              if (AR != 0) {
                  if (IR & 04) {
                      AR ++;
                  }
                  if ((AR & BIT8) != 0) {
                      SC += 1;
                      AR >>= 1;
                  }
                   if (SC >= 0600)
                      fxu_hold_set = 1;
                  if (flag1)  {
                      AR = (AR ^ MMASK) + 1;
                      AR |= SMASK;
                  }
              } else if (flag1) {
                 AR =  SMASK | BIT9;
                 SC++;
              } else {
                 AR = 0;
                 SC = 0;
              }
              if (((SC & 0400) != 0)) {
                  FLAGS |= OVR|FLTOVR|TRP1;
                  if (!fxu_hold_set) {
                      FLAGS |= FLTUND;
                  }
                  check_apr_irq();
              }
              SCAD = SC ^ ((AR & SMASK) ? 0377 : 0);
              AR &= SMASK|MMASK;
              AR |= ((uint64)(SCAD & 0377)) << 27;
              /* FDVL */
              if ((IR & 07) == 01 && MQ != 0) {
                  MQ &= MMASK;
                  if (SC & 0400) {
                     FE--;
                  }
                  FE ^= (AR & SMASK) ? 0377 : 0;
                  MQ |= ((uint64)(FE & 0377)) << 27;
              }
              break;

    case 0200: /* FWT */    /* MOVE, MOVS */
              break;

    case 0210:              /* MOVN, MOVM */
              if ((IR & 04) != 0 && (AR & SMASK) == 0)
                  break;
              flag1 = flag3 = 0;
              FLAGS &= 01777;
              if ((((AR & CMASK) ^ CMASK) + 1) & SMASK) {
                  FLAGS |= CRY1;
                  flag1 = 1;
              }
              AD = CM(AR) + 1;
              if (AD & C1) {
                  FLAGS |= CRY0;
                  flag3 = 1;
              }
              if (flag1 != flag3 && !pi_cycle) {
                  FLAGS |= OVR|TRP1;
                  check_apr_irq();
              }
#if KI
              if (AR == SMASK & !pi_cycle)
                  FLAGS |= TRP1;
#endif
              AR = AD & FMASK;
              break;

    case 0220:      /* IMUL, MUL */
              flag3 = 0;
              if (AR & SMASK) {
                 AR = (CM(AR) + 1) & FMASK;
                 flag3 = 1;
              }
              if (BR & SMASK) {
                 BR = (CM(BR) + 1) & FMASK;
                 flag3 = !flag3;
              }

              if ((AR == 0) || (BR == 0)) {
                 AR = MQ = 0;
                 break;
              }
#if !KI
              if ((BR == SMASK))              /* Handle special case */
                 flag3 = !flag3;
#endif
              MQ = AR * (BR & RMASK);         /* 36 * low 18 = 54 bits */
              AR = AR * ((BR >> 18) & RMASK); /* 36 * high 18 = 54 bits */
              MQ += (AR << 18) & LMASK;       /* low order bits */
              AR >>= 18;
              AR = (AR << 1) + (MQ >> 35);
              MQ &= CMASK;
              if ((IR & 4) == 0) {           /* IMUL */
                 if (AR > flag3) {
                     FLAGS |= OVR;
                     check_apr_irq();
                  }
                  if (flag3) {
                      MQ ^= CMASK;
                      MQ++;
                      MQ |= SMASK;
                  }
                  AR = MQ;
                  break;
              }
              if ((AR & SMASK) != 0) {
                 FLAGS |= OVR;
                 check_apr_irq();
              }
              if (flag3) {
                 AR ^= FMASK;
                 MQ ^= CMASK;
                 MQ += 1;
                 if ((MQ & SMASK) != 0) {
                    AR += 1;
                    MQ &= CMASK;
                 }
              }
              AR &= FMASK;
              MQ = (MQ & ~SMASK) | (AR & SMASK);
              break;

    case 0230:       /* IDIV, DIV */
              flag1 = 0;
              flag3 = 0;
              if ((IR & 4) == 0) { /* IDIV */
                  if (BR & SMASK) {
                     BR = (CM(BR) + 1) & FMASK;
                     flag1 = !flag1;
                  }

                  if (BR == 0) {          /* Check for overflow */
                      FLAGS |= OVR|NODIV; /* Overflow and No Divide */
                      sac_inh=1;          /* Don't touch AC */
                      check_apr_irq();
                      break;              /* Done */
                  }

                  if (AR & SMASK) {
                     AR = (CM(AR) + 1) & FMASK;
                     flag1 = !flag1;
                     flag3 = 1;
                  }

                  MQ = AR % BR;
                  AR = AR / BR;
                  if (flag1)
                     AR = (CM(AR) + 1) & FMASK;
                  if (flag3)
                     MQ = (CM(MQ) + 1) & FMASK;
              } else {             /* DIV */
                   MQ = get_reg(AC + 1);
                   if (AR & SMASK) {
                       AD = (CM(MQ) + 1) & FMASK;
                       MQ = AR;
                       AR = AD;
                       AD = (CM(MQ)) & FMASK;
                       MQ = AR;
                       AR = AD;
                       if ((MQ & CMASK) == 0)
                           AR = (AR + 1) & FMASK;
                       flag1 = 1;
                   }
                   if (BR & SMASK)
                        AD = (AR + BR) & FMASK;
                   else
                        AD = (AR + CM(BR) + 1) & FMASK;
                   MQ = (MQ << 1) & FMASK;
                   MQ |= (AD & SMASK) != 0;
                   SC = 35;
                   if ((AD & SMASK) == 0) {
                       FLAGS |= OVR|NODIV; /* Overflow and No Divide */
                       sac_inh=1;
                       check_apr_irq();
                       break;      /* Done */
                   }

                   while (SC != 0) {
                           if (((BR & SMASK) != 0) ^ ((MQ & 01) != 0))
                                AD = (AR + CM(BR) + 1);
                           else
                                AD = (AR + BR);
                           AR = (AD << 1) | ((MQ & SMASK) ? 1 : 0);
                           AR &= FMASK;
                           MQ = (MQ << 1) & FMASK;
                           MQ |= (AD & SMASK) == 0;
                           SC--;
                   }
                   if (((BR & SMASK) != 0) ^ ((MQ & 01) != 0))
                       AD = (AR + CM(BR) + 1);
                   else
                       AD = (AR + BR);
                   AR = AD & FMASK;
                   MQ = (MQ << 1) & FMASK;
                   MQ |= (AD & SMASK) == 0;
                   if (AR & SMASK) {
                        if (BR & SMASK)
                             AD = (AR + CM(BR) + 1) & FMASK;
                        else
                             AD = (AR + BR) & FMASK;
                        AR = AD;
                   }

                   if (flag1)
                       AR = (CM(AR) + 1) & FMASK;
                   if (flag1 ^ ((BR & SMASK) != 0)) {
                       AD = (CM(MQ) + 1) & FMASK;
                       MQ = AR;
                       AR = AD;
                   } else {
                       AD = MQ;
                       MQ = AR;
                       AR = AD;
                   }
              }
              break;

    case 0240: /* Shift */
              BR = AB;
       switch (IR & 07) {
       case 0: /* ASH */
              SC = ((AB & LSIGN) ? (0377 ^ AB) + 1 : AB) & 0377;
              if (SC == 0)
                  break;
              AD = (AR & SMASK) ? FMASK : 0;
              if (AB & LSIGN) {
                  if (SC < 35)
                     AR = ((AR >> SC) | (AD << (36 - SC))) & FMASK;
                 else
                     AR = AD;
              } else {
                 if (((AD << SC) & ~CMASK) != ((AR << SC) & ~CMASK)) {
                     FLAGS |= OVR;
                     check_apr_irq();
                 }
                 AR = ((AR << SC) & CMASK) | (AR & SMASK);
              }
              break;
       case 1: /* ROT */
#if KI
              SC = (AB & LSIGN) ? 
                      ((AB & 0377) ? (((0377 ^ AB) + 1) & 0377) : 0400) : (AB & 0377);
#else
              SC = ((AB & LSIGN) ? (0377 ^ AB) + 1 : AB) & 0377;
#endif
              if (SC == 0)
                  break;
              SC = SC % 36;
              if (AB & LSIGN)
                  SC = 36 - SC;
              AR = ((AR << SC) | (AR >> (36 - SC))) & FMASK;
              break;

       case 2: /* LSH */
              SC = ((AB & LSIGN) ? (0377 ^ AB) + 1 : AB) & 0777;
              if (SC == 0)
                  break;
              if (AB & LSIGN) {
                  AR = AR >> SC;
              } else {
                  AR = (AR << SC) & FMASK;
              }
               break;

       case 3:  /* JFFO */
              SC = 0;
              if (AR != 0) {
                  PC = AB;
                  f_pc_inh = 1;
                  SC = nlzero(AR);
              }
              set_reg(AC + 1, SC, 0);
              break;

       case 4: /* ASHC */
              SC = ((AB & LSIGN) ? (0377 ^ AB) + 1 : AB) & 0377;
              if (SC == 0)
                  break;
              if (SC > 70)
                   SC = 70;
              AD = (AR & SMASK) ? FMASK : 0;
              AR &= CMASK;
              MQ &= CMASK;
              if (AB & LSIGN) {
                 if (SC >= 35) {
                     MQ = ((AR >> (SC - 35)) | (AD << (70 - SC))) & FMASK;
                     AR = AD;
                 } else {
                     MQ = (AD & SMASK) | (MQ >> SC) |
                             ((AR << (35 - SC)) & CMASK);
                     AR = (AD & SMASK) |
                     ((AR >> SC) | (AD << (35 - SC))) & FMASK;
                 }
              } else {
                 if (SC >= 35) {
                      if (((AD << SC) & ~CMASK) != ((AR << SC) & ~CMASK)) {
                         FLAGS |= OVR;
                         check_apr_irq();
                      }
                      AR = (AD & SMASK) | ((AR << (SC - 35)) & CMASK);
                      MQ = (AD & SMASK);
                 } else {
                      if ((((AD & CMASK) << SC) & ~CMASK) != ((AR << SC) & ~CMASK)) {
                         FLAGS |= OVR;
                         check_apr_irq();
                      }
                      AR = (AD & SMASK) | ((AR << SC) & CMASK) |
                             (MQ >> (35 - SC));
                      MQ = (AD & SMASK) | ((MQ << SC) & CMASK);
                 }
              }
              break;

       case 5: /* ROTC */
#if KI
              SC = (AB & LSIGN) ? 
                      ((AB & 0377) ? (((0377 ^ AB) + 1) & 0377) : 0400) : (AB & 0377);
#else
              SC = ((AB & LSIGN) ? (0777 ^ AB) + 1 : AB) & 0777;
#endif
              if (SC == 0)
                  break;
              SC = SC % 72;
              if (AB & LSIGN)
                  SC = 72 - SC;
              if (SC >= 36) {
                  AD = MQ;
                  MQ = AR;
                  AR = AD;
                  SC -= 36;
              }
              AD = ((AR << SC) | (MQ >> (36 - SC))) & FMASK;
              MQ = ((MQ << SC) | (AR >> (36 - SC))) & FMASK;
              AR = AD;
              break;

       case 6: /* LSHC */
              SC = ((AB & LSIGN) ? (0377 ^ AB) + 1 : AB) & 0377;
              if (SC == 0)
                  break;
              if (SC > 71) {
                  AR = 0;
                  MQ = 0;
              } else {
                  if (SC > 36) {
                     if (AB & LSIGN) {
                         AR = MQ;
                         MQ = 0;
                     } else {
                         MQ = AR;
                         AR = 0;
                     }
                     SC -= 36;
                 }
                 if (AB & LSIGN) {
                     MQ = ((MQ >> SC) | (AR << (36 - SC))) & FMASK;
                     AR = AR >> SC;
                 } else {
                     AR = ((AR << SC) | (MQ >> (36 - SC))) & FMASK;
                     MQ = (MQ << SC) & FMASK;
                 }
              }
              break;

       case 7: /* UUO  */
              goto unasign;
       }
       break;

    case 0250: /* Branch */
       switch(IR & 07) {
       case 0: /* EXCH */
              set_reg(AC, BR, 0);
              break;

       case 1: /* BLT */
              BR = AB;
              do {
                 if (sim_interval <= 0) {
                      sim_process_event ();
                 }
                 /* Allow for interrupt */
                 if (pi_enable && pi_pending) {
                      pi_rq = check_irq_level();
                      if (pi_rq) {
                              f_pc_inh = 1;
                              f_load_pc = 0;
                              f_inst_fetch = 0;
                              set_reg(AC, AR, 0);
                              break;
                      }
                 }
                 AB = (AR >> 18) & RMASK;
                 if (Mem_read(0))
                      break;
                 AB = (AR & RMASK);
                 if (Mem_write(0))
                      break;
                 AD = (AR & RMASK) + CM(BR) + 1;
                 AR = (AR + 01000001LL);
              } while ((AD & C1) == 0);
              break;

       case 2: /* AOBJP */
              AR = AOB(AR);
              if ((AR & SMASK) == 0) {
                  PC = AB;
                  f_pc_inh = 1;
              }
              AR &= FMASK;
              break;

       case 3: /* AOBJN */
              AR = AOB(AR);
              if ((AR & SMASK) != 0) {
                  PC = AB;
                  f_pc_inh = 1;
              }
              AR &= FMASK;
              break;

       case 4: /* JRST */      /* AR Frm PC */
              PC = AR & RMASK;
              if (uuo_cycle | pi_cycle) {
                 FLAGS &= ~USER; /* Clear USER */
              }
              /* JEN */
              if (AC & 010) { /* Restore interrupt level. */
                 if ((FLAGS & (USER|USERIO)) == USER) {
                      goto uuo;
                 } else {
                      pi_restore = 1;
                 }
              }
              /* HALT */
              if (AC & 04) {
                 if ((FLAGS & (USER|USERIO)) == USER) {
                      goto uuo;
                 } else {
                      reason = STOP_HALT;
                 }
              }
              /* JRSTF */
              if (AC & 02) {
                 FLAGS &= ~(OVR|NODIV|FLTUND|BYTI|FLTOVR|CRY1|CRY0);
                 /* If executive mode, copy USER and UIO */
                 if ((FLAGS & USER) == 0)
                    FLAGS |= (AR >> 23) & (USER|USERIO);
                 /* Can always clear UIO */
                 if (((AR >> 23) & 0100) == 0)
                    FLAGS &= ~USERIO;
                 FLAGS |= (AR >> 23) & (OVR|NODIV|FLTUND|BYTI|FLTOVR|CRY1|CRY0);
                 check_apr_irq();

              }
              if (AC & 01) {  /* Enter User Mode */
                 FLAGS |= USER;
#if KI
                 FLAGS &= ~PUBLIC;
#endif
              }
              f_pc_inh = 1;
              break;

       case 5: /* JFCL */
              if ((FLAGS >> 9) & AC) {
                  PC = AR;
                  f_pc_inh = 1;
              }
              FLAGS &=  017777 ^ (AC << 9);
              break;

       case 6: /* XCT */
              f_load_pc = 0;
              f_pc_inh = 1;
#if KI
              if ((FLAGS & USER) == 0)
                  xct_flag = AC;
#endif
              break;

       case 7:  /* MAP */
#if KI
              f = AB >> 9;
              if ((FLAGS & USER) != 0) {
                  /* Check if small user and outside range */
                  if (small_user && (f & 0340) != 0) {
                      AR = 0420000LL;   /* Page failure, no match */
                      break;
                  }
                  AR = ub_ptr;
              } else {
                  if (!page_enable) {
                      AR = 0020000LL + (uint64)f; /* direct map */
                      break;
                  }

                  /* Map executive to use space */
                  if ((f & 0340) == 0340) {
                      AR = ub_ptr;
                      f += 01000 - 0340;
                  /* Executive high segment */
                  } else if (f & 0400) {
                      AR = eb_ptr;
                  } else {
                      AR = 0020000LL + (uint64)f; /* direct map */
                      break;
                  }
              }
              AB = AR + (f >> 1);
              Mem_read(0);
              AR = MB;
              if (f & 1)
                 AR >>= 18;
              AR &= 0357777LL;
#endif
              break;
       }
       break;

    case 0260:      /* Stack, JUMP */
       switch(IR & 07) {
       case 0: /* PUSHJ */     /* AR Frm PC */
              BR = AB;
              AR = AOB(AR);
              AB = AR & RMASK;
              if (AR & C1) {
                 push_ovf = 1;
#if KI
                 FLAGS |= TRP2;
#endif
                 check_apr_irq();
              }
              AR &= FMASK;
              MB = ((uint64)(FLAGS) << 23) | ((PC + !pi_cycle) & RMASK);
              FLAGS &= ~ 0434;
              if (uuo_cycle | pi_cycle) {
                 FLAGS &= ~USER; /* Clear USER */
              }
              Mem_write(uuo_cycle | pi_cycle);
              PC = BR & RMASK;
              f_pc_inh = 1;
              break;

       case 1: /* PUSH */
              AR = AOB(AR);
              AB = AR & RMASK;
              if (AR & C1) {
                 push_ovf = 1;
#if KI
                 FLAGS |= TRP2;
#endif
                 check_apr_irq();
              }
              AR &= FMASK;
              MB = BR;
              Mem_write(0);
              break;

       case 2: /* POP */
              AB = AR & RMASK;
              if (Mem_read(0))
                  break;
              AR = SOB(AR);
              AB = BR;
              if (Mem_write(0))
                  break;
              if ((AR & C1) == 0) {
                  push_ovf = 1;
#if KI
                  FLAGS |= TRP2;
#endif
                  check_apr_irq();
              }
              AR &= FMASK;
              break;

       case 3: /* POPJ */
              AB = AR & RMASK;
              if (Mem_read(0))
                  break;
              PC = MB & RMASK;
              AR = SOB(AR);
              if ((AR & C1) == 0) {
                  push_ovf = 1;
#if KI           
                  FLAGS |= TRP2;
#endif
                  check_apr_irq();
              }
              AR &= FMASK;
              f_pc_inh = 1;
              break;

       case 4: /* JSR */       /* AR Frm PC */
              AD = ((uint64)(FLAGS) << 23) |
                      ((PC + !pi_cycle) & RMASK);
              FLAGS &= ~ 0434;
              if (uuo_cycle | pi_cycle) {
                 FLAGS &= ~USER; /* Clear USER */
              }
              PC = (AR + pi_cycle) & RMASK;
              AR = AD;
              break;

       case 5: /* JSP */       /* AR Frm PC */
              AD = ((uint64)(FLAGS) << 23) |
                      ((PC + !pi_cycle) & RMASK);
              FLAGS &= ~ 0434;
              if (uuo_cycle | pi_cycle) {
                 FLAGS &= ~USER; /* Clear USER */
              }
              PC = AR & RMASK;
              AR = AD;
              f_pc_inh = 1;
              break;

       case 6: /* JSA */       /* AR Frm PC */
              set_reg(AC, (AR << 18) | ((PC + 1) & RMASK), 0);
              if (uuo_cycle | pi_cycle) {
                 FLAGS &= ~USER; /* Clear USER */
              }
              PC = AR & RMASK;
              AR = BR;
              break;

       case 7: /* JRA */
              AD = AB;        /* Not in hardware */
              AB = (get_reg(AC) >> 18) & RMASK;
              if (Mem_read(uuo_cycle | pi_cycle))
                   break;
              set_reg(AC, MB, 0);
              PC = AD & RMASK;
              f_pc_inh = 1;
              break;
       }
       break;

    case 0270: /* ADD, SUB */
              if (IR & 04) {
                  flag1 = flag3 = 0;
                  FLAGS &= 01777;
                  if ((((AR & CMASK) ^ CMASK) + (BR & CMASK) + 1) & SMASK) {
                      FLAGS |= CRY1;
                      flag1 = 1;
                  }
                  BR = CM(AR) + BR + 1;
                  if (BR & C1) {
                      FLAGS |= CRY0;
                      flag3 = 1;
                  }
                  if (flag1 != flag3) {
                      FLAGS |= OVR;
                      check_apr_irq();
                  }
              } else {
                  flag1 = flag3 = 0;
                  FLAGS &= 01777;
                  if (((AR & CMASK) + (BR & CMASK)) & SMASK) {
                      FLAGS |= CRY1;
                      flag1 = 1;
                  }
                  BR = AR + BR;
                  if (BR & C1) {
                      FLAGS |= CRY0;
                      flag3 = 1;
                  }
                  if (flag1 != flag3) {
                      FLAGS |= OVR;
                      check_apr_irq();
                  }
              }
              BR &= FMASK;
              AR = BR;
              break;

    case 0300: /* SKIP */   /* CAM */
    case 0310:              /* CAI */
              f = 0;
              AD = (CM(AR) + get_reg(AC)) + 1;
              if (((get_reg(AC) & SMASK) != 0) && (AR & SMASK) == 0)
                 f = 1;
              if (((get_reg(AC) & SMASK) == (AR & SMASK)) &&
                      (AD & SMASK) != 0)
                 f = 1;
              goto skip_op;

    case 0320: /* JUMP */
    case 0330: /* SKIP */
              AD = AR;
              f = ((AD & SMASK) != 0);
              goto skip_op;                   /* JUMP, SKIP */

    case 0340: /* AOJ */
    case 0350: /* AOS */
    case 0360: /* SOJ */
    case 0370: /* SOS */
              flag1 = flag3 = 0;
              FLAGS &= 01777;
              AD = (IR & 020) ? FMASK : 1;
              if (((AR & CMASK) + (AD & CMASK)) & SMASK) {
                  FLAGS |= CRY1;
                  flag1 = 1;
              }
              AD = AR + AD;
              if (AD & C1) {
                  FLAGS |= CRY0;
                  flag3 = 1;
              }
              if (flag1 != flag3) {
                  FLAGS |= OVR;
                  check_apr_irq();
              }
              f = ((AD & SMASK) != 0);
skip_op:
              AD &= FMASK;
              AR = AD;
              f |= ((AD == 0) << 1);
              f = f & IR;
              if (((IR & 04) != 0) == (f == 0)) {
                 switch(IR & 070) {
                 case 000:
                 case 010:
                 case 030:
                 case 050:
                 case 070:
                         PC = (PC + 1) & RMASK;
                         break;
                 case 020:
                 case 040:
                 case 060:
                         PC = AB;
                         f_pc_inh = 1;
                         break;
                 }
#if KI
              } else if (pi_cycle) {
                 switch(IR & 070) {
                 case 030:
                 case 050:
                 case 070:
                         pi_ov = pi_hold = 1;
                         break;
                 case 000:
                 case 010:
                 case 020:
                 case 040:
                 case 060:
                         break;
                 }
#endif
              }
              break;

    case 0400: /* Bool */
    case 0410:
    case 0420:
    case 0430:
    case 0440:
    case 0450:
    case 0460:
    case 0470:
       switch ((IR >> 2) & 017) {
       case  0: AR = 0;                   break; /* SETZ */
       case  1: AR = AR & BR;             break; /* AND */
       case  2: AR = AR & CM(BR);         break; /* ANDCA */
       case  3:                           break; /* SETM */
       case  4: AR = CM(AR) & BR;         break; /* ANDCM */
       case  5: AR = BR;                  break; /* SETA */
       case  6: AR = AR ^ BR;             break; /* XOR */
       case  7: AR = CM(CM(AR) & CM(BR)); break; /* IOR */
       case  8: AR = CM(AR) & CM(BR);     break; /* ANDCB */
       case  9: AR = CM(AR ^ BR);         break; /* EQV */
       case 10: AR = CM(BR);              break; /* SETCA */
       case 11: AR = CM(CM(AR) & BR);     break; /* ORCA */
       case 12: AR = CM(AR);              break; /* SETCM */
       case 13: AR = CM(AR & CM(BR));     break; /* ORCM */
       case 14: AR = CM(AR & BR);         break; /* ORCB */
       case 15: AR = FMASK;               break; /* SETO */
       }
       break;

    case 0500: /* HxL */
              AR = (AR & LMASK) | (BR & RMASK);
              break;

    case 0510: /* HxLZ */
              AR = (AR & LMASK);
              break;

    case 0520: /* HxLO */
              AR = (AR & LMASK) | RMASK;
              break;

    case 0530: /* HxLE */
              AD = ((AR & SMASK) != 0) ? RMASK : 0;
              AR = (AR & LMASK) | AD;
              break;

    case 0540: /* HxR */
              AR = (BR & LMASK) | (AR & RMASK);
              break;

    case 0550: /* HxRZ */
              AR = (AR & RMASK);
              break;

    case 0560: /* HxRO */
              AR = LMASK | (AR & RMASK);
              break;

    case 0570: /* HxRE */
              AD = ((AR & LSIGN) != 0) ? LMASK: 0;
              AR = AD | (AR & RMASK);
              break;

    case 0600: /* TxN */
    case 0610:
              MQ = AR;            /* N */
              goto test_op;

    case 0620: /* TxZ */
    case 0630:
              MQ = CM(AR) & BR;   /* Z */
              goto test_op;

    case 0640: /* TxC */
    case 0650:
              MQ = AR ^ BR;       /* C */
              goto test_op;

    case 0660: /* TxO */
    case 0670:
              MQ = AR | BR;       /* O */
test_op:
              AR &= BR;
              f = ((AR == 0) & ((IR >> 1) & 1)) ^ ((IR >> 2) & 1);
              if (f)
                  PC = (PC + 1) & RMASK;
              AR = MQ;
              break;

    case 0700: /* IOT */
    case 0710:
    case 0720:
    case 0730:
    case 0740:
    case 0750:
    case 0760:
    case 0770:
              if ((FLAGS & (USER|USERIO)) == USER && !pi_cycle) {
                  /* User and not User I/O */
                  goto muuo;
              } else {
                  int d = ((IR & 077) << 1) | ((AC & 010) != 0);
                  switch(AC & 07) {
                  case 0:     /* 00 BLKI */
                  case 2:     /* 10 BLKO */
                          if (Mem_read(pi_cycle))
                              break;
                          AR = MB;
                          if (hst_lnt) {
                                  hst[hst_p].mb = AR;
                          }
                          AC |= 1;    /* Make into DATAI/DATAO */
                          f_load_pc = 0;
                          f_inst_fetch = 0;
                          AR = AOB(AR);
                          if (AR & C1) {
                              pi_ov = f_pc_inh = 1;
                          } else if (!pi_cycle) {
                              PC = (PC + 1) & RMASK;
                          }
                          AR &= FMASK;
                          MB = AR;
                          if (Mem_write(pi_cycle))
                              break;
                          AB = AR & RMASK;
                          goto fetch_opr;
                          break;
                  case 1:     /* 04 DATAI */
                          dev_tab[d](DATAI|(d<<2), &AR);
                          MB = AR;
                          Mem_write(pi_cycle);
                          break;
                  case 3:     /* 14 DATAO */
                          if (Mem_read(pi_cycle))
                             break;
                          AR = MB;
                          dev_tab[d](DATAO|(d<<2), &AR);
                          break;
                  case 4:     /* 20 CONO */
                          dev_tab[d](CONO|(d<<2), &AR);
                          break;
                  case 5:     /* 24 CONI */
                          dev_tab[d](CONI|(d<<2), &AR);
                          MB = AR;
                          Mem_write(pi_cycle);
                          break;
                  case 6:     /* 30 CONSZ */
                          dev_tab[d](CONI|(d<<2), &AR);
                          AR &= AB;
                          if (AR == 0)
                              PC = (PC + 1) & RMASK;
                          break;
                  case 7:     /* 34 CONSO */
                          dev_tab[d](CONI|(d<<2), &AR);
                          AR &= AB;
                          if (AR != 0)
                              PC = (PC + 1) & RMASK;
                          break;
                  }
              }
              break;
    }
    if (!sac_inh && (i_flags & (SCE|FCEPSE))) {
        MB = AR;
        if (Mem_write(0))
           goto last;
    }
    if (!sac_inh && ((i_flags & SAC) || ((i_flags & SACZ) && AC != 0)))
        set_reg(AC, AR, 0);    /* blank, I, B */

    if (!sac_inh && (i_flags & SAC2))
        set_reg((AC+1) & 017, MQ, 0);

    if (hst_lnt) {
        hst[hst_p].fmb = AR;
    }


last:
    if (!f_pc_inh & !pi_cycle) {
        PC = (PC + 1) & RMASK;
    }

    if (pi_cycle) {
       if ((IR & 0700) == 0700 && ((AC & 04) == 0)) {
           pi_hold = pi_ov;
           if (!pi_hold & f_inst_fetch) {
                pi_restore = 1;
                pi_cycle = 0;
           } else {
                AB = 040 | (pi_enc << 1) | pi_ov;
                pi_ov = 0;
                pi_hold = 0;
                goto fetch;
           }
       } else if (pi_hold) {
            AB = 040 | (pi_enc << 1) | pi_ov;
            pi_ov = 0;
            pi_hold = 0;
            goto fetch;
       } else {
            f_inst_fetch = 1;
            f_load_pc = 1;
            pi_cycle = 0;
       }
    }

    if (pi_restore) {
        restore_pi_hold();
        pi_restore = 0;
    }
    sim_interval--;
}
/* Should never get here */

return reason;
}

t_stat
rtc_srv(UNIT * uptr)
{
    int32 t;
    t = sim_rtcn_calb (rtc_tps, TMR_RTC);
    sim_activate_after(uptr, 1000000/rtc_tps);
    tmxr_poll = t/2;
    clk_flg = 1;
    if (clk_en) {
        set_interrupt(4, clk_irq);
    }
    return SCPE_OK;
}

/* Reset routine */

t_stat cpu_reset (DEVICE *dptr)
{
int     i;
BYF5 = uuo_cycle = 0;
#if !KI
Pl = Ph = Rl = Rh = Pflag = 0;
#endif
push_ovf = mem_prot = nxm_flag = clk_flg = 0;
PIR = PIH = PIE = pi_enable = parity_irq = 0;
pi_pending = pi_req = pi_enc = apr_irq = 0;
ov_irq =fov_irq =clk_en =clk_irq = 0;
pi_restore = pi_hold = 0;
#if KI
ub_ptr = eb_ptr = 0;
pag_reload = ac_stack = 0;
fm_sel = small_user = user_addr_cmp = page_enable = 0;
#endif
for(i=0; i < 128; dev_irq[i++] = 0);
sim_brk_types = sim_brk_dflt = SWMASK ('E');
sim_rtcn_init (cpu_unit.wait, TMR_RTC);
sim_activate(&cpu_unit, cpu_unit.wait);
return SCPE_OK;
}

/* Memory examine */

t_stat cpu_ex (t_value *vptr, t_addr ea, UNIT *uptr, int32 sw)
{
if (vptr == NULL)
    return SCPE_ARG;
if (ea < 020)
    *vptr = FM[ea] & FMASK;
else {
    if (sw & SWMASK ('V')) {
        if (ea >= MAXMEMSIZE)
            return SCPE_REL;
        }
    if (ea >= MEMSIZE)
        return SCPE_NXM;
    *vptr = M[ea] & FMASK;
    }
return SCPE_OK;
}

/* Memory deposit */

t_stat cpu_dep (t_value val, t_addr ea, UNIT *uptr, int32 sw)
{
if (ea < 020)
    FM[ea] = val & FMASK;
else {
    if (sw & SWMASK ('V')) {
        if (ea >= MAXMEMSIZE)
            return SCPE_REL;
        }
    if (ea >= MEMSIZE)
        return SCPE_NXM;
    M[ea] = val & FMASK;
    }
return SCPE_OK;
}

/* Memory size change */

t_stat cpu_set_size (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
uint64 mc = 0;
uint32 i;

if ((val <= 0) || ((val * 1024) > MAXMEMSIZE))
    return SCPE_ARG;
for (i = val; i < MEMSIZE; i++)
    mc = mc | M[i];
if ((mc != 0) && (!get_yn ("Really truncate memory [N]?", FALSE)))
    return SCPE_OK;
MEMSIZE = val * 16 * 1024;
for (i = MEMSIZE; i < MAXMEMSIZE; i++)
    M[i] = 0;
return SCPE_OK;
}

/* Build device dispatch table */
t_bool build_dev_tab (void)
{
DEVICE *dptr;
DIB *dibp;
uint32 i, j;

for (i = 0; i < 128; i++)
    dev_tab[i] = &null_dev;
dev_tab[0] = &dev_apr;
dev_tab[1] = &dev_pi;
#if KI
dev_tab[2] = &dev_pag;
#endif
for (i = 0; (dptr = sim_devices[i]) != NULL; i++) {
    dibp = (DIB *) dptr->ctxt;
    if (dibp && !(dptr->flags & DEV_DIS)) {             /* enabled? */
         for (j = 0; j < dibp->num_devs; j++) {         /* loop thru disp */
              if (dibp->io) {                           /* any dispatch? */
                   if (dev_tab[(dibp->dev_num >> 2) + j] != &null_dev) {
                                                       /* already filled? */
                           printf ("%s device number conflict at %02o\n",
                              sim_dname (dptr), dibp->dev_num + j << 2);
                      if (sim_log)
                        fprintf (sim_log, "%s device number conflict at %02o\n",
                                   sim_dname (dptr), dibp->dev_num + j << 2);
                       return TRUE;
                   }
              dev_tab[(dibp->dev_num >> 2) + j] = dibp->io;  /* fill */
             }                                       /* end if dsp */
           }                                           /* end for j */
        }                                               /* end if enb */
    }                                                   /* end for i */
return FALSE;
}


/* Set history */

t_stat cpu_set_hist (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
int32 i, lnt;
t_stat r;

if (cptr == NULL) {
    for (i = 0; i < hst_lnt; i++)
        hst[i].pc = 0;
    hst_p = 0;
    return SCPE_OK;
    }
lnt = (int32) get_uint (cptr, 10, HIST_MAX, &r);
if ((r != SCPE_OK) || (lnt && (lnt < HIST_MIN)))
    return SCPE_ARG;
hst_p = 0;
if (hst_lnt) {
    free (hst);
    hst_lnt = 0;
    hst = NULL;
    }
if (lnt) {
    hst = (InstHistory *) calloc (lnt, sizeof (InstHistory));
    if (hst == NULL)
        return SCPE_MEM;
    hst_lnt = lnt;
    }
return SCPE_OK;
}

/* Show history */

t_stat cpu_show_hist (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
int32 k, di, lnt;
char *cptr = (char *) desc;
t_stat r;
int reg;
t_value sim_eval;
InstHistory *h;

if (hst_lnt == 0)                                       /* enabled? */
    return SCPE_NOFNC;
if (cptr) {
    lnt = (int32) get_uint (cptr, 10, hst_lnt, &r);
    if ((r != SCPE_OK) || (lnt == 0))
        return SCPE_ARG;
    }
else lnt = hst_lnt;
di = hst_p - lnt;                                       /* work forward */
if (di < 0)
    di = di + hst_lnt;
fprintf (st, "PC      AC            EA        AR            RES           FLAGS IR\n\n");
for (k = 0; k < lnt; k++) {                             /* print specified */
    h = &hst[(++di) % hst_lnt];                         /* entry pointer */
    if (h->pc & HIST_PC) {                              /* instruction? */
        fprintf (st, "%06o  ", (uint32)(h->pc & RMASK));
        fprint_val (st, h->ac, 8, 36, PV_RZRO);
        fputs ("  ", st);
        fprintf (st, "%06o  ", h->ea);
        fputs ("  ", st);
        fprint_val (st, h->mb, 8, 36, PV_RZRO);
        fputs ("  ", st);
        fprint_val (st, h->fmb, 8, 36, PV_RZRO);
        fputs ("  ", st);
        fprintf (st, "%06o  ", h->flags);
        sim_eval = h->ir;
        fprint_val (st, sim_eval, 8, 36, PV_RZRO);
        fputs ("  ", st);
        if ((fprint_sym (st, h->pc & RMASK, &sim_eval, &cpu_unit, SWMASK ('M'))) > 0) {
            fputs ("(undefined) ", st);
            fprint_val (st, h->ir, 8, 36, PV_RZRO);
            }
        fputc ('\n', st);                               /* end line */
        }                                               /* end else instruction */
    }                                                   /* end for */
return SCPE_OK;
}

t_stat
cpu_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
#if !KI10
    fprintf(st, "KA10 CPU\n\n");
#else
    fprintf(st, "KI10 CPU\n\n");
#endif
    fprintf(st, "To stop the cpu use the command:\n\n");
    fprintf(st, "    sim> SET CTY STOP\n\n");
    fprintf(st, "This will write a 1 to location %03o, causing TOPS10 to stop\n", CTY_SWITCH);
    fprint_set_help(st, dptr);
    fprint_show_help(st, dptr);
    return SCPE_OK;
}

const char *
cpu_description (DEVICE *dptr)
{
#if !KI10
    return "KA10 CPU";
#else
    return "KI10 CPU";
#endif
}
