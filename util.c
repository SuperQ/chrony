/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009, 2012-2016
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

  Various utility functions
  */

#include "config.h"

#include "sysincl.h"

#include "logging.h"
#include "memory.h"
#include "util.h"
#include "hash.h"

/* ================================================== */

void
UTI_TimevalToDouble(struct timeval *a, double *b)
{
  *b = (double)(a->tv_sec) + 1.0e-6 * (double)(a->tv_usec);

}

/* ================================================== */

void
UTI_DoubleToTimeval(double a, struct timeval *b)
{
  long int_part;
  double frac_part;
  int_part = (long)(a);
  frac_part = 1.0e6 * (a - (double)(int_part));
  frac_part = frac_part > 0 ? frac_part + 0.5 : frac_part - 0.5;
  b->tv_sec = int_part;
  b->tv_usec = (long)frac_part;
  UTI_NormaliseTimeval(b);
}

/* ================================================== */

int
UTI_CompareTimevals(struct timeval *a, struct timeval *b)
{
  if (a->tv_sec < b->tv_sec) {
    return -1;
  } else if (a->tv_sec > b->tv_sec) {
    return +1;
  } else {
    if (a->tv_usec < b->tv_usec) {
      return -1;
    } else if (a->tv_usec > b->tv_usec) {
      return +1;
    } else {
      return 0;
    }
  }
}

/* ================================================== */

void
UTI_NormaliseTimeval(struct timeval *x)
{
  /* Reduce tv_usec to within +-1000000 of zero. JGH */
  if ((x->tv_usec >= 1000000) || (x->tv_usec <= -1000000)) {
    x->tv_sec += x->tv_usec/1000000;
    x->tv_usec = x->tv_usec%1000000;
  }

  /* Make tv_usec positive. JGH */
   if (x->tv_usec < 0) {
    --x->tv_sec;
    x->tv_usec += 1000000;
 }

}

/* ================================================== */

void
UTI_DiffTimevals(struct timeval *result,
                 struct timeval *a,
                 struct timeval *b)
{
  result->tv_sec  = a->tv_sec  - b->tv_sec;
  result->tv_usec = a->tv_usec - b->tv_usec;

  /* Correct microseconds field to bring it into the range
     (0,1000000) */

  UTI_NormaliseTimeval(result); /* JGH */
}

/* ================================================== */

/* Calculate result = a - b and return as a double */
void
UTI_DiffTimevalsToDouble(double *result, 
                         struct timeval *a,
                         struct timeval *b)
{
  *result = (double)(a->tv_sec - b->tv_sec) +
    (double)(a->tv_usec - b->tv_usec) * 1.0e-6;
}

/* ================================================== */

void
UTI_AddDoubleToTimeval(struct timeval *start,
                       double increment,
                       struct timeval *end)
{
  long int_part, frac_part;

  /* Don't want to do this by using (long)(1000000 * increment), since
     that will only cope with increments up to +/- 2148 seconds, which
     is too marginal here. */

  int_part = (long) increment;
  increment = (increment - int_part) * 1.0e6;
  frac_part = (long) (increment > 0.0 ? increment + 0.5 : increment - 0.5);

  end->tv_sec  = int_part  + start->tv_sec;
  end->tv_usec = frac_part + start->tv_usec;

  UTI_NormaliseTimeval(end);
}

/* ================================================== */

/* Calculate the average and difference (as a double) of two timevals */
void
UTI_AverageDiffTimevals (struct timeval *earlier,
                         struct timeval *later,
                         struct timeval *average,
                         double *diff)
{
  struct timeval tvdiff;
  struct timeval tvhalf;

  UTI_DiffTimevals(&tvdiff, later, earlier);
  *diff = (double)tvdiff.tv_sec + 1.0e-6 * (double)tvdiff.tv_usec;

  if (*diff < 0.0) {
    /* Either there's a bug elsewhere causing 'earlier' and 'later' to
       be backwards, or something wierd has happened.  Maybe when we
       change the frequency on Linux? */

    /* Assume the required behaviour is to treat it as zero */
    *diff = 0.0;
  }

  tvhalf.tv_sec = tvdiff.tv_sec / 2;
  tvhalf.tv_usec = tvdiff.tv_usec / 2 + (tvdiff.tv_sec % 2) * 500000; /* JGH */
  
  average->tv_sec  = earlier->tv_sec  + tvhalf.tv_sec;
  average->tv_usec = earlier->tv_usec + tvhalf.tv_usec;
  
  /* Bring into range */
  UTI_NormaliseTimeval(average);

 }

/* ================================================== */

void
UTI_AddDiffToTimeval(struct timeval *a, struct timeval *b,
                     struct timeval *c, struct timeval *result)
{
  double diff;

  UTI_DiffTimevalsToDouble(&diff, a, b);
  UTI_AddDoubleToTimeval(c, diff, result);
}

/* ================================================== */

#define POOL_ENTRIES 16
#define BUFFER_LENGTH 64
static char buffer_pool[POOL_ENTRIES][BUFFER_LENGTH];
static int  pool_ptr = 0;

#define NEXT_BUFFER (buffer_pool[pool_ptr = ((pool_ptr + 1) % POOL_ENTRIES)])

/* ================================================== */
/* Convert a timeval into a temporary string, largely for diagnostic
   display */

char *
UTI_TimevalToString(struct timeval *tv)
{
  char *result;

  result = NEXT_BUFFER;
#ifdef HAVE_LONG_TIME_T
  snprintf(result, BUFFER_LENGTH, "%"PRId64".%06lu",
      (int64_t)tv->tv_sec, (unsigned long)tv->tv_usec);
#else
  snprintf(result, BUFFER_LENGTH, "%ld.%06lu",
      (long)tv->tv_sec, (unsigned long)tv->tv_usec);
#endif
  return result;
}

/* ================================================== */
/* Convert an NTP timestamp into a temporary string, largely
   for diagnostic display */

char *
UTI_TimestampToString(NTP_int64 *ts)
{
  struct timeval tv;
  UTI_Int64ToTimeval(ts, &tv);
  return UTI_TimevalToString(&tv);
}

/* ================================================== */

char *
UTI_RefidToString(uint32_t ref_id)
{
  unsigned int i, j, c;
  char *result;

  result = NEXT_BUFFER;

  for (i = j = 0; i < 4 && i < BUFFER_LENGTH - 1; i++) {
    c = (ref_id >> (24 - i * 8)) & 0xff;
    if (isprint(c))
      result[j++] = c;
  }

  result[j] = '\0';

  return result;
}

/* ================================================== */

char *
UTI_IPToString(IPAddr *addr)
{
  unsigned long a, b, c, d, ip;
  uint8_t *ip6;
  char *result;

  result = NEXT_BUFFER;
  switch (addr->family) {
    case IPADDR_UNSPEC:
      snprintf(result, BUFFER_LENGTH, "[UNSPEC]");
      break;
    case IPADDR_INET4:
      ip = addr->addr.in4;
      a = (ip>>24) & 0xff;
      b = (ip>>16) & 0xff;
      c = (ip>> 8) & 0xff;
      d = (ip>> 0) & 0xff;
      snprintf(result, BUFFER_LENGTH, "%ld.%ld.%ld.%ld", a, b, c, d);
      break;
    case IPADDR_INET6:
      ip6 = addr->addr.in6;
#ifdef FEAT_IPV6
      inet_ntop(AF_INET6, ip6, result, BUFFER_LENGTH);
#else
      snprintf(result, BUFFER_LENGTH, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
               ip6[0], ip6[1], ip6[2], ip6[3], ip6[4], ip6[5], ip6[6], ip6[7],
               ip6[8], ip6[9], ip6[10], ip6[11], ip6[12], ip6[13], ip6[14], ip6[15]);
#endif
      break;
    default:
      snprintf(result, BUFFER_LENGTH, "[UNKNOWN]");
  }
  return result;
}

/* ================================================== */

int
UTI_StringToIP(const char *addr, IPAddr *ip)
{
#ifdef FEAT_IPV6
  struct in_addr in4;
  struct in6_addr in6;

  if (inet_pton(AF_INET, addr, &in4) > 0) {
    ip->family = IPADDR_INET4;
    ip->addr.in4 = ntohl(in4.s_addr);
    return 1;
  }

  if (inet_pton(AF_INET6, addr, &in6) > 0) {
    ip->family = IPADDR_INET6;
    memcpy(ip->addr.in6, in6.s6_addr, sizeof (ip->addr.in6));
    return 1;
  }
#else
  unsigned long a, b, c, d, n;

  n = sscanf(addr, "%lu.%lu.%lu.%lu", &a, &b, &c, &d);
  if (n == 4) {
    ip->family = IPADDR_INET4;
    ip->addr.in4 = ((a & 0xff) << 24) | ((b & 0xff) << 16) | 
                   ((c & 0xff) << 8) | (d & 0xff);
    return 1;
  }
#endif

  return 0;
}

/* ================================================== */

uint32_t
UTI_IPToRefid(IPAddr *ip)
{
  static int MD5_hash = -1;
  unsigned char buf[16];

  switch (ip->family) {
    case IPADDR_INET4:
      return ip->addr.in4;
    case IPADDR_INET6:
      if (MD5_hash < 0) {
        MD5_hash = HSH_GetHashId("MD5");
        assert(MD5_hash >= 0);
      }

      if (HSH_Hash(MD5_hash, (unsigned const char *)ip->addr.in6, sizeof
            (ip->addr.in6), NULL, 0, buf, 16) != 16) {
        assert(0);
        return 0;
      };
      return (uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
  }
  return 0;
}

/* ================================================== */

uint32_t
UTI_IPToHash(IPAddr *ip)
{
  unsigned char *addr;
  unsigned int i, len;
  uint32_t hash;

  switch (ip->family) {
    case IPADDR_INET4:
      addr = (unsigned char *)&ip->addr.in4;
      len = sizeof (ip->addr.in4);
      break;
    case IPADDR_INET6:
      addr = ip->addr.in6;
      len = sizeof (ip->addr.in6);
      break;
    default:
      return 0;
  }

  for (i = 0, hash = 0; i < len; i++)
    hash = 71 * hash + addr[i];

  return hash;
}

/* ================================================== */

void
UTI_IPHostToNetwork(IPAddr *src, IPAddr *dest)
{
  /* Don't send uninitialized bytes over network */
  memset(dest, 0, sizeof (IPAddr));

  dest->family = htons(src->family);

  switch (src->family) {
    case IPADDR_INET4:
      dest->addr.in4 = htonl(src->addr.in4);
      break;
    case IPADDR_INET6:
      memcpy(dest->addr.in6, src->addr.in6, sizeof (dest->addr.in6));
      break;
  }
}

/* ================================================== */

void
UTI_IPNetworkToHost(IPAddr *src, IPAddr *dest)
{
  dest->family = ntohs(src->family);

  switch (dest->family) {
    case IPADDR_INET4:
      dest->addr.in4 = ntohl(src->addr.in4);
      break;
    case IPADDR_INET6:
      memcpy(dest->addr.in6, src->addr.in6, sizeof (dest->addr.in6));
      break;
  }
}

/* ================================================== */

int
UTI_CompareIPs(IPAddr *a, IPAddr *b, IPAddr *mask)
{
  int i, d;

  if (a->family != b->family)
    return a->family - b->family;

  if (mask && mask->family != b->family)
    mask = NULL;

  switch (a->family) {
    case IPADDR_UNSPEC:
      return 0;
    case IPADDR_INET4:
      if (mask)
        return (a->addr.in4 & mask->addr.in4) - (b->addr.in4 & mask->addr.in4);
      else
        return a->addr.in4 - b->addr.in4;
    case IPADDR_INET6:
      for (i = 0, d = 0; !d && i < 16; i++) {
        if (mask)
          d = (a->addr.in6[i] & mask->addr.in6[i]) -
              (b->addr.in6[i] & mask->addr.in6[i]);
        else
          d = a->addr.in6[i] - b->addr.in6[i];
      }
      return d;
  }
  return 0;
}

/* ================================================== */

void
UTI_SockaddrToIPAndPort(struct sockaddr *sa, IPAddr *ip, unsigned short *port)
{
  switch (sa->sa_family) {
    case AF_INET:
      ip->family = IPADDR_INET4;
      ip->addr.in4 = ntohl(((struct sockaddr_in *)sa)->sin_addr.s_addr);
      *port = ntohs(((struct sockaddr_in *)sa)->sin_port);
      break;
#ifdef FEAT_IPV6
    case AF_INET6:
      ip->family = IPADDR_INET6;
      memcpy(ip->addr.in6, ((struct sockaddr_in6 *)sa)->sin6_addr.s6_addr,
             sizeof (ip->addr.in6));
      *port = ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
      break;
#endif
    default:
      ip->family = IPADDR_UNSPEC;
      *port = 0;
  }
}

/* ================================================== */

int
UTI_IPAndPortToSockaddr(IPAddr *ip, unsigned short port, struct sockaddr *sa)
{
  switch (ip->family) {
    case IPADDR_INET4:
      memset(sa, 0, sizeof (struct sockaddr_in));
      sa->sa_family = AF_INET;
      ((struct sockaddr_in *)sa)->sin_addr.s_addr = htonl(ip->addr.in4);
      ((struct sockaddr_in *)sa)->sin_port = htons(port);
#ifdef SIN6_LEN
      ((struct sockaddr_in *)sa)->sin_len = sizeof (struct sockaddr_in);
#endif
      return sizeof (struct sockaddr_in);
#ifdef FEAT_IPV6
    case IPADDR_INET6:
      memset(sa, 0, sizeof (struct sockaddr_in6));
      sa->sa_family = AF_INET6;
      memcpy(((struct sockaddr_in6 *)sa)->sin6_addr.s6_addr, ip->addr.in6,
             sizeof (ip->addr.in6));
      ((struct sockaddr_in6 *)sa)->sin6_port = htons(port);
#ifdef SIN6_LEN
      ((struct sockaddr_in6 *)sa)->sin6_len = sizeof (struct sockaddr_in6);
#endif
      return sizeof (struct sockaddr_in6);
#endif
    default:
      memset(sa, 0, sizeof (struct sockaddr));
      sa->sa_family = AF_UNSPEC;
      return 0;
  }
}

/* ================================================== */

char *UTI_SockaddrToString(struct sockaddr *sa)
{
  unsigned short port;
  IPAddr ip;
  char *result;

  result = NEXT_BUFFER;

  switch (sa->sa_family) {
    case AF_INET:
#ifdef AF_INET6
    case AF_INET6:
#endif
      UTI_SockaddrToIPAndPort(sa, &ip, &port);
      snprintf(result, BUFFER_LENGTH, "%s:%hu", UTI_IPToString(&ip), port);
      break;
    case AF_UNIX:
      snprintf(result, BUFFER_LENGTH, "%s", ((struct sockaddr_un *)sa)->sun_path);
      break;
    default:
      snprintf(result, BUFFER_LENGTH, "[UNKNOWN]");
  }

  return result;
}

/* ================================================== */

const char *
UTI_SockaddrFamilyToString(int family)
{
  switch (family) {
    case AF_INET:
      return "IPv4";
#ifdef AF_INET6
    case AF_INET6:
      return "IPv6";
#endif
    case AF_UNIX:
      return "Unix";
    case AF_UNSPEC:
      return "UNSPEC";
    default:
      return "?";
  }
}

/* ================================================== */

char *
UTI_TimeToLogForm(time_t t)
{
  struct tm stm;
  char *result;

  result = NEXT_BUFFER;

  stm = *gmtime(&t);
  strftime(result, BUFFER_LENGTH, "%Y-%m-%d %H:%M:%S", &stm);

  return result;
}

/* ================================================== */

void
UTI_AdjustTimeval(struct timeval *old_tv, struct timeval *when, struct timeval *new_tv, double *delta_time, double dfreq, double doffset)
{
  double elapsed;

  UTI_DiffTimevalsToDouble(&elapsed, when, old_tv);
  *delta_time = elapsed * dfreq - doffset;
  UTI_AddDoubleToTimeval(old_tv, *delta_time, new_tv);
}

/* ================================================== */

void
UTI_GetInt64Fuzz(NTP_int64 *ts, int precision)
{
  int start, bits;

  assert(precision >= -32 && precision <= 32);

  start = sizeof (*ts) - (precision + 32 + 7) / 8;
  ts->hi = ts->lo = 0;

  UTI_GetRandomBytes((unsigned char *)ts + start, sizeof (*ts) - start);

  bits = (precision + 32) % 8;
  if (bits)
    ((unsigned char *)ts)[start] %= 1U << bits;
}

/* ================================================== */

double
UTI_Int32ToDouble(NTP_int32 x)
{
  return (double) ntohl(x) / 65536.0;
}

/* ================================================== */

#define MAX_NTP_INT32 (4294967295.0 / 65536.0)

NTP_int32
UTI_DoubleToInt32(double x)
{
  if (x > MAX_NTP_INT32)
    x = MAX_NTP_INT32;
  else if (x < 0)
    x = 0.0;
  return htonl((NTP_int32)(0.5 + 65536.0 * x));
}

/* ================================================== */

/* Seconds part of NTP timestamp correponding to the origin of the
   struct timeval format. */
#define JAN_1970 0x83aa7e80UL

void
UTI_TimevalToInt64(struct timeval *src,
                   NTP_int64 *dest, NTP_int64 *fuzz)
{
  uint32_t hi, lo, sec, usec;

  sec = (uint32_t)src->tv_sec;
  usec = (uint32_t)src->tv_usec;

  /* Recognize zero as a special case - it always signifies
     an 'unknown' value */
  if (!usec && !sec) {
    hi = lo = 0;
  } else {
    hi = htonl(sec + JAN_1970);

    /* This formula gives an error of about 0.1us worst case */
    lo = htonl(4295 * usec - (usec >> 5) - (usec >> 9));

    /* Add the fuzz */
    if (fuzz) {
      hi ^= fuzz->hi;
      lo ^= fuzz->lo;
    }
  }

  dest->hi = hi;
  dest->lo = lo;
}

/* ================================================== */

void
UTI_Int64ToTimeval(NTP_int64 *src,
                   struct timeval *dest)
{
  uint32_t ntp_sec, ntp_frac;

  /* As yet, there is no need to check for zero - all processing that
     has to detect that case is in the NTP layer */

  ntp_sec = ntohl(src->hi);
  ntp_frac = ntohl(src->lo);

#ifdef HAVE_LONG_TIME_T
  dest->tv_sec = ntp_sec - (uint32_t)(NTP_ERA_SPLIT + JAN_1970) +
                 (time_t)NTP_ERA_SPLIT;
#else
  dest->tv_sec = ntp_sec - JAN_1970;
#endif
  
  /* Until I invent a slick way to do this, just do it the obvious way */
  dest->tv_usec = (int)(0.5 + (double)(ntp_frac) / 4294.967296);
}

/* ================================================== */

/* Maximum offset between two sane times */
#define MAX_OFFSET 4294967296.0

/* Minimum allowed distance from maximum 32-bit time_t */
#define MIN_ENDOFTIME_DISTANCE (365 * 24 * 3600)

int
UTI_IsTimeOffsetSane(struct timeval *tv, double offset)
{
  double t;

  /* Handle nan correctly here */
  if (!(offset > -MAX_OFFSET && offset < MAX_OFFSET))
    return 0;

  UTI_TimevalToDouble(tv, &t);
  t += offset;

  /* Time before 1970 is not considered valid */
  if (t < 0.0)
    return 0;

#ifdef HAVE_LONG_TIME_T
  /* Check if it's in the interval to which NTP time is mapped */
  if (t < (double)NTP_ERA_SPLIT || t > (double)(NTP_ERA_SPLIT + (1LL << 32)))
    return 0;
#else
  /* Don't get too close to 32-bit time_t overflow */
  if (t > (double)(0x7fffffff - MIN_ENDOFTIME_DISTANCE))
    return 0;
#endif

  return 1;
}

/* ================================================== */

double
UTI_Log2ToDouble(int l)
{
  if (l >= 0) {
    if (l > 31)
      l = 31;
    return (uint32_t)1 << l;
  } else {
    if (l < -31)
      l = -31;
    return 1.0 / ((uint32_t)1 << -l);
  }
}

/* ================================================== */

void
UTI_TimevalNetworkToHost(Timeval *src, struct timeval *dest)
{
  uint32_t sec_low;
#ifdef HAVE_LONG_TIME_T
  uint32_t sec_high;
#endif

  dest->tv_usec = ntohl(src->tv_nsec) / 1000;
  sec_low = ntohl(src->tv_sec_low);
#ifdef HAVE_LONG_TIME_T
  sec_high = ntohl(src->tv_sec_high);
  if (sec_high == TV_NOHIGHSEC)
    sec_high = 0;

  dest->tv_sec = (uint64_t)sec_high << 32 | sec_low;
#else
  dest->tv_sec = sec_low;
#endif
}

/* ================================================== */

void
UTI_TimevalHostToNetwork(struct timeval *src, Timeval *dest)
{
  dest->tv_nsec = htonl(src->tv_usec * 1000);
#ifdef HAVE_LONG_TIME_T
  dest->tv_sec_high = htonl((uint64_t)src->tv_sec >> 32);
#else
  dest->tv_sec_high = htonl(TV_NOHIGHSEC);
#endif
  dest->tv_sec_low = htonl(src->tv_sec);
}

/* ================================================== */

#define FLOAT_EXP_BITS 7
#define FLOAT_EXP_MIN (-(1 << (FLOAT_EXP_BITS - 1)))
#define FLOAT_EXP_MAX (-FLOAT_EXP_MIN - 1)
#define FLOAT_COEF_BITS ((int)sizeof (int32_t) * 8 - FLOAT_EXP_BITS)
#define FLOAT_COEF_MIN (-(1 << (FLOAT_COEF_BITS - 1)))
#define FLOAT_COEF_MAX (-FLOAT_COEF_MIN - 1)

double
UTI_FloatNetworkToHost(Float f)
{
  int32_t exp, coef;
  uint32_t x;

  x = ntohl(f.f);

  exp = (x >> FLOAT_COEF_BITS) - FLOAT_COEF_BITS;
  if (exp >= 1 << (FLOAT_EXP_BITS - 1))
      exp -= 1 << FLOAT_EXP_BITS;

  coef = x % (1U << FLOAT_COEF_BITS);
  if (coef >= 1 << (FLOAT_COEF_BITS - 1))
      coef -= 1 << FLOAT_COEF_BITS;

  return coef * pow(2.0, exp);
}

Float
UTI_FloatHostToNetwork(double x)
{
  int32_t exp, coef, neg;
  Float f;

  if (x < 0.0) {
    x = -x;
    neg = 1;
  } else if (x >= 0.0) {
    neg = 0;
  } else {
    /* Save NaN as zero */
    x = 0.0;
    neg = 0;
  }

  if (x < 1.0e-100) {
    exp = coef = 0;
  } else if (x > 1.0e100) {
    exp = FLOAT_EXP_MAX;
    coef = FLOAT_COEF_MAX + neg;
  } else {
    exp = log(x) / log(2) + 1;
    coef = x * pow(2.0, -exp + FLOAT_COEF_BITS) + 0.5;

    assert(coef > 0);

    /* we may need to shift up to two bits down */
    while (coef > FLOAT_COEF_MAX + neg) {
      coef >>= 1;
      exp++;
    }

    if (exp > FLOAT_EXP_MAX) {
      /* overflow */
      exp = FLOAT_EXP_MAX;
      coef = FLOAT_COEF_MAX + neg;
    } else if (exp < FLOAT_EXP_MIN) {
      /* underflow */
      if (exp + FLOAT_COEF_BITS >= FLOAT_EXP_MIN) {
        coef >>= FLOAT_EXP_MIN - exp;
        exp = FLOAT_EXP_MIN;
      } else {
        exp = coef = 0;
      }
    }
  }

  /* negate back */
  if (neg)
    coef = (uint32_t)-coef << FLOAT_EXP_BITS >> FLOAT_EXP_BITS;

  f.f = htonl((uint32_t)exp << FLOAT_COEF_BITS | coef);
  return f;
}

/* ================================================== */

int
UTI_FdSetCloexec(int fd)
{
  int flags;

  flags = fcntl(fd, F_GETFD);
  if (flags != -1) {
    flags |= FD_CLOEXEC;
    return !fcntl(fd, F_SETFD, flags);
  }

  return 0;
}

/* ================================================== */

int
UTI_GenerateNTPAuth(int hash_id, const unsigned char *key, int key_len,
    const unsigned char *data, int data_len, unsigned char *auth, int auth_len)
{
  return HSH_Hash(hash_id, key, key_len, data, data_len, auth, auth_len);
}

/* ================================================== */

int
UTI_CheckNTPAuth(int hash_id, const unsigned char *key, int key_len,
    const unsigned char *data, int data_len, const unsigned char *auth, int auth_len)
{
  unsigned char buf[MAX_HASH_LENGTH];

  return UTI_GenerateNTPAuth(hash_id, key, key_len, data, data_len,
        buf, sizeof (buf)) == auth_len && !memcmp(buf, auth, auth_len);
}

/* ================================================== */

int
UTI_DecodePasswordFromText(char *key)
{
  int i, j, len = strlen(key);
  char buf[3], *p;

  if (!strncmp(key, "ASCII:", 6)) {
    memmove(key, key + 6, len - 6);
    return len - 6;
  } else if (!strncmp(key, "HEX:", 4)) {
    if ((len - 4) % 2)
      return 0;

    for (i = 0, j = 4; j + 1 < len; i++, j += 2) {
      buf[0] = key[j], buf[1] = key[j + 1], buf[2] = '\0';
      key[i] = strtol(buf, &p, 16);

      if (p != buf + 2)
        return 0;
    }

    return i;
  } else {
    /* assume ASCII */
    return len;
  }
}

/* ================================================== */

int
UTI_SetQuitSignalsHandler(void (*handler)(int))
{
  struct sigaction sa;

  sa.sa_handler = handler;
  sa.sa_flags = SA_RESTART;
  if (sigemptyset(&sa.sa_mask) < 0)
    return 0;

#ifdef SIGINT
  if (sigaction(SIGINT, &sa, NULL) < 0)
    return 0;
#endif
#ifdef SIGTERM
  if (sigaction(SIGTERM, &sa, NULL) < 0)
    return 0;
#endif
#ifdef SIGQUIT
  if (sigaction(SIGQUIT, &sa, NULL) < 0)
    return 0;
#endif
#ifdef SIGHUP
  if (sigaction(SIGHUP, &sa, NULL) < 0)
    return 0;
#endif

  return 1;
}

/* ================================================== */

char *
UTI_PathToDir(const char *path)
{
  char *dir, *slash;

  slash = strrchr(path, '/');

  if (!slash)
    return Strdup(".");

  if (slash == path)
    return Strdup("/");

  dir = Malloc(slash - path + 1);
  snprintf(dir, slash - path + 1, "%s", path);

  return dir;
}

/* ================================================== */

static int
create_dir(char *p, mode_t mode, uid_t uid, gid_t gid)
{
  int status;
  struct stat buf;

  /* See if directory exists */
  status = stat(p, &buf);

  if (status < 0) {
    if (errno != ENOENT) {
      LOG(LOGS_ERR, LOGF_Util, "Could not access %s : %s", p, strerror(errno));
      return 0;
    }
  } else {
    if (S_ISDIR(buf.st_mode))
      return 1;
    LOG(LOGS_ERR, LOGF_Util, "%s is not directory", p);
    return 0;
  }

  /* Create the directory */
  if (mkdir(p, mode) < 0) {
    LOG(LOGS_ERR, LOGF_Util, "Could not create directory %s : %s", p, strerror(errno));
    return 0;
  }

  /* Set its owner */
  if (chown(p, uid, gid) < 0) {
    LOG(LOGS_ERR, LOGF_Util, "Could not change ownership of %s : %s", p, strerror(errno));
    /* Don't leave it there with incorrect ownership */
    rmdir(p);
    return 0;
  }

  return 1;
}

/* ================================================== */
/* Return 0 if the directory couldn't be created, 1 if it could (or
   already existed) */
int
UTI_CreateDirAndParents(const char *path, mode_t mode, uid_t uid, gid_t gid)
{
  char *p;
  int i, j, k, last;

  /* Don't try to create current directory */
  if (!strcmp(path, "."))
    return 1;

  p = (char *)Malloc(1 + strlen(path));

  i = k = 0;
  while (1) {
    p[i++] = path[k++];

    if (path[k] == '/' || !path[k]) {
      /* Check whether its end of string, a trailing / or group of / */
      last = 1;
      j = k;
      while (path[j]) {
        if (path[j] != '/') {
          /* Pick up a / into p[] thru the assignment at the top of the loop */
          k = j - 1;
          last = 0;
          break;
        }
        j++;
      }

      p[i] = 0;

      if (!create_dir(p, last ? mode : 0755, last ? uid : 0, last ? gid : 0)) {
        Free(p);
        return 0;
      }

      if (last)
        break;
    }

    if (!path[k])
      break;
  }

  Free(p);
  return 1;
}

/* ================================================== */

int
UTI_CheckDirPermissions(const char *path, mode_t perm, uid_t uid, gid_t gid)
{
  struct stat buf;

  if (stat(path, &buf)) {
    LOG(LOGS_ERR, LOGF_Util, "Could not access %s : %s", path, strerror(errno));
    return 0;
  }

  if (!S_ISDIR(buf.st_mode)) {
    LOG(LOGS_ERR, LOGF_Util, "%s is not directory", path);
    return 0;
  }

  if ((buf.st_mode & 0777) & ~perm) {
    LOG(LOGS_ERR, LOGF_Util, "Wrong permissions on %s", path);
    return 0;
  }

  if (buf.st_uid != uid) {
    LOG(LOGS_ERR, LOGF_Util, "Wrong owner of %s (%s != %d)", path, "UID", uid);
    return 0;
  }

  if (buf.st_gid != gid) {
    LOG(LOGS_ERR, LOGF_Util, "Wrong owner of %s (%s != %d)", path, "GID", gid);
    return 0;
  }

  return 1;
}

/* ================================================== */

void
UTI_DropRoot(uid_t uid, gid_t gid)
{
  /* Drop supplementary groups */
  if (setgroups(0, NULL))
    LOG_FATAL(LOGF_Util, "setgroups() failed : %s", strerror(errno));

  /* Set effective, saved and real group ID */
  if (setgid(gid))
    LOG_FATAL(LOGF_Util, "setgid(%d) failed : %s", gid, strerror(errno));

  /* Set effective, saved and real user ID */
  if (setuid(uid))
    LOG_FATAL(LOGF_Util, "setuid(%d) failed : %s", uid, strerror(errno));

  DEBUG_LOG(LOGF_Util, "Dropped root privileges: UID %d GID %d", uid, gid);
}

/* ================================================== */

#define DEV_URANDOM "/dev/urandom"

void
UTI_GetRandomBytesUrandom(void *buf, unsigned int len)
{
  static FILE *f = NULL;

  if (!f)
    f = fopen(DEV_URANDOM, "r");
  if (!f)
    LOG_FATAL(LOGF_Util, "Can't open %s : %s", DEV_URANDOM, strerror(errno));
  if (fread(buf, 1, len, f) != len)
    LOG_FATAL(LOGF_Util, "Can't read from %s", DEV_URANDOM);
}

/* ================================================== */

void
UTI_GetRandomBytes(void *buf, unsigned int len)
{
#ifdef HAVE_ARC4RANDOM
  arc4random_buf(buf, len);
#else
  UTI_GetRandomBytesUrandom(buf, len);
#endif
}
