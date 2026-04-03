/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Hook: opus_encode di libsamp.so — pitch shift sebelum encode
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

typedef unsigned int   DWORD;
typedef unsigned int   HRECORD;
typedef int            opus_int32;
typedef short          opus_int16;

static float g_pitch   = 1.0f;
static int   g_enabled = 0;

#define MAX_BUF 65536
static opus_int16 g_buf[MAX_BUF];  // output
static opus_int16 g_in[MAX_BUF];   // input copy

// Ring buffer untuk persistent read position
#define RING_BITS 15
#define RING_SIZE (1 << RING_BITS)  // 32768
#define RING_MASK (RING_SIZE - 1)

static float g_ring[RING_SIZE];
static int   g_wpos      = 0;
static float g_rpos      = 0.0f;
static int   g_frame_cnt = 0;

static inline float ring_lerp(float pos) {
    int   i0   = (int)pos & RING_MASK;
    int   i1   = (i0 + 1) & RING_MASK;
    float frac = pos - floorf(pos);
    return g_ring[i0] * (1.0f - frac) + g_ring[i1] * frac;
}

static inline opus_int16 clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (opus_int16)v;
}

// opus_encode original
typedef int (*opus_encode_t)(void*, const opus_int16*, int, unsigned char*, opus_int32);
static opus_encode_t orig_opus_encode = nullptr;

// Hook opus_encode
static int hook_opus_encode(void* st, const opus_int16* pcm, int frame_size,
                             unsigned char* data, opus_int32 max_data_bytes) {

    static int first = 0;
    if (!first) {
        first = 1;
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "[VFX] opus_encode! frame_size=%d", frame_size);
        logf(tmp);
    }

    const opus_int16* send_pcm = pcm;

    if (g_enabled && g_pitch != 1.0f && pcm && frame_size > 0 && frame_size <= MAX_BUF) {
        // Copy input ke g_in dulu (hindari baca dari buffer yang sudah dimodifikasi)
        memcpy(g_in, pcm, frame_size * sizeof(opus_int16));

        float factor = g_pitch;
        int   n      = frame_size;

        for (int i = 0; i < n; i++) {
            float srcF = i * factor;
            int   src0 = (int)srcF % n;
            int   src1 = (src0 + 1) % n;
            float frac = srcF - (int)srcF;
            g_buf[i] = clamp16(g_in[src0] * (1.f - frac) + g_in[src1] * frac);
        }

        send_pcm = g_buf;
    }

    return orig_opus_encode(st, send_pcm, frame_size, data, max_data_bytes);
}

// Dobby
static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

struct VcAPI {
    void  (*set_pitch)(float);
    void  (*enable)(void);
    void  (*disable)(void);
    int   (*is_enabled)(void);
    float (*get_pitch)(void);
};

static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
    // Reset ring saat pitch berubah
    g_frame_cnt = 0;
    g_rpos = (float)((g_wpos - 512 + RING_SIZE) & RING_MASK);
}
static void  _vc_enable(void)     { g_enabled = 1; g_frame_cnt = 0; }
static void  _vc_disable(void)    { g_enabled = 0; }
static int   _vc_is_enabled(void) { return g_enabled; }
static float _vc_get_pitch(void)  { return g_pitch; }

extern "C" {

VcAPI vc_api = {
    _vc_set_pitch,
    _vc_enable,
    _vc_disable,
    _vc_is_enabled,
    _vc_get_pitch,
};

void* __GetModInfo() {
    static const char* info = "libvoicefx|3.0|VoiceFX opus hook|brruham";
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

    // Hook opus_encode di libsamp.so
    void* addr = pDobbySymbolResolver("libsamp.so", "opus_encode");
    if (!addr) {
        logf("[VFX] ERROR: opus_encode tidak ditemukan di libsamp.so");
        return;
    }

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] opus_encode addr=%p", addr);
    logf(tmp);

    if (pDobbyHook(addr, (void*)hook_opus_encode, (void**)&orig_opus_encode) != 0) {
        logf("[VFX] ERROR: DobbyHook opus_encode gagal"); return;
    }

    g_pitch   = 1.0f;
    g_enabled = 0;
    memset(g_ring, 0, sizeof(g_ring));
    g_wpos = 0; g_rpos = 0.0f; g_frame_cnt = 0;

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX] OnModLoad SELESAI - hook opus_encode!");
}

} // extern "C"
