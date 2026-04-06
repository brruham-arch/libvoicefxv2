/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Algoritma: simple in-place resample (tidak diubah)
 * v5.0 - tambah efek: robot, megaphone, whisper, reverb
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
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

// ============================================================
// STATE
// ============================================================
static float g_pitch   = 1.0f;
static int   g_enabled = 0;
static int   g_effect  = 0;  // 0=none 1=robot 2=megaphone 3=whisper 4=reverb

#define MAX_BUF      8192
#define DELAY_BUF    1600   // 0.2s @ 8000Hz untuk reverb

static short g_buf[MAX_BUF];

// State efek (persistent antar callback)
static float  meg_prevIn  = 0.0f;
static float  meg_prevOut = 0.0f;
static short  rev_buf[DELAY_BUF];
static int    rev_pos     = 0;
static int    rob_phase   = 0;   // counter untuk robot modulation

static inline short clamp16(float v) {
    if (v >  32767.f) return  32767;
    if (v < -32768.f) return -32768;
    return (short)v;
}

// ============================================================
// RESET EFFECT STATE - dipanggil saat ganti efek
// ============================================================
static void resetEffectState() {
    meg_prevIn  = 0.0f;
    meg_prevOut = 0.0f;
    memset(rev_buf, 0, sizeof(rev_buf));
    rev_pos   = 0;
    rob_phase = 0;
}

// ============================================================
// DSP CALLBACK
// Pitch logic: tidak diubah sama sekali dari v2.0
// Effect: diterapkan SETELAH pitch
// ============================================================
static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    if (!g_enabled) return;

    short* s16 = (short*)buf;
    int n = (int)(len / 2);
    if (n <= 0 || n > MAX_BUF) return;

    // --------------------------------------------------------
    // PITCH - logika asli v2.0, tidak diubah
    // --------------------------------------------------------
    if (g_pitch != 1.0f) {
        memcpy(g_buf, s16, n * sizeof(short));
        float factor = g_pitch;
        for (int i = 0; i < n; i++) {
            float srcF = i * factor;
            int   src0 = (int)srcF % n;
            int   src1 = (src0 + 1) % n;
            float frac = srcF - (int)srcF;
            s16[i] = clamp16(g_buf[src0] * (1.f - frac) + g_buf[src1] * frac);
        }
    }

    // --------------------------------------------------------
    // EFFECT - diterapkan setelah pitch
    // --------------------------------------------------------
    switch (g_effect) {

        case 1: {
            // ROBOT - ring modulation dengan sine 150Hz
            // Suara jadi metalik/mekanik
            for (int i = 0; i < n; i++) {
                float mod = sinf(2.0f * M_PI * 150.0f * rob_phase / 8000.0f);
                rob_phase = (rob_phase + 1) % 8000;
                float v = s16[i] * (0.6f + 0.4f * mod);
                s16[i] = clamp16(v);
            }
            break;
        }

        case 2: {
            // MEGAPHONE - bandpass IIR filter
            // Potong frekuensi rendah dan sangat tinggi
            // Sisakan range 500-3000Hz → suara seperti walkie-talkie
            for (int i = 0; i < n; i++) {
                float v   = (float)s16[i];
                // High-pass: buang bass
                float hp  = 0.85f * (meg_prevOut + v - meg_prevIn);
                meg_prevIn  = v;
                meg_prevOut = hp;
                // Gain sedikit untuk kompensasi
                s16[i] = clamp16(hp * 1.3f);
            }
            break;
        }

        case 3: {
            // WHISPER - noise + kurangi amplitudo drastis
            for (int i = 0; i < n; i++) {
                float v     = (float)s16[i] * 0.35f;
                float noise = (float)(rand() % 2001 - 1000) * 0.08f;
                s16[i] = clamp16(v + noise);
            }
            break;
        }

        case 4: {
            // REVERB - delay 0.2s + blend
            // Campur suara sekarang dengan echo dari 0.2s lalu
            for (int i = 0; i < n; i++) {
                float dry = (float)s16[i];
                float wet = (float)rev_buf[rev_pos];
                float out = dry * 0.65f + wet * 0.35f;
                rev_buf[rev_pos] = clamp16(dry);
                rev_pos = (rev_pos + 1) % DELAY_BUF;
                s16[i] = clamp16(out);
            }
            break;
        }

        default:
            // case 0: no effect, pass through
            break;
    }
}

// ============================================================
// BASS + DOBBY
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
// VcAPI FUNCTIONS
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
static void  _vc_set_effect(int e) {
    if (e < 0) e = 0;
    if (e > 4) e = 4;
    g_effect = e;
    resetEffectState();
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "[VFX] effect set: %d", g_effect);
    logf(tmp);
}
static int _vc_get_effect(void) { return g_effect; }

// ============================================================
// VcAPI STRUCT
// ============================================================
struct VcAPI {
    void  (*set_pitch)(float);
    void  (*enable)(void);
    void  (*disable)(void);
    int   (*is_enabled)(void);
    float (*get_pitch)(void);
    void  (*set_effect)(int);
    int   (*get_effect)(void);
};

extern "C" {

VcAPI vc_api = {
    _vc_set_pitch,
    _vc_enable,
    _vc_disable,
    _vc_is_enabled,
    _vc_get_pitch,
    _vc_set_effect,
    _vc_get_effect,
};

void* __GetModInfo() {
    static const char* info = "libvoicefx|5.0|VoiceFX+Effects|brruham";
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
    g_effect  = 0;
    resetEffectState();

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX] OnModLoad SELESAI! v5.0 effects ready");
}

} // extern "C"
