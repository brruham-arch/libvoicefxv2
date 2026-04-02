/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * v2.2 - Fix echo & choppy: crossfade menggunakan ekor frame sebelumnya
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

static float g_pitch   = 1.0f;
static int   g_enabled = 0;

#define MAX_BUF  8192
#define XFADE    64      // crossfade zone samples

static short g_buf[MAX_BUF];

// --- FIX v2.2: simpan frame sebelumnya untuk crossfade ---
static short g_prev[MAX_BUF];
static int   g_prev_n = 0;

static inline short clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

// ============================================================
// DSP CALLBACK
// ============================================================
static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    if (!g_enabled || g_pitch == 1.0f) return;

    short* s16 = (short*)buf;
    int n = (int)(len / 2);
    if (n <= 0 || n > MAX_BUF) return;

    // Salin input ke working buffer
    memcpy(g_buf, s16, n * sizeof(short));

    float factor = g_pitch;

    for (int i = 0; i < n; i++) {
        float srcF = (float)i * factor;

        // Posisi dalam buffer dengan wrap
        float pos  = fmodf(srcF, (float)n);
        int   wrap = (int)(srcF / (float)n);

        // Interpolasi linear di posisi baca
        int   p0   = (int)pos;
        int   p1   = (p0 + 1 < n) ? p0 + 1 : p0;
        float frac = pos - p0;
        float val  = g_buf[p0] * (1.f - frac) + g_buf[p1] * frac;

        // Crossfade di titik wrap:
        // Gunakan EKOR frame SEBELUMNYA (g_prev), bukan bagian lain
        // dari frame sekarang. Ini menghilangkan echo dan patah-patah.
        if (wrap > 0 && pos < (float)XFADE && g_prev_n == n) {
            float alpha   = pos / (float)XFADE;           // 0.0 → 1.0
            int   pp      = n - XFADE + (int)pos;
            if (pp >= n) pp = n - 1;
            float prev_val = (float)g_prev[pp];
            // Fade dari ekor frame lama ke awal frame baru
            val = prev_val * (1.f - alpha) + val * alpha;
        }

        s16[i] = clamp16(val);
    }

    // Simpan frame ini untuk dipakai di pemanggilan berikutnya
    memcpy(g_prev, g_buf, n * sizeof(short));
    g_prev_n = n;
}

// ============================================================
// HOOK
// ============================================================
static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int)    = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)                = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)       = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                     = nullptr;

static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    g_recHandle = handle;
    if (pBASSChannelSetDSP)
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);
    logf("[VFX] RecordStart hooked, DSP dipasang");
    return handle;
}

// ============================================================
// PUBLIC API
// ============================================================
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
    static const char* info = "libvoicefx|2.2|VoiceFX prev-frame crossfade|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX] OnModPreLoad v2.2");
    // Reset state frame buffer
    memset(g_prev, 0, sizeof(g_prev));
    g_prev_n = 0;
}

void OnModLoad() {
    logf("[VFX] OnModLoad v2.2");

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

    logf("[VFX] OnModLoad SELESAI v2.2!");
}

} // extern "C"
