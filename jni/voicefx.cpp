/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Hook: opus_encode + ring buffer antar frame
 * Sisa frame tidak dibuang, disambung ke frame berikutnya
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

// Ring buffer input — tulis semua PCM masuk
#define RING_BITS 16
#define RING_SIZE (1 << RING_BITS)  // 65536
#define RING_MASK (RING_SIZE - 1)

static float g_ring[RING_SIZE];
static int   g_wpos = 0;       // posisi tulis (integer)
static float g_rpos = 0.0f;    // posisi baca (float, sub-sample)
static int   g_init = 0;       // apakah ring sudah diinisialisasi

#define MAX_OUT 65536
static opus_int16 g_out[MAX_OUT];

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

    if (g_enabled && g_pitch != 1.0f && pcm && frame_size > 0 && frame_size <= MAX_OUT) {

        // 1. Tulis frame baru ke ring buffer
        for (int i = 0; i < frame_size; i++)
            g_ring[(g_wpos + i) & RING_MASK] = (float)pcm[i];
        g_wpos = (g_wpos + frame_size) & RING_MASK;

        // 2. Inisialisasi rpos — mulai baca 1 frame sebelum wpos
        if (!g_init) {
            g_init = 1;
            g_rpos = (float)((g_wpos - frame_size + RING_SIZE) & RING_MASK);
        }

        // 3. Jaga rpos tidak terlalu jauh tertinggal atau mendahului
        float wposF = (float)g_wpos;
        float diff  = wposF - g_rpos;
        if (diff < 0) diff += RING_SIZE;
        // Kalau tertinggal > setengah ring, skip ke posisi aman
        if (diff > RING_SIZE * 0.5f) {
            g_rpos = (float)((g_wpos - frame_size + RING_SIZE) & RING_MASK);
        }

        // 4. Baca frame_size sample dengan kecepatan pitch_factor
        float factor = g_pitch;
        float rpos   = g_rpos;

        for (int i = 0; i < frame_size; i++) {
            g_out[i] = clamp16(ring_lerp(rpos));
            rpos    += factor;
            if (rpos >= RING_SIZE) rpos -= RING_SIZE;
        }

        // 5. Simpan rpos untuk frame berikutnya — TIDAK direset
        g_rpos = rpos;

        send_pcm = g_out;
    }

    return orig_opus_encode(st, send_pcm, frame_size, data, max_data_bytes);
}

static void* (*pDobbySymbolResolver)(const char*, const char*) = nullptr;
static int   (*pDobbyHook)(void*, void*, void**)               = nullptr;

static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
    g_init  = 0; // reset posisi saat pitch berubah
}
static void  _vc_enable(void)     { g_enabled = 1; g_init = 0; }
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
    static const char* info = "libvoicefx|4.0|VoiceFX ring buffer|brruham";
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

    g_pitch = 1.0f;
    g_enabled = 0;
    memset(g_ring, 0, sizeof(g_ring));
    g_wpos = 0; g_rpos = 0.0f; g_init = 0;

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX] OnModLoad SELESAI!");
}

} // extern "C"
