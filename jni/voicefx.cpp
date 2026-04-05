/**
 * voicefx.cpp - AML Voice FX Mod untuk SA-MP Android
 * Algoritma: WSOLA (Waveform Similarity Overlap-Add)
 * v3.2 - fix FRAME/SEARCH threshold untuk buffer BASS Android (n=128)
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>

#define LOG_TAG  "libvoicefx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGFILE  "/storage/emulated/0/voicefx_log.txt"

static void logf(const char* msg) {
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    LOGI("%s", msg);
}

typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HDSP;
typedef void (*DSPPROC)(HDSP, DWORD, void*, DWORD, void*);

// ============================================================
// WSOLA PARAMETERS
// Disesuaikan dengan buffer BASS Android (n=128 per callback)
// Threshold = FRAME = 128, tercapai setelah 1 callback
// ============================================================
#define FRAME       128
#define HOP_A       32
#define SEARCH      16
#define BUF_SIZE    4096

// ============================================================
// GLOBAL STATE
// ============================================================
static float   g_pitch    = 1.0f;
static int     g_enabled  = 0;

static int16_t inBuf[BUF_SIZE];
static int16_t outBuf[BUF_SIZE];
static double  win[FRAME];
static double  prevFrame[FRAME];

static int inWrite    = 0;
static int inRead     = 0;
static int outWrite   = 0;
static int outRead    = 0;
static int inAvail    = 0;
static int outAvail   = 0;
static int firstFrame = 1;

// ============================================================
// HANN WINDOW
// ============================================================
static void initWindow() {
    for (int i = 0; i < FRAME; i++)
        win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (FRAME - 1)));
    logf("[VFX] Hann window initialized");
}

// ============================================================
// RESET STATE
// ============================================================
static void resetState() {
    inWrite = inRead = outWrite = outRead = 0;
    inAvail = outAvail = 0;
    firstFrame = 1;
    memset(inBuf,     0, sizeof(inBuf));
    memset(outBuf,    0, sizeof(outBuf));
    memset(prevFrame, 0, sizeof(prevFrame));
}

// ============================================================
// CROSS-CORRELATION
// pakai raw inBuf vs prevFrame (raw), bukan windowed
// ============================================================
static int findBestOffset(int readPos, int hopS) {
    if (firstFrame) return 0;

    int    bestOffset = 0;
    double bestCorr   = -1e18;
    int    half       = SEARCH / 2;

    for (int delta = -half; delta <= half; delta++) {
        double corr = 0.0;
        for (int i = 0; i < FRAME; i++) {
            int idx = ((readPos + hopS + delta + i) % BUF_SIZE + BUF_SIZE) % BUF_SIZE;
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

    // analysis hop: berubah sesuai pitch
    int hopS = (int)fmaxf(1.0f, (float)HOP_A / g_pitch);

    // Threshold = FRAME saja, tanpa + SEARCH
    while (inAvail >= FRAME) {
        int offset = findBestOffset(inRead, hopS);

        // Simpan raw ke prevFrame, lalu overlap-add dengan window
        for (int i = 0; i < FRAME; i++) {
            int idx = ((inRead + hopS + offset + i) % BUF_SIZE + BUF_SIZE) % BUF_SIZE;
            prevFrame[i] = (double)inBuf[idx];

            int    oIdx = (outWrite + i) % BUF_SIZE;
            double v    = prevFrame[i] * win[i];
            double sum  = (double)outBuf[oIdx] + v;
            if (sum >  32767.0) sum =  32767.0;
            if (sum < -32768.0) sum = -32768.0;
            outBuf[oIdx] = (int16_t)sum;
        }

        firstFrame = 0;

        // synthesis hop TETAP HOP_A
        outWrite = (outWrite + HOP_A) % BUF_SIZE;
        outAvail += HOP_A;

        // inRead maju HOP_A juga supaya inAvail berkurang konsisten
        inRead  = (inRead + HOP_A) % BUF_SIZE;
        inAvail -= HOP_A;
    }

    // Baca output ke buffer
    for (int i = 0; i < n; i++) {
        if (outAvail > 0) {
            s16[i]          = outBuf[outRead];
            outBuf[outRead] = 0;
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
static int dbgCount = 0;

static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    if (dbgCount++ % 200 == 0) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp),
            "[VFX] DSP#%d enabled=%d pitch=%.2f n=%d inAvail=%d outAvail=%d",
            dbgCount, g_enabled, g_pitch, (int)(len / 2), inAvail, outAvail);
        logf(tmp);
    }

    short* s16 = (short*)buf;
    int    n   = (int)(len / 2);
    if (n <= 0 || n > BUF_SIZE) return;
    wsolaProcess(s16, n);
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
    logf("[VFX] hook_BASSRecordStart dipanggil");
    HRECORD handle = orig_BASSRecordStart(freq, chans, flags, proc, user);
    g_recHandle = handle;

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] recHandle = %u", handle);
    logf(tmp);

    if (pBASSChannelSetDSP) {
        g_dspHandle = pBASSChannelSetDSP(handle, dspCallback, nullptr, 1);
        snprintf(tmp, sizeof(tmp), "[VFX] dspHandle = %u", g_dspHandle);
        logf(tmp);
    } else {
        logf("[VFX] ERROR: pBASSChannelSetDSP null");
    }
    return handle;
}

// ============================================================
// VcAPI FUNCTIONS
// ============================================================
static void _vc_set_pitch(float f) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] set_pitch called: %.2f", f);
    logf(tmp);

    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    resetState();
    g_pitch = f;

    snprintf(tmp, sizeof(tmp), "[VFX] g_pitch now: %.2f", g_pitch);
    logf(tmp);
}

static void _vc_enable(void) {
    logf("[VFX] enable called");
    resetState();
    g_enabled = 1;
}

static void _vc_disable(void) {
    logf("[VFX] disable called");
    g_enabled = 0;
    resetState();
}

static int   _vc_is_enabled(void) { return g_enabled; }
static float _vc_get_pitch(void)  { return g_pitch; }

// ============================================================
// VcAPI STRUCT
// ============================================================
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
    static const char* info = "libvoicefx|3.2|VoiceFX WSOLA|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX] OnModPreLoad");
}

void OnModLoad() {
    logf("[VFX] OnModLoad start");

    initWindow();

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf("[VFX] ERROR: libdobby.so tidak ditemukan"); return; }
    logf("[VFX] libdobby.so loaded");

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) {
        logf("[VFX] ERROR: Dobby symbols tidak ditemukan");
        return;
    }
    logf("[VFX] Dobby symbols OK");

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { logf("[VFX] ERROR: libBASS.so tidak ditemukan"); return; }
    logf("[VFX] libBASS.so loaded");

    pBASSChannelSetDSP    = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    if (!pBASSChannelSetDSP) {
        logf("[VFX] ERROR: BASS_ChannelSetDSP tidak ditemukan");
        return;
    }
    logf("[VFX] BASS DSP symbols OK");

    void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addr) { logf("[VFX] ERROR: BASS_RecordStart tidak ditemukan"); return; }

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[VFX] BASS_RecordStart addr: %p", addr);
    logf(tmp);

    if (pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        logf("[VFX] ERROR: DobbyHook gagal");
        return;
    }
    logf("[VFX] Hook BASS_RecordStart OK");

    g_pitch   = 1.0f;
    g_enabled = 0;
    resetState();

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) {
        fprintf(af, "%lu\n", (unsigned long)&vc_api);
        fclose(af);
        snprintf(tmp, sizeof(tmp), "[VFX] vc_api addr: %lu", (unsigned long)&vc_api);
        logf(tmp);
    } else {
        logf("[VFX] ERROR: tidak bisa tulis voicefx_addr.txt");
    }

    logf("[VFX] OnModLoad SELESAI - WSOLA v3.2 ready!");
}

} // extern "C"
