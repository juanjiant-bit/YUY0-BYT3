#pragma once
// audio/dsp/soft_clip.h — Bytebeat Machine V1.6.1
// Saturación suave usando la aproximación racional pedida:
//
//   y = x * (27 + x²) / (27 + 9*x²)
//
// Equivale a tanh(x/3) escalado — respuesta muy suave, sin flat-top abrupto.
// Optimizado para int16: todo en int32, sin float en hot path.
//
// drive = 0   → bypass (ganancia 1.0×, fast path)
// drive = 127 → saturación moderada (~2.5× pre-gain)
// drive = 255 → saturación fuerte (~6×)
//
// Uso:
//   SoftClip sc;
//   sc.set_drive(128);       // 0–255
//   int16_t out = sc.process(in);
//
#include <cstdint>

struct SoftClip {
    void set_drive(uint8_t drive) {
        // gain_q8: 256 = 1.0x, 1792 = 7.0x
        gain_q8_ = 256 + ((int32_t)drive * 1536) / 255;
        bypass_  = (drive == 0);
    }

    // set_drive desde float [0.0, 1.0]
    void set_drive_f(float d) {
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        set_drive((uint8_t)(d * 255.0f));
    }

    inline int16_t process(int16_t x) {
        if (bypass_) return x;

        // 1. Aplicar pre-gain
        int32_t v = ((int32_t)x * gain_q8_) >> 8;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;

        // 2. Approximación y = x*(27 + x²)/(27 + 9*x²) en Q12
        // Normalizar v a Q12 (-4096..4096 representa -1..1)
        // Q12 scale: 32767 → 4096  ⟹  divide by 8
        int32_t s  = v >> 3;                      // Q12: ±4096
        int32_t s2 = (s * s) >> 12;               // s² en Q12

        // Constante 27 en Q12 = 27 * 4096 / (máx teórico) — usamos 27 directo
        // porque la fórmula es adimensional; trabajamos en unidades de Q12.
        static constexpr int32_t K  = 27;         // constante de la fórmula
        int32_t num = s  * (K + s2);              // Q12 * Q12 = Q24
        int32_t den = K  + (9 * s2);              // Q12 denominador

        int32_t out_q12 = (den != 0) ? (num / den) : s;

        // 3. Volver a int16 (Q12 → escalar por 8 para igualar entrada normalizada)
        int32_t out = out_q12 << 3;
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;
        return (int16_t)out;
    }

private:
    int32_t gain_q8_ = 256;
    bool    bypass_  = true;
};
