#include "encoder.h"
#include "hardware/gpio.h"
#include "pico/time.h"

void Encoder::init(uint pin_a, uint pin_b, uint pin_sw)
{
    pin_a_ = pin_a;
    pin_b_ = pin_b;
    pin_sw_ = pin_sw;

    gpio_init(pin_a_);
    gpio_init(pin_b_);
    gpio_init(pin_sw_);

    gpio_set_dir(pin_a_, GPIO_IN);
    gpio_set_dir(pin_b_, GPIO_IN);
    gpio_set_dir(pin_sw_, GPIO_IN);

    gpio_pull_up(pin_a_);
    gpio_pull_up(pin_b_);
    gpio_pull_up(pin_sw_);

    last_state_ = (gpio_get(pin_a_) << 1) | gpio_get(pin_b_);
    last_sw_ = !gpio_get(pin_sw_);
}

void Encoder::update()
{
    int state = (gpio_get(pin_a_) << 1) | gpio_get(pin_b_);
    if (state != last_state_) {
        if ((last_state_ == 0 && state == 1) ||
            (last_state_ == 1 && state == 3) ||
            (last_state_ == 3 && state == 2) ||
            (last_state_ == 2 && state == 0)) {
            delta_++;
        } else {
            delta_--;
        }
        last_state_ = state;
    }

    bool sw = !gpio_get(pin_sw_);

    if (sw && !last_sw_) {
        // flanco de bajada: empieza presión
        sw_press_ms_ = to_ms_since_boot(get_absolute_time());
        hold_fired_  = false;
    }

    if (sw && !hold_fired_) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - sw_press_ms_;
        if (elapsed >= HOLD_MS) {
            hold_       = true;
            hold_fired_ = true;
            // Cancela el click: si el usuario hizo hold, no queremos
            // que al soltar se dispare también un click accidental
            click_ = false;
        }
    }

    if (!sw && last_sw_) {
        // flanco de subida: soltar
        if (!hold_fired_) {
            // presión corta → click normal
            click_ = true;
        }
        // si hubo hold, click_ ya fue cancelado arriba
    }

    last_sw_ = sw;
}

int Encoder::read_delta()
{
    int d = delta_;
    delta_ = 0;
    return d;
}

bool Encoder::read_click()
{
    bool c = click_;
    click_ = false;
    return c;
}

bool Encoder::read_hold()
{
    bool h = hold_;
    hold_ = false;
    return h;
}
