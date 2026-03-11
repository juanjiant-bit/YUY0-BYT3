#pragma once
// ring_buffer.h — buffer circular lock-free (single producer / single consumer)
// Core1 produce (push), Core0 consume (pop) — seguro sin mutex en ese escenario.
#include <cstdint>
#include <cstring>

template<typename T, uint8_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N debe ser potencia de 2");
public:
    bool push(const T& item) {
        uint8_t next = (write_ + 1) & (N - 1);
        if (next == read_) return false;  // lleno
        buf_[write_] = item;
        write_       = next;
        return true;
    }

    bool pop(T& out) {
        if (read_ == write_) return false;  // vacío
        out   = buf_[read_];
        read_ = (read_ + 1) & (N - 1);
        return true;
    }

    bool empty() const { return read_ == write_; }
    uint8_t size() const {
        return (uint8_t)((write_ - read_) & (N - 1));
    }
    static constexpr uint8_t capacity() { return (uint8_t)(N - 1); }

private:
    T                buf_[N];
    volatile uint8_t write_ = 0;
    volatile uint8_t read_  = 0;
};
