/**
 * voicefx.cpp - v3.3 ANTI-DUPLICATE (FINAL NO ECHO!)
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

static float g_pitch = 1.0f;
static int   g_enabled = 0;

// DUAL POS + ANTI-DUPLICATE
#define MAX_BUF        8192
#define RING_SIZE      (MAX_BUF * 16)
#define RING_MASK      (RING_SIZE - 1)
#define LATENCY_SAMPLES (MAX_BUF * 2)

static short g_ring[RING_SIZE];
static int64_t g_wPos = 0;
static double  g_playPos = 0.0;
static int     g_latency_filled = 0;

// ANTI-DUPLICATE
static int64_t  g_last_wPos = 0;
static int      g_frame_processed = 0;

static inline short clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

// FINAL DSP - NO DUPLICATES!
static void dspCallback(HDSP dsp, DWORD channel, void* buf, DWORD length, void* user) {
    static uint64_t frame_counter = 0;
    frame_counter++;

    // 🔥 CRITICAL: BLOCK DUPLICATE CALLS
    if (g_last_wPos == g_wPos && g_frame_processed) {
        return;  // SILENT BLOCK!
    }

    if (!g_enabled || fabs(g_pitch - 1.0f) < 0.01f) return;

    short* s16 = (short*)buf;
    int n = (int)(length / 2);
    if (n <= 0 || n > MAX_BUF) return;

    g_frame_processed = 1;
    g_last_wPos = g_wPos;

    // WRITE
    for (int i = 0; i < n; i++) {
        g_ring[(g_wPos + i) & RING_MASK] = s16[i];
    }
    int64_t new_wPos = g_wPos + n;

    // LATENCY
    if (!g_latency_filled) {
        g_playPos = (double)(new_wPos - LATENCY_SAMPLES);
        g_latency_filled = 1;
        g_wPos = new_wPos;
        return;
    }

    // RESAMPLE
    double pitch = g_pitch;
    for (int i = 0; i < n; i++) {
        if (g_playPos >= (double)new_wPos) {
            int64_t safe = new_wPos - LATENCY_SAMPLES;
            s16[i] = g_ring[safe & RING_MASK];
            g_playPos = (double)safe;
            continue;
        }

        if ((double)new_wPos - g_playPos > (double)(RING_SIZE * 0.9)) {
            g_playPos = (double)(new_wPos - LATENCY_SAMPLES);
            float fade = 0.3f + 0.7f * (float)i / n;
            int64_t p0 = (int64_t)g_playPos;
            s16[i] = clamp16((float)g_ring[p0 & RING_MASK] * fade);
            g_playPos += pitch;
            continue;
        }

        int64_t p0 = (int64_t)g_playPos;
        float frac = (float)(g_playPos - (double)p0);
        int64_t p1 = p0 + 1;
        if (p1 >= new_wPos) p1 = new_wPos - 1;
        
        float s0 = g_ring[p0 & RING_MASK];
        float s1 = g_ring[p1 & RING_MASK];
        s16[i] = clamp16(s0 * (1.f - frac) + s1 * frac);
        g_playPos += pitch;
    }
    
    g_wPos = new_wPos;
}

// HOOKS & API (sama persis seperti v3.2)
static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int)    = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)                = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)      = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                    = nullptr;

static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    g_recHandle = handle;
    
    if (handle && pBASSChannelSetDSP) {
        if (g_dspHandle) pBASSChannelRemoveDSP(handle, g_dspHandle);
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);
    }
    return handle;
}

static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
}

static void _vc_enable(void) {
    g_enabled = 1;
    g_latency_filled = 0;
    g_frame_processed = 0;
    logf("[VFX v3.3] ENABLED - ANTI-DUPLICATE!");
}

static void _vc_disable(void) {
    g_enabled = 0;
    g_frame_processed = 0;
    logf("[VFX] DISABLED");
}

static int _vc_is_enabled(void) { return g_enabled; }
static float _vc_get_pitch(void) { return g_pitch; }

struct VcAPI {
    void  (*set_pitch)(float);
    void  (*enable)(void);
    void  (*disable)(void);
    int   (*is_enabled)(void);
    float (*get_pitch)(void);
};

extern "C" {
VcAPI vc_api = { _vc_set_pitch, _vc_enable, _vc_disable, _vc_is_enabled, _vc_get_pitch };

void* __GetModInfo() {
    return (void*)"libvoicefx|3.3|ANTI-DUPLICATE|brruham";
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX v3.3] ANTI-DUPLICATE EDITION");
    memset(g_ring, 0, sizeof(g_ring));
    g_wPos = 0; g_playPos = 0.0; g_latency_filled = 0;
    g_last_wPos = 0; g_frame_processed = 0;
    g_pitch = 1.0f; g_enabled = 0;
}

void OnModLoad() {
    logf("[VFX v3.3] Loading ANTI-DUPLICATE...");
    
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) return;

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    pBASSChannelSetDSP = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    void* target = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (target && pDobbyHook(target, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) == 0) {
        FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
        if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }
        logf("[VFX v3.3] ANTI-DUPLICATE LOADED! ✅");
    }
}
}