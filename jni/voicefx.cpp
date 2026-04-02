/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Syarat AML: __GetModInfo, OnModPreLoad, OnModLoad (extern "C")
 * ABI: armeabi-v7a ONLY
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
typedef unsigned int  DWORD;
typedef unsigned int  HRECORD;
typedef unsigned int  HDSP;
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

static VoiceFX g_vfx = {};
static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int) = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)             = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)   = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                 = nullptr;

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

static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
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
// PUBLIC API (dipanggil Lua via dlsym)
// ============================================================
extern "C" {

void vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_vfx.pitch_factor = f;
}
void  vc_enable(void)     { g_vfx.enabled = 1; }
void  vc_disable(void)    { g_vfx.enabled = 0; }
int   vc_is_enabled(void) { return g_vfx.enabled; }
float vc_get_pitch(void)  { return g_vfx.pitch_factor; }

// ============================================================
// AML REQUIRED FUNCTIONS
// ============================================================
void* __GetModInfo() {
    // Return pointer ke string info mod (format bebas)
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

    // Load Dobby
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) {
        logf("[VFX] ERROR: libdobby.so gagal");
        LOGE("libdobby: %s", dlerror());
        return;
    }
    logf("[VFX] libdobby OK");

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) {
        logf("[VFX] ERROR: Dobby symbols tidak ditemukan");
        return;
    }
    logf("[VFX] Dobby symbols OK");

    // Load BASS
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) {
        logf("[VFX] ERROR: libBASS.so gagal");
        return;
    }
    logf("[VFX] libBASS OK");

    pBASSChannelSetDSP    = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addr) {
        logf("[VFX] ERROR: BASS_RecordStart tidak ditemukan");
        return;
    }

    int r = pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart);
    if (r != 0) {
        logf("[VFX] ERROR: DobbyHook gagal");
        return;
    }

    memset(&g_vfx, 0, sizeof(g_vfx));
    g_vfx.pitch_factor = 1.0f;

    logf("[VFX] OnModLoad SELESAI - siap!");
    LOGI("OnModLoad done");
}

} // extern "C"
