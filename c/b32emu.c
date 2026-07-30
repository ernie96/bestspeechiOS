/* b32emu -- see b32emu.h. Public domain. */
#include "b32emu.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unicorn/unicorn.h>

#include "sonic/sonic.h"

/* ------------------------------------------------------------ memory map
 * Kept identical to the Python reference harness so output stays comparable. */
#define IMAGE_BASE   0x10000000u
#define STUB_BASE    0x0F000000u   /* one slot per shimmed API              */
#define SLOT         64u           /* must exceed the largest trampoline    */
#define RETURN_MAGIC (STUB_BASE + 0xFE0u)
#define SCRATCH      0x0E000000u
#define SCRATCH_SIZE 0x10000u
#define STACK_BASE   0x00100000u
#define STACK_SIZE   0x00100000u
#define STACK_TOP    (STACK_BASE + STACK_SIZE - 0x1000u)
#define HEAP_BASE    0x20000000u
#define HEAP_SIZE    0x04000000u
#define TEB_BASE     0x7FFDE000u
#define PEB_BASE     0x7FFDD000u
#define GDT_BASE     0x7FF00000u

/* trampoline parameter cells */
#define G_HWND   (SCRATCH + 0u)
#define G_MSG    (SCRATCH + 4u)
#define G_WP     (SCRATCH + 8u)
#define G_LP     (SCRATCH + 12u)
#define G_PROC   (SCRATCH + 16u)
#define G_TTS    (SCRATCH + 20u)
#define G_RELBUF (SCRATCH + 24u)

#define CMDLINE_ADDR (SCRATCH + 0x100u)
#define ENVSTR_ADDR  (SCRATCH + 0x140u)
#define MODNAME_ADDR (SCRATCH + 0x180u)
#define ARGS_ADDR    (SCRATCH + 0x1000u)
#define TEXT_ADDR    (ARGS_ADDR + 0x100u)
#define TEXT_MAX     0x4000u

/* Prefixed because unicorn.h drags in windows.h on Windows hosts, which
 * already defines the unprefixed WHDR_* names. */
#define WM_REL_BUF        0x401u
#define B32_WHDR_DONE     0x1u
#define B32_WHDR_PREPARED 0x2u
#define B32_WHDR_INQUEUE  0x10u

#define MAX_MSGS    256
#define MAX_CLASSES 16
#define MAX_WINDOWS 16
#define MAX_TLS     128
#define API_SLOTS   64

typedef struct { char name[32]; uint32_t va; } pe_export;
typedef struct { char name[64]; uint32_t wndproc; } guest_class;
typedef struct { uint32_t hwnd, wndproc; } guest_window;
typedef struct { uint32_t hwnd, msg, wp, lp; } guest_msg;

typedef uint32_t (*api_fn)(b32_emu *, const uint32_t *);
typedef struct { const char *name; int nargs; api_fn fn; int dispatch; } api_ent;

struct b32_emu {
    uc_engine *uc;
    uint8_t   *img;          /* host copy of the mapped image */
    uint32_t   img_size;
    uint32_t   entry;
    uint32_t   heap_ptr;
    uint32_t   tts;

    uint32_t   retstub16;
    uint32_t   relbuf_stub;

    pe_export *exports;
    int        nexports;
    struct {
        uint32_t bstCreate, bstClose, bstDestroy, bstSetParams;
        uint32_t bstRelBuf, TtsWav;
    } ex;

    uint32_t   tls[MAX_TLS];
    int        tls_next;

    guest_msg  msgs[MAX_MSGS];
    int        msg_head, msg_tail;

    guest_class classes[MAX_CLASSES];
    int         nclasses;
    guest_window windows[MAX_WINDOWS];
    int          nwindows;
    uint32_t     next_hwnd;

    /* audio capture */
    b32_audio_cb cb;
    void        *cb_user;
    int          abort;
    int16_t     *audio;
    size_t       audio_len;   /* bytes */
    size_t       audio_cap;   /* bytes */
    uint32_t     wave_rate, wave_bits, wave_ch;
    uint32_t     wave_hwo;

    sonicStream  sonic;       /* created lazily, only when speed != 1 */
    float        speed;
    int          rate;        /* emitted as a ~r code, see b32_set_rate */

    /* Voice-quality overrides, B32_VOICE_DEFAULT to defer to the voice. */
    int          inflection, head_size, excitation, unvoiced;
    int          pitch_pct;
    unsigned     parse_flags;
    int          phrase_pred;

    /* Silence trimming. Near-silent samples are withheld in `hold` until we
     * know whether the run ends inside the utterance or at its end, since the
     * two have different caps and a stream cannot see ahead. */
    int          pause_cap_ms, tail_cap_ms;
    int16_t     *hold;
    size_t       hold_run;    /* length of the run so far, in samples */
    size_t       hold_len;    /* how much of it was kept (see hold_add) */
    size_t       hold_cap;    /* samples allocated */

    int   trace;
    char  err[256];
};

/* --------------------------------------------------------------- helpers */
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static void seterr(b32_emu *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->err, sizeof e->err, fmt, ap);
    va_end(ap);
}

static uint32_t r32(b32_emu *e, uint32_t addr) {
    uint32_t v = 0;
    uc_mem_read(e->uc, addr, &v, 4);
    return v;
}
static void w32(b32_emu *e, uint32_t addr, uint32_t v) {
    uc_mem_write(e->uc, addr, &v, 4);
}
static void read_cstr(b32_emu *e, uint32_t addr, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        uint8_t c = 0;
        if (uc_mem_read(e->uc, addr + i, &c, 1) != UC_ERR_OK || c == 0) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}
static uint32_t heap_alloc(b32_emu *e, uint32_t size) {
    uint32_t p = e->heap_ptr;
    if (size == 0) size = 1;
    e->heap_ptr = (e->heap_ptr + size + 15u) & ~15u;
    if (e->heap_ptr > HEAP_BASE + HEAP_SIZE) {
        seterr(e, "guest heap exhausted");
        return 0;
    }
    return p;
}
static void queue_msg(b32_emu *e, uint32_t hwnd, uint32_t msg,
                      uint32_t wp, uint32_t lp) {
    int nxt = (e->msg_tail + 1) % MAX_MSGS;
    if (nxt == e->msg_head) return;         /* queue full: drop, as Windows may */
    e->msgs[e->msg_tail].hwnd = hwnd;
    e->msgs[e->msg_tail].msg = msg;
    e->msgs[e->msg_tail].wp = wp;
    e->msgs[e->msg_tail].lp = lp;
    e->msg_tail = nxt;
}

/* ------------------------------------------------------- audio output path */

/* Hand a finished chunk to the caller, or accumulate it internally. */
static void emit_pcm(b32_emu *e, const int16_t *pcm, size_t bytes) {
    if (bytes == 0 || e->abort) return;
    if (e->cb) {
        if (!e->cb(pcm, bytes, e->cb_user)) e->abort = 1;
        return;
    }
    if (e->audio_len + bytes > e->audio_cap) {
        size_t cap = e->audio_cap ? e->audio_cap : 16384;
        while (cap < e->audio_len + bytes) cap *= 2;
        int16_t *grown = (int16_t *)realloc(e->audio, cap);
        if (!grown) { e->abort = 1; return; }
        e->audio = grown;
        e->audio_cap = cap;
    }
    memcpy((uint8_t *)e->audio + e->audio_len, pcm, bytes);
    e->audio_len += bytes;
}

/* Pull everything sonic is willing to give us right now. Its own buffer is the
 * one that grows, so a fixed read block here is safe at any speed -- unlike
 * reading back into the input buffer, which overflows when slowing down. */
static void drain_sonic(b32_emu *e) {
    int16_t out[2048];
    while (sonicSamplesAvailable(e->sonic) > 0) {
        int n = sonicReadShortFromStream(e->sonic, out,
                                         (int)(sizeof out / sizeof out[0]));
        if (n <= 0) break;
        emit_pcm(e, out, (size_t)n * sizeof out[0]);
        if (e->abort) return;
    }
}

static void push_audio(b32_emu *e, const int16_t *pcm, size_t bytes) {
    if (!e->sonic || e->speed == 1.0f) {
        emit_pcm(e, pcm, bytes);
        return;
    }
    if (!sonicWriteShortToStream(e->sonic, (short *)pcm,
                                 (int)(bytes / sizeof(int16_t)))) {
        seterr(e, "sonic ran out of memory");
        e->abort = 1;
        return;
    }
    drain_sonic(e);
}

/* ---- silence trimming, ahead of sonic so the caps are in engine time ----
 *
 * Anything this quiet counts as silence. The engine's pauses are digital zero
 * bar a handful of stray samples under 200, and its trailing silence is exactly
 * zero, so the threshold only has to clear that noise floor: -42 dBFS, far
 * below anything audible as speech. */
#define SILENCE_LEVEL 256
/* Not const: sonic's write entry point takes a non-const short*. */
static int16_t ZEROS[512];

static int is_silent(int16_t s) {
    return s > -SILENCE_LEVEL && s < SILENCE_LEVEL;
}
static size_t ms_samples(int ms) {
    return ms <= 0 ? 0 : (size_t)ms * B32_SAMPLE_RATE / 1000;
}

/* Withhold silence: whether a run is an interior pause or the utterance's tail
 * is only knowable once sound resumes or the engine stops. */
static void hold_add(b32_emu *e, const int16_t *pcm, size_t n) {
    e->hold_run += n;
    /* Runs longer than this cannot be reproduced sample-exactly when trimming
     * is off, and are padded with zeros instead -- which for this engine is
     * what they contain. Its longest pause is under half a second. */
    if (e->hold_len >= (size_t)B32_SAMPLE_RATE * 2) return;
    if (e->hold_len + n > e->hold_cap) {
        size_t cap = e->hold_cap ? e->hold_cap : 4096;
        while (cap < e->hold_len + n) cap *= 2;
        int16_t *grown = (int16_t *)realloc(e->hold, cap * sizeof *grown);
        if (!grown) { e->abort = 1; return; }
        e->hold = grown;
        e->hold_cap = cap;
    }
    memcpy(e->hold + e->hold_len, pcm, n * sizeof *pcm);
    e->hold_len += n;
}

/* Release a withheld run, keeping at most `keep` samples. They are taken from
 * the start so the preceding sound's decay survives; the rest is silence
 * either way. */
static void hold_release(b32_emu *e, size_t keep) {
    if (!e->hold_run) return;
    if (keep > e->hold_run) keep = e->hold_run;
    size_t have = keep < e->hold_len ? keep : e->hold_len;
    if (have) push_audio(e, e->hold, have * sizeof *e->hold);
    for (size_t left = keep - have; left && !e->abort; ) {
        size_t n = left < sizeof ZEROS / sizeof ZEROS[0]
                 ? left : sizeof ZEROS / sizeof ZEROS[0];
        push_audio(e, ZEROS, n * sizeof ZEROS[0]);
        left -= n;
    }
    e->hold_len = e->hold_run = 0;
}

/* Split a chunk into silence runs and sound, withholding the runs. */
static void trim_audio(b32_emu *e, const int16_t *pcm, size_t bytes) {
    if (!e->pause_cap_ms && !e->tail_cap_ms) { push_audio(e, pcm, bytes); return; }
    size_t n = bytes / sizeof *pcm, i = 0;
    while (i < n && !e->abort) {
        size_t j = i;
        if (is_silent(pcm[i])) {
            while (j < n && is_silent(pcm[j])) j++;
            hold_add(e, pcm + i, j - i);
        } else {
            while (j < n && !is_silent(pcm[j])) j++;
            /* Sound again, so that run was an interior pause. */
            hold_release(e, e->pause_cap_ms ? ms_samples(e->pause_cap_ms)
                                            : e->hold_run);
            push_audio(e, pcm + i, (j - i) * sizeof *pcm);
        }
        i = j;
    }
}

/* Called once the engine has finished, so no tail is left withheld or in sonic. */
static void flush_audio(b32_emu *e) {
    /* Anything still withheld is the utterance's trailing silence. */
    hold_release(e, e->tail_cap_ms ? ms_samples(e->tail_cap_ms) : e->hold_run);
    if (!e->sonic || e->speed == 1.0f) return;
    sonicFlushStream(e->sonic);
    drain_sonic(e);
}

/* ============================================================ API shims */
static uint32_t a_zero(b32_emu *e, const uint32_t *a) { (void)e; (void)a; return 0; }
static uint32_t a_one(b32_emu *e, const uint32_t *a)  { (void)e; (void)a; return 1; }
static uint32_t a_arg0(b32_emu *e, const uint32_t *a) { (void)e; return a[0]; }

/* ---- kernel32: process / CRT startup */
static uint32_t k_GetVersion(b32_emu *e, const uint32_t *a) {
    (void)e; (void)a; return 0xC3B60004u;         /* Windows 95 */
}
static uint32_t k_GetCommandLineA(b32_emu *e, const uint32_t *a) {
    (void)e; (void)a; return CMDLINE_ADDR;
}
static uint32_t k_GetEnvironmentStrings(b32_emu *e, const uint32_t *a) {
    (void)e; (void)a; return ENVSTR_ADDR;
}
static uint32_t k_GetStartupInfoA(b32_emu *e, const uint32_t *a) {
    uint8_t si[68];
    memset(si, 0, sizeof si);
    si[0] = 68;
    uc_mem_write(e->uc, a[0], si, sizeof si);
    return 0;
}
static uint32_t k_GetModuleHandleA(b32_emu *e, const uint32_t *a) {
    if (e->trace && a[0]) {
        char nm[128];
        read_cstr(e, a[0], nm, sizeof nm);
        fprintf(stderr, "  GetModuleHandleA(\"%s\")\n", nm);
    }
    return IMAGE_BASE;
}
static uint32_t k_GetModuleFileNameA(b32_emu *e, const uint32_t *a) {
    char nm[260];
    read_cstr(e, MODNAME_ADDR, nm, sizeof nm);
    uint32_t len = (uint32_t)strlen(nm);
    if (len + 1 > a[2]) len = a[2] ? a[2] - 1 : 0;
    uc_mem_write(e->uc, a[1], nm, len);
    uint8_t z = 0;
    uc_mem_write(e->uc, a[1] + len, &z, 1);
    return len;
}
static uint32_t api_addr_by_name(b32_emu *e, const char *name);
static uint32_t k_GetProcAddress(b32_emu *e, const uint32_t *a) {
    char nm[128];
    if (a[1] & 0xFFFF0000u) read_cstr(e, a[1], nm, sizeof nm);
    else snprintf(nm, sizeof nm, "ord%u", a[1]);
    uint32_t slot = api_addr_by_name(e, nm);
    /* The CRT probes for CompareStringA/W, which this DLL does not import.
     * Reporting them unavailable is correct -- the CRT has a fallback. */
    if (e->trace || !slot)
        fprintf(stderr, "  GetProcAddress(\"%s\") -> 0x%x\n", nm, slot);
    return slot;
}
static uint32_t k_ExitProcess(b32_emu *e, const uint32_t *a) {
    seterr(e, "guest called ExitProcess(%u)", a[0]);
    uc_emu_stop(e->uc);
    return 0;
}
static uint32_t k_GetCurrentThreadId(b32_emu *e, const uint32_t *a) {
    (void)e; (void)a; return 0x1000;
}

/* ---- kernel32: memory */
static uint32_t k_VirtualAlloc(b32_emu *e, const uint32_t *a) {
    if (a[0]) return a[0];                      /* commit of a reserved block */
    return heap_alloc(e, align_up(a[1], 0x1000));
}
static uint32_t k_LocalAlloc(b32_emu *e, const uint32_t *a) {
    uint32_t p = heap_alloc(e, a[1]);
    if (p && (a[0] & 0x40u)) {                  /* LMEM_ZEROINIT */
        void *z = calloc(1, a[1] ? a[1] : 1);
        if (z) { uc_mem_write(e->uc, p, z, a[1]); free(z); }
    }
    return p;
}
static uint32_t k_GlobalAlloc(b32_emu *e, const uint32_t *a) {
    uint32_t p = heap_alloc(e, a[1]);
    if (p && (a[0] & 0x40u)) {                  /* GMEM_ZEROINIT */
        void *z = calloc(1, a[1] ? a[1] : 1);
        if (z) { uc_mem_write(e->uc, p, z, a[1]); free(z); }
    }
    return p;
}

/* ---- kernel32: TLS */
static uint32_t k_TlsAlloc(b32_emu *e, const uint32_t *a) {
    (void)a;
    if (e->tls_next >= MAX_TLS) return 0xFFFFFFFFu;
    return (uint32_t)e->tls_next++;
}
static uint32_t k_TlsFree(b32_emu *e, const uint32_t *a) {
    if (a[0] < MAX_TLS) e->tls[a[0]] = 0;
    return 1;
}
static uint32_t k_TlsSetValue(b32_emu *e, const uint32_t *a) {
    if (a[0] < MAX_TLS) e->tls[a[0]] = a[1];
    return 1;
}
static uint32_t k_TlsGetValue(b32_emu *e, const uint32_t *a) {
    return a[0] < MAX_TLS ? e->tls[a[0]] : 0;
}

/* ---- kernel32: stdio-ish */
static uint32_t k_GetStdHandle(b32_emu *e, const uint32_t *a) {
    (void)e;
    if (a[0] == 0xFFFFFFF6u) return 0x0007;
    if (a[0] == 0xFFFFFFF5u) return 0x000B;
    if (a[0] == 0xFFFFFFF4u) return 0x000F;
    return 0x0007;
}
static uint32_t k_GetFileType(b32_emu *e, const uint32_t *a) {
    (void)e; (void)a; return 2;                 /* FILE_TYPE_CHAR */
}
static uint32_t k_WriteFile(b32_emu *e, const uint32_t *a) {
    if (a[2] && a[2] < 0x10000u) {
        char *buf = (char *)malloc(a[2] + 1);
        if (buf) {
            if (uc_mem_read(e->uc, a[1], buf, a[2]) == UC_ERR_OK) {
                buf[a[2]] = 0;
                fprintf(stderr, "  [guest stdout] %s", buf);
            }
            free(buf);
        }
    }
    if (a[3]) w32(e, a[3], a[2]);
    return 1;
}

/* ---- kernel32: locale / codepage */
static uint32_t k_GetACP(b32_emu *e, const uint32_t *a) { (void)e; (void)a; return 1252; }
static uint32_t k_GetOEMCP(b32_emu *e, const uint32_t *a) { (void)e; (void)a; return 437; }
static uint32_t k_GetCPInfo(b32_emu *e, const uint32_t *a) {
    uint8_t cp[18];
    memset(cp, 0, sizeof cp);
    cp[0] = 1;                                  /* MaxCharSize */
    cp[4] = '?';                                /* DefaultChar */
    uc_mem_write(e->uc, a[1], cp, sizeof cp);
    return 1;
}
static uint32_t k_MultiByteToWideChar(b32_emu *e, const uint32_t *a) {
    uint32_t src = a[2], cb = a[3], dst = a[4], cch = a[5];
    uint32_t n;
    if (cb == 0xFFFFFFFFu) {
        char tmp[1024];
        read_cstr(e, src, tmp, sizeof tmp);
        n = (uint32_t)strlen(tmp) + 1;
    } else {
        n = cb;
    }
    if (cch == 0) return n;
    if (n > cch) n = cch;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = 0;
        uc_mem_read(e->uc, src + i, &c, 1);
        uint16_t w = c;                         /* latin-1 widening */
        uc_mem_write(e->uc, dst + i * 2, &w, 2);
    }
    return n;
}
static uint32_t k_WideCharToMultiByte(b32_emu *e, const uint32_t *a) {
    uint32_t src = a[2], cchw = a[3], dst = a[4], cbm = a[5];
    uint32_t n = 0;
    if (cchw == 0xFFFFFFFFu) {
        for (;; n++) {
            uint16_t w = 0;
            uc_mem_read(e->uc, src + n * 2, &w, 2);
            if (w == 0) { n++; break; }
            if (n > 4096) break;
        }
    } else {
        n = cchw;
    }
    if (cbm == 0) return n;
    if (n > cbm) n = cbm;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t w = 0;
        uc_mem_read(e->uc, src + i * 2, &w, 2);
        uint8_t c = (w < 256) ? (uint8_t)w : (uint8_t)'?';
        uc_mem_write(e->uc, dst + i, &c, 1);
    }
    return n;
}

/* ---- kernel32: time */
static uint32_t k_GetLocalTime(b32_emu *e, const uint32_t *a) {
    /* Fixed timestamp: the engine only uses this for a seed-ish purpose, and a
     * constant keeps synthesis bit-for-bit reproducible. */
    uint16_t st[8] = { 1995, 9, 3, 27, 12, 0, 0, 0 };
    uc_mem_write(e->uc, a[0], st, sizeof st);
    return 0;
}
static uint32_t k_GetTimeZoneInformation(b32_emu *e, const uint32_t *a) {
    void *z = calloc(1, 172);
    if (z) { uc_mem_write(e->uc, a[0], z, 172); free(z); }
    return 0;
}

/* ---- user32 */
static uint32_t u_RegisterClassA(b32_emu *e, const uint32_t *a) {
    uint32_t wndproc = r32(e, a[0] + 4);
    uint32_t name_ptr = r32(e, a[0] + 36);
    char nm[64];
    if (name_ptr & 0xFFFF0000u) read_cstr(e, name_ptr, nm, sizeof nm);
    else snprintf(nm, sizeof nm, "atom%u", name_ptr);
    if (e->nclasses < MAX_CLASSES) {
        snprintf(e->classes[e->nclasses].name,
                 sizeof e->classes[0].name, "%s", nm);
        e->classes[e->nclasses].wndproc = wndproc;
        e->nclasses++;
    }
    if (e->trace)
        fprintf(stderr, "  RegisterClassA(\"%s\", wndproc=0x%x)\n", nm, wndproc);
    return 0xC001;
}
static uint32_t u_CreateWindowExA(b32_emu *e, const uint32_t *a) {
    char nm[64];
    if (a[1] & 0xFFFF0000u) read_cstr(e, a[1], nm, sizeof nm);
    else snprintf(nm, sizeof nm, "atom%u", a[1]);
    uint32_t proc = 0;
    for (int i = 0; i < e->nclasses; i++)
        if (strcmp(e->classes[i].name, nm) == 0) { proc = e->classes[i].wndproc; break; }
    uint32_t hwnd = e->next_hwnd++;
    if (e->nwindows < MAX_WINDOWS) {
        e->windows[e->nwindows].hwnd = hwnd;
        e->windows[e->nwindows].wndproc = proc;
        e->nwindows++;
    }
    if (e->trace)
        fprintf(stderr, "  CreateWindowExA(class=\"%s\") -> hwnd=0x%x\n", nm, hwnd);
    return hwnd;
}
static uint32_t u_PeekMessageA(b32_emu *e, const uint32_t *a) {
    uint32_t pmsg = a[0], filter = a[1], mn = a[2], mx = a[3], remove = a[4];
    for (int i = e->msg_head; i != e->msg_tail; i = (i + 1) % MAX_MSGS) {
        guest_msg *m = &e->msgs[i];
        if (filter && m->hwnd != filter) continue;
        if ((mn || mx) && !(m->msg >= mn && (mx == 0 || m->msg <= mx))) continue;
        uint32_t out[7] = { m->hwnd, m->msg, m->wp, m->lp, 0, 0, 0 };
        uc_mem_write(e->uc, pmsg, out, sizeof out);
        if (remove & 1) {
            /* compact: shift the ring down over slot i */
            int j = i;
            while (j != e->msg_head) {
                int prev = (j - 1 + MAX_MSGS) % MAX_MSGS;
                e->msgs[j] = e->msgs[prev];
                j = prev;
            }
            e->msg_head = (e->msg_head + 1) % MAX_MSGS;
        }
        return 1;
    }
    return 0;
}
static uint32_t u_DispatchMessageA(b32_emu *e, const uint32_t *a) {
    uint32_t m[4] = { 0, 0, 0, 0 };
    uc_mem_read(e->uc, a[0], m, sizeof m);
    uint32_t proc;
    if (m[1] == WM_REL_BUF) {
        /* The engine requires bstRelBuf to run from its message loop, once per
         * waveOutWrite, or TtsWav never returns. Route it to a trampoline so
         * the call happens inside the guest rather than re-entering from here. */
        proc = e->relbuf_stub;
        w32(e, G_TTS, e->tts);                /* G_RELBUF was set at open() */
    } else {
        proc = e->retstub16;
        for (int i = 0; i < e->nwindows; i++)
            if (e->windows[i].hwnd == m[0] && e->windows[i].wndproc) {
                proc = e->windows[i].wndproc;
                break;
            }
    }
    if (e->trace)
        fprintf(stderr, "  DispatchMessageA(hwnd=0x%x msg=0x%x) -> proc 0x%x\n",
                m[0], m[1], proc);
    w32(e, G_HWND, m[0]);
    w32(e, G_MSG, m[1]);
    w32(e, G_WP, m[2]);
    w32(e, G_LP, m[3]);
    w32(e, G_PROC, proc);
    return 0;                                  /* slot body performs the call */
}

/* ---- winmm: we are the audio device */
static uint32_t w_waveOutOpen(b32_emu *e, const uint32_t *a) {
    uint32_t phwo = a[0], pfmt = a[2], flags = a[5];
    uint16_t tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    uc_mem_read(e->uc, pfmt + 0, &tag, 2);
    uc_mem_read(e->uc, pfmt + 2, &ch, 2);
    uc_mem_read(e->uc, pfmt + 4, &rate, 4);
    uc_mem_read(e->uc, pfmt + 14, &bits, 2);
    e->wave_rate = rate; e->wave_ch = ch; e->wave_bits = bits;
    if (e->trace)
        fprintf(stderr, "  waveOutOpen: %u Hz, %u-bit, %uch, tag=%u, flags=0x%x\n",
                rate, bits, ch, tag, flags);
    if (flags & 1u) return 0;                  /* WAVE_FORMAT_QUERY */
    if (phwo) w32(e, phwo, e->wave_hwo);
    /* No MM_WOM_OPEN: the callback "window" is the caller's opaque cookie, and
     * on real Windows those messages never reach the engine. */
    return 0;
}
static uint32_t w_waveOutPrepareHeader(b32_emu *e, const uint32_t *a) {
    w32(e, a[1] + 16, r32(e, a[1] + 16) | B32_WHDR_PREPARED);
    return 0;
}
static uint32_t w_waveOutWrite(b32_emu *e, const uint32_t *a) {
    uint32_t pwh = a[1];
    uint32_t lp_data = r32(e, pwh);
    uint32_t len = r32(e, pwh + 4);
    w32(e, pwh + 16, (r32(e, pwh + 16) | B32_WHDR_DONE) & ~B32_WHDR_INQUEUE);
    /* Must happen even when aborting, or TtsWav will not unwind. */
    queue_msg(e, 0, WM_REL_BUF, 0, 0);

    if (e->abort || len == 0) return 0;
    int16_t *tmp = (int16_t *)malloc(len);
    if (!tmp) { e->abort = 1; return 0; }
    if (uc_mem_read(e->uc, lp_data, tmp, len) != UC_ERR_OK) {
        free(tmp);
        seterr(e, "waveOutWrite: unreadable buffer at 0x%x", lp_data);
        e->abort = 1;
        return 0;
    }
    if (e->trace)
        fprintf(stderr, "  waveOutWrite: %u bytes\n", len);
    trim_audio(e, tmp, len);
    free(tmp);
    return 0;
}
static uint32_t w_waveOutUnprepareHeader(b32_emu *e, const uint32_t *a) {
    w32(e, a[1] + 16, r32(e, a[1] + 16) & ~B32_WHDR_PREPARED);
    return 0;
}

/* The table order defines slot layout; keep it stable. */
static const api_ent API[] = {
    /* kernel32 */
    { "ExitProcess",               1, k_ExitProcess,            0 },
    { "MultiByteToWideChar",       6, k_MultiByteToWideChar,    0 },
    { "SetEnvironmentVariableA",   2, a_one,                    0 },
    { "GetTimeZoneInformation",    1, k_GetTimeZoneInformation, 0 },
    { "GetLocalTime",              1, k_GetLocalTime,           0 },
    { "CloseHandle",               1, a_one,                    0 },
    { "FlushFileBuffers",          1, a_one,                    0 },
    { "LocalFree",                 1, a_zero,                   0 },
    { "LocalUnlock",               1, a_one,                    0 },
    { "LocalLock",                 1, a_arg0,                   0 },
    { "LocalAlloc",                2, k_LocalAlloc,             0 },
    { "GlobalFree",                1, a_zero,                   0 },
    { "GlobalLock",                1, a_arg0,                   0 },
    { "GlobalAlloc",               2, k_GlobalAlloc,            0 },
    { "GlobalUnlock",              1, a_one,                    0 },
    { "TlsGetValue",               1, k_TlsGetValue,            0 },
    { "TlsFree",                   1, k_TlsFree,                0 },
    { "LeaveCriticalSection",      1, a_zero,                   0 },
    { "EnterCriticalSection",      1, a_zero,                   0 },
    { "DeleteCriticalSection",     1, a_zero,                   0 },
    { "InitializeCriticalSection", 1, a_zero,                   0 },
    { "SetFilePointer",            4, a_zero,                   0 },
    { "GetEnvironmentStrings",     0, k_GetEnvironmentStrings,  0 },
    { "GetCommandLineA",           0, k_GetCommandLineA,        0 },
    { "GetVersion",                0, k_GetVersion,             0 },
    { "WideCharToMultiByte",       8, k_WideCharToMultiByte,    0 },
    { "GetProcAddress",            2, k_GetProcAddress,         0 },
    { "GetModuleHandleA",          1, k_GetModuleHandleA,       0 },
    { "SetStdHandle",              2, a_one,                    0 },
    { "GetCurrentThreadId",        0, k_GetCurrentThreadId,     0 },
    { "TlsSetValue",               2, k_TlsSetValue,            0 },
    { "TlsAlloc",                  0, k_TlsAlloc,               0 },
    { "GetLastError",              0, a_zero,                   0 },
    { "GetOEMCP",                  0, k_GetOEMCP,               0 },
    { "VirtualFree",               3, a_one,                    0 },
    { "VirtualAlloc",              4, k_VirtualAlloc,           0 },
    { "GetModuleFileNameA",        3, k_GetModuleFileNameA,     0 },
    { "GetACP",                    0, k_GetACP,                 0 },
    { "GetCPInfo",                 2, k_GetCPInfo,              0 },
    { "GetStdHandle",              1, k_GetStdHandle,           0 },
    { "GetFileType",               1, k_GetFileType,            0 },
    { "GetStartupInfoA",           1, k_GetStartupInfoA,        0 },
    { "WriteFile",                 5, k_WriteFile,              0 },
    /* user32 */
    { "DefWindowProcA",            4, a_zero,                   0 },
    { "RegisterClassA",            1, u_RegisterClassA,         0 },
    { "CreateWindowExA",          12, u_CreateWindowExA,        0 },
    { "MessageBeep",               1, a_one,                    0 },
    { "PeekMessageA",              5, u_PeekMessageA,           0 },
    { "TranslateMessage",          1, a_zero,                   0 },
    { "DispatchMessageA",          1, u_DispatchMessageA,       1 },
    /* winmm */
    { "waveOutReset",              1, a_zero,                   0 },
    { "waveOutClose",              1, a_zero,                   0 },
    { "waveOutPrepareHeader",      3, w_waveOutPrepareHeader,   0 },
    { "waveOutWrite",              3, w_waveOutWrite,           0 },
    { "waveOutUnprepareHeader",    3, w_waveOutUnprepareHeader, 0 },
    { "waveOutOpen",               6, w_waveOutOpen,            0 },
};
#define API_N ((int)(sizeof API / sizeof API[0]))

static uint32_t api_addr_by_name(b32_emu *e, const char *name) {
    (void)e;
    for (int i = 0; i < API_N; i++)
        if (strcmp(API[i].name, name) == 0) return STUB_BASE + (uint32_t)i * SLOT;
    return 0;
}

/* ---------------------------------------------------------------- voices */
/*                 name       ~v ~e  ~h  ~u  ~f  ~r  */
static const b32_voice VOICES[] = {
    { "Fred",              0, 3,   0,   0,  80,   0 },
    { "Sara",              2, 3, -20,   0, 175,   0 },
    { "Hary",              3, 3,  10,   0,  65,   5 },
    { "Wendy",             2, 1,  50,   0, 150,  -5 },
    { "Dexter",            6, 6,   0, -25,  90,   7 },
    { "Alien",             4, 6, -50, -20, 115, -20 },
    { "Kit",               5, 3,  40,   0, 230, -10 },
    { "Bruno",             3, 3,  50,   0,  60,   8 },
    { "Ghost",             3, 2,  50,   0,  60,   8 },
    { "Peeper",            2, 2,   0,   5,  80,   0 },
    { "Dracula",           3, 3,  45,  -5,  47,  10 },
    { "Granny",            4, 3, -60,   0, 350,  20 },
    { "Martha",            6, 4, 100,  -5, 300, -10 },
    { "Tim",               3, 4, -10,   0,  60, -10 },
};
#define VOICE_N ((int)(sizeof VOICES / sizeof VOICES[0]))

const b32_voice *b32_voices(int *count) {
    if (count) *count = VOICE_N;
    return VOICES;
}
const b32_voice *b32_voice_by_name(const char *name) {
    for (int i = 0; i < VOICE_N; i++) {
        const char *a = VOICES[i].name, *b = name;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b) return &VOICES[i];
    }
    return NULL;
}

/* ----------------------------------------------------------------- hooks */
static void hook_stub(uc_engine *uc, uint64_t addr, uint32_t size, void *ud) {
    (void)size;
    b32_emu *e = (b32_emu *)ud;
    uint32_t off = (uint32_t)addr - STUB_BASE;
    if (off % SLOT) return;                    /* only the slot's first insn */
    int idx = (int)(off / SLOT);
    if (idx < 0 || idx >= API_N) return;       /* retstub16 / relbuf trampoline */

    uint32_t esp = 0;
    uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    uint32_t args[16];
    int n = API[idx].nargs;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) args[i] = r32(e, esp + 4 + 4u * (uint32_t)i);

    if (e->trace) {
        fprintf(stderr, "  api %s(", API[idx].name);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "%s0x%x", i ? ", " : "", args[i]);
        fprintf(stderr, ")\n");
    }
    uint32_t ret = API[idx].fn(e, args);
    uc_reg_write(uc, UC_X86_REG_EAX, &ret);
}

static bool hook_badmem(uc_engine *uc, uc_mem_type type, uint64_t addr,
                        int size, int64_t value, void *ud) {
    (void)value;
    b32_emu *e = (b32_emu *)ud;
    uint32_t eip = 0, esp = 0;
    uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    seterr(e, "invalid memory access type=%d addr=0x%llx size=%d eip=0x%x esp=0x%x",
           (int)type, (unsigned long long)addr, size, eip, esp);
    return false;                              /* stop emulation */
}

/* ------------------------------------------------------------ GDT / TEB */
static void put_gdt_entry(uint8_t *p, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFFu);
    d |= (uint64_t)(base & 0xFFFFFFu) << 16;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xFu) << 48;
    d |= (uint64_t)(flags & 0xFu) << 52;
    d |= (uint64_t)((base >> 24) & 0xFFu) << 56;
    memcpy(p, &d, 8);
}

static int setup_gdt_teb(b32_emu *e) {
    uint8_t gdt[4 * 8];
    memset(gdt, 0, sizeof gdt);
    put_gdt_entry(gdt + 8,  0, 0xFFFFF, 0x9B, 0xC);        /* 1: code       */
    put_gdt_entry(gdt + 16, 0, 0xFFFFF, 0x93, 0xC);        /* 2: data       */
    put_gdt_entry(gdt + 24, TEB_BASE, 0xFFF, 0x93, 0x4);   /* 3: FS -> TEB  */
    if (uc_mem_write(e->uc, GDT_BASE, gdt, sizeof gdt) != UC_ERR_OK) return -1;

    uc_x86_mmr gdtr;
    memset(&gdtr, 0, sizeof gdtr);
    gdtr.base = GDT_BASE;
    gdtr.limit = (uint32_t)sizeof gdt - 1;
    if (uc_reg_write(e->uc, UC_X86_REG_GDTR, &gdtr) != UC_ERR_OK) return -1;

    int cs = 1 << 3, ds = 2 << 3, fs = 3 << 3;
    uc_reg_write(e->uc, UC_X86_REG_CS, &cs);
    uc_reg_write(e->uc, UC_X86_REG_DS, &ds);
    uc_reg_write(e->uc, UC_X86_REG_ES, &ds);
    uc_reg_write(e->uc, UC_X86_REG_GS, &ds);
    uc_reg_write(e->uc, UC_X86_REG_SS, &ds);
    uc_reg_write(e->uc, UC_X86_REG_FS, &fs);

    uint8_t teb[0x40];
    memset(teb, 0, sizeof teb);
    uint32_t v;
    v = 0xFFFFFFFFu; memcpy(teb + 0x00, &v, 4);   /* SEH chain end */
    v = STACK_TOP;   memcpy(teb + 0x04, &v, 4);   /* stack base    */
    v = STACK_BASE;  memcpy(teb + 0x08, &v, 4);   /* stack limit   */
    v = TEB_BASE;    memcpy(teb + 0x18, &v, 4);   /* TEB self      */
    v = PEB_BASE;    memcpy(teb + 0x30, &v, 4);   /* PEB           */
    return uc_mem_write(e->uc, TEB_BASE, teb, sizeof teb) == UC_ERR_OK ? 0 : -1;
}

/* ---------------------------------------------------------- stub building */
static int build_stubs(b32_emu *e) {
    uint8_t slot[SLOT];
    for (int i = 0; i < API_N; i++) {
        uint32_t addr = STUB_BASE + (uint32_t)i * SLOT;
        memset(slot, 0xCC, SLOT);
        size_t n = 0;
        if (API[i].dispatch) {
            /* Call the guest's window proc with (hwnd, msg, wParam, lParam),
             * then clean up DispatchMessageA's own stdcall argument. This body
             * is 33 bytes, which is why SLOT must exceed 32. */
            const uint32_t cells[4] = { G_LP, G_WP, G_MSG, G_HWND };
            for (int c = 0; c < 4; c++) {
                slot[n++] = 0xFF; slot[n++] = 0x35;          /* push [imm32] */
                memcpy(slot + n, &cells[c], 4); n += 4;
            }
            slot[n++] = 0xFF; slot[n++] = 0x15;              /* call [imm32] */
            uint32_t proc = G_PROC;
            memcpy(slot + n, &proc, 4); n += 4;
            slot[n++] = 0xC2; slot[n++] = 0x04; slot[n++] = 0x00;   /* ret 4 */
        } else if (API[i].nargs) {
            uint16_t pop = (uint16_t)(API[i].nargs * 4);
            slot[n++] = 0xC2;                                /* ret imm16   */
            memcpy(slot + n, &pop, 2); n += 2;
        } else {
            slot[n++] = 0xC3;                                /* ret         */
        }
        if (n > SLOT) { seterr(e, "%s trampoline overruns its slot", API[i].name); return -1; }
        if (uc_mem_write(e->uc, addr, slot, SLOT) != UC_ERR_OK) return -1;
    }

    /* wndproc-shaped `ret 16` for windows we do not know */
    e->retstub16 = STUB_BASE + (uint32_t)API_N * SLOT;
    memset(slot, 0xCC, SLOT);
    slot[0] = 0xC2; slot[1] = 0x10; slot[2] = 0x00;
    if (uc_mem_write(e->uc, e->retstub16, slot, SLOT) != UC_ERR_OK) return -1;

    /* wndproc-shaped trampoline that calls bstRelBuf(G_TTS) inside the guest */
    e->relbuf_stub = STUB_BASE + (uint32_t)(API_N + 1) * SLOT;
    memset(slot, 0xCC, SLOT);
    {
        size_t n = 0;
        uint32_t c;
        slot[n++] = 0xFF; slot[n++] = 0x35;                  /* push [G_TTS]    */
        c = G_TTS;    memcpy(slot + n, &c, 4); n += 4;
        slot[n++] = 0xFF; slot[n++] = 0x15;                  /* call [G_RELBUF] */
        c = G_RELBUF; memcpy(slot + n, &c, 4); n += 4;
        slot[n++] = 0x83; slot[n++] = 0xC4; slot[n++] = 0x04; /* add esp, 4     */
        slot[n++] = 0xC2; slot[n++] = 0x10; slot[n++] = 0x00; /* ret 16         */
    }
    if (uc_mem_write(e->uc, e->relbuf_stub, slot, SLOT) != UC_ERR_OK) return -1;

    uint8_t hlt = 0xF4;
    return uc_mem_write(e->uc, RETURN_MAGIC, &hlt, 1) == UC_ERR_OK ? 0 : -1;
}

/* ------------------------------------------------------------- PE loading */
struct pe_info {
    uint32_t entry;
    uint32_t size;
    pe_export *exports;
    int nexports;
};

static int load_pe(b32_emu *e, const uint8_t *raw, size_t len,
                   struct pe_info *out) {
    if (len < 0x40) { seterr(e, "file too small"); return -1; }
    if (rd16(raw) != 0x5A4D) { seterr(e, "not an MZ image"); return -1; }
    uint32_t pe_off = rd32(raw + 0x3C);
    if (pe_off + 0xF8 > len) { seterr(e, "bad e_lfanew"); return -1; }
    if (rd32(raw + pe_off) != 0x00004550u) { seterr(e, "bad PE signature"); return -1; }

    const uint8_t *coff = raw + pe_off + 4;
    uint16_t machine = rd16(coff + 0);
    uint16_t nsec    = rd16(coff + 2);
    uint16_t opt_sz  = rd16(coff + 16);
    if (machine != 0x14C) { seterr(e, "not i386 (machine=0x%x)", machine); return -1; }

    const uint8_t *opt = coff + 20;
    if (rd16(opt) != 0x10B) { seterr(e, "not PE32"); return -1; }
    uint32_t image_base  = rd32(opt + 28);
    uint32_t size_image  = rd32(opt + 56);
    uint32_t size_hdrs   = rd32(opt + 60);
    uint32_t exp_rva     = rd32(opt + 96 + 0 * 8);
    uint32_t imp_rva     = rd32(opt + 96 + 1 * 8);
    uint32_t entry_rva   = rd32(opt + 16);

    if (image_base != IMAGE_BASE) {
        /* Relocation is unnecessary for this DLL and unimplemented; bail loudly
         * rather than silently producing garbage. */
        seterr(e, "unexpected ImageBase 0x%x (expected 0x%x)", image_base, IMAGE_BASE);
        return -1;
    }

    uint32_t img_size = align_up(size_image, 0x1000);
    uint8_t *img = (uint8_t *)calloc(1, img_size);
    if (!img) { seterr(e, "out of memory"); return -1; }
    uint32_t hcopy = size_hdrs < len ? size_hdrs : (uint32_t)len;
    memcpy(img, raw, hcopy);

    const uint8_t *sh = opt + opt_sz;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = sh + i * 40;
        uint32_t vsz = rd32(s + 8), va = rd32(s + 12);
        uint32_t rsz = rd32(s + 16), ptr = rd32(s + 20);
        uint32_t n = rsz < vsz ? rsz : vsz;
        if (ptr + n > len || va + vsz > img_size) continue;
        if (n) memcpy(img + va, raw + ptr, n);
    }

    /* exports */
    pe_export *exps = NULL;
    int nexp = 0;
    if (exp_rva && exp_rva + 40 <= img_size) {
        const uint8_t *ed = img + exp_rva;
        uint32_t nnames = rd32(ed + 24);
        uint32_t a_func  = rd32(ed + 28);
        uint32_t a_names = rd32(ed + 32);
        uint32_t a_ords  = rd32(ed + 36);
        exps = (pe_export *)calloc(nnames ? nnames : 1, sizeof *exps);
        if (!exps) { free(img); seterr(e, "out of memory"); return -1; }
        for (uint32_t i = 0; i < nnames; i++) {
            uint32_t nrva = rd32(img + a_names + i * 4);
            uint16_t ord  = rd16(img + a_ords + i * 2);
            uint32_t frva = rd32(img + a_func + ord * 4);
            if (nrva >= img_size) continue;
            snprintf(exps[nexp].name, sizeof exps[0].name, "%s",
                     (const char *)(img + nrva));
            exps[nexp].va = IMAGE_BASE + frva;
            nexp++;
        }
    }

    /* imports: point every thunk at its stub slot */
    if (imp_rva) {
        for (const uint8_t *d = img + imp_rva; d + 20 <= img + img_size; d += 20) {
            uint32_t oft = rd32(d + 0), name_rva = rd32(d + 12), ft = rd32(d + 16);
            if (!name_rva && !ft && !oft) break;
            uint32_t lookup = oft ? oft : ft;
            if (!lookup || !ft) continue;
            for (uint32_t i = 0;; i++) {
                if (lookup + i * 4 + 4 > img_size) break;
                uint32_t t = rd32(img + lookup + i * 4);
                if (!t) break;
                char nm[128];
                if (t & 0x80000000u) snprintf(nm, sizeof nm, "ord%u", t & 0xFFFFu);
                else if (t + 2 < img_size)
                    snprintf(nm, sizeof nm, "%s", (const char *)(img + t + 2));
                else continue;
                uint32_t slot = api_addr_by_name(e, nm);
                if (!slot) {
                    fprintf(stderr, "  WARNING: unshimmed import %s\n", nm);
                    slot = STUB_BASE + (uint32_t)API_N * SLOT;   /* retstub16 */
                }
                memcpy(img + ft + i * 4, &slot, 4);
            }
        }
    }

    if (uc_mem_map(e->uc, IMAGE_BASE, img_size, UC_PROT_ALL) != UC_ERR_OK) {
        free(img); free(exps);
        seterr(e, "cannot map image");
        return -1;
    }
    if (uc_mem_write(e->uc, IMAGE_BASE, img, img_size) != UC_ERR_OK) {
        free(img); free(exps);
        seterr(e, "cannot write image");
        return -1;
    }
    e->img = img;
    e->img_size = img_size;
    out->entry = IMAGE_BASE + entry_rva;
    out->size = img_size;
    out->exports = exps;
    out->nexports = nexp;
    return 0;
}

static uint32_t export_va(struct pe_info *pi, const char *name) {
    for (int i = 0; i < pi->nexports; i++)
        if (strcmp(pi->exports[i].name, name) == 0) return pi->exports[i].va;
    return 0;
}

/* ------------------------------------------------------- host -> guest call */
static int guest_call(b32_emu *e, uint32_t va, const uint32_t *args, int nargs,
                      uint64_t timeout_us, uint32_t *ret) {
    uint32_t esp = STACK_TOP - 0x2000;
    uint32_t frame[17];
    frame[0] = RETURN_MAGIC;
    for (int i = 0; i < nargs && i < 16; i++) frame[1 + i] = args[i];
    if (uc_mem_write(e->uc, esp, frame, 4u * (uint32_t)(1 + nargs)) != UC_ERR_OK) {
        seterr(e, "cannot write call frame");
        return -1;
    }
    uint32_t eflags = 0x202;
    uc_reg_write(e->uc, UC_X86_REG_ESP, &esp);
    uc_reg_write(e->uc, UC_X86_REG_EBP, &esp);
    uc_reg_write(e->uc, UC_X86_REG_EFLAGS, &eflags);

    e->err[0] = 0;
    uc_err r = uc_emu_start(e->uc, va, RETURN_MAGIC, timeout_us, 0);
    if (r != UC_ERR_OK) {
        if (!e->err[0]) seterr(e, "emulation error: %s", uc_strerror(r));
        return -1;
    }
    uint32_t eip = 0;
    uc_reg_read(e->uc, UC_X86_REG_EIP, &eip);
    if (eip != RETURN_MAGIC) {
        if (!e->err[0])
            seterr(e, "emulation stopped at 0x%x, not the return magic", eip);
        return -1;
    }
    if (ret) uc_reg_read(e->uc, UC_X86_REG_EAX, ret);
    return 0;
}

/* --------------------------------------------------------------- lifecycle */
b32_emu *b32_open(const void *dll_bytes, size_t dll_len,
                  char *errbuf, size_t errlen) {
    b32_emu *e = (b32_emu *)calloc(1, sizeof *e);
    if (!e) {
        if (errbuf) snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }
    e->tls_next = 1;
    e->next_hwnd = 0x000A0001;
    e->heap_ptr = HEAP_BASE;
    e->wave_hwo = 0xBEEF0001;
    e->wave_rate = B32_SAMPLE_RATE;
    e->wave_bits = B32_BITS;
    e->wave_ch = B32_CHANNELS;
    e->speed = 1.0f;
    e->inflection = e->head_size = e->excitation = e->unvoiced = B32_VOICE_DEFAULT;
    e->pitch_pct = 100;
    e->phrase_pred = 1;
    /* On by default: the engine appends about 477 ms of silence to every
     * utterance, which a caller queueing the next one waits out for nothing. */
    e->tail_cap_ms = 60;

    uc_err r = uc_open(UC_ARCH_X86, UC_MODE_32, &e->uc);
    if (r != UC_ERR_OK) { seterr(e, "uc_open: %s", uc_strerror(r)); goto fail; }

    if (uc_mem_map(e->uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL) ||
        uc_mem_map(e->uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL) ||
        uc_mem_map(e->uc, SCRATCH, SCRATCH_SIZE, UC_PROT_ALL) ||
        uc_mem_map(e->uc, STUB_BASE, 0x1000, UC_PROT_ALL) ||
        uc_mem_map(e->uc, TEB_BASE, 0x1000, UC_PROT_ALL) ||
        uc_mem_map(e->uc, PEB_BASE, 0x1000, UC_PROT_ALL) ||
        uc_mem_map(e->uc, GDT_BASE, 0x1000, UC_PROT_ALL)) {
        seterr(e, "cannot map guest memory");
        goto fail;
    }
    uc_mem_write(e->uc, CMDLINE_ADDR, "bsthost\0", 8);
    uc_mem_write(e->uc, ENVSTR_ADDR, "\0\0", 2);
    uc_mem_write(e->uc, MODNAME_ADDR, "C:\\B32\\B32_TTS.DLL\0", 19);

    struct pe_info pi;
    memset(&pi, 0, sizeof pi);
    if (load_pe(e, (const uint8_t *)dll_bytes, dll_len, &pi) != 0) goto fail;
    e->entry = pi.entry;
    e->exports = pi.exports;
    e->nexports = pi.nexports;

    if (build_stubs(e) != 0) goto fail;
    if (setup_gdt_teb(e) != 0) { seterr(e, "cannot set up GDT/TEB"); goto fail; }

    uc_hook h1, h2;
    uc_hook_add(e->uc, &h1, UC_HOOK_CODE, (void *)hook_stub, e,
                STUB_BASE, STUB_BASE + 0x1000);
    uc_hook_add(e->uc, &h2, UC_HOOK_MEM_UNMAPPED, (void *)hook_badmem, e, 1, 0);

    e->ex.bstCreate    = export_va(&pi, "bstCreate");
    e->ex.bstClose     = export_va(&pi, "bstClose");
    e->ex.bstDestroy   = export_va(&pi, "bstDestroy");
    e->ex.bstSetParams = export_va(&pi, "bstSetParams");
    e->ex.bstRelBuf    = export_va(&pi, "bstRelBuf");
    e->ex.TtsWav       = export_va(&pi, "TtsWav");
    if (!e->ex.bstCreate || !e->ex.TtsWav || !e->ex.bstRelBuf || !e->ex.bstSetParams) {
        seterr(e, "required exports missing from DLL");
        goto fail;
    }
    w32(e, G_RELBUF, e->ex.bstRelBuf);

    /* CRT init */
    uint32_t args[3] = { IMAGE_BASE, 1 /* DLL_PROCESS_ATTACH */, 0 }, rv = 0;
    if (guest_call(e, e->entry, args, 3, 60ull * 1000000ull, &rv) != 0) goto fail;
    if (rv == 0) { seterr(e, "DllMain returned FALSE"); goto fail; }

    /* create the engine instance */
    w32(e, ARGS_ADDR, 0);
    uint32_t a1[1] = { ARGS_ADDR };
    if (guest_call(e, e->ex.bstCreate, a1, 1, 60ull * 1000000ull, &rv) != 0) goto fail;
    if (rv != 0) { seterr(e, "bstCreate failed: %u", rv); goto fail; }
    e->tts = r32(e, ARGS_ADDR);
    if (!e->tts) { seterr(e, "bstCreate produced a null handle"); goto fail; }

    /* Bit depth only: the caller sets rate and gain afterwards, so the
     * bstSetParams call order matches the Python reference harness exactly. */
    {
        uint32_t p[3] = { e->tts, BST_BIT_DEPTH_SETTING, 16 };
        if (guest_call(e, e->ex.bstSetParams, p, 3, 10ull * 1000000ull, NULL) != 0)
            goto fail;
    }
    return e;

fail:
    if (errbuf) snprintf(errbuf, errlen, "%s", e->err[0] ? e->err : "unknown error");
    if (e->uc) uc_close(e->uc);
    free(e->img);
    free(e);
    return NULL;
}

void b32_close(b32_emu *e) {
    if (!e) return;
    if (e->tts && e->ex.bstClose) {
        uint32_t a[1] = { e->tts };
        guest_call(e, e->ex.bstClose, a, 1, 10ull * 1000000ull, NULL);
    }
    if (e->ex.bstDestroy)
        guest_call(e, e->ex.bstDestroy, NULL, 0, 10ull * 1000000ull, NULL);
    if (e->uc) uc_close(e->uc);
    if (e->sonic) sonicDestroyStream(e->sonic);
    free(e->img);
    free(e->audio);
    free(e->hold);
    free(e->exports);
    free(e);
}

void b32_set_trace(b32_emu *e, int on) { if (e) e->trace = on; }
const char *b32_error(const b32_emu *e) { return e ? e->err : ""; }

void b32_set_rate(b32_emu *e, int rate) {
    /* Deliberately NOT bstSetParams(BST_RATE_SETTING): that only takes effect
     * for the first TtsWav call after bstCreate. Every utterance after it
     * reverts to normal speed no matter how often the parameter is re-set, so
     * an engine reused across utterances (like an Android TTS service) speaks
     * the first phrase at the requested rate and everything after it at 1x.
     * The ~r text code is honoured on every utterance, so rate is applied
     * there instead -- see b32_speak_voice. */
    if (e) e->rate = rate;
}
void b32_set_gain(b32_emu *e, int gain) {
    /* Unlike rate, this parameter does keep working: measured byte-identical on
     * the first and third utterance of one engine, and to the ~g text code. */
    uint32_t p[3] = { e->tts, BST_GAIN_SETTING, (uint32_t)gain };
    guest_call(e, e->ex.bstSetParams, p, 3, 10ull * 1000000ull, NULL);
}

void b32_set_voice_params(b32_emu *e, int inflection, int head_size,
                          int excitation, int unvoiced) {
    if (!e) return;
    e->inflection = inflection;
    e->head_size  = head_size;
    e->excitation = excitation;
    e->unvoiced   = unvoiced;
}

void b32_set_pitch(b32_emu *e, int percent) {
    if (e) e->pitch_pct = percent > 0 ? percent : 100;
}

void b32_set_parse_flags(b32_emu *e, unsigned flags) {
    if (e) e->parse_flags = flags;
}

void b32_set_phrase_prediction(b32_emu *e, int on) {
    if (e) e->phrase_pred = on ? 1 : 0;
}

void b32_set_pause_caps(b32_emu *e, int interior_ms, int tail_ms) {
    if (!e) return;
    e->pause_cap_ms = interior_ms > 0 ? interior_ms : 0;
    e->tail_cap_ms  = tail_ms > 0 ? tail_ms : 0;
}

void b32_set_speed(b32_emu *e, float speed) {
    if (!e) return;
    if (speed <= 0.0f) speed = 1.0f;
    e->speed = speed;
    if (speed == 1.0f) return;              /* no stream needed for passthrough */
    if (!e->sonic) {
        e->sonic = sonicCreateStream(B32_SAMPLE_RATE, B32_CHANNELS);
        if (!e->sonic) {
            seterr(e, "cannot create sonic stream");
            e->speed = 1.0f;
            return;
        }
    }
    sonicSetSpeed(e->sonic, speed);
}

float b32_get_speed(const b32_emu *e) { return e ? e->speed : 1.0f; }

int b32_speak(b32_emu *e, const char *text, b32_audio_cb cb, void *user) {
    if (!e || !text) return -1;
    size_t n = strlen(text);
    if (n + 1 > TEXT_MAX) { seterr(e, "text too long (%zu bytes)", n); return -1; }
    if (uc_mem_write(e->uc, TEXT_ADDR, text, n + 1) != UC_ERR_OK) {
        seterr(e, "cannot write text into guest memory");
        return -1;
    }
    e->cb = cb;
    e->cb_user = user;
    e->abort = 0;
    e->msg_head = e->msg_tail = 0;
    e->hold_len = e->hold_run = 0;   /* an aborted utterance may have left some */
    if (!cb) e->audio_len = 0;

    /* arg 2 is an opaque cookie the engine hands back as the waveOut callback
     * "window"; the Windows wrapper uses it to identify its own output. */
    uint32_t a[3] = { e->tts, 0x0000CAFEu, TEXT_ADDR };
    int rc = guest_call(e, e->ex.TtsWav, a, 3, 300ull * 1000000ull, NULL);
    if (rc == 0) flush_audio(e);       /* sonic holds a tail past the last write */
    e->cb = NULL;
    e->cb_user = NULL;
    return rc;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* An override, or the voice's own value when none was set. */
static int pick(int over, int voice_value) {
    return over == B32_VOICE_DEFAULT ? voice_value : over;
}

/* Bit in parse_flags -> the engine's ~nN option number. */
static const struct { unsigned bit; int n; } NFLAGS[] = {
    { B32_PARSE_LETTER_NAMES,  1 }, { B32_PARSE_DIGITS,       2 },
    { B32_PARSE_PUNCTUATION,   3 }, { B32_PARSE_WHITESPACE,   4 },
    { B32_PARSE_FULL_NUMBERS,  6 }, { B32_PARSE_UPPER_WORDS,  7 },
    { B32_PARSE_TIMES,         9 }, { B32_PARSE_ABBREV,      10 },
};

/* Every parameter the engine takes, in the one order that works. */
static int build_prefix(b32_emu *e, const b32_voice *v, char *out, size_t cap) {
    int hz = clampi(v->base_hz * e->pitch_pct / 100, 43, 600);
    /* ~f discards a ~h set before it, which is why inflection comes after the
     * baseline frequency here -- with the documented ordering the voice's own
     * pitch range was silently dropped. ~r is a percentage where lower is
     * faster, the opposite of this API, hence the negation; the caller's rate
     * replaces the voice's own rather than composing with it, so a request for
     * 300% means the same speed whichever voice is selected. */
    int k = snprintf(out, cap, "~v%d]~e%d]~u%d]~f%d]~h%d]~r%d]",
                     clampi(pick(e->head_size,  v->head_size),    0,   6),
                     clampi(pick(e->excitation, v->excitation),   1,   6),
                     clampi(pick(e->unvoiced,   v->unvoiced),   -70,  20),
                     hz,
                     clampi(pick(e->inflection, v->inflection), -300, 100),
                     -e->rate);
    if (k < 0 || (size_t)k >= cap) return -1;
    /* Text-parser options are sticky engine state, so each is stated outright
     * every utterance -- omitting one would leave a previous setting in force
     * instead of turning it off. All-off is byte-identical to sending none. */
    for (size_t i = 0; i < sizeof NFLAGS / sizeof NFLAGS[0]; i++) {
        int n = snprintf(out + k, cap - (size_t)k, "~n%d,%d]", NFLAGS[i].n,
                         (e->parse_flags & NFLAGS[i].bit) ? 1 : 0);
        if (n < 0 || (size_t)n >= cap - (size_t)k) return -1;
        k += n;
    }
    int n = snprintf(out + k, cap - (size_t)k, "~~2,%d]", e->phrase_pred ? 0 : 1);
    if (n < 0 || (size_t)n >= cap - (size_t)k) return -1;
    return k + n;
}

int b32_speak_voice(b32_emu *e, const b32_voice *v, const char *text,
                    b32_audio_cb cb, void *user) {
    if (!e || !text) return -1;
    if (!v) return b32_speak(e, text, cb, user);

    char pre[256];
    int plen = build_prefix(e, v, pre, sizeof pre);
    if (plen < 0) { seterr(e, "voice prefix does not fit"); return -1; }

    size_t n = (size_t)plen + strlen(text) + 1;
    char *buf = (char *)malloc(n);
    if (!buf) { seterr(e, "out of memory"); return -1; }
    snprintf(buf, n, "%s%s", pre, text);
    int rc = b32_speak(e, buf, cb, user);
    free(buf);
    return rc;
}

int16_t *b32_take_audio(b32_emu *e, size_t *bytes) {
    if (!e) return NULL;
    int16_t *p = e->audio;
    if (bytes) *bytes = e->audio_len;
    e->audio = NULL;
    e->audio_len = e->audio_cap = 0;
    return p;
}

/* ------------------------------------------------------------ WAV output */
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

int b32_write_wav(const char *path, const int16_t *pcm, size_t bytes,
                  unsigned rate) {
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, (uint32_t)(36 + bytes));
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);                      /* PCM            */
    put16(h + 22, B32_CHANNELS);
    put32(h + 24, rate);
    put32(h + 28, rate * B32_CHANNELS * (B32_BITS / 8));
    put16(h + 32, B32_CHANNELS * (B32_BITS / 8));
    put16(h + 34, B32_BITS);
    memcpy(h + 36, "data", 4);
    put32(h + 40, (uint32_t)bytes);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h &&
             (bytes == 0 || fwrite(pcm, 1, bytes, f) == bytes);
    fclose(f);
    return ok ? 0 : -1;
}
