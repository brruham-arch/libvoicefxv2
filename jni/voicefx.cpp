/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Hook: intercept record proc + debug output vs input
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>

#define LOG_TAG "libvoicefx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
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

#define MAX_BUF 65536
static short g_buf[MAX_BUF];
static short g_out[MAX_BUF];

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
        if (n > 0 && n <= MAX_BUF) {
            memcpy(g_buf, src, n * sizeof(short));

            float factor = g_pitch;
            for (int i = 0; i < n; i++) {
                float srcF = i * factor;
                int   src0 = (int)srcF % n;
                int   src1 = (src0 + 1) % n;
                float frac = srcF - (int)srcF;
                g_out[i] = clamp16(g_buf[src0] * (1.f - frac) + g_buf[src1] * frac);
            }

            // Debug: bandingkan sample pertama
            static int log2 = 0;
            if (!log2) {
                log2 = 1;
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "[VFX] in[0]=%d in[1]=%d out[0]=%d out[1]=%d n=%d pitch=%.2f",
                    g_buf[0], g_buf[1], g_out[0], g_out[1], n, g_pitch);
                logf(tmp);
            }

            send_buf = (const void*)g_out;
        }
    }

    if (g_origProc)
        return g_origProc(handle, send_buf, length, g_origUser);
    return 1;
}

static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int)    = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)                = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)       = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                     = nullptr;

static HRECORD g_recHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    g_origProc = (RECORDPROC)proc;
    g_origUser = user;

    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, (void*)recordProcHook, user);
    g_recHandle = handle;

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "[VFX] hooked! handle=%u freq=%u", handle, freq);
    logf(tmp);
    return handle;
}

static void  _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
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
    static const char* info = "libvoicefx|2.0|VoiceFX|brruham";
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

    pBASSChannelSetDSP    = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

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
