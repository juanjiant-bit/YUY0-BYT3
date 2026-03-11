// stutter_fx.cpp — Bytebeat Machine V1.14
// Optimizado a fixed-point en el hot path.
#include "stutter_fx.h"
#include <cstring>

void StutterFx::init() {
    memset(buf_l_, 0, sizeof(buf_l_));
    memset(buf_r_, 0, sizeof(buf_r_));
    gate_on_      = false;
    write_pos_       = 0;
    read_pos_q16_    = 0;
    loop_start_      = 0;
    depth_target_q15_= 0;
    depth_smooth_q15_= 0;
    rate_q16_        = (1u << 16);
    rate_smooth_q16_ = (1u << 16);
    loop_len_        = BUF_SAMPLES;
}

void StutterFx::gate_on() {
    gate_on_ = true;
    loop_len_     = BUF_SAMPLES;
    loop_start_   = (write_pos_ + BUF_SAMPLES - loop_len_) % BUF_SAMPLES;
    read_pos_q16_ = (loop_start_ << 16);
    if (depth_target_q15_ < 328) depth_target_q15_ = 32767;
}

void StutterFx::gate_repeat(uint16_t divisor) {
    if (divisor < 1) divisor = 1;
    gate_on_ = true;
    loop_len_ = BUF_SAMPLES / divisor;
    if (loop_len_ < 64) loop_len_ = 64;
    loop_start_ = (write_pos_ + BUF_SAMPLES - loop_len_) % BUF_SAMPLES;
    read_pos_q16_ = (loop_start_ << 16);
    depth_target_q15_ = 32767;
}

void StutterFx::gate_off() {
    gate_on_         = false;
    depth_target_q15_ = 0;  // decae suave via depth_smooth_q15_
}

void StutterFx::set_pressure(float p) {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    if (p < 0.01f) {
        // Sin presion: gate off suave
        depth_target_q15_ = 0;
        rate_q16_         = (1u << 16);
    } else if (p < 0.4f) {
        // Rango 0.0-0.4: depth 0->1 (activa el stutter progresivamente)
        depth_target_q15_ = (int32_t)((p / 0.4f) * 32767.0f);
        if (depth_target_q15_ > 32767) depth_target_q15_ = 32767;
        rate_q16_         = (1u << 16);
    } else {
        // Rango 0.4-1.0: depth=1, rate 1.0->3.0 (stutter se acelera)
        depth_target_q15_ = 32767;
        float rate_f       = 1.0f + (p - 0.4f) / 0.6f * 2.0f;
        if (rate_f < 0.25f) rate_f = 0.25f;
        if (rate_f > 4.0f)  rate_f = 4.0f;
        rate_q16_ = (uint32_t)(rate_f * 65536.0f);
    }
}

void StutterFx::set_depth(float d) {
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    depth_target_q15_ = (int32_t)(d * 32767.0f);
}

void StutterFx::set_rate(float r) {
    if (r < 0.25f) r = 0.25f;
    if (r > 4.0f)  r = 4.0f;
    rate_q16_ = (uint32_t)(r * 65536.0f);
}

void StutterFx::process(int16_t& in_l, int16_t& in_r) {
    // Siempre grabar en el buffer circular (captura continua)
    buf_l_[write_pos_] = in_l;
    buf_r_[write_pos_] = in_r;
    write_pos_ = (write_pos_ + 1) % BUF_SAMPLES;

    // Suavizar depth hacia target (evita clicks al activar/desactivar)
    depth_smooth_q15_ += (int32_t)(((int64_t)(depth_target_q15_ - depth_smooth_q15_) * DEPTH_ALPHA_Q15) >> 15);
    rate_smooth_q16_ += (uint32_t)(((int64_t)((int64_t)rate_q16_ - (int64_t)rate_smooth_q16_) * RATE_ALPHA_Q15) >> 15);

    // Si depth casi cero y gate off: bypass total
    if (!gate_on_ && depth_smooth_q15_ < 32) {
        depth_smooth_q15_ = 0;
        return;  // bypass, no toca la señal
    }

    // Leer del buffer con interpolacion lineal en Q16
    const uint32_t read_i = (read_pos_q16_ >> 16) % BUF_SAMPLES;
    const uint32_t ri1 = (read_i + 1) % BUF_SAMPLES;
    const uint32_t frac_q16 = read_pos_q16_ & 0xFFFFu;
    const int32_t frac_q15 = (int32_t)(frac_q16 >> 1);
    const int32_t inv_q15  = 32767 - frac_q15;

    const int32_t loop_l = ((int32_t)buf_l_[read_i] * inv_q15 + (int32_t)buf_l_[ri1] * frac_q15) >> 15;
    const int32_t loop_r = ((int32_t)buf_r_[read_i] * inv_q15 + (int32_t)buf_r_[ri1] * frac_q15) >> 15;

    // Avanzar posicion de lectura con wrap dentro del loop congelado
    read_pos_q16_ += rate_smooth_q16_;
    const uint32_t loop_end_q16 = ((loop_start_ + loop_len_) << 16);
    if (read_pos_q16_ >= loop_end_q16) {
        read_pos_q16_ = (loop_start_ << 16);
    }

    // Crossfade dry/wet en Q15
    const int32_t wet_q15 = depth_smooth_q15_;
    const int32_t dry_q15 = 32767 - wet_q15;
    int32_t out_l = ((int32_t)in_l * dry_q15 + loop_l * wet_q15) >> 15;
    int32_t out_r = ((int32_t)in_r * dry_q15 + loop_r * wet_q15) >> 15;
    if (out_l >  32767) out_l =  32767;
    if (out_l < -32768) out_l = -32768;
    if (out_r >  32767) out_r =  32767;
    if (out_r < -32768) out_r = -32768;
    in_l = (int16_t)out_l;
    in_r = (int16_t)out_r;
}
