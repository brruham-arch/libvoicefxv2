/**
 * voicefx_fft.cpp - AML Voice FX Mod untuk SA-MP Android
 * v4.1 - FFT-based pitch shifting (tanpa <complex>)
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>

#define LOG_TAG "libvoicefx_fft"
#define LOGFILE "/storage/emulated/0/voicefx_log.txt"

static void logf(const char* msg) {
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HDSP;
typedef void (*DSPPROC)(HDSP, DWORD, void*, DWORD, void*);

static float g_pitch   = 1.0f;
static int   g_enabled = 0;

// ============================================================
// Complex struct
// ============================================================
struct Complex {
    float re;
    float im;
};

static inline Complex makeComplex(float re, float im) {
    Complex c; c.re = re; c.im = im; return c;
}
static inline Complex add(Complex a, Complex b) {
    return makeComplex(a.re + b.re, a.im + b.im);
}
static inline Complex sub(Complex a, Complex b) {
    return makeComplex(a.re - b.re, a.im - b.im);
}
static inline Complex mul(Complex a, Complex b) {
    return makeComplex(a.re*b.re - a.im*b.im, a.re*b.im + a.im*b.re);
}
static inline Complex conjC(Complex a) {
    return makeComplex(a.re, -a.im);
}

// ============================================================
// FFT PARAMETERS
// ============================================================
#define FRAME_SIZE 1024

static short g_inbuf[FRAME_SIZE];
static short g_outbuf[FRAME_SIZE*2];
static int   g_inpos = 0;
static int   g_outpos = 0;

// ============================================================
// FFT IMPLEMENTATION (naive Cooley-Tukey)
// ============================================================
static void fft(Complex* buf, int n) {
    if (n <= 1) return;
    Complex* even = new Complex[n/2];
    Complex* odd  = new Complex[n/2];
    for (int i=0; i<n/2; i++) {
        even[i] = buf[i*2];
        odd[i]  = buf[i*2+1];
    }
    fft(even, n/2);
    fft(odd, n/2);
    for (int k=0; k<n/2; k++) {
        float ang = -2.0f * M_PI * k / n;
        Complex tw = makeComplex(cosf(ang), sinf(ang));
        Complex t = mul(tw, odd[k]);
        buf[k]     = add(even[k], t);
        buf[k+n/2] = sub(even[k], t);
    }
    delete[] even;
    delete[] odd;
}

static void ifft(Complex* buf, int n) {
    for (int i=0; i<n; i++) buf[i] = conjC(buf[i]);
    fft(buf, n);
    for (int i=0; i<n; i++) {
        buf[i] = conjC(buf[i]);
        buf[i].re /= n;
        buf[i].im /= n;
    }
}

// ============================================================
// DSP CALLBACK (lanjutan sudah di Bagian 1)
// ============================================================

// ============================================================
// HOOK & API
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
    logf("[VFX-FFT] RecordStart hooked, DSP dipasang");
    return handle;
}

// ============================================================
// PUBLIC API
// ============================================================
static void _vc_set_pitch(float f) {
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    g_pitch = f;
}
static void  _vc_enable(void) {
    memset(g_inbuf, 0, sizeof(g_inbuf));
    memset(g_outbuf, 0, sizeof(g_outbuf));
    g_inpos = 0;
    g_outpos = 0;
    g_enabled = 1;
}
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
    static const char* info = "libvoicefx_fft|4.1|VoiceFX FFT|brruham";
    return (void*)info;
}

void OnModPreLoad() {
    remove(LOGFILE);
    logf("[VFX-FFT] OnModPreLoad v4.1");
    memset(g_inbuf, 0, sizeof(g_inbuf));
    memset(g_outbuf, 0, sizeof(g_outbuf));
    g_inpos = 0;
    g_outpos = 0;
}

void OnModLoad() {
    logf("[VFX-FFT] OnModLoad v4.1");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf("[VFX-FFT] ERROR: libdobby"); return; }

    pDobbySymbolResolver = (void*(*)(const char*,const char*))dlsym(hDobby, "DobbySymbolResolver");
    pDobbyHook           = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!pDobbySymbolResolver || !pDobbyHook) { logf("[VFX-FFT] ERROR: Dobby sym"); return; }

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { logf("[VFX-FFT] ERROR: libBASS"); return; }

    pBASSChannelSetDSP    = (HDSP(*)(HRECORD,DSPPROC,void*,int))dlsym(hBASS, "BASS_ChannelSetDSP");
    pBASSChannelRemoveDSP = (int(*)(HRECORD,HDSP))dlsym(hBASS, "BASS_ChannelRemoveDSP");

    void* addr = pDobbySymbolResolver("libBASS.so", "BASS_RecordStart");
    if (!addr) { logf("[VFX-FFT] ERROR: BASS_RecordStart"); return; }

    if (pDobbyHook(addr, (void*)hook_BASSRecordStart, (void**)&orig_BASSRecordStart) != 0) {
        logf("[VFX-FFT] ERROR: DobbyHook"); return;
    }

    g_pitch   = 1.0f;
    g_enabled = 0;

    FILE* af = fopen("/storage/emulated/0/voicefx_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&vc_api); fclose(af); }

    logf("[VFX-FFT] OnModLoad SELESAI v4.1!");
}

} // extern "C"