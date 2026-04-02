/**

* voicefx.cpp - AML Voice FX Mod (HYBRID FIX BUILD)
* Sudah fix error NDK + hybrid pitch changer
  */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_TAG "libvoicefx"
#define LOGFILE "/storage/emulated/0/voicefx_log.txt"

static void logf(const char* msg) {
FILE* f = fopen(LOGFILE, "a");
if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HDSP;



// ============================================================
// CONFIG
// ============================================================

#define RING_BITS  14
#define RING_SIZE  (1 << RING_BITS)
#define RING_MASK  (RING_SIZE - 1)

static float g_pitch   = 1.0f;
static int   g_enabled = 0;

static float g_ring[RING_SIZE];
static int   g_ring_write = 0;
static float g_read_pos = 0.0f;
static float g_last_sample = 0.0f;

// ============================================================

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
// DSP
// ============================================================

static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*)
{
short* s16 = (short*)buf;
int n = (int)(len / 2);
if (n <= 0 || n > 4096) return;

// simpan input ke ring
for (int i = 0; i < n; i++)
    g_ring[(g_ring_write + i) & RING_MASK] = (float)s16[i];

g_ring_write = (g_ring_write + n) & RING_MASK;

if (!g_enabled) return;

float pos = g_read_pos;
float pitch = g_pitch;

for (int i = 0; i < n; i++)
{
    float s = ring_read(pos);

    // smoothing ringan
    float out = (s * 0.85f) + (g_last_sample * 0.15f);
    g_last_sample = out;

    s16[i] = clamp16(out);

    pos += pitch;
    if (pos >= RING_SIZE) pos -= RING_SIZE;
}

g_read_pos = pos;

}

// ============================================================
// HOOK (FIX POINTER)
// ============================================================

static HDSP (pBASSChannelSetDSP)(HRECORD, DSPPROC, void, int) = nullptr;
static int  (pBASSChannelRemoveDSP)(HRECORD, HDSP) = nullptr;
static HRECORD (orig_BASSRecordStart)(DWORD, DWORD, DWORD, void, void) = nullptr;

static void* (pDobbySymbolResolver)(const char, const char*) = nullptr;
static int   (pDobbyHook)(void, void*, void**) = nullptr;

static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user)
{
HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
g_recHandle = handle;

memset(g_ring, 0, sizeof(g_ring));
g_ring_write = 0;
g_read_pos   = 0.0f;
g_last_sample = 0.0f;

if (pBASSChannelSetDSP)
    g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);

logf("[VFX] HYBRID DSP attached");
return handle;

}

// ============================================================
// API
// ============================================================

static void  _vc_set_pitch(float f) {
if (f < 0.3f) f = 0.3f;
if (f > 3.0f) f = 3.0f;
g_pitch = f;
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
static const char* info = "libvoicefx|4.1|VoiceFX Hybrid Fixed|brruham";
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

void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
if (!hBASS) { logf("[VFX] ERROR: BASS"); return; }

pBASSChannelSetDSP = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");

void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
if (!addr) { logf("[VFX] ERROR: RecordStart"); return; }

if (pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
    logf("[VFX] ERROR: hook"); return;
}

FILE* f = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
if (f) { fprintf(f, "%lu\n", (unsigned long)&vc_api); fclose(f); }

logf("[VFX] READY");

}

}