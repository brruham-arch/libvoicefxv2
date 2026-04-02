/**
 * voicefx.cpp - AML Voice FX Mod (WSOLA)
 * WSOLA real-time pitch shifting (stabil, minim glitch)
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>

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

// ============================================================
// WSOLA CONFIG
// ============================================================

#define RING_BITS   14
#define RING_SIZE   (1 << RING_BITS)
#define RING_MASK   (RING_SIZE - 1)

#define FRAME_SIZE  512
#define HOP_SIZE    128
#define SEARCH_SIZE 96

static float g_pitch   = 1.0f;
static int   g_enabled = 0;

// ring buffer
static float g_ring[RING_SIZE];
static int   g_ring_write = 0;

// synthesis position
static float g_synth_pos = 0.0f;

// prev frame for WSOLA matching
static float g_prev_frame[FRAME_SIZE];
static int   g_prev_valid = 0;

// window
static float g_hann[FRAME_SIZE];
static int   g_hann_ready = 0;

static void init_hann() {
    if (g_hann_ready) return;
    for (int i = 0; i < FRAME_SIZE; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FRAME_SIZE - 1)));
    g_hann_ready = 1;
}

static inline float ring_read(float pos) {
    int i0 = ((int)pos) & RING_MASK;
    int i1 = (i0 + 1) & RING_MASK;
    float frac = pos - floorf(pos);
    return g_ring[i0] * (1.0f - frac) + g_ring[i1] * frac;
}

static inline short clamp16(float v) {
    if (v > 32767.f) return 32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

// ============================================================
// WSOLA SEARCH
// ============================================================

static int find_best_offset(float base_pos)
{
    int best_offset = 0;
    float best_corr = -1e30f;

    for (int off = -SEARCH_SIZE; off <= SEARCH_SIZE; off++)
    {
        float corr = 0.0f;

        for (int i = 0; i < FRAME_SIZE; i++)
        {
            float a = g_prev_frame[i];
            float b = ring_read(base_pos + off + i);
            corr += a * b;
        }

        if (corr > best_corr)
        {
            best_corr = corr;
            best_offset = off;
        }
    }

    return best_offset;
}

// ============================================================
// DSP CALLBACK (WSOLA)
// ============================================================

static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*)
{
    if (!g_enabled || g_pitch == 1.0f) return;

    short* s16 = (short*)buf;
    int n = (int)(len / 2);
    if (n <= 0 || n > 4096) return;

    init_hann();

    // input → ring
    for (int i = 0; i < n; i++)
        g_ring[(g_ring_write + i) & RING_MASK] = (float)s16[i];

    g_ring_write = (g_ring_write + n) & RING_MASK;

    static float out[4096];
    static float norm[4096];

    memset(out,  0, n * sizeof(float));
    memset(norm, 0, n * sizeof(float));

    float syn_pos = g_synth_pos;
    float factor  = g_pitch;

    int hops = (n + HOP_SIZE - 1) / HOP_SIZE;

    for (int h = 0; h < hops; h++)
    {
        int out_start = h * HOP_SIZE;

        float base_pos = syn_pos;
        int best_off = 0;

        if (g_prev_valid)
            best_off = find_best_offset(base_pos);

        float final_pos = base_pos + best_off;

        // overlap-add
        for (int i = 0; i < FRAME_SIZE; i++)
        {
            int out_idx = out_start + i;
            if (out_idx >= n) break;

            float sample = ring_read(final_pos + i) * g_hann[i];

            out[out_idx]  += sample;
            norm[out_idx] += g_hann[i];
        }

        // simpan frame
        for (int i = 0; i < FRAME_SIZE; i++)
            g_prev_frame[i] = ring_read(final_pos + i);

        g_prev_valid = 1;

        syn_pos += HOP_SIZE * factor;
        syn_pos = fmodf(syn_pos, (float)RING_SIZE);
    }

    g_synth_pos = syn_pos;

    // normalisasi
    for (int i = 0; i < n; i++)
    {
        float w = norm[i];
        if (w < 1e-6f) w = 1.0f;
        s16[i] = clamp16(out[i] / w);
    }
}

// ============================================================
// HOOK
// ============================================================

static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int)    = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)                = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*) = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)      = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                    = nullptr;

static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user)
{
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    g_recHandle = handle;

    memset(g_ring, 0, sizeof(g_ring));
    memset(g_prev_frame, 0, sizeof(g_prev_frame));

    g_ring_write = 0;
    g_synth_pos  = 0.0f;
    g_prev_valid = 0;

    if (pBASSChannelSetDSP)
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);

    logf("[VFX] WSOLA DSP attached");
    return handle;
}

// ============================================================
// API
// ============================================================

static void  _vc_set_pitch(float f) {
    if (f < 0.3f) f = 0.3f;
    if (f > 3.0f) f = 3.0f;
    g_pitch = f;
    g_prev_valid = 0;
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
    static const char* info = "libvoicefx|3.0|VoiceFX WSOLA|brruham";
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
    if (!pDobbySymbolResolver || !pDobbyHook) { logf("[VFX] ERROR: dobby"); return; }

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { logf("[VFX] ERROR: BASS"); return; }

    pBASSChannelSetDSP    = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addr) { logf("[VFX] ERROR: RecordStart"); return; }

    if (pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        logf("[VFX] ERROR: hook"); return;
    }

    g_pitch = 1.0f;
    g_enabled = 0;
    init_hann();

    FILE* f = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (f) { fprintf(f, "%lu\n", (unsigned long)&vc_api); fclose(f); }

    logf("[VFX] WSOLA READY");
}

}