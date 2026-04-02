/**
 * voicefx.cpp - AML Voice FX Mod v3.2 NO ECHO EDITION
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

// RING BUFFER DUAL POSITION (NO ECHO!)
#define MAX_BUF        8192
#define RING_SIZE      (MAX_BUF * 16)
#define RING_MASK      (RING_SIZE - 1)
#define LATENCY_SAMPLES (MAX_BUF * 2)  // 16384 samples

static short g_ring[RING_SIZE];
static int64_t g_wPos = 0;           // WRITE pos (input)
static double  g_playPos = 0.0;      // PLAYBACK pos (output TERPISAH!)
static int     g_latency_filled = 0;

static inline short clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

// FIXED DSP - NO ECHO!
static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    if (!g_enabled || fabs(g_pitch - 1.0f) < 0.01f) return;

    short* s16 = (short*)buf;
    int n = (int)(len / 2);
    if (n <= 0 || n > MAX_BUF) return;

    // 1. WRITE INPUT
    for (int i = 0; i < n; i++) {
        g_ring[(g_wPos + i) & RING_MASK] = s16[i];
    }
    g_wPos += n;

    // 2. SETUP PLAYBACK (TERPISAH)
    if (!g_latency_filled) {
        g_playPos = (double)(g_wPos - LATENCY_SAMPLES);
        g_latency_filled = 1;
        return;
    }

    double pitch = g_pitch;
    double buffer_fill = (double)g_wPos - g_playPos;

    // 3. READ OUTPUT dari g_playPos
    for (int i = 0; i < n; i++) {
        if (g_playPos >= (double)g_wPos) {
            int64_t safe_pos = g_wPos - LATENCY_SAMPLES;
            if (safe_pos < 0) safe_pos = 0;
            s16[i] = g_ring[safe_pos & RING_MASK];
            g_playPos = (double)safe_pos;
            continue;
        }

        if (buffer_fill > (double)(RING_SIZE * 0.9)) {
            g_playPos = (double)(g_wPos - LATENCY_SAMPLES);
            float fade = 0.2f + 0.8f * (float)i / (float)n;
            int64_t p0 = (int64_t)g_playPos;
            float val = (float)g_ring[p0 & RING_MASK] * fade;
            s16[i] = clamp16(val);
            g_playPos += pitch;
            continue;
        }

        int64_t p0 = (int64_t)g_playPos;
        double frac_d = g_playPos - (double)p0;
        float frac = (float)frac_d;
        
        int64_t p1 = p0 + 1;
        if (p1 >= g_wPos) {
            p1 = g_wPos - 1;
            frac = 0.0f;
        }
        
        float s0 = (float)g_ring[p0 & RING_MASK];
        float s1 = (float)g_ring[p1 & RING_MASK];
        float val = s0 * (1.f - frac) + s1 * frac;

        s16[i] = clamp16(val);
        g_playPos += pitch;
    }
}

// HOOKS (sama seperti sebelumnya)
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
    
    char msg[128];
    sprintf(msg, "[VFX] RecordStart: handle=%u DSP=%u", (unsigned)handle, (unsigned)g_dspHandle);
    logf(msg);
    return handle;
}

// API
static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
}

static void _vc_enable(void) {
    g_enabled = 1;
    g_latency_filled = 0;
    logf("[VFX v3.2] ENABLED - NO ECHO!");
}

static void _vc_disable(void) {
    g_enabled = 0;
    g_latency_filled = 0;
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
    static const char* info = "libvoicefx|3.2|NO ECHO DUAL POS|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX v3.2] PreLoad - NO ECHO EDITION");
    memset(g_ring, 0, sizeof(g_ring));
    g_wPos = 0; g_playPos = 0.0; g_latency_filled = 0;
    g_pitch = 1.0f; g_enabled = 0;
}

void OnModLoad() {
    logf("[VFX v3.2] Loading NO ECHO hooks...");
    
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) return logf("[VFX] FATAL: no dobby");

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) return logf("[VFX] FATAL: no BASS");

    pBASSChannelSetDSP = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    void* target = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (target && pDobbyHook(target, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) == 0) {
        
        FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
        if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }
        
        logf("[VFX v3.2] NO ECHO LOADED! 🎉");
    }
}
}