#pragma once
// audio/dsp/limiter.h — Bytebeat Machine V1.6.1
// Brickwall limiter con:
//   threshold ≈ 0.9 full scale (29491 / 32767)
//   attack:  instantáneo (1 sample)
//   release: ~30ms @ 44100 Hz (coeff 0.9993)
//
// Sin float en hot path — todo Q15.
// Stereo: procesar L y R con instancias separadas, o usar process_stereo().
//
#include <cstdint>

struct Limiter {
    // threshold: fracción de full scale (0.0–1.0)
    // Default 0.9 = -0.9dBFS — deja headroom mínimo, no silencia nada notable.
    static constexpr float    THRESH_F      = 0.9f;
    static constexpr int32_t  THRESH        = (int32_t)(32767 * THRESH_F); // 29490
    static constexpr int32_t  GAIN_FULL_Q15 = 32767;

    // Release: ~30ms @ 44100
    // coeff = exp(-1 / (0.030 * 44100)) ≈ 0.9992
    // En Q15: 0.9992 * 32767 ≈ 32741
    static constexpr int32_t  RELEASE_Q15   = 32741;

    void reset() { gain_q15_ = GAIN_FULL_Q15; }

    inline int16_t process(int16_t x) {
        int32_t abs_x = (x < 0) ? -(int32_t)x : (int32_t)x;

        if (abs_x > THRESH) {
            // Attack instantáneo: gain reduction exacta en 1 sample
            int32_t needed = (THRESH << 15) / abs_x;
            if (needed < gain_q15_) gain_q15_ = needed;
        } else {
            // Release suave
            gain_q15_ = (gain_q15_ * RELEASE_Q15) >> 15;
            if (gain_q15_ > GAIN_FULL_Q15) gain_q15_ = GAIN_FULL_Q15;
        }

        int32_t out = ((int32_t)x * gain_q15_) >> 15;
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;
        return (int16_t)out;
    }

    // Stereo atado: el gain se calcula del canal más fuerte
    // para evitar distorsión por diferencia de ganancia entre L y R
    void process_stereo(int16_t& l, int16_t& r) {
        int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
        int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;
        int32_t peak = (al > ar) ? al : ar;

        if (peak > THRESH) {
            int32_t needed = (THRESH << 15) / peak;
            if (needed < gain_q15_) gain_q15_ = needed;
        } else {
            gain_q15_ = (gain_q15_ * RELEASE_Q15) >> 15;
            if (gain_q15_ > GAIN_FULL_Q15) gain_q15_ = GAIN_FULL_Q15;
        }

        int32_t ol = ((int32_t)l * gain_q15_) >> 15;
        int32_t or_ = ((int32_t)r * gain_q15_) >> 15;
        if (ol >  32767) ol =  32767; if (ol < -32768) ol = -32768;
        if (or_ >  32767) or_ =  32767; if (or_ < -32768) or_ = -32768;
        l = (int16_t)ol;
        r = (int16_t)or_;
    }

    int32_t gain_q15_ = GAIN_FULL_Q15;  // readable for debug
};
