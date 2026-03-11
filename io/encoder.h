#pragma once
#include <stdint.h>
#include "pico/types.h"

class Encoder {
public:
    void init(uint pin_a, uint pin_b, uint pin_sw);
    void update();
    int  read_delta();
    bool read_click();

    // Hold detection: devuelve true en el momento en que el hold se confirma
    // (una sola vez por presión, a los ~700ms). No interfiere con read_click()
    // porque si el hold se dispara, el click queda cancelado.
    bool read_hold();

    // true mientras el switch esté físicamente presionado (para saber si
    // el hold sigue activo y poder mostrar feedback visual progresivo)
    bool is_pressed() const { return last_sw_; }

private:
    uint pin_a_ = 0;
    uint pin_b_ = 0;
    uint pin_sw_ = 0;

    int  last_state_ = 0;
    int  delta_      = 0;
    bool last_sw_    = false;
    bool click_      = false;
    bool hold_       = false;
    bool hold_fired_ = false;  // evita disparar hold múltiples veces
    uint32_t sw_press_ms_ = 0;
    static constexpr uint32_t HOLD_MS = 700u;
};
