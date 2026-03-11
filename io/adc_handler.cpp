// adc_handler.cpp — Bytebeat Machine V1.10
// CAMBIOS: loop ahora itera hasta NUM_POTS=6 (CH5 automáticamente incluido).
#include "adc_handler.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

void AdcHandler::init() {
    gpio_init(MUX_S0); gpio_set_dir(MUX_S0, GPIO_OUT);
    gpio_init(MUX_S1); gpio_set_dir(MUX_S1, GPIO_OUT);
    gpio_init(MUX_S2); gpio_set_dir(MUX_S2, GPIO_OUT);
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(0);
}

void AdcHandler::select_channel(uint8_t ch) {
    gpio_put(MUX_S0,  ch & 0x01);
    gpio_put(MUX_S1, (ch >> 1) & 0x01);
    gpio_put(MUX_S2, (ch >> 2) & 0x01);
}

uint16_t AdcHandler::read_adc() { return adc_read(); }

void AdcHandler::poll() {
    for (uint8_t i = 0; i < NUM_POTS; i++) {
        select_channel(i);
        sleep_us(20);

        uint16_t raw = read_adc();
        int32_t delta = (int32_t)raw - (int32_t)last_raw_[i];
        if (delta < 0) delta = -delta;
        if (delta < (int32_t)HYST) raw = last_raw_[i];
        else                       last_raw_[i] = raw;

        float target  = raw / 4095.0f;
        smoothed_[i] += SMOOTH * (target - smoothed_[i]);
    }

    // ── CV IN: CH6 del MUX ───────────────────────────────────────
    // El divisor resistivo 18kΩ/33kΩ + BAT43 limita a 0–3.3V el rango.
    // Leemos sin hysteresis (CV puede moverse rápido y queremos tracking limpio).
    // El suavizado CV_SMOOTH (0.15) es más rápido que el de los pots (0.05)
    // para seguir LFOs lentos pero rechazar picos de ruido.
    select_channel(CV_CH);
    sleep_us(20);
    float cv_raw  = read_adc() / 4095.0f;
    // Clamp defensivo por si el divisor deja escapar algo fuera de rango
    if (cv_raw < 0.0f) cv_raw = 0.0f;
    if (cv_raw > 1.0f) cv_raw = 1.0f;
    cv_smoothed_ += CV_SMOOTH * (cv_raw - cv_smoothed_);
}

float AdcHandler::get(uint8_t idx) {
    if (idx >= NUM_POTS) return 0.0f;
    return smoothed_[idx];
}
