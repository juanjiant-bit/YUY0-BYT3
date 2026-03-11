#pragma once
// Shared hardware pin definitions
// Encoder remapped away from the pad matrix / audio PWM pins.
// Encoder RGB LED support was removed; page feedback is shown on the WS2812 bar.

#define PIN_AUDIO_PWM   18
#define PIN_NEOPIXEL    22
#define PIN_ONBOARD_LED 25   // GP25 = LED onboard del Pico (no Pico W)

#define ENC_A_PIN  19
#define ENC_B_PIN  20
#define ENC_SW_PIN 21

