#include "pitch.h"
#include "esp_log.h"
#include <stdlib.h>
#include <math.h>
#include <float.h>

static const char *TAG = "pitch";

static const char * const s_note_names[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void pitch_hz_to_note(float hz, char *buf, size_t len) {
    if (hz < 20.0f || len < 2) { buf[0] = '-'; buf[1] = '\0'; return; }
    int midi = (int)roundf(69.0f + 12.0f * log2f(hz / 440.0f));
    const char *name = s_note_names[((midi % 12) + 12) % 12];
    size_t i = 0;
    while (name[i] && i < len - 1) { buf[i] = name[i]; i++; }
    buf[i] = '\0';
}

float pitch_hz_to_nearest_hz(float hz) {
    if (hz < 20.0f) return 440.0f;
    int midi = (int)roundf(69.0f + 12.0f * log2f(hz / 440.0f));
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}

float pitch_hz_to_cents(float hz) {
    if (hz < 20.0f) return 0.0f;
    return 1200.0f * log2f(hz / pitch_hz_to_nearest_hz(hz));
}

static float *s_diff   = NULL;
static float *s_cmnd   = NULL;
static float *s_window = NULL;
static float *s_wbuf   = NULL;
static size_t s_half   = 0;

esp_err_t pitch_init(size_t buf_len) {
    s_half   = buf_len / 2;
    s_diff   = malloc(s_half * sizeof(float));
    s_cmnd   = malloc(s_half * sizeof(float));
    s_window = malloc(s_half * sizeof(float));
    s_wbuf   = malloc(s_half * sizeof(float));
    if (!s_diff || !s_cmnd || !s_window || !s_wbuf) {
        free(s_diff);   free(s_cmnd);
        free(s_window); free(s_wbuf);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_half; i++)
        s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)(s_half - 1)));
    ESP_LOGI(TAG, "init OK, half=%u", (unsigned)s_half);
    return ESP_OK;
}

float pitch_detect(const int16_t *buf, size_t len, float sample_rate) {
    size_t half = (len / 2 < s_half) ? len / 2 : s_half;

    /* Copy to float working buffer — no pre-window; NSDF normalisation handles it */
    float *wx = s_wbuf;
    for (size_t i = 0; i < half; i++)
        wx[i] = (float)buf[i];

    /* NSDF: s_diff[] = m[] (autocorrelation), s_cmnd[] = nsdf[] */
    for (size_t tau = 0; tau < half; tau++) {
        float m_val = 0.0f, r0s = 0.0f, r0e = 0.0f;
        for (size_t j = 0; j < half - tau; j++) {
            m_val += wx[j] * wx[j + tau];
            r0s   += wx[j] * wx[j];
        }
        for (size_t j = tau; j < half; j++) {
            r0e += wx[j] * wx[j];
        }
        float denom = r0s + r0e;
        s_diff[tau] = m_val;
        s_cmnd[tau] = (denom > 0.0f) ? 2.0f * m_val / denom : 0.0f;
    }

    size_t tau_min = (size_t)(sample_rate / 1200.0f) + 1;
    size_t tau_max = (size_t)(sample_rate / 40.0f);
    if (tau_max >= half) tau_max = half - 1;

    /* Global max for key maximum threshold */
    float global_max = 0.0f;
    for (size_t tau = 0; tau < half; tau++)
        if (s_cmnd[tau] > global_max) global_max = s_cmnd[tau];
    float threshold = 0.8f * global_max;

    /* Key maximum selection: first local max above threshold in guitar range */
    for (size_t tau = tau_min; tau <= tau_max; tau++) {
        float right = (tau + 1 < half) ? s_cmnd[tau + 1] : 0.0f;
        if (s_cmnd[tau] > s_cmnd[tau - 1] &&   /* tau_min >= 1, so [tau-1] is safe */
            s_cmnd[tau] >= right &&
            s_cmnd[tau] > threshold) {
            /* Parabolic interpolation */
            float bt = (float)tau;
            if (tau > 0 && tau < half - 1) {
                float s0 = s_cmnd[tau-1], s1 = s_cmnd[tau], s2 = s_cmnd[tau+1];
                float denom2 = 2.0f * (2.0f * s1 - s2 - s0);
                if (fabsf(denom2) > FLT_EPSILON) bt = (float)tau + (s2 - s0) / denom2;
            }
            return sample_rate / bt;
        }
    }
    return 0.0f;
}

#ifdef PITCH_TEST_HARNESS
#include "esp_timer.h"
float pitch_detect_full(const int16_t *buf, size_t len, float sample_rate,
                        const tuner_params_t *params, pitch_frame_t *frame)
{
    static float s_smooth_hz = 0.0f;

    size_t half = (len / 2 < s_half) ? len / 2 : s_half;

    float *wx = s_wbuf;
    for (size_t i = 0; i < half; i++)
        wx[i] = (float)buf[i];

    for (size_t tau = 0; tau < half; tau++) {
        float m_val = 0.0f, r0s = 0.0f, r0e = 0.0f;
        for (size_t j = 0; j < half - tau; j++) {
            m_val += wx[j] * wx[j + tau];
            r0s   += wx[j] * wx[j];
        }
        for (size_t j = tau; j < half; j++)
            r0e += wx[j] * wx[j];
        float denom = r0s + r0e;
        s_diff[tau] = m_val;
        s_cmnd[tau] = (denom > 0.0f) ? 2.0f * m_val / denom : 0.0f;
    }

    size_t tau_min = (size_t)(sample_rate / params->pitch_max_hz) + 1;
    size_t tau_max = (size_t)(sample_rate / params->pitch_min_hz);
    if (tau_max >= half) tau_max = half - 1;

    float global_max = 0.0f;
    for (size_t tau = 0; tau < half; tau++)
        if (s_cmnd[tau] > global_max) global_max = s_cmnd[tau];
    float threshold = params->threshold_coeff * global_max;

    float  detected_hz = 0.0f;
    size_t tau_det     = 0;
    float  tau_interp  = 0.0f;
    float  peak_val    = 0.0f;

    for (size_t tau = tau_min; tau <= tau_max; tau++) {
        float right = (tau + 1 < half) ? s_cmnd[tau + 1] : 0.0f;
        if (s_cmnd[tau] > s_cmnd[tau - 1] &&
            s_cmnd[tau] >= right &&
            s_cmnd[tau] > threshold) {
            tau_det  = tau;
            peak_val = s_cmnd[tau];
            float bt = (float)tau;
            if (tau > 0 && tau < half - 1) {
                float s0 = s_cmnd[tau-1], s1 = s_cmnd[tau], s2 = s_cmnd[tau+1];
                float d2 = 2.0f * (2.0f * s1 - s2 - s0);
                if (fabsf(d2) > FLT_EPSILON) bt = (float)tau + (s2 - s0) / d2;
            }
            tau_interp  = bt;
            detected_hz = sample_rate / bt;
            break;
        }
    }

    /* EMA smoothing: alpha=0 → raw, alpha→1 → heavy smoothing */
    if (detected_hz > 0.0f) {
        s_smooth_hz = (s_smooth_hz == 0.0f)
            ? detected_hz
            : params->smooth_alpha * s_smooth_hz + (1.0f - params->smooth_alpha) * detected_hz;
    } else {
        s_smooth_hz = 0.0f;
    }

    frame->timestamp_us     = (uint64_t)esp_timer_get_time();
    frame->detected_hz      = detected_hz;
    frame->smooth_hz        = s_smooth_hz;
    frame->cents            = pitch_hz_to_cents(detected_hz > 0.0f ? detected_hz : 1.0f);
    frame->nsdf_peak_val    = peak_val;
    frame->nsdf_global_max  = global_max;
    frame->threshold_used   = threshold;
    frame->tau_detected     = tau_det;
    frame->tau_interpolated = tau_interp;
    frame->nsdf_len         = (uint16_t)half;
    pitch_hz_to_note(detected_hz > 0.0f ? detected_hz : s_smooth_hz,
                     frame->note, sizeof(frame->note));
    for (size_t i = 0; i < half; i++) frame->nsdf[i] = s_cmnd[i];

    return detected_hz;
}
#endif /* PITCH_TEST_HARNESS */
