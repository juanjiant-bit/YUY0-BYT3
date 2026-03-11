#pragma once
// cap_pad_handler.h — Bytebeat Machine V1.6.1
// Pads capacitivos con auto-calibración mejorada:
//   - 200 muestras en calibrate() (era 32/48)
//   - threshold dinámico: baseline + baseline*0.25 (pedido V1.6.1)
//   - recalibrate(): llama calibrate() automáticamente si hay drift grande
//   - baseline[pad] + threshold[pad] como campos separados para diagnóstico
//
// CIRCUITO:
//   ROW_PIN --[1MΩ]-- PAD -- COL_PIN
//   100nF cerámica entre ROW_PIN y GND por fila.
//   COL_PIN: SIN pull interno (gpio_disable_pulls). CRÍTICO.
//
#include <cstdint>

class CapPadHandler {
public:
    static constexpr uint8_t ROWS     = 3;
    static constexpr uint8_t COLS     = 5;
    static constexpr uint8_t NUM_PADS = ROWS * COLS;
    static constexpr uint8_t ROW_PINS[ROWS] = {5, 6, 7};
    static constexpr uint8_t COL_PINS[COLS] = {8, 9, 13, 14, 15};

    // ── Preset ────────────────────────────────────────────────
    struct Preset {
        uint32_t discharge_us;   // tiempo de descarga antes de medir
        uint32_t max_charge_us;  // timeout = pad no conectado
        uint32_t hyst_on_us;     // umbral de activación (delta > este → ON)
        uint32_t hyst_off_us;    // umbral de desactivación
        uint32_t at_range_us;    // rango aftertouch sobre hyst_on
        float    at_curve;       // gamma de la curva de presión
        uint8_t  calib_samples;  // muestras para calibrar baseline (200 default)
        float    baseline_alpha; // EMA para adaptive baseline en scan()

        // DRY: uso en estudio, temperatura controlada
        static constexpr Preset DRY() {
            return {150, 2000, 30, 18, 120, 1.5f, 200, 0.002f};
        }
        // STAGE: escenario con humedad y temperatura variable
        static constexpr Preset STAGE() {
            return {200, 2000, 55, 35, 220, 2.0f, 200, 0.004f};
        }
    };

    // ── Inicialización ────────────────────────────────────────
    void init(Preset p = Preset::DRY());

    // Calibración completa:
    //   1. Tomar calib_samples mediciones por pad
    //   2. Calcular baseline = media
    //   3. threshold[pad] = baseline + baseline * 0.25 (threshold dinámico)
    // No llamar con dedo en los pads.
    void calibrate();

    // Cambiar preset y recalibrar
    void set_preset(Preset p);

    // Recalibrar si el drift promedio supera drift_threshold_us.
    // Llamar periódicamente desde Core1 (ej. cada 30s en idle).
    // Retorna true si se recalibró.
    bool recalibrate_if_drifted(uint32_t drift_threshold_us = 40);

    // ── Scan (llamar cada 5ms desde Core1) ───────────────────
    void scan();

    // ── Lecturas ──────────────────────────────────────────────
    bool     is_pressed   (uint8_t pad) const;
    bool     just_pressed (uint8_t pad) const;
    bool     just_released(uint8_t pad) const;
    uint16_t get_state()         const { return state_confirmed_; }
    uint16_t get_just_pressed()  const { return  state_confirmed_ & ~state_prev_; }
    uint16_t get_just_released() const { return ~state_confirmed_ &  state_prev_; }

    // Aftertouch: presión suavizada [0.0, 1.0]
    float get_pressure(uint8_t pad) const {
        return (pad < NUM_PADS) ? pressure_smooth_[pad] : 0.0f;
    }

    // Diagnóstico
    uint32_t get_raw_us      (uint8_t pad) const { return raw_us_[pad]; }
    uint32_t get_baseline_us (uint8_t pad) const { return (uint32_t)baseline_f_[pad]; }
    uint32_t get_threshold_us(uint8_t pad) const { return threshold_us_[pad]; }
    uint32_t get_delta_us    (uint8_t pad) const {
        uint32_t b = (uint32_t)baseline_f_[pad];
        return (raw_us_[pad] > b) ? (raw_us_[pad] - b) : 0u;
    }
    bool is_calibrated() const { return calibrated_; }

private:
    uint32_t measure_charge_us(uint8_t row, uint8_t col);
    float    compute_pressure (uint32_t delta_us) const;

    Preset   preset_;
    float    baseline_f_      [NUM_PADS] = {};
    uint32_t threshold_us_    [NUM_PADS] = {};  // V1.6.1: threshold dinámico por pad
    uint32_t raw_us_          [NUM_PADS] = {};
    float    pressure_        [NUM_PADS] = {};
    float    pressure_smooth_ [NUM_PADS] = {};
    uint16_t state_confirmed_ = 0;
    uint16_t state_prev_      = 0;
    bool     calibrated_      = false;
};
