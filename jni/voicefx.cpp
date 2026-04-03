/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Pitch shift dengan ring buffer kontinyu antar frame
 * Read position bergerak dengan kecepatan pitch_factor
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>

#define LOG_TAG "libvoicefx"
#define LOGFILE "/storage/emulated/0/voicefx_log.txt"

static void logf(const char* msg) {
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HDSP;
typedef void (*DSPPROC)(HDSP, DWORD, void*, DWORD, void*);
typedef int  (*RECORDPROC)(HRECORD, const void*, DWORD, void*);

static float g_pitch   = 1.0f;
static int   g_enabled = 0;

// Ring buffer — tulis input kontinyu, baca dengan kecepatan pitch
#define RING_BITS 15
#define RING_SIZE (1 << RING_BITS)  // 32768
#define RING_MASK (RING_SIZE - 1)

static float g_ring[RING_SIZE];     // input dalam float
static int   g_wpos = 0;            // posisi tulis (integer)
static float g_rpos = 0.0f;         // posisi baca (float, sub-sample)

#define MAX_OUT 65536
static short g_out[MAX_OUT];

static inline float ring_lerp(float pos) {
    int   i0   = (int)pos & RING_MASK;
    int   i1   = (i0 + 1) & RING_MASK;
    float frac = pos - floorf(pos);
    return g_ring[i0] * (1.0f - frac) + g_ring[i1] * frac;
}

static inline short clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

static RECORDPROC g_origProc = nullptr;
static void*      g_origUser = nullptr;

static int recordProcHook(HRECORD handle, const void* buffer, DWORD length, void* user) {
    static int first = 0;
    if (!first) {
        first = 1;
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "[VFX] recordProc len=%u", length);
        logf(tmp);
    }

    const void* send_buf = buffer;

    if (g_enabled && g_pitch != 1.0f && buffer && length > 0) {
        const short* src = (const short*)buffer;
        int n = (int)(length / 2);

        if (n > 0 && n <= MAX_OUT) {
            // 1. Tulis n sample input ke ring buffer
            for (int i = 0; i < n; i++) {
                g_ring[(g_wpos + i) & RING_MASK] = (float)src[i];
            }
            g_wpos = (g_wpos + n) & RING_MASK;

            // 2. Pastikan rpos tidak terlalu jauh tertinggal
            // rpos harus dalam window [wpos - RING_SIZE/2, wpos]
            float wposF = (float)g_wpos;
            float diff  = wposF - g_rpos;
            if (diff < 0) diff += RING_SIZE;
            if (diff > RING_SIZE * 0.75f || diff < 0) {
                // Reset rpos ke n frame sebelum wpos
                g_rpos = (float)((g_wpos - n * 2 + RING_SIZE) & RING_MASK);
            }

            // 3. Baca n sample dari ring dengan kecepatan pitch_factor
            float factor = g_pitch;
            float rpos   = g_rpos;

            for (int i = 0; i < n; i++) {
                g_out[i] = clamp16(ring_lerp(rpos));
                rpos += factor;
                // Wrap dalam RING_SIZE
                if (rpos >= RING_SIZE) rpos -= RING_SIZE;
            }

            g_rpos = rpos;

            // Debug sekali
            static int log2 = 0;
            if (!log2) {
                log2 = 1;
                char tmp[128];
                snprintf(tmp, sizeof(tmp),
                    "[VFX] in[0]=%d out[0]=%d in[100]=%d out[100]=%d",
                    src[0], g_out[0], src[100], g_out[100]);
                logf(tmp);
            }

            send_buf = (const void*)g_out;
        }
    }

    if (g_origProc)
        return g_origProc(handle, send_buf, length, g_origUser);
    return 1;
}

static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)       = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                     = nullptr;

static HRECORD g_recHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    g_origProc = (RECORDPROC)proc;
    g_origUser = user;

    // Reset ring buffer
    memset(g_ring, 0, sizeof(g_ring));
    g_wpos = 0;
    g_rpos = 0.0f;

    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, (void*)recordProcHook, user);
    g_recHandle = handle;

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] hooked handle=%u freq=%u", handle, freq);
    logf(tmp);
    return handle;
}

static void  _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
    // Reset posisi saat pitch berubah
    g_rpos = (float)((g_wpos - 512 + RING_SIZE) & RING_MASK);
}
static void  _vc_enable(void)     { g_enabled = 1; }
static void  _vc_disable(void)    { g_enabled = 0; }
static int   _vc_is_enabled(void) { return g_enabled; }
static float _vc_get_pitch(void)  { return g_pitch; }

struct VcAPI {
    void  (*set_pitch)(float);
    void  (*enable)(void);
    void  (*disable)(void);
    int   (*is_enabled)(void);
    float (*get_pitch)(void);
};

extern "C" {

VcAPI vc_api = {
    _vc_set_pitch,
    _vc_enable,
    _vc_disable,
    _vc_is_enabled,
    _vc_get_pitch,
};

void* __GetModInfo() {
    static const char* info = "libvoicefx|3.0|VoiceFX ring buffer|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX] OnModPreLoad");
}

void OnModLoad() {
    logf("[VFX] OnModLoad");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf("[VFX] ERROR: libdobby"); return; }

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) { logf("[VFX] ERROR: Dobby sym"); return; }

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { logf("[VFX] ERROR: libBASS"); return; }

    void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addr) { logf("[VFX] ERROR: BASS_RecordStart"); return; }

    if (pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        logf("[VFX] ERROR: DobbyHook"); return;
    }

    g_pitch   = 1.0f;
    g_enabled = 0;

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX] OnModLoad SELESAI!");
}

} // extern "C"
