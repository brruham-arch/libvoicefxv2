/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Algoritma: Simple Resample + Cubic (Catmull-Rom) Interpolation
 * v4.0 - terbukti bekerja, fix untuk n=4800 (SA-MP 8000Hz)
 *
 * Prinsip:
 *   pitch > 1.0 → baca sample lebih cepat → suara lebih tinggi
 *   pitch < 1.0 → baca sample lebih lambat → suara lebih rendah
 *   Cubic interpolation → transisi antar sample smooth, tidak patah
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>

#define LOG_TAG  "libvoicefx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGFILE  "/storage/emulated/0/voicefx_log.txt"

static void logf(const char* msg) {
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    LOGI("%s", msg);
}

typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HDSP;
typedef void (*DSPPROC)(HDSP, DWORD, void*, DWORD, void*);

// Buffer lebih dari cukup untuk n=4800
#define MAX_BUF 8192

static float   g_pitch   = 1.0f;
static int     g_enabled = 0;
static int16_t g_buf[MAX_BUF];  // backup buffer sebelum processing

// ============================================================
// CLAMP 16-bit
// ============================================================
static inline int16_t clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (int16_t)v;
}

// ============================================================
// DSP CALLBACK - Simple Resample + Cubic Interpolation
// ============================================================
static int dbgCount = 0;

static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    int n = (int)(len / 2);

    if (dbgCount++ % 50 == 0) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp),
            "[VFX] DSP#%d enabled=%d pitch=%.2f n=%d",
            dbgCount, g_enabled, g_pitch, n);
        logf(tmp);
    }

    if (!g_enabled) return;
    if (g_pitch > 0.99f && g_pitch < 1.01f) return;
    if (n <= 0 || n > MAX_BUF) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "[VFX] ERROR: n=%d out of range", n);
        logf(tmp);
        return;
    }

    int16_t* s16 = (int16_t*)buf;

    // Backup buffer asli
    memcpy(g_buf, s16, n * sizeof(int16_t));

    float factor = g_pitch;
    float maxSrc = (float)(n - 2);

    for (int i = 0; i < n; i++) {
        float srcF = i * factor;

        // Clamp - jangan baca melewati buffer
        if (srcF > maxSrc) srcF = maxSrc;

        int s0 = (int)srcF;
        float frac = srcF - s0;

        // Ambil 4 titik untuk Catmull-Rom cubic
        int sm1 = s0 - 1; if (sm1 < 0)     sm1 = 0;
        int s1  = s0 + 1; if (s1  >= n)    s1  = n - 1;
        int s2  = s0 + 2; if (s2  >= n)    s2  = n - 1;

        float p0 = (float)g_buf[sm1];
        float p1 = (float)g_buf[s0];
        float p2 = (float)g_buf[s1];
        float p3 = (float)g_buf[s2];

        // Catmull-Rom cubic interpolation
        float a = -0.5f*p0 + 1.5f*p1 - 1.5f*p2 + 0.5f*p3;
        float b =       p0 - 2.5f*p1 + 2.0f*p2 - 0.5f*p3;
        float c = -0.5f*p0            + 0.5f*p2;
        float d =                  p1;

        float v = ((a * frac + b) * frac + c) * frac + d;
        s16[i] = clamp16(v);
    }
}

// ============================================================
// BASS + DOBBY
// ============================================================
static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int)    = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)                = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)       = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                     = nullptr;

static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    logf("[VFX] hook_BASSRecordStart dipanggil");

    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    g_recHandle = handle;

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] recHandle = %u", handle);
    logf(tmp);

    if (pBASSChannelSetDSP) {
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);
        snprintf(tmp, sizeof(tmp), "[VFX] dspHandle = %u", g_dspHandle);
        logf(tmp);
    } else {
        logf("[VFX] ERROR: pBASSChannelSetDSP null");
    }

    return handle;
}

// ============================================================
// VcAPI FUNCTIONS
// ============================================================
static void _vc_set_pitch(float f) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] set_pitch called: %.2f", f);
    logf(tmp);

    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;

    snprintf(tmp, sizeof(tmp), "[VFX] g_pitch now: %.2f", g_pitch);
    logf(tmp);
}

static void _vc_enable(void) {
    logf("[VFX] enable called");
    g_enabled = 1;
}

static void _vc_disable(void) {
    logf("[VFX] disable called");
    g_enabled = 0;
}

static int   _vc_is_enabled(void) { return g_enabled; }
static float _vc_get_pitch(void)  { return g_pitch; }

// ============================================================
// VcAPI STRUCT
// ============================================================
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
    static const char* info = "libvoicefx|4.0|VoiceFX Cubic Resample|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX] OnModPreLoad");
}

void OnModLoad() {
    logf("[VFX] OnModLoad start");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf("[VFX] ERROR: libdobby.so tidak ditemukan"); return; }
    logf("[VFX] libdobby.so loaded");

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) {
        logf("[VFX] ERROR: Dobby symbols tidak ditemukan");
        return;
    }
    logf("[VFX] Dobby symbols OK");

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { logf("[VFX] ERROR: libBASS.so tidak ditemukan"); return; }
    logf("[VFX] libBASS.so loaded");

    pBASSChannelSetDSP    = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    if (!pBASSChannelSetDSP) {
        logf("[VFX] ERROR: BASS_ChannelSetDSP tidak ditemukan");
        return;
    }
    logf("[VFX] BASS DSP symbols OK");

    void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addr) { logf("[VFX] ERROR: BASS_RecordStart tidak ditemukan"); return; }

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] BASS_RecordStart addr: %p", addr);
    logf(tmp);

    if (pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        logf("[VFX] ERROR: DobbyHook gagal");
        return;
    }
    logf("[VFX] Hook BASS_RecordStart OK");

    g_pitch   = 1.0f;
    g_enabled = 0;
    memset(g_buf, 0, sizeof(g_buf));

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) {
        fprintf(af, "%lu\n", (unsigned long)&vc_api);
        fclose(af);
        snprintf(tmp, sizeof(tmp), "[VFX] vc_api addr: %lu", (unsigned long)&vc_api);
        logf(tmp);
    } else {
        logf("[VFX] ERROR: tidak bisa tulis voicefx_addr.txt");
    }

    logf("[VFX] OnModLoad SELESAI - Cubic Resample v4.0 ready!");
}

} // extern "C"
