#pragma once
// uart_midi.h — Bytebeat Machine V1.5.1
// MIDI IN/OUT via UART0 (GP0=TX, GP1=RX) a 31250 baud.
//
// CIRCUITO MIDI IN:
//   DIN pin 4 -> 220R -> 6N138 pin 3 (anode)
//   DIN pin 2 -> 6N138 pin 2 (cathode) -> GND
//   6N138 pin 5 (collector) -> 470R -> 3V3
//   6N138 pin 5 ----------------------> GP1 (UART0 RX)
//   6N138 pin 6 (emitter)  -> GND
//   6N138 pin 8 (Vcc)      -> 3V3
//
// CIRCUITO MIDI OUT:
//   GP0 (UART0 TX) -> 74HC14 pin1 (in) -> 74HC14 pin2 (out)
//   74HC14 pin2 -> 220R -> DIN pin 5
//   DIN pin 4   -> 220R -> 3V3 (corriente de loop)
//   DIN pin 2   -> GND
//   (Alternativamente: GP0 directo + transistor NPN BC547)
//
// NOTA: UART0 comparte GP0/GP1 con UART debug stdio.
//       Compilar con pico_enable_stdio_uart(target 0) si se usa MIDI.
//       El debug queda por USB (stdio_usb sigue activo).
//
#include <cstdint>
#include "../sequencer/event_types.h"
#include "../utils/ring_buffer.h"

// ── Tipos MIDI ───────────────────────────────────────────────
struct MidiEvent {
    uint8_t status;   // status byte (tipo + canal)
    uint8_t data1;    // nota / CC number / 0 para realtime
    uint8_t data2;    // velocity / CC value / 0 para realtime
};

enum MidiStatus : uint8_t {
    MIDI_NOTE_OFF   = 0x80,
    MIDI_NOTE_ON    = 0x90,
    MIDI_CC         = 0xB0,
    MIDI_PROG_CHG   = 0xC0,
    MIDI_PITCHBEND  = 0xE0,
    MIDI_CLOCK      = 0xF8,
    MIDI_START      = 0xFA,
    MIDI_CONTINUE   = 0xFB,
    MIDI_STOP       = 0xFC,
    MIDI_SYSEX      = 0xF0,
    MIDI_SYSEX_END  = 0xF7,
};

// ── Configuracion ────────────────────────────────────────────
struct MidiConfig {
    uint8_t rx_channel;   // canal MIDI IN de escucha (1-16, 0=omni)
    uint8_t tx_channel;   // canal MIDI OUT (1-16)

    // Mapeo CC IN -> parametro interno
    // cc_map[i] = numero de CC que controla pot i (0=desactivado)
    uint8_t cc_map[6];    // pots 0-5 (V1.10: pot 5 = envelope)

    // Mapeo Note IN -> snapshot trigger
    // note_map[i] = nota MIDI que dispara snapshot i (0=desactivado)
    uint8_t note_map[8];  // snapshots 0-7

    // CC OUT: enviar valores de pot como CC
    bool    cc_out_enable;
    uint8_t cc_out_map[6]; // CC number para cada pot (V1.10: pot 5 = envelope)

    // Clock OUT: enviar MIDI clock
    bool    clock_out_enable;

    // Clock IN: sincronizar BPM con MIDI clock
    bool    clock_in_enable;
};

// ── UartMidi ─────────────────────────────────────────────────
class UartMidi {
public:
    void init(const MidiConfig& cfg);
    void set_config(const MidiConfig& cfg) { cfg_ = cfg; }

    // Llamar desde Core1 loop (~cada 1ms)
    // Procesa bytes UART disponibles y llena midi_queue_
    void poll_rx();

    // Enviar eventos MIDI OUT
    void send_note_on (uint8_t note, uint8_t vel);
    void send_note_off(uint8_t note, uint8_t vel = 0);
    void send_cc      (uint8_t cc,   uint8_t val);
    void send_clock   ();
    void send_start   ();
    void send_stop    ();

    // Pot -> CC OUT (val 0.0-1.0 -> 0-127)
    void send_pot_cc(uint8_t pot_idx, float val);

    // Cola de eventos MIDI IN para consumir en Core1
    bool pop_event(MidiEvent& ev) { return rx_queue_.pop(ev); }
    bool has_event()        const { return !rx_queue_.empty(); }

    // Clock IN: ticks recibidos (consumir con consume_clock_tick)
    bool consume_clock_tick();
    bool is_running() const { return running_; }

private:
    void     parse_byte(uint8_t b);
    void     emit_event(uint8_t status, uint8_t d1, uint8_t d2);
    uint8_t  uart_read_byte_nb();  // non-blocking, returns 0xFF si vacio
    void     uart_write_byte(uint8_t b);

    MidiConfig cfg_ = {};

    // Parser estado
    uint8_t  status_       = 0;    // running status
    uint8_t  data_[2]      = {};
    uint8_t  data_count_   = 0;
    uint8_t  expected_     = 0;    // bytes esperados para mensaje actual
    bool     sysex_        = false;

    // Colas
    RingBuffer<MidiEvent, 32>  rx_queue_;
    volatile uint32_t          clock_ticks_ = 0;
    bool                       running_     = false;
};
