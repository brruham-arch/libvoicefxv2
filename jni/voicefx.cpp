/**
 * voicefx.cpp - upgrade ke WSOLA
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <android/log.h>
#include <stdio.h>
#include <dlfcn.h>

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
// WSOLA PARAMETERS
// ============================================================
#define FRAME       256
#define HOP_A       64
#define SEARCH      32
#define BUF_SIZE    8192

// ============================================================
// STATE
// ============================================================
static float   g_pitch   = 1.0f;
static int     g_enabled = 0;

static int16_t inBuf[BUF_SIZE];
static int16_t outBuf[BUF_SIZE];
static double  win[FRAME];
static double  frame[FRAME];
static double  prevFrame[FRAME];    // raw, bukan windowed

static int inWrite   = 0;
static int inRead    = 0;
static int outWrite  = 0;
static int outRead   = 0;
static int inAvail   = 0;
static int outAvail  = 0;
static int firstFrame = 1;

// ============================================================
// PRE-COMPUTE HANN WINDOW
// ============================================================
static void initWindow() {
    for (int i = 0; i < FRAME; i++)
        win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (FRAME - 1)));
}

// ============================================================
// CROSS-CORRELATION — cari offset terbaik
// ============================================================
static int findBestOffset(int readPos, int hopS) {
    if (firstFrame) return 0;

    int   bestOffset = 0;
    double bestCorr  = -1e18;
    int   half       = SEARCH / 2;

    for (int delta = -half; delta <= half; delta++) {
        double corr = 0.0;
        for (int i = 0; i < FRAME; i++) {
            int idx = (readPos + hopS + delta + i + BUF_SIZE * 4) % BUF_SIZE;
            // pakai raw inBuf, bukan windowed
            corr += (double)inBuf[idx] * prevFrame[i];
        }
        if (corr > bestCorr) {
            bestCorr   = corr;
            bestOffset = delta;
        }
    }
    return bestOffset;
}

// ============================================================
// WSOLA PROCESS
// ============================================================
static void wsolaProcess(short* s16, int n) {
    if (!g_enabled || g_pitch == 1.0f) return;

    // Tulis input ke ring buffer
    for (int i = 0; i < n; i++) {
        inBuf[inWrite] = s16[i];
        inWrite  = (inWrite + 1) % BUF_SIZE;
        inAvail++;
    }

    int hopS = (int)fmaxf(1.0f, (float)HOP_A / g_pitch);  // analysis hop

    while (inAvail >= FRAME + SEARCH) {
        int offset = findBestOffset(inRead, hopS);

        // Simpan raw ke prevFrame SEBELUM windowing
        for (int i = 0; i < FRAME; i++) {
            int idx = (inRead + hopS + offset + i + BUF_SIZE * 4) % BUF_SIZE;
            prevFrame[i] = (double)inBuf[idx];  // raw
        }

        // Apply window + overlap-add ke outBuf
        for (int i = 0; i < FRAME; i++) {
            int    oIdx = (outWrite + i) % BUF_SIZE;
            double v    = prevFrame[i] * win[i];
            double sum  = (double)outBuf[oIdx] + v;
            if (sum >  32767.0) sum =  32767.0;
            if (sum < -32768.0) sum = -32768.0;
            outBuf[oIdx] = (int16_t)sum;
        }

        // Advance — synthesis hop TETAP HOP_A
        outWrite = (outWrite + HOP_A) % BUF_SIZE;
        outAvail += HOP_A;

        // Advance input — analysis hop = hopS
        inRead  = (inRead + HOP_A) % BUF_SIZE;
        inAvail -= HOP_A;
    }
    firstFrame = 0;

    // Baca output
    for (int i = 0; i < n; i++) {
        if (outAvail > 0) {
            s16[i]           = outBuf[outRead];
            outBuf[outRead]  = 0;
            outRead  = (outRead + 1) % BUF_SIZE;
            outAvail--;
        } else {
            s16[i] = 0;
        }
    }
}

// ============================================================
// DSP CALLBACK
// ============================================================
static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    short* s16 = (short*)buf;
    int    n   = (int)(len / 2);
    if (n <= 0 || n > BUF_SIZE) return;
    wsolaProcess(s16, n);
}

// ============================================================
// BASS + DOBBY (sama dengan sebelumnya)
// ============================================================
static HDSP    (*pBASSChannelSetDSP)(HRECORD, DSPPROC, void*, int)     = nullptr;
static int     (*pBASSChannelRemoveDSP)(HRECORD, HDSP)                 = nullptr;
static HRECORD (*orig_BASSRecordStart)(DWORD,DWORD,DWORD,void*,void*)  = nullptr;
static void*   (*pDobbySymbolResolver)(const char*, const char*)        = nullptr;
static int     (*pDobbyHook)(void*, void*, void**)                      = nullptr;

static HRECORD g_recHandle = 0;
static HDSP    g_dspHandle = 0;

static HRECORD hook_BASSRecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user) {
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    g_recHandle = handle;
    if (pBASSChannelSetDSP)
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);
    logf("[VFX] RecordStart hooked, WSOLA DSP dipasang");
    return handle;
}

// ============================================================
// VcAPI — sama, Lua tetap bisa kontrol
// ============================================================
static void  _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    // Reset state saat pitch berubah
    inWrite = inRead = outWrite = outRead = 0;
    inAvail = outAvail = 0;
    firstFrame = 1;
    memset(inBuf,    0, sizeof(inBuf));
    memset(outBuf,   0, sizeof(outBuf));
    memset(prevFrame,0, sizeof(prevFrame));
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
    static const char* info = "libvoicefx|3.0|VoiceFX WSOLA|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX] OnModPreLoad");
}

void OnModLoad() {
    logf("[VFX] OnModLoad");

    initWindow();  // pre-compute Hann

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

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX] OnModLoad SELESAI - WSOLA ready!");
}

} // extern "C"