// uart_midi.cpp — Bytebeat Machine V1.5.1
#include "uart_midi.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

static constexpr uint32_t MIDI_BAUD = 31250;
static uart_inst_t* const UART      = uart0;  // no constexpr: uart0 es static
static constexpr uint8_t  TX_PIN    = 0;   // GP0
static constexpr uint8_t  RX_PIN    = 1;   // GP1

void UartMidi::init(const MidiConfig& cfg) {
    cfg_ = cfg;

    uart_init(UART, MIDI_BAUD);
    gpio_set_function(TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RX_PIN, GPIO_FUNC_UART);

    // Sin paridad, 8 bits, 1 stop — MIDI standard
    uart_set_format(UART, 8, 1, UART_PARITY_NONE);

    // FIFO habilitada para no perder bytes en bursts de clock
    uart_set_fifo_enabled(UART, true);

    status_     = 0;
    data_count_ = 0;
    expected_   = 0;
    sysex_      = false;
    running_    = false;
    clock_ticks_= 0;
}

// ── Envio ────────────────────────────────────────────────────
void UartMidi::uart_write_byte(uint8_t b) {
    uart_putc_raw(UART, (char)b);
}

void UartMidi::send_note_on(uint8_t note, uint8_t vel) {
    uart_write_byte(MIDI_NOTE_ON  | ((cfg_.tx_channel - 1) & 0x0F));
    uart_write_byte(note & 0x7F);
    uart_write_byte(vel  & 0x7F);
}

void UartMidi::send_note_off(uint8_t note, uint8_t vel) {
    uart_write_byte(MIDI_NOTE_OFF | ((cfg_.tx_channel - 1) & 0x0F));
    uart_write_byte(note & 0x7F);
    uart_write_byte(vel  & 0x7F);
}

void UartMidi::send_cc(uint8_t cc, uint8_t val) {
    uart_write_byte(MIDI_CC | ((cfg_.tx_channel - 1) & 0x0F));
    uart_write_byte(cc  & 0x7F);
    uart_write_byte(val & 0x7F);
}

void UartMidi::send_clock()  { uart_write_byte(MIDI_CLOCK);   }
void UartMidi::send_start()  { uart_write_byte(MIDI_START);   }
void UartMidi::send_stop()   { uart_write_byte(MIDI_STOP);    }

void UartMidi::send_pot_cc(uint8_t pot_idx, float val) {
    if (!cfg_.cc_out_enable) return;
    if (pot_idx >= 5) return;
    uint8_t cc_num = cfg_.cc_out_map[pot_idx];
    if (cc_num == 0) return;
    uint8_t val7 = (uint8_t)(val * 127.0f);
    send_cc(cc_num, val7);
}

// ── Recepcion ────────────────────────────────────────────────
uint8_t UartMidi::uart_read_byte_nb() {
    if (!uart_is_readable(UART)) return 0xFF;
    return (uint8_t)uart_getc(UART);
}

void UartMidi::poll_rx() {
    // Procesar hasta 32 bytes por llamada (evitar bloquear Core1)
    for (uint8_t i = 0; i < 32; i++) {
        uint8_t b = uart_read_byte_nb();
        if (b == 0xFF) break;
        parse_byte(b);
    }
}

void UartMidi::emit_event(uint8_t status, uint8_t d1, uint8_t d2) {
    MidiEvent ev{status, d1, d2};
    rx_queue_.push(ev);
}

// Retorna cuantos data bytes espera un status byte
static uint8_t expected_data_bytes(uint8_t status) {
    uint8_t type = status & 0xF0;
    if (type == 0xC0 || type == 0xD0) return 1;  // prog change, aftertouch
    if (status >= 0xF4) return 0;                 // realtime + sysex end
    if (status == 0xF1 || status == 0xF3) return 1;
    if (status == 0xF2) return 2;
    return 2;  // note on/off, cc, pitchbend, etc.
}

void UartMidi::parse_byte(uint8_t b) {
    // ── Realtime messages (pueden llegar en cualquier momento) ──
    if (b == MIDI_CLOCK) {
        clock_ticks_++;
        return;
    }
    if (b == MIDI_START || b == MIDI_CONTINUE) {
        running_ = true;
        emit_event(b, 0, 0);
        return;
    }
    if (b == MIDI_STOP) {
        running_ = false;
        emit_event(b, 0, 0);
        return;
    }

    // ── SysEx: ignorar hasta EOX ──────────────────────────────
    if (b == MIDI_SYSEX)  { sysex_ = true;  return; }
    if (b == MIDI_SYSEX_END) { sysex_ = false; return; }
    if (sysex_) return;

    // ── Status byte ──────────────────────────────────────────
    if (b & 0x80) {
        status_     = b;
        data_count_ = 0;
        expected_   = expected_data_bytes(b);
        // Mensajes sin data bytes: emitir inmediatamente
        if (expected_ == 0) emit_event(status_, 0, 0);
        return;
    }

    // ── Data byte ────────────────────────────────────────────
    if (status_ == 0) return;  // sin status valido, ignorar

    // Filtrar canal si no es omni
    uint8_t type = status_ & 0xF0;
    if (type < 0xF0 && cfg_.rx_channel != 0) {
        uint8_t ch = (status_ & 0x0F) + 1;
        if (ch != cfg_.rx_channel) {
            // Canal incorrecto — acumular data de todas formas para
            // no romper el running status, pero no emitir
            data_[data_count_++] = b;
            if (data_count_ >= expected_) data_count_ = 0;
            return;
        }
    }

    data_[data_count_++] = b;

    if (data_count_ >= expected_) {
        emit_event(status_, data_[0], expected_ > 1 ? data_[1] : 0);
        data_count_ = 0;
        // Running status: mantener status_ para el proximo mensaje
    }
}

bool UartMidi::consume_clock_tick() {
    if (clock_ticks_ == 0) return false;
    clock_ticks_--;
    return true;
}
