#pragma once
// adc_handler.h — Bytebeat Machine V1.10
// CAMBIOS: NUM_POTS 5 → 6. Pot 5 = CH5 del 74HC4051 (ENV_RELEASE/ATTACK).
// V1.15: CV input en CH6 del MUX (GP29). Solo lectura, 0.0–1.0 post-clamp.
//        Circuito: divisor 18kΩ/33kΩ + 2×BAT43 limita a 0–3.3V.
//        Rango esperado: CV ±5V → 0.0–1.0 (sección 0–3.3V = 0.0–1.0).
#include <cstdint>

class AdcHandler {
public:
    static constexpr uint8_t  NUM_POTS  = 6;   // V1.10: pot 5 = envelope
    static constexpr uint8_t  CV_CH     = 6;   // V1.15: CH6 del MUX = CV IN
    static constexpr uint8_t  MUX_S0   = 2;
    static constexpr uint8_t  MUX_S1   = 3;
    static constexpr uint8_t  MUX_S2   = 4;
    static constexpr uint8_t  ADC_PIN  = 26;
    static constexpr float    SMOOTH   = 0.05f;
    static constexpr float    CV_SMOOTH= 0.15f;  // más rápido para tracking de CV
    static constexpr uint16_t HYST     = 20;

    void  init();
    void  poll();
    float get(uint8_t idx);  // 0.0–1.0, pots 0–5

    // CV IN: valor suavizado 0.0–1.0. Listo para usarse como modulación.
    // Llamar poll() primero (ya lo hace el loop normal).
    float get_cv() const { return cv_smoothed_; }

    // true si hay señal CV presente (detectado por superar NOISE_FLOOR)
    bool  cv_active() const { return cv_smoothed_ > CV_NOISE_FLOOR; }

private:
    void     select_channel(uint8_t ch);
    uint16_t read_adc();

    float    smoothed_[NUM_POTS] = {};
    uint16_t last_raw_[NUM_POTS] = {};

    // CV IN state
    float    cv_smoothed_          = 0.0f;
    static constexpr float CV_NOISE_FLOOR = 0.02f;  // <2% = ruido, ignorar
};
