// cap_pad_handler.cpp — Bytebeat Machine V1.6.1
// Auto-calibración mejorada: 200 muestras, threshold dinámico, recalibrate.
#include "cap_pad_handler.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "../utils/debug_log.h"
#include <cmath>

constexpr uint8_t CapPadHandler::ROW_PINS[];
constexpr uint8_t CapPadHandler::COL_PINS[];

void CapPadHandler::init(Preset p) {
    preset_ = p;
    for (uint8_t r = 0; r < ROWS; r++) {
        gpio_init(ROW_PINS[r]);
        gpio_set_dir(ROW_PINS[r], GPIO_OUT);
        gpio_put(ROW_PINS[r], 0);
    }
    for (uint8_t c = 0; c < COLS; c++) {
        gpio_init(COL_PINS[c]);
        gpio_set_dir(COL_PINS[c], GPIO_IN);
        gpio_disable_pulls(COL_PINS[c]);  // CRÍTICO: sin pull interno
    }
    sleep_ms(20);
    calibrate();
}

// ── Hardware: medir tiempo de carga RC ───────────────────────
uint32_t CapPadHandler::measure_charge_us(uint8_t row, uint8_t col) {
    uint8_t rp = ROW_PINS[row];
    uint8_t cp = COL_PINS[col];
    gpio_put(rp, 0);
    sleep_us(preset_.discharge_us);
    uint32_t t0 = time_us_32();
    gpio_put(rp, 1);
    while (!gpio_get(cp)) {
        if ((time_us_32() - t0) >= preset_.max_charge_us) {
            gpio_put(rp, 0);
            return preset_.max_charge_us;
        }
    }
    uint32_t dt = time_us_32() - t0;
    gpio_put(rp, 0);
    return dt;
}

// ── Calibración completa (200 muestras, threshold dinámico) ──
void CapPadHandler::calibrate() {
#ifdef DEBUG_PADS
    printf("[PAD] calibrando %u pads × %u muestras...\n",
           NUM_PADS, preset_.calib_samples);
#endif
    for (uint8_t r = 0; r < ROWS; r++) gpio_put(ROW_PINS[r], 0);
    sleep_ms(30);  // estabilizar

    for (uint8_t r = 0; r < ROWS; r++) {
        for (uint8_t c = 0; c < COLS; c++) {
            uint8_t  idx = r * COLS + c;
            uint64_t sum = 0;
            for (uint8_t s = 0; s < preset_.calib_samples; s++) {
                sum += measure_charge_us(r, c);
                sleep_us(80);
            }
            float baseline = (float)(sum / preset_.calib_samples);
            baseline_f_[idx]   = baseline;
            raw_us_[idx]       = (uint32_t)baseline;
            pressure_[idx]     = 0.0f;
            pressure_smooth_[idx] = 0.0f;

            // Threshold dinámico: baseline + 25% de baseline
            // (pedido explícito V1.6.1 — más robusto que umbral fijo)
            threshold_us_[idx] = (uint32_t)(baseline * 1.25f);

#ifdef DEBUG_PADS
            printf("[PAD] pad%02u  baseline=%lu  threshold=%lu\n",
                   idx,
                   (unsigned long)(uint32_t)baseline,
                   (unsigned long)threshold_us_[idx]);
#endif
        }
    }
    state_confirmed_ = 0;
    state_prev_      = 0;
    calibrated_      = true;
#ifdef DEBUG_PADS
    printf("[PAD] calibración completa\n");
#endif
}

void CapPadHandler::set_preset(Preset p) {
    preset_ = p;
    calibrate();
}

// ── Recalibración automática por drift ───────────────────────
bool CapPadHandler::recalibrate_if_drifted(uint32_t drift_threshold_us) {
    if (!calibrated_) return false;
    // Verificar si algún pad no presionado tiene delta > drift_threshold_us
    uint32_t max_drift = 0;
    for (uint8_t i = 0; i < NUM_PADS; i++) {
        if ((state_confirmed_ >> i) & 1) continue;  // ignorar pads activos
        uint32_t b = (uint32_t)baseline_f_[i];
        uint32_t d = (raw_us_[i] > b) ? (raw_us_[i] - b) : 0u;
        if (d > max_drift) max_drift = d;
    }
    if (max_drift > drift_threshold_us) {
#ifdef DEBUG_PADS
        printf("[PAD] drift=%lu > threshold=%lu — recalibrando\n",
               (unsigned long)max_drift, (unsigned long)drift_threshold_us);
#endif
        calibrate();
        return true;
    }
    return false;
}

// ── Cálculo de presión ────────────────────────────────────────
float CapPadHandler::compute_pressure(uint32_t delta_us) const {
    if (delta_us < preset_.hyst_on_us) return 0.0f;
    float n = (float)(delta_us - preset_.hyst_on_us) / (float)preset_.at_range_us;
    if (n > 1.0f) n = 1.0f;
    return powf(n, preset_.at_curve);
}

// ── Scan (cada 5ms) ───────────────────────────────────────────
void CapPadHandler::scan() {
    if (!calibrated_) return;
    state_prev_       = state_confirmed_;
    uint16_t new_conf = 0;

    for (uint8_t r = 0; r < ROWS; r++) {
        gpio_put(ROW_PINS[r], 0);
        for (uint8_t c = 0; c < COLS; c++) {
            uint8_t  idx = r * COLS + c;
            uint32_t t   = measure_charge_us(r, c);
            raw_us_[idx] = t;

            // Usar threshold dinámico calculado en calibrate()
            uint32_t thr = threshold_us_[idx];
            bool on      = (state_confirmed_ >> idx) & 1u;

            // Hysteresis: ON si t > threshold, OFF si t < hyst_off relativo al baseline
            uint32_t b    = (uint32_t)baseline_f_[idx];
            uint32_t d    = (t > b) ? (t - b) : 0u;
            bool touched  = on ? (d >= preset_.hyst_off_us)
                               : (t >= thr && t < preset_.max_charge_us);

            if (touched) new_conf |= (1u << idx);

            // Aftertouch: suavizado por EMA doble (sube rápido, baja lento)
            float raw_p = touched ? compute_pressure(d) : 0.0f;
            float alpha = (raw_p > pressure_smooth_[idx]) ? 0.4f : 0.15f;
            pressure_smooth_[idx] += alpha * (raw_p - pressure_smooth_[idx]);
            pressure_[idx] = pressure_smooth_[idx];

            // Adaptive baseline: EMA muy lenta cuando pad no está activo
            if (!touched) {
                baseline_f_[idx] += preset_.baseline_alpha *
                                    ((float)t - baseline_f_[idx]);
                // Actualizar threshold dinámico en tiempo real
                threshold_us_[idx] = (uint32_t)(baseline_f_[idx] * 1.25f);
            }
        }
    }
    state_confirmed_ = new_conf;
}

bool CapPadHandler::is_pressed   (uint8_t p) const { return (state_confirmed_ >> p) & 1; }
bool CapPadHandler::just_pressed (uint8_t p) const {
    return ((state_confirmed_ >> p) & 1) && !((state_prev_ >> p) & 1);
}
bool CapPadHandler::just_released(uint8_t p) const {
    return !((state_confirmed_ >> p) & 1) && ((state_prev_ >> p) & 1);
}
