#pragma once
// grain_freeze.h — Bytebeat Machine V1.14
// Grain Freeze: captura un buffer de audio y lo reproduce en loop continuo.
//
// Comportamiento según el brief:
//   - POT3 en 0.0 → sin efecto (bypass, grabando siempre)
//   - POT3 > 0.0  → FREEZE: congela el buffer actual y lo reproduce en loop
//   - Cada movimiento del pot > threshold → recaptura el buffer (refresca)
//   - El buffer tiene crossfade de entrada/salida para evitar clicks
//
// Tamaño de buffer: ~186ms @ 44100Hz = 8192 samples (potencia de 2 → módulo eficiente)
// RAM: 8192 × 2 × 2 bytes (stereo) = ~32KB
//
// El loop de playback puede ser:
//   - Forward (amount 0.0–0.5)
//   - Reverse (amount 0.5–1.0) — invierte dirección para más texturas
//
#include <cstdint>
#include <cstring>

struct GrainFreeze {
    static constexpr uint16_t BUF_SIZE   = 8192;   // ~186ms, potencia de 2
    static constexpr uint16_t XFADE_LEN  = 256;    // crossfade 5.8ms
    // Threshold de movimiento de pot para refrescar (±2%)
    static constexpr float    REFRESH_THRESH = 0.02f;

    void init() {
        memset(buf_l_, 0, sizeof(buf_l_));
        memset(buf_r_, 0, sizeof(buf_r_));
        write_pos_ = 0;
        read_pos_  = 0;
        frozen_    = false;
        amount_    = 0.0f;
        last_amount_ = -1.0f;
        xfade_pos_ = 0;
        going_in_  = false;
    }

    // Llamar desde AudioEngine safe point cuando el pot cambia.
    // amount [0.0, 1.0].
    // Retorna true si se acaba de activar un refresh (útil para debug).
    bool set_amount(float a) {
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;

        bool refreshed = false;

        if (a < 0.02f) {
            // Bypass: dejar de congelar
            frozen_      = false;
            amount_      = 0.0f;
            last_amount_ = a;
            going_in_    = false;
            return false;
        }

        // Detectar movimiento del pot mientras está frozen → refrescar
        if (frozen_ && last_amount_ >= 0.0f &&
            (a - last_amount_ >  REFRESH_THRESH ||
             a - last_amount_ < -REFRESH_THRESH)) {
            // Refrescar: copiar posición de escritura actual al buffer freeze
            // y empezar a reproducir desde aquí
            freeze_pos_ = write_pos_;
            read_pos_   = write_pos_;
            // Activar crossfade de entrada para transición suave
            xfade_pos_ = 0;
            going_in_  = true;
            refreshed  = true;
        }

        if (!frozen_) {
            // Primera activación: congelar ahora
            freeze_pos_ = write_pos_;
            read_pos_   = write_pos_;
            frozen_     = true;
            xfade_pos_  = 0;
            going_in_   = true;
        }

        // Direction: amount < 0.5 → forward, ≥ 0.5 → reverse
        reverse_   = (a >= 0.5f);
        amount_    = a;
        last_amount_ = a;
        return refreshed;
    }

    bool is_active() const { return frozen_ && (amount_ > 0.02f || force_hold_); }

    // Punch freeze momentáneo: captura el buffer actual y lo mantiene sin
    // depender del pot de grain. Se libera con force_release().
    void force_freeze() {
        freeze_pos_ = write_pos_;
        read_pos_   = write_pos_;
        frozen_     = true;
        force_hold_ = true;
        reverse_    = false;
        xfade_pos_  = 0;
        going_in_   = true;
    }

    void force_release() {
        force_hold_ = false;
        if (amount_ < 0.02f) {
            frozen_   = false;
            going_in_ = false;
        }
    }

    // Procesar stereo — llama a process() cada sample desde el hot path
    void process(int16_t& l, int16_t& r) {
        // Siempre escribir en el ring buffer (para captura continua)
        buf_l_[write_pos_] = l;
        buf_r_[write_pos_] = r;
        write_pos_ = (write_pos_ + 1) & (BUF_SIZE - 1);

        if (!frozen_ || (!force_hold_ && amount_ < 0.02f)) return;

        // Leer desde buffer congelado
        int16_t frozen_l = buf_l_[read_pos_];
        int16_t frozen_r = buf_r_[read_pos_];

        // Avanzar posición de lectura (forward o reverse)
        if (!reverse_) {
            read_pos_ = (read_pos_ + 1) & (BUF_SIZE - 1);
        } else {
            read_pos_ = (read_pos_ + BUF_SIZE - 1) & (BUF_SIZE - 1);
        }

        // Crossfade al entrar al loop (going_in_ = true los primeros XFADE_LEN samples)
        int32_t wet_q15;
        if (going_in_ && xfade_pos_ < XFADE_LEN) {
            wet_q15  = (int32_t)xfade_pos_ * 32767 / XFADE_LEN;
            xfade_pos_++;
            if (xfade_pos_ >= XFADE_LEN) going_in_ = false;
        } else {
            wet_q15 = 32767;
        }
        int32_t dry_q15 = 32767 - wet_q15;

        int32_t out_l = ((int32_t)frozen_l * wet_q15 + (int32_t)l * dry_q15) >> 15;
        int32_t out_r = ((int32_t)frozen_r * wet_q15 + (int32_t)r * dry_q15) >> 15;

        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;

        l = (int16_t)out_l;
        r = (int16_t)out_r;
    }

private:
    int16_t  buf_l_[BUF_SIZE] = {};
    int16_t  buf_r_[BUF_SIZE] = {};
    uint16_t write_pos_  = 0;
    uint16_t read_pos_   = 0;
    uint16_t freeze_pos_ = 0;
    uint16_t xfade_pos_  = 0;
    bool     frozen_     = false;
    bool     going_in_   = false;
    bool     reverse_    = false;
    bool     force_hold_ = false;
    float    amount_     = 0.0f;
    float    last_amount_= -1.0f;
};
