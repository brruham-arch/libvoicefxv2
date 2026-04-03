/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Hook: opus_encode
 * Algoritma: resample per-frame + crossfade 64 sample antar frame
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

typedef unsigned int DWORD;
typedef int          opus_int32;
typedef short        opus_int16;

static float g_pitch   = 1.0f;
static int   g_enabled = 0;

#define MAX_BUF  65536
#define XFADE    64

static opus_int16 g_in[MAX_BUF];
static opus_int16 g_out[MAX_BUF];
// Simpan XFADE sample akhir dari frame sebelumnya untuk crossfade
static float      g_prev[XFADE];
static int        g_have_prev = 0;

static inline opus_int16 clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (opus_int16)v;
}

typedef int (*opus_encode_t)(void*, const opus_int16*, int, unsigned char*, opus_int32);
static opus_encode_t orig_opus_encode = nullptr;

static int hook_opus_encode(void* st, const opus_int16* pcm, int frame_size,
                             unsigned char* data, opus_int32 max_data_bytes) {
    static int first = 0;
    if (!first) {
        first = 1;
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "[VFX] opus_encode frame_size=%d", frame_size);
        logf(tmp);
    }

    const opus_int16* send_pcm = pcm;

    if (g_enabled && g_pitch != 1.0f && pcm && frame_size > 0 && frame_size <= MAX_BUF) {

        // Copy input ke buffer terpisah
        memcpy(g_in, pcm, frame_size * sizeof(opus_int16));

        float factor = g_pitch;
        int   n      = frame_size;

        // Resample
        for (int i = 0; i < n; i++) {
            float srcF = i * factor;
            int   src0 = (int)srcF;
            int   src1 = src0 + 1;
            float frac = srcF - src0;

            if (src0 >= n) { g_out[i] = 0; continue; }
            if (src1 >= n)   src1 = n - 1;

            g_out[i] = clamp16(g_in[src0] * (1.f - frac) + g_in[src1] * frac);
        }

        // Crossfade XFADE sample pertama dengan akhir frame sebelumnya
        if (g_have_prev) {
            for (int i = 0; i < XFADE && i < n; i++) {
                float alpha = (float)i / XFADE;
                g_out[i] = clamp16(g_prev[i] * (1.f - alpha) + g_out[i] * alpha);
            }
        }

        // Simpan XFADE sample terakhir untuk frame berikutnya
        for (int i = 0; i < XFADE; i++)
            g_prev[i] = (float)g_out[n - XFADE + i];
        g_have_prev = 1;

        send_pcm = g_out;
    } else {
        // Jika disabled, reset prev agar tidak ada ghost saat ON lagi
        g_have_prev = 0;
    }

    return orig_opus_encode(st, send_pcm, frame_size, data, max_data_bytes);
}

static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch     = f;
    g_have_prev = 0;
}
static void  _vc_enable(void)     { g_enabled = 1; g_have_prev = 0; }
static void  _vc_disable(void)    { g_enabled = 0; g_have_prev = 0; }
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
    static const char* info = "libvoicefx|4.1|VoiceFX crossfade|brruham";
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

    void* addr = pDobbySymbolResolver("libsamp.so", "opus_encode");
    if (!addr) { logf("[VFX] ERROR: opus_encode"); return; }

    if (pDobbyHook(addr, (void*)hook_opus_encode, (void**)&orig_opus_encode) != 0) {
        logf("[VFX] ERROR: DobbyHook"); return;
    }

    g_pitch = 1.0f; g_enabled = 0; g_have_prev = 0;

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX] OnModLoad SELESAI!");
}

} // extern "C"
