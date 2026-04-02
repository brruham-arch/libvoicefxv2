/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * v3.1 - Ring buffer resampler FIXED: smooth, no crackle, no echo
 *
 * FIXES:
 * - Fixed latency dengan LATENCY_SAMPLES tetap
 * - Guard logic lebih smooth + fade glitch
 * - Enable/disable tanpa reset ring (mulus)
 * - Bounds check double untuk interpolasi
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
// RING BUFFER - FIXED
// ============================================================
#define MAX_BUF       8192
#define RING_SIZE     (MAX_BUF * 16)  // 131072 samples
#define RING_MASK     (RING_SIZE - 1)
#define LATENCY_SAMPLES (MAX_BUF * 2) // 16384 samples (~170ms @48kHz)

static short g_ring[RING_SIZE];
static int64_t g_wPos = 0;
static double  g_rPos = 0.0;
static int     g_ring_ready = 0;

static inline short clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

// ============================================================
// DSP CALLBACK - FULLY FIXED
// ============================================================
static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    if (!g_enabled || fabs(g_pitch - 1.0f) < 0.01f) return;

    short* s16 = (short*)buf;
    int n = (int)(len / 2);
    if (n <= 0 || n > MAX_BUF) return;

    // ------------------------------------------------------
    // LANGKAH 1: TULIS INPUT KE RING BUFFER
    // ------------------------------------------------------
    int64_t old_wPos = g_wPos;
    for (int i = 0; i < n; i++) {
        g_ring[(g_wPos + i) & RING_MASK] = s16[i];
    }
    g_wPos += n;

    // Inisialisasi LATENCY tetap (hanya sekali)
    if (!g_ring_ready) {
        g_rPos = (double)(g_wPos - LATENCY_SAMPLES);
        g_ring_ready = 1;
        return;  // Skip output pertama (fill latency)
    }

    // ------------------------------------------------------
    // LANGKAH 2: BACA OUTPUT DENGAN PITCH
    // ------------------------------------------------------
    double pitch = (double)g_pitch;

    for (int i = 0; i < n; i++) {
        // UNDERRUN PROTECTION (rPos nyampe wPos)
        if (g_rPos >= (double)g_wPos) {
            int64_t safe_pos = g_wPos - LATENCY_SAMPLES / 2;
            if (safe_pos < 0) safe_pos = 0;
            s16[i] = g_ring[safe_pos & RING_MASK];
            g_rPos = (double)safe_pos;
            continue;
        }

        // OVERFLOW PROTECTION (rPos terlalu jauh)
        if ((double)g_wPos - g_rPos > (double)(RING_SIZE * 0.8)) {
            // Reset ke latency dengan fade-out glitch
            g_rPos = (double)(g_wPos - LATENCY_SAMPLES);
            float fade = 0.3f + 0.7f * (float)i / (float)n;  // fade in
            int64_t p0 = (int64_t)g_rPos;
            float val = (float)g_ring[p0 & RING_MASK] * fade;
            s16[i] = clamp16(val);
            g_rPos += pitch;
            continue;
        }

        // INTERPOLASI LINEAR (bounds-checked)
        int64_t p0 = (int64_t)g_rPos;
        double frac_d = g_rPos - (double)p0;
        float frac = (float)frac_d;
        
        int64_t p1 = p0 + 1;
        // Pastikan p1 tidak melebihi data yang ada
        if (p1 >= g_wPos) {
            p1 = g_wPos - 1;
            frac = 0.0f;  // clamp ke sample terakhir
        }
        
        float s0 = (float)g_ring[p0 & RING_MASK];
        float s1 = (float)g_ring[p1 & RING_MASK];
        float val = s0 * (1.f - frac) + s1 * frac;

        s16[i] = clamp16(val);
        g_rPos += pitch;
    }
}

// ============================================================
// HOOK FUNCTIONS
// ============================================================
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
        if (g_dspHandle) {
            pBASSChannelRemoveDSP(handle, g_dspHandle);  // cleanup lama
        }
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);
    }
    
    char msg[128];
    sprintf(msg, "[VFX] RecordStart hooked: handle=%u, DSP=%u", (unsigned)handle, (unsigned)g_dspHandle);
    logf(msg);
    
    return handle;
}

// ============================================================
// PUBLIC API - FIXED
// ============================================================
static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
}

static void _vc_enable(void) {
    g_enabled = 1;
    g_ring_ready = 0;  // Reset latency tanpa hapus ring
    logf("[VFX] ENABLED - smooth transition");
}

static void _vc_disable(void) {
    g_enabled = 0;
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

VcAPI vc_api = {
    _vc_set_pitch,
    _vc_enable,
    _vc_disable,
    _vc_is_enabled,
    _vc_get_pitch,
};

// Mod info
void* __GetModInfo() {
    static const char* info = "libvoicefx|3.1|VoiceFX FIXED smooth|brruham";
    return (void*)info;
}

// Preload
void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX v3.1] OnModPreLoad - FIXED version");
    memset(g_ring, 0, sizeof(g_ring));
    g_wPos = 0;
    g_rPos = 0.0;
    g_ring_ready = 0;
    g_pitch = 1.0f;
    g_enabled = 0;
}

// Main load
void OnModLoad() {
    logf("[VFX v3.1] OnModLoad - Loading hooks...");

    // Load Dobby
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { 
        logf("[VFX] FATAL: libdobby.so not found"); 
        return; 
    }

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    
    if (!pDobbySymbolResolver || !pDobbyHook) { 
        logf("[VFX] FATAL: Dobby symbols not found"); 
        return; 
    }

    // Load BASS
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { 
        logf("[VFX] FATAL: libBASS.so not found"); 
        return; 
    }

    pBASSChannelSetDSP = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    if (!pBASSChannelSetDSP || !pBASSChannelRemoveDSP) {
        logf("[VFX] FATAL: BASS DSP symbols not found");
        return;
    }

    // Hook BASS_RecordStart
    void* target = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!target) { 
        logf("[VFX] FATAL: BASS_RecordStart not found"); 
        return; 
    }

    if (pDobbyHook(target, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        logf("[VFX] FATAL: DobbyHook failed");
        return;
    }

    // Write API address
    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { 
        fprintf(af, "%lu\n", (unsigned long)&vc_api); 
        fclose(af); 
    }

    logf("[VFX v3.1] LOADED SUCCESSFULLY!");
    logf("Pitch range: 0.25x - 4.0x");
    logf("Latency: ~170ms, Buffer: 131k samples");
}

} // extern "C"