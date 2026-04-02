/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * v3.0 - Ring buffer resampler: tidak ada wrap per-frame, tidak ada echo
 *
 * Cara kerja:
 *   - Semua input sample ditulis ke ring buffer secara kontinu (wPos++)
 *   - Output dibaca dari ring buffer dengan kecepatan g_pitch (rPos += pitch)
 *   - Tidak ada batas/wrap per frame → transisi antar frame mulus
 *   - Tidak ada dua sumber dicampur di waktu bersamaan → tidak ada echo
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

// ============================================================
// RING BUFFER
// Ukuran = 16x MAX_BUF (power-of-2 agar modulo bisa pakai AND)
// ============================================================
#define MAX_BUF    8192
#define RING_SIZE  (MAX_BUF * 16)   // 131072 samples
#define RING_MASK  (RING_SIZE - 1)  // untuk modulo cepat

static short g_ring[RING_SIZE];

static int64_t g_wPos       = 0;
static double  g_rPos       = 0.0;
static int     g_ring_ready = 0;

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

    // -----------------------------------------------------------
    // LANGKAH 1: Tulis n sample input ke ring buffer
    // -----------------------------------------------------------
    for (int i = 0; i < n; i++) {
        g_ring[(g_wPos + i) & RING_MASK] = s16[i];
    }
    g_wPos += n;

    // Inisialisasi rPos satu frame di belakang wPos
    if (!g_ring_ready) {
        g_rPos       = (double)(g_wPos - n);
        g_ring_ready = 1;
    }

    // -----------------------------------------------------------
    // LANGKAH 2: Baca n sample output dengan kecepatan pitch
    // -----------------------------------------------------------
    double pitch = (double)g_pitch;

    for (int i = 0; i < n; i++) {

        // Guard underrun: rPos mengejar wPos (pitch terlalu tinggi)
        if (g_rPos + 1.0 >= (double)g_wPos) {
            s16[i] = g_ring[(g_wPos - 1) & RING_MASK];
            g_rPos  = (double)(g_wPos - 1);
            continue;
        }

        // Guard overflow: rPos terlalu jauh di belakang
        if ((double)g_wPos - g_rPos > (double)(RING_SIZE - n)) {
            g_rPos = (double)(g_wPos - n);
        }

        // Interpolasi linear
        int64_t p0   = (int64_t)g_rPos;
        float   frac = (float)(g_rPos - (double)p0);
        float   val  = g_ring[ p0      & RING_MASK] * (1.f - frac)
                     + g_ring[(p0 + 1) & RING_MASK] * frac;

        s16[i]  = clamp16(val);
        g_rPos += pitch;
    }
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
static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
}
static void  _vc_enable(void) {
    // Reset ring setiap enable agar tidak ada sisa data lama
    memset(g_ring, 0, sizeof(g_ring));
    g_wPos       = 0;
    g_rPos       = 0.0;
    g_ring_ready = 0;
    g_enabled    = 1;
}
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
    static const char* info = "libvoicefx|3.0|VoiceFX ring-buffer|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX] OnModPreLoad v3.0");
    memset(g_ring, 0, sizeof(g_ring));
    g_wPos       = 0;
    g_rPos       = 0.0;
    g_ring_ready = 0;
}

void OnModLoad() {
    logf("[VFX] OnModLoad v3.0");

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

    logf("[VFX] OnModLoad SELESAI v3.0!");
}

} // extern "C"
