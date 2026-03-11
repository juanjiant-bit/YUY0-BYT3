// audio_output_pwm.cpp — V1.4.4
#include "audio_output_pwm.h"
#include "hardware/gpio.h"

void AudioOutputPWM::init() {
    gpio_set_function(PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(PIN_R, GPIO_FUNC_PWM);

    slice_ = pwm_gpio_to_slice_num(PIN_L);  // slice 5

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, PWM_WRAP);
    pwm_init(slice_, &cfg, true);

    // Silencio: mitad de escala
    pwm_set_both_levels(slice_, PWM_WRAP / 2, PWM_WRAP / 2);
}

void AudioOutputPWM::write(int16_t left, int16_t right) {
    // Conversión sin división: shift Q16
    // level = (sample + 32768) * (PWM_WRAP+1) >> 16
    int32_t l = ((int32_t)left  + 32768) * (PWM_WRAP + 1);
    int32_t r = ((int32_t)right + 32768) * (PWM_WRAP + 1);

    auto clamp = [](int32_t v) -> uint16_t {
        if (v < 0)             return 0;
        if ((v>>16) > PWM_WRAP) return PWM_WRAP;
        return (uint16_t)(v >> 16);
    };

    pwm_set_both_levels(slice_, clamp(l), clamp(r));
}
