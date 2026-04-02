// ============================================================
// OLA ENGINE - PERBAIKAN
// ============================================================

#define RING_BITS  14
#define RING_SIZE  (1 << RING_BITS)
#define RING_MASK  (RING_SIZE - 1)

#define FRAME_SIZE 1024                // Naikkan sedikit untuk kualitas lebih baik (power of 2 bagus)
#define HOP_SIZE   256                // 50% overlap

static float  g_pitch   = 1.0f;
static int    g_enabled = 0;

static float  g_ring[RING_SIZE] = {0};
static int    g_ring_write = 0;

static float  g_synth_pos = 0.0f;

static float  g_overlap[FRAME_SIZE] = {0};   // overlap buffer untuk akumulasi
static int    g_overlap_pos = 0;             // posisi saat ini di overlap buffer

static float  g_hann[FRAME_SIZE];
static int    g_hann_ready = 0;

static void init_hann() {
    if (g_hann_ready) return;
    for (int i = 0; i < FRAME_SIZE; i++) {
        float t = (float)i / (FRAME_SIZE - 1);
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * t));
    }
    g_hann_ready = 1;
}

static inline float ring_read(float pos) {
    int i0 = (int)pos & RING_MASK;
    int i1 = (i0 + 1) & RING_MASK;
    float frac = pos - floorf(pos);
    return g_ring[i0] * (1.0f - frac) + g_ring[i1] * frac;
}

static inline short clamp16(float v) {
    return (short)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : v));
}

// ============================================================
// DSP CALLBACK - VERSI PERBAIKAN
// ============================================================
static void dspCallback(HDSP, DWORD, void* buf, DWORD len, void*) {
    if (!g_enabled || g_pitch == 1.0f) return;

    short* s16 = (short*)buf;
    int n = (int)(len / 2);
    if (n <= 0) return;

    init_hann();

    // 1. Tulis input ke ring buffer
    for (int i = 0; i < n; i++) {
        g_ring[(g_ring_write + i) & RING_MASK] = (float)s16[i];
    }
    g_ring_write = (g_ring_write + n) & RING_MASK;

    // 2. Synthesis dengan OLA yang benar
    float factor = g_pitch;
    float syn_pos = g_synth_pos;

    // Output temporary
    float* out = (float*)alloca(n * sizeof(float));  // atau static buffer besar
    memset(out, 0, n * sizeof(float));

    // Proses sample per sample dengan hop
    for (int i = 0; i < n; i++) {
        // Jika sudah waktunya mulai frame baru (setiap HOP_SIZE)
        if ((i % HOP_SIZE) == 0) {
            // Ambil frame baru dari ring buffer
            for (int j = 0; j < FRAME_SIZE; j++) {
                float sample = ring_read(syn_pos + j) * g_hann[j];

                int out_idx = i + j;
                if (out_idx < n) {
                    out[out_idx] += sample;           // akumulasi ke output
                } else {
                    // Simpan ke overlap buffer untuk frame berikutnya
                    int ov_idx = out_idx - n;
                    if (ov_idx < FRAME_SIZE)
                        g_overlap[ov_idx] += sample;
                }
            }
            syn_pos += HOP_SIZE * factor;
        }
    }

    // Tambahkan sisa overlap dari frame sebelumnya
    for (int i = 0; i < n && i < FRAME_SIZE; i++) {
        out[i] += g_overlap[i];
    }

    // Geser overlap buffer (sisa untuk next call)
    for (int i = 0; i < FRAME_SIZE - n; i++) {
        g_overlap[i] = g_overlap[i + n];
    }
    for (int i = FRAME_SIZE - n; i < FRAME_SIZE; i++) {
        g_overlap[i] = 0.0f;
    }

    g_synth_pos = fmodf(syn_pos, (float)RING_SIZE);

    // 3. Normalisasi (Hann 50% overlap ≈ faktor 2)
    float norm = 2.0f / (1.0f + fabsf(factor - 1.0f) * 0.2f);  // sedikit adaptif

    for (int i = 0; i < n; i++) {
        s16[i] = clamp16(out[i] * norm);
    }
}