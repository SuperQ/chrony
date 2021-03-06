/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
 * Copyright (C) Miroslav Lichvar  2009-2012, 2014-2015
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  This is the module specific to the Linux operating system.

  */

#include "config.h"

#include "sysincl.h"

#include <sys/utsname.h>

#if defined(HAVE_SCHED_SETSCHEDULER)
#  include <sched.h>
#endif

#if defined(HAVE_MLOCKALL)
#  include <sys/mman.h>
#include <sys/resource.h>
#endif

#ifdef FEAT_PRIVDROP
#include <sys/prctl.h>
#include <sys/capability.h>
#endif

#ifdef FEAT_SCFILTER
#include <sys/prctl.h>
#include <seccomp.h>
#ifdef FEAT_PHC
#include <linux/ptp_clock.h>
#endif
#ifdef FEAT_PPS
#include <linux/pps.h>
#endif
#ifdef FEAT_RTC
#include <linux/rtc.h>
#endif
#endif

#include "sys_linux.h"
#include "sys_timex.h"
#include "conf.h"
#include "logging.h"
#include "privops.h"
#include "util.h"

/* Frequency scale to convert from ppm to the timex freq */
#define FREQ_SCALE (double)(1 << 16)

/* Definitions used if missed in the system headers */
#ifndef ADJ_SETOFFSET
#define ADJ_SETOFFSET           0x0100  /* add 'time' to current time */
#endif
#ifndef ADJ_NANO
#define ADJ_NANO                0x2000  /* select nanosecond resolution */
#endif

/* This is the uncompensated system tick value */
static int nominal_tick;

/* Current tick value */
static int current_delta_tick;

/* The maximum amount by which 'tick' can be biased away from 'nominal_tick'
   (sys_adjtimex() in the kernel bounds this to 10%) */
static int max_tick_bias;

/* The kernel USER_HZ constant */
static int hz;
static double dhz; /* And dbl prec version of same for arithmetic */

/* Flag indicating whether adjtimex() can step the clock */
static int have_setoffset;

/* The assumed rate at which the effective frequency and tick values are
   updated in the kernel */
static int tick_update_hz;

/* ================================================== */

inline static long
our_round(double x)
{
  long y;

  if (x > 0.0)
    y = x + 0.5;
  else
    y = x - 0.5;

  return y;
}

/* ================================================== */
/* Positive means currently fast of true time, i.e. jump backwards */

static int
apply_step_offset(double offset)
{
  struct timex txc;

  txc.modes = ADJ_SETOFFSET | ADJ_NANO;
  txc.time.tv_sec = -offset;
  txc.time.tv_usec = 1.0e9 * (-offset - txc.time.tv_sec);
  if (txc.time.tv_usec < 0) {
    txc.time.tv_sec--;
    txc.time.tv_usec += 1000000000;
  }

  if (SYS_Timex_Adjust(&txc, 1) < 0)
    return 0;

  return 1;
}

/* ================================================== */
/* This call sets the Linux kernel frequency to a given value in parts
   per million relative to the nominal running frequency.  Nominal is taken to
   be tick=10000, freq=0 (for a USER_HZ==100 system, other values otherwise).
   The convention is that this is called with a positive argument if the local
   clock runs fast when uncompensated.  */

static double
set_frequency(double freq_ppm)
{
  struct timex txc;
  long required_tick;
  double required_freq;
  int required_delta_tick;

  required_delta_tick = our_round(freq_ppm / dhz);

  /* Older kernels (pre-2.6.18) don't apply the frequency offset exactly as
     set by adjtimex() and a scaling constant (that depends on the internal
     kernel HZ constant) would be needed to compensate for the error. Because
     chronyd is closed loop it doesn't matter much if we don't scale the
     required frequency, but we want to prevent thrashing between two states
     when the system's frequency error is close to a multiple of USER_HZ.  With
     USER_HZ <= 250, the maximum frequency adjustment of 500 ppm overlaps at
     least two ticks and we can stick to the current tick if it's next to the
     required tick. */
  if (hz <= 250 && (required_delta_tick + 1 == current_delta_tick ||
                    required_delta_tick - 1 == current_delta_tick)) {
    required_delta_tick = current_delta_tick;
  }

  required_freq = -(freq_ppm - dhz * required_delta_tick);
  required_tick = nominal_tick - required_delta_tick;

  txc.modes = ADJ_TICK | ADJ_FREQUENCY;
  txc.freq = required_freq * FREQ_SCALE;
  txc.tick = required_tick;

  SYS_Timex_Adjust(&txc, 0);

  current_delta_tick = required_delta_tick;

  return dhz * current_delta_tick - txc.freq / FREQ_SCALE;
}

/* ================================================== */
/* Read the ppm frequency from the kernel */

static double
read_frequency(void)
{
  struct timex txc;

  txc.modes = 0;

  SYS_Timex_Adjust(&txc, 0);

  current_delta_tick = nominal_tick - txc.tick;

  return dhz * current_delta_tick - txc.freq / FREQ_SCALE;
}

/* ================================================== */

/* Estimate the value of USER_HZ given the value of txc.tick that chronyd finds when
 * it starts.  The only credible values are 100 (Linux/x86) or powers of 2.
 * Also, the bounds checking inside the kernel's adjtimex system call enforces
 * a +/- 10% movement of tick away from the nominal value 1e6/USER_HZ. */

static int
guess_hz(void)
{
  struct timex txc;
  int i, tick, tick_lo, tick_hi, ihz;
  double tick_nominal;

  txc.modes = 0;
  SYS_Timex_Adjust(&txc, 0);
  tick = txc.tick;

  /* Pick off the hz=100 case first */
  if (tick >= 9000 && tick <= 11000) {
    return 100;
  }

  for (i=4; i<16; i++) { /* surely 16 .. 32768 is a wide enough range? */
    ihz = 1 << i;
    tick_nominal = 1.0e6 / (double) ihz;
    tick_lo = (int)(0.5 + tick_nominal*2.0/3.0);
    tick_hi = (int)(0.5 + tick_nominal*4.0/3.0);
    
    if (tick_lo < tick && tick <= tick_hi) {
      return ihz;
    }
  }

  /* oh dear.  doomed. */
  LOG_FATAL(LOGF_SysLinux, "Can't determine hz from tick %d", tick);

  return 0;
}

/* ================================================== */

static int
get_hz(void)
{
#ifdef _SC_CLK_TCK
  int hz;

  if ((hz = sysconf(_SC_CLK_TCK)) < 1)
    return 0;

  return hz;
#else
  return 0;
#endif
}

/* ================================================== */

static int
kernelvercmp(int major1, int minor1, int patch1,
    int major2, int minor2, int patch2)
{
  if (major1 != major2)
    return major1 - major2;
  if (minor1 != minor2)
    return minor1 - minor2;
  return patch1 - patch2;
}

/* ================================================== */
/* Compute the scaling to use on any frequency we set, according to
   the vintage of the Linux kernel being used. */

static void
get_version_specific_details(void)
{
  int major, minor, patch;
  struct utsname uts;
  
  hz = get_hz();

  if (!hz)
    hz = guess_hz();

  dhz = (double) hz;
  nominal_tick = (1000000L + (hz/2))/hz; /* Mirror declaration in kernel */
  max_tick_bias = nominal_tick / 10;

  /* We can't reliably detect the internal kernel HZ, it may not even be fixed
     (CONFIG_NO_HZ aka tickless), assume the lowest commonly used fixed rate */
  tick_update_hz = 100;

  if (uname(&uts) < 0) {
    LOG_FATAL(LOGF_SysLinux, "Cannot uname(2) to get kernel version, sorry.");
  }

  patch = 0;
  if (sscanf(uts.release, "%d.%d.%d", &major, &minor, &patch) < 2) {
    LOG_FATAL(LOGF_SysLinux, "Cannot read information from uname, sorry");
  }

  DEBUG_LOG(LOGF_SysLinux, "Linux kernel major=%d minor=%d patch=%d", major, minor, patch);

  if (kernelvercmp(major, minor, patch, 2, 2, 0) < 0) {
    LOG_FATAL(LOGF_SysLinux, "Kernel version not supported, sorry.");
  }

  if (kernelvercmp(major, minor, patch, 2, 6, 27) >= 0 &&
      kernelvercmp(major, minor, patch, 2, 6, 33) < 0) {
    /* Tickless kernels before 2.6.33 accumulated ticks only in
       half-second intervals */
    tick_update_hz = 2;
  }

  /* ADJ_SETOFFSET support */
  if (kernelvercmp(major, minor, patch, 2, 6, 39) < 0) {
    have_setoffset = 0;
  } else {
    have_setoffset = 1;
  }

  DEBUG_LOG(LOGF_SysLinux, "hz=%d nominal_tick=%d max_tick_bias=%d",
      hz, nominal_tick, max_tick_bias);
}

/* ================================================== */

static void
reset_adjtime_offset(void)
{
  struct timex txc;

  /* Reset adjtime() offset */
  txc.modes = ADJ_OFFSET_SINGLESHOT;
  txc.offset = 0;

  SYS_Timex_Adjust(&txc, 0);
}

/* ================================================== */

static int
test_step_offset(void)
{
  struct timex txc;

  /* Zero maxerror and check it's reset to a maximum after ADJ_SETOFFSET.
     This seems to be the only way how to verify that the kernel really
     supports the ADJ_SETOFFSET mode as it doesn't return an error on unknown
     mode. */

  txc.modes = MOD_MAXERROR;
  txc.maxerror = 0;

  if (SYS_Timex_Adjust(&txc, 1) < 0 || txc.maxerror != 0)
    return 0;

  txc.modes = ADJ_SETOFFSET | ADJ_NANO;
  txc.time.tv_sec = 0;
  txc.time.tv_usec = 0;

  if (SYS_Timex_Adjust(&txc, 1) < 0 || txc.maxerror < 100000)
    return 0;

  return 1;
}

/* ================================================== */
/* Initialisation code for this module */

void
SYS_Linux_Initialise(void)
{
  get_version_specific_details();

  reset_adjtime_offset();

  if (have_setoffset && !test_step_offset()) {
    LOG(LOGS_INFO, LOGF_SysLinux, "adjtimex() doesn't support ADJ_SETOFFSET");
    have_setoffset = 0;
  }

  SYS_Timex_InitialiseWithFunctions(1.0e6 * max_tick_bias / nominal_tick,
                                    1.0 / tick_update_hz,
                                    read_frequency, set_frequency,
                                    have_setoffset ? apply_step_offset : NULL,
                                    0.0, 0.0, NULL, NULL);
}

/* ================================================== */
/* Finalisation code for this module */

void
SYS_Linux_Finalise(void)
{
  SYS_Timex_Finalise();
}

/* ================================================== */

#ifdef FEAT_PRIVDROP
void
SYS_Linux_DropRoot(uid_t uid, gid_t gid)
{
  const char *cap_text;
  cap_t cap;

  if (prctl(PR_SET_KEEPCAPS, 1)) {
    LOG_FATAL(LOGF_SysLinux, "prctl() failed");
  }
  
  UTI_DropRoot(uid, gid);

  /* Keep CAP_NET_BIND_SERVICE only if NTP port can be opened */
  cap_text = CNF_GetNTPPort() ?
             "cap_net_bind_service,cap_sys_time=ep" : "cap_sys_time=ep";

  if ((cap = cap_from_text(cap_text)) == NULL) {
    LOG_FATAL(LOGF_SysLinux, "cap_from_text() failed");
  }

  if (cap_set_proc(cap)) {
    LOG_FATAL(LOGF_SysLinux, "cap_set_proc() failed");
  }

  cap_free(cap);
}
#endif

/* ================================================== */

#ifdef FEAT_SCFILTER
static
void check_seccomp_applicability(void)
{
  int mail_enabled;
  double mail_threshold;
  char *mail_user;

  CNF_GetMailOnChange(&mail_enabled, &mail_threshold, &mail_user);
  if (mail_enabled)
    LOG_FATAL(LOGF_SysLinux, "mailonchange directive cannot be used with -F enabled");
}

/* ================================================== */

void
SYS_Linux_EnableSystemCallFilter(int level)
{
  const int syscalls[] = {
    /* Clock */
    SCMP_SYS(adjtimex), SCMP_SYS(gettimeofday), SCMP_SYS(settimeofday),
    SCMP_SYS(time),
    /* Process */
    SCMP_SYS(clone), SCMP_SYS(exit), SCMP_SYS(exit_group), SCMP_SYS(getrlimit),
    SCMP_SYS(rt_sigaction), SCMP_SYS(rt_sigreturn), SCMP_SYS(rt_sigprocmask),
    SCMP_SYS(set_tid_address), SCMP_SYS(sigreturn), SCMP_SYS(wait4),
    /* Memory */
    SCMP_SYS(brk), SCMP_SYS(madvise), SCMP_SYS(mmap), SCMP_SYS(mmap2),
    SCMP_SYS(mprotect), SCMP_SYS(mremap), SCMP_SYS(munmap), SCMP_SYS(shmdt),
    /* Filesystem */
    SCMP_SYS(access), SCMP_SYS(chmod), SCMP_SYS(chown), SCMP_SYS(chown32),
    SCMP_SYS(fstat), SCMP_SYS(fstat64), SCMP_SYS(lseek), SCMP_SYS(rename),
    SCMP_SYS(stat), SCMP_SYS(stat64), SCMP_SYS(statfs), SCMP_SYS(statfs64),
    SCMP_SYS(unlink),
    /* Socket */
    SCMP_SYS(bind), SCMP_SYS(connect), SCMP_SYS(getsockname),
    SCMP_SYS(recvfrom), SCMP_SYS(recvmsg), SCMP_SYS(sendmmsg),
    SCMP_SYS(sendmsg), SCMP_SYS(sendto),
    /* TODO: check socketcall arguments */
    SCMP_SYS(socketcall),
    /* General I/O */
    SCMP_SYS(_newselect), SCMP_SYS(close), SCMP_SYS(open), SCMP_SYS(pipe),
    SCMP_SYS(poll), SCMP_SYS(read), SCMP_SYS(futex), SCMP_SYS(select),
    SCMP_SYS(set_robust_list), SCMP_SYS(write),
    /* Miscellaneous */
    SCMP_SYS(uname),
  };

  const int socket_domains[] = {
    AF_NETLINK, AF_UNIX, AF_INET,
#ifdef FEAT_IPV6
    AF_INET6,
#endif
  };

  const static int socket_options[][2] = {
    { SOL_IP, IP_PKTINFO }, { SOL_IP, IP_FREEBIND },
#ifdef FEAT_IPV6
    { SOL_IPV6, IPV6_V6ONLY }, { SOL_IPV6, IPV6_RECVPKTINFO },
#endif
    { SOL_SOCKET, SO_BROADCAST }, { SOL_SOCKET, SO_REUSEADDR },
    { SOL_SOCKET, SO_TIMESTAMP },
  };

  const static int fcntls[] = { F_GETFD, F_SETFD };

  const static unsigned long ioctls[] = {
    FIONREAD, TCGETS,
#ifdef FEAT_PPS
    PTP_SYS_OFFSET,
#endif
#ifdef FEAT_PPS
    PPS_FETCH,
#endif
#ifdef FEAT_RTC
    RTC_RD_TIME, RTC_SET_TIME, RTC_UIE_ON, RTC_UIE_OFF,
#endif
  };

  scmp_filter_ctx *ctx;
  int i;

  /* Check if the chronyd configuration is supported */
  check_seccomp_applicability();

  /* Start the helper process, which will run without any seccomp filter.  It
     will be used for getaddrinfo(), for which it's difficult to maintain a
     list of required system calls (with glibc it depends on what NSS modules
     are installed and enabled on the system). */
  PRV_StartHelper();

  ctx = seccomp_init(level > 0 ? SCMP_ACT_KILL : SCMP_ACT_TRAP);
  if (ctx == NULL)
      LOG_FATAL(LOGF_SysLinux, "Failed to initialize seccomp");

  /* Add system calls that are always allowed */
  for (i = 0; i < (sizeof (syscalls) / sizeof (*syscalls)); i++) {
    if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, syscalls[i], 0) < 0)
      goto add_failed;
  }

  /* Allow sockets to be created only in selected domains */
  for (i = 0; i < sizeof (socket_domains) / sizeof (*socket_domains); i++) {
    if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
                         SCMP_A0(SCMP_CMP_EQ, socket_domains[i])) < 0)
      goto add_failed;
  }

  /* Allow setting only selected sockets options */
  for (i = 0; i < sizeof (socket_options) / sizeof (*socket_options); i++) {
    if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setsockopt), 3,
                         SCMP_A1(SCMP_CMP_EQ, socket_options[i][0]),
                         SCMP_A2(SCMP_CMP_EQ, socket_options[i][1]),
                         SCMP_A4(SCMP_CMP_LE, sizeof (int))) < 0)
      goto add_failed;
  }

  /* Allow only selected fcntl calls */
  for (i = 0; i < sizeof (fcntls) / sizeof (*fcntls); i++) {
    if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl), 1,
                         SCMP_A1(SCMP_CMP_EQ, fcntls[i])) < 0 ||
        seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl64), 1,
                         SCMP_A1(SCMP_CMP_EQ, fcntls[i])) < 0)
      goto add_failed;
  }

  /* Allow only selected ioctls */
  for (i = 0; i < sizeof (ioctls) / sizeof (*ioctls); i++) {
    if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
                         SCMP_A1(SCMP_CMP_EQ, ioctls[i])) < 0)
      goto add_failed;
  }

  if (seccomp_load(ctx) < 0)
    LOG_FATAL(LOGF_SysLinux, "Failed to load seccomp rules");

  LOG(LOGS_INFO, LOGF_SysLinux, "Loaded seccomp filter");
  seccomp_release(ctx);
  return;

add_failed:
  LOG_FATAL(LOGF_SysLinux, "Failed to add seccomp rules");
}
#endif

/* ================================================== */

#if defined(HAVE_SCHED_SETSCHEDULER)
  /* Install SCHED_FIFO real-time scheduler with specified priority */
void SYS_Linux_SetScheduler(int SchedPriority)
{
  int pmax, pmin;
  struct sched_param sched;

  if (SchedPriority < 1 || SchedPriority > 99) {
    LOG_FATAL(LOGF_SysLinux, "Bad scheduler priority: %d", SchedPriority);
  } else {
    sched.sched_priority = SchedPriority;
    pmax = sched_get_priority_max(SCHED_FIFO);
    pmin = sched_get_priority_min(SCHED_FIFO);
    if ( SchedPriority > pmax ) {
      sched.sched_priority = pmax;
    }
    else if ( SchedPriority < pmin ) {
      sched.sched_priority = pmin;
    }
    if ( sched_setscheduler(0, SCHED_FIFO, &sched) == -1 ) {
      LOG(LOGS_ERR, LOGF_SysLinux, "sched_setscheduler() failed");
    }
    else {
      DEBUG_LOG(LOGF_SysLinux, "Enabled SCHED_FIFO with priority %d",
          sched.sched_priority);
    }
  }
}
#endif /* HAVE_SCHED_SETSCHEDULER */

#if defined(HAVE_MLOCKALL)
/* Lock the process into RAM so that it will never be swapped out */ 
void SYS_Linux_MemLockAll(int LockAll)
{
  struct rlimit rlim;
  if (LockAll == 1 ) {
    /* Make sure that we will be able to lock all the memory we need */
    /* even after dropping privileges.  This does not actually reaerve any memory */
    rlim.rlim_max = RLIM_INFINITY;
    rlim.rlim_cur = RLIM_INFINITY;
    if (setrlimit(RLIMIT_MEMLOCK, &rlim) < 0) {
      LOG(LOGS_ERR, LOGF_SysLinux, "setrlimit() failed: not locking into RAM");
    }
    else {
      if (mlockall(MCL_CURRENT|MCL_FUTURE) < 0) {
	LOG(LOGS_ERR, LOGF_SysLinux, "mlockall() failed");
      }
      else {
	DEBUG_LOG(LOGF_SysLinux, "Successfully locked into RAM");
      }
    }
  }
}
#endif /* HAVE_MLOCKALL */
