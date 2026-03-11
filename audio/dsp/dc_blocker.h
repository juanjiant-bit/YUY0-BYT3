#pragma once
// audio/dsp/dc_blocker.h — Bytebeat Machine V1.6.1
// HPF 1er orden para eliminar DC offset del bytebeat.
//
// Ecuación: y[n] = x[n] - x[n-1] + R * y[n-1]
// R = 0.995 → fc ≈ 35 Hz @ 44100 Hz
// (ligeramente más alto que v1.5 (0.9972/20Hz) para convergencia más rápida)
//
// Todo en int32 — sin float en hot path.
// warm(): pre-converge el estado antes de audio real (elimina thump al cargar snapshot).
//
#include <cstdint>

struct DcBlocker {
    // Warm-up: corre N iteraciones con el valor DC esperado para que
    // el filtro converja antes de que llegue audio real.
    // Llamar después de cada snapshot trigger.
    void warm(int16_t dc_value, int n = 512) {
        for (int i = 0; i < n; i++) process(dc_value);
    }

    void reset() { x_prev_ = 0; y_acc_ = 0; }

    inline int16_t process(int16_t x) {
        // R = 9950/10000 = 0.995
        y_acc_ = (int32_t)x - x_prev_ + (y_acc_ * 9950) / 10000;
        x_prev_ = (int32_t)x;
        if (y_acc_ >  32767) return  32767;
        if (y_acc_ < -32768) return -32768;
        return (int16_t)y_acc_;
    }

private:
    int32_t x_prev_ = 0;
    int32_t y_acc_  = 0;
};
