/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Algoritma: OLA (Overlap-Add) dengan ring buffer kontinyu
 * Tidak ada discontinuity, tidak ada wrap artifacts
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
// OLA ENGINE
// Ring buffer menyimpan audio kontinyu antar frame
// synth_pos bergerak dengan kecepatan pitch_factor
// Output di-overlap-add dengan Hann window
// ============================================================

#define RING_BITS  14
#define RING_SIZE  (1 << RING_BITS)   // 16384 samples
#define RING_MASK  (RING_SIZE - 1)

#define FRAME_SIZE 256                // ukuran frame OLA
#define HOP_SIZE   128                // hop antar frame (50% overlap)

static float  g_pitch   = 1.0f;
static int    g_enabled = 0;

// Ring buffer input (float untuk akurasi interpolasi)
static float  g_ring[RING_SIZE];
static int    g_ring_write = 0;

// Synthesis position (float untuk sub-sample accuracy)
static float  g_synth_pos = 0.0f;

// Output overlap buffer
static float  g_overlap[FRAME_SIZE];
static int    g_overlap_valid = 0;

// Hann window precomputed
static float  g_hann[FRAME_SIZE];
static int    g_hann_ready = 0;

static void init_hann() {
    if (g_hann_ready) return;
    for (int i = 0; i < FRAME_SIZE; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FRAME_SIZE - 1)));
    g_hann_ready = 1;
}

static inline float ring_read(float pos) {
    int   i0   = (int)pos & RING_MASK;
    int   i1   = (i0 + 1) & RING_MASK;
    float frac = pos - floorf(pos);
    return g_ring[i0] * (1.0f - frac) + g_ring[i1] * frac;
}

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
    if (n <= 0 || n > RING_SIZE / 4) return;

    init_hann();

    // 1. Tulis input ke ring buffer (konversi ke float)
    for (int i = 0; i < n; i++) {
        g_ring[(g_ring_write + i) & RING_MASK] = (float)s16[i];
    }

    // Pastikan synth_pos tidak terlalu jauh tertinggal
    float available = (float)(g_ring_write - (int)g_synth_pos);
    if (available < 0) available += RING_SIZE;
    if (available > RING_SIZE * 0.75f) {
        g_synth_pos = (float)((g_ring_write - n * 2 + RING_SIZE) & RING_MASK);
    }

    g_ring_write = (g_ring_write + n) & RING_MASK;

    // 2. OLA synthesis
    // Output buffer (float untuk akumulasi overlap)
    static float out[4096];
    if (n > 4096) return;
    memset(out, 0, n * sizeof(float));

    float factor  = g_pitch;
    float syn_pos = g_synth_pos;

    // Proses per frame dengan hop
    int num_hops = (n + HOP_SIZE - 1) / HOP_SIZE;

    for (int h = 0; h < num_hops; h++) {
        int out_start = h * HOP_SIZE;

        // Baca FRAME_SIZE sample dari ring buffer pada posisi syn_pos
        for (int i = 0; i < FRAME_SIZE; i++) {
            int out_idx = out_start + i;
            if (out_idx >= n) break;

            float in_pos = syn_pos + (float)i;
            float sample = ring_read(in_pos) * g_hann[i];
            out[out_idx] += sample;
        }

        // Maju syn_pos dengan HOP_SIZE * factor
        syn_pos += HOP_SIZE * factor;
        syn_pos = fmodf(syn_pos, (float)RING_SIZE);
    }

    g_synth_pos = syn_pos;

    // 3. Normalisasi dan tulis ke output
    // Faktor normalisasi OLA dengan 50% overlap Hann = 0.5
    float norm = 2.0f;
    for (int i = 0; i < n; i++) {
        s16[i] = clamp16(out[i] * norm);
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

    // Reset engine
    memset(g_ring,    0, sizeof(g_ring));
    memset(g_overlap, 0, sizeof(g_overlap));
    g_ring_write    = 0;
    g_synth_pos     = 0.0f;
    g_overlap_valid = 0;

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
    // Reset overlap saat pitch berubah
    memset(g_overlap, 0, sizeof(g_overlap));
    g_overlap_valid = 0;
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
    static const char* info = "libvoicefx|2.0|VoiceFX OLA|brruham";
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
    init_hann();

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX] OnModLoad SELESAI - OLA engine siap!");
}

} // extern "C"
