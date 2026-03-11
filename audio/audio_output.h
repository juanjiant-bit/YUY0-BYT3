#pragma once
// audio_output.h — abstracción de salida de audio
// Permite swap PWM → I2S sin tocar el motor de síntesis
#include <cstdint>

class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual void init()  = 0;
    virtual void write(int16_t left, int16_t right) = 0;
    virtual void start() {}
    virtual void stop()  {}
};
