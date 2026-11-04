#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#if defined(__MVS__)
#  include <_Nascii.h>
#endif

#include "dynalloc.h"
#include "utils.h"

#ifdef __clang__
#  if __clang_major__ < 18
#    define ZL_NR(attr, reg)  attr "NR:" #reg
#  else
#    define ZL_NR(attr, reg)  attr "{" #reg "}"
#  endif
#endif

#define SLAB_SIZE (2 * 1024 * 1024)

typedef struct { char *base; unsigned cap; unsigned off; } slab31_t;
static slab31_t        g_slab;
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static int              g_slab_ready = 0;

static int slab_init(void)
{
    if (g_slab_ready) return 0;
#if defined(__MVS__)
    g_slab.base = (char *)__malloc31(SLAB_SIZE);
#else
    g_slab.base = (char *)malloc(SLAB_SIZE);
#endif
    if (!g_slab.base) return -1;
    memset(g_slab.base, 0, SLAB_SIZE);
    g_slab.cap  = SLAB_SIZE;
    g_slab.off  = 0;
    g_slab_ready = 1;
    return 0;
}

static void slab_reset(void)
{
    if (g_slab_ready) {
        memset(g_slab.base, 0, g_slab.off);
        g_slab.off = 0;
    }
}

static void *slab_alloc(unsigned n)
{
    unsigned aligned = (n + 7u) & ~7u;
    if (g_slab.off + aligned > g_slab.cap) return NULL;
    void *p = g_slab.base + g_slab.off;
    g_slab.off += aligned;
    return p;
}

static unsigned int s31(void *p) { return (unsigned int)(uintptr_t)p; }

typedef struct {
    unsigned short len;
    char           str[1];
} vchar_t;

static unsigned int make_char(const char *s, int specified_len)
{
    int len = specified_len ? specified_len : (int)strlen(s);
    int cpy = (int)strlen(s);
    if (cpy > len) cpy = len;
    char *p = (char *)slab_alloc((unsigned)(len + 1));
    if (!p) return 0;
    memcpy(p, s, (size_t)cpy);
    p[cpy] = '\0';
    if (cpy > 0) a2e_mem(p, cpy);
    return s31(p);
}

static unsigned int make_vchar(const char *str, int specified_len)
{
    int klen = (int)strlen(str);
    vchar_t *v = (vchar_t *)slab_alloc((unsigned)(2 + specified_len + 1));
    if (!v) return 0;
    v->len = (unsigned short)specified_len;
    if (klen > 0) {
        memcpy(v->str, str, (size_t)klen);
        a2e_mem(v->str, klen);
    }
    return s31(v);
}

static unsigned int make_plist(int argc, ...)
{
    if (argc < 1) return 0;
    unsigned int *pl = (unsigned int *)slab_alloc((unsigned)(argc * 4));
    if (!pl) return 0;
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++)
        pl[i] = va_arg(ap, unsigned int);
    va_end(ap);
    pl[argc - 1] |= 0x80000000u;
    return s31(pl);
}

typedef struct {
    unsigned int pgm;
    unsigned int dsa[18];
} bpxwdyn_wa_t;

#if defined(__MVS__) && defined(__clang__)

static int call_svc6(bpxwdyn_wa_t *wa, unsigned int r1_plist)
{
    unsigned int r15 = s31(&wa->pgm);
    unsigned int r13 = s31(&wa->dsa[0]);
    unsigned int r0  = 0u;
    int saved_mode;

    saved_mode = __ae_thread_swapmode(__AE_EBCDIC_MODE);

    __asm(" sam31\n"
          " svc 6\n"
          " sam64\n"
          : ZL_NR("+", r15)(r15)
          : ZL_NR("",  r0)(r0),
            ZL_NR("",  r1)(r1_plist),
            ZL_NR("",  r13)(r13)
          : "cc", "r14");

    __ae_thread_swapmode(saved_mode);
    return (int)r15;
}

#elif defined(__MVS__) && !defined(__clang__)

#pragma linkage(svc6_raw, OS)
extern int svc6_raw(void *r15, void *r1, void *r13, int r0);

static int call_svc6(bpxwdyn_wa_t *wa, unsigned int r1_plist)
{
    int saved_mode = __ae_thread_swapmode(__AE_EBCDIC_MODE);
    int rc = svc6_raw((void *)(uintptr_t)s31(&wa->pgm),
                      (void *)(uintptr_t)r1_plist,
                      (void *)(uintptr_t)s31(&wa->dsa[0]),
                      0);
    __ae_thread_swapmode(saved_mode);
    return rc;
}

#else

static int call_svc6(bpxwdyn_wa_t *wa, unsigned int r1_plist)
{
    (void)wa; (void)r1_plist;
    return -1;
}

#endif

int dynalloc(const char *cmd,
             char *msg,   size_t msglen,
             char *ddn,   size_t ddn_sz,
             char *dsn,   size_t dsn_sz,
             char *vol,   size_t vol_sz)
{
    int return_vars = ((ddn && ddn_sz > 0) ||
                       (dsn && dsn_sz > 0) ||
                       (vol && vol_sz > 0));
    int rc, i;

    pthread_mutex_lock(&g_lock);

    if (slab_init() != 0) {
        pthread_mutex_unlock(&g_lock);
        if (msg && msglen > 0)
            snprintf(msg, msglen, "dynalloc: slab init failed");
        return -1;
    }
    slab_reset();

    bpxwdyn_wa_t *wa = (bpxwdyn_wa_t *)slab_alloc(sizeof(*wa));
    if (!wa) goto slab_full;

    wa->pgm = make_char("BPXWDYN ", 8);
    if (!wa->pgm) goto slab_full;

    unsigned int cmd31 = make_char(cmd, 0);
    if (!cmd31) goto slab_full;

    unsigned int rtddn_vc = 0, rtdsn_vc = 0, rtvol_vc = 0;
    if (return_vars) {
        rtddn_vc = make_vchar("RTDDN",  9);
        rtdsn_vc = make_vchar("RTDSN", 55);
        rtvol_vc = make_vchar("RTVOL",  9);
        if (!rtddn_vc || !rtdsn_vc || !rtvol_vc) goto slab_full;
    }

    unsigned int msg_vc[5];
    msg_vc[0] = make_vchar("MSG",   258);
    msg_vc[1] = make_vchar("MSG.1", 258);
    msg_vc[2] = make_vchar("MSG.2", 258);
    msg_vc[3] = make_vchar("MSG.3", 258);
    msg_vc[4] = make_vchar("MSG.4", 258);
    for (i = 0; i < 5; i++)
        if (!msg_vc[i]) goto slab_full;

    unsigned int plist31;
    if (return_vars) {
        plist31 = make_plist(9,
            cmd31,
            rtddn_vc, rtdsn_vc, rtvol_vc,
            msg_vc[0], msg_vc[1], msg_vc[2], msg_vc[3], msg_vc[4]);
    } else {
        plist31 = make_plist(6,
            cmd31,
            msg_vc[0], msg_vc[1], msg_vc[2], msg_vc[3], msg_vc[4]);
    }
    if (!plist31) goto slab_full;

    rc = call_svc6(wa, plist31);

    if (rc == 0 && return_vars) {
        if (ddn && ddn_sz > 0 && rtddn_vc) {
            vchar_t *v = (vchar_t *)(uintptr_t)rtddn_vc;
            int len = (int)strlen(v->str);
            if (len > (int)ddn_sz - 1) len = (int)ddn_sz - 1;
            if (len > 0) {
                memcpy(ddn, v->str, (size_t)len);
                ddn[len] = '\0';
                e2a_mem(ddn, len);
            } else {
                ddn[0] = '\0';
            }
        }
        if (dsn && dsn_sz > 0 && rtdsn_vc) {
            vchar_t *v = (vchar_t *)(uintptr_t)rtdsn_vc;
            int len = (int)strlen(v->str);
            if (len > (int)dsn_sz - 1) len = (int)dsn_sz - 1;
            if (len > 0) {
                memcpy(dsn, v->str, (size_t)len);
                dsn[len] = '\0';
                e2a_mem(dsn, len);
            } else {
                dsn[0] = '\0';
            }
        }
        if (vol && vol_sz > 0 && rtvol_vc) {
            vchar_t *v = (vchar_t *)(uintptr_t)rtvol_vc;
            int len = (int)strlen(v->str);
            if (len > (int)vol_sz - 1) len = (int)vol_sz - 1;
            if (len > 0) {
                memcpy(vol, v->str, (size_t)len);
                vol[len] = '\0';
                e2a_mem(vol, len);
            } else {
                vol[0] = '\0';
            }
        }
    } else if (rc != 0 && msg && msglen > 0) {
        unsigned short *size = (unsigned short *)(uintptr_t)msg_vc[0];
        int cnt = 0;
        if (*size > 0 && *size <= 257) {
            char ctmp[4];
            int clen = *size < 3 ? *size : 3;
            char *hstr = (char *)((uintptr_t)msg_vc[0] + 2);
            memcpy(ctmp, hstr, (size_t)clen);
            ctmp[clen] = '\0';
            e2a_mem(ctmp, clen);
            cnt = atoi(ctmp);
            if (cnt < 0 || cnt > 4) cnt = 0;
        }
        size_t pos = 0;
        for (i = 0; i < cnt; i++) {
            unsigned short *msz = (unsigned short *)(uintptr_t)msg_vc[i + 1];
            int mlen = (int)*msz;
            if (mlen <= 0 || mlen > 257) continue;
            char *mstr = (char *)((uintptr_t)msg_vc[i + 1] + 2);
            e2a_mem(mstr, mlen);
            while (mlen > 0 && mstr[mlen-1] == ' ') mlen--;
            if (mlen <= 0) continue;
            if (pos + (size_t)mlen + 2 < msglen) {
                if (pos > 0) msg[pos++] = '\n';
                memcpy(msg + pos, mstr, (size_t)mlen);
                pos += (size_t)mlen;
                msg[pos] = '\0';
            }
        }
        if (pos == 0)
            snprintf(msg, msglen, "dynalloc: BPXWDYN rc=%d", rc);
    }

    pthread_mutex_unlock(&g_lock);
    return rc;

slab_full:
    pthread_mutex_unlock(&g_lock);
    if (msg && msglen > 0)
        snprintf(msg, msglen, "dynalloc: slab exhausted");
    return -1;
}

int dynalloc_str(const char *cmd, char *msg, size_t msglen)
{
    return dynalloc(cmd, msg, msglen, NULL, 0, NULL, 0, NULL, 0);
}

int dynalloc_temp_dsn(char *out_dsn, size_t out_len, const char *prefix)
{
    if (!out_dsn || out_len < 44) return -1;
    const char *hlq = get_current_hlq();
    pid_t pid = getpid();
    time_t now = time(NULL);
    unsigned int r = (unsigned int)(now ^ (pid << 8)) & 0xFFFF;

    if (!prefix || !*prefix) prefix = "XMI";

    snprintf(out_dsn, out_len, "%s.%s.T%04X.P%d", hlq, prefix, r, (int)(pid % 100000));
    return 0;
}

int dynalloc_alloc_temp_seq(const char *dsn, int pri_tracks, int sec_tracks)
{
    char cmd[256];
    char msg[256];
    if (pri_tracks <= 0) pri_tracks = 10;
    if (sec_tracks <= 0) sec_tracks = 50;

    snprintf(cmd, sizeof(cmd),
             "alloc da('%s') new catalog space(%d,%d) tracks recfm(f,b) lrecl(80) blksize(3120)",
             dsn, pri_tracks, sec_tracks);

    log_verbose("dynalloc: %s", cmd);
    int rc = dynalloc_str(cmd, msg, sizeof(msg));
    if (rc != 0) {
        log_error("Failed to allocate temporary dataset '%s': %s (rc=%d)", dsn, msg, rc);
    }
    return rc;
}

int dynalloc_free_dsn(const char *dsn)
{
    char cmd[128];
    char msg[128];
    snprintf(cmd, sizeof(cmd), "free da('%s')", dsn);
    return dynalloc_str(cmd, msg, sizeof(msg));
}

int dynalloc_delete_dsn(const char *dsn)
{
    char cmd[128];
    char msg[128];
    snprintf(cmd, sizeof(cmd), "alloc fi(DELDD) da('%s') old delete", dsn);
    log_verbose("dynalloc: %s", cmd);
    int rc = dynalloc_str(cmd, msg, sizeof(msg));
    if (rc == 0) {
        dynalloc_str("free fi(DELDD)", msg, sizeof(msg));
    }
    return rc;
}
