/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Syarat AML: __GetModInfo, OnModPreLoad, OnModLoad (extern "C")
 * ABI: armeabi-v7a ONLY
 *
 * Trick: export struct berisi semua function pointer
 * Lua baca 1 simbol "vc_api" lalu dapat semua fungsi
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>

#define LOG_TAG  "libvoicefx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGFILE "/storage/emulated/0/voicefx_log.txt"

static void logf(const char* msg) {
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// ============================================================
// TYPES
// ============================================================
typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HDSP;
typedef void (*DSPPROC)(HDSP, DWORD, void*, DWORD, void*);

// ============================================================
// AUDIO ENGINE
// ============================================================
#define MAX_SAMPLES  4096
#define OVERLAP_SIZE 256
#define RING_SIZE    8192

struct VoiceFX {
    float  pitch_factor;
    int    enabled;
    int    sample_rate;
    int    channels;
    short  ring[RING_SIZE];
    int    ring_write;
    float  synth_pos;
    float  overlap[OVERLAP_SIZE];
};

static VoiceFX g_vfx     = {};
static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int)     = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)                 = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*)  = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)        = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                      = nullptr;

static inline float hann_w(int i, int n) {
    return 0.5f * (1.0f - cosf(6.28318f * i / (n - 1)));
}
static inline short ring_get(int pos) {
    return g_vfx.ring[pos & (RING_SIZE - 1)];
}
static inline short clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

static int dsp_call_count = 0;
static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    dsp_call_count++;
    if (dsp_call_count == 1) logf("[VFX] dspCallback dipanggil pertama!");
    if (dsp_call_count % 100 == 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "[VFX] dsp calls: %d", dsp_call_count);
        logf(tmp);
    }
    if (!g_vfx.enabled || g_vfx.pitch_factor == 1.0f) return;
    short* s16 = (short*)buf;
    int n = (int)(len / 2);
    if (n <= 0 || n > MAX_SAMPLES) return;

    int base = g_vfx.ring_write;
    for (int i = 0; i < n; i++)
        g_vfx.ring[(base + i) & (RING_SIZE-1)] = s16[i];
    g_vfx.ring_write = base + n;

    if ((g_vfx.ring_write - (int)g_vfx.synth_pos) > RING_SIZE - 256)
        g_vfx.synth_pos = (float)(g_vfx.ring_write - n);

    float factor = g_vfx.pitch_factor;
    float pos    = g_vfx.synth_pos;

    for (int i = 0; i < n; i++) {
        int   p0   = (int)pos;
        float frac = pos - p0;
        float val  = ring_get(p0) * (1.f - frac) + ring_get(p0+1) * frac;
        if (i < OVERLAP_SIZE) {
            float w = hann_w(i, OVERLAP_SIZE * 2);
            val = val * w + g_vfx.overlap[i] * (1.f - w);
        }
        s16[i] = clamp16(val);
        pos += factor;
    }

    float sp = pos - OVERLAP_SIZE;
    for (int i = 0; i < OVERLAP_SIZE; i++) {
        int   p0   = (int)sp;
        float frac = sp - p0;
        g_vfx.overlap[i] = ring_get(p0) * (1.f - frac) + ring_get(p0+1) * frac;
        sp += factor;
    }
    g_vfx.synth_pos = pos;
}

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    g_recHandle = handle;

    memset(&g_vfx, 0, sizeof(g_vfx));
    g_vfx.pitch_factor = 1.0f;
    g_vfx.sample_rate  = (int)freq;
    g_vfx.channels     = (int)chans;

    if (pBASSChannelSetDSP)
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);

    logf("[VFX] RecordStart hooked, DSP dipasang");
    LOGI("RecordStart hooked handle=%u freq=%u", handle, freq);
    return handle;
}

// ============================================================
// INTERNAL FUNCTIONS
// ============================================================
static void  _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_vfx.pitch_factor = f;
}
static void  _vc_enable(void)     { g_vfx.enabled = 1; }
static void  _vc_disable(void)    { g_vfx.enabled = 0; }
static int   _vc_is_enabled(void) { return g_vfx.enabled; }
static float _vc_get_pitch(void)  { return g_vfx.pitch_factor; }

// ============================================================
// EXPORTED API STRUCT
// Lua baca pointer struct ini, lalu panggil tiap fungsi
// ============================================================
struct VcAPI {
    void  (*set_pitch)(float);
    void  (*enable)(void);
    void  (*disable)(void);
    int   (*is_enabled)(void);
    float (*get_pitch)(void);
};

extern "C" {

// Struct ini yang dibaca Lua — 1 simbol, semua fungsi
VcAPI vc_api = {
    _vc_set_pitch,
    _vc_enable,
    _vc_disable,
    _vc_is_enabled,
    _vc_get_pitch,
};

// ============================================================
// AML REQUIRED FUNCTIONS
// ============================================================
void* __GetModInfo() {
    static const char* info = "libvoicefx|1.0|VoiceFX for SAMP|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX] OnModPreLoad dipanggil");
    LOGI("OnModPreLoad");
}

void OnModLoad() {
    logf("[VFX] OnModLoad dipanggil");
    LOGI("OnModLoad");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf("[VFX] ERROR: libdobby.so gagal"); return; }
    logf("[VFX] libdobby OK");

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) {
        logf("[VFX] ERROR: Dobby symbols tidak ditemukan"); return;
    }
    logf("[VFX] Dobby symbols OK");

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { logf("[VFX] ERROR: libBASS.so gagal"); return; }
    logf("[VFX] libBASS OK");

    pBASSChannelSetDSP    = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addr) { logf("[VFX] ERROR: BASS_RecordStart tidak ditemukan"); return; }

    int r = pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart);
    if (r != 0) { logf("[VFX] ERROR: DobbyHook gagal"); return; }

    memset(&g_vfx, 0, sizeof(g_vfx));
    g_vfx.pitch_factor = 1.0f;

    logf("[VFX] OnModLoad SELESAI - siap!");

    // Tulis alamat vc_api ke file untuk Lua
    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) {
        fprintf(af, "%lu\n", (unsigned long)&vc_api);
        fclose(af);
        logf("[VFX] vc_api addr ditulis ke voicefx_addr.txt");
    }
    LOGI("OnModLoad done");
}

} // extern "C"

// TAMBAHAN: tulis alamat vc_api ke file agar Lua bisa baca
// Dipanggil di akhir OnModLoad
