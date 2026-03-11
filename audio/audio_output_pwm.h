#pragma once
// audio_output_pwm.h — V1.4.4
// GP10 = PWM Left, GP11 = PWM Right (slice 5)
// PWM_WRAP=499 → ~250kHz, ~9 bits efectivos
#include "audio_output.h"
#include "hardware/pwm.h"

class AudioOutputPWM : public AudioOutput {
public:
    void init()  override;
    void write(int16_t left, int16_t right) override;

    static constexpr uint8_t  PIN_L    = 10;
    static constexpr uint8_t  PIN_R    = 11;
    static constexpr uint16_t PWM_WRAP = 499;

private:
    uint slice_ = 0;
};
