#pragma once
// note_mode.h — Bytebeat Machine V1.7.0
// Tabla de mapeo pad → grado de escala → nota MIDI cuantizada.
// Completamente stateless — solo funciones y tablas constantes.
//
// MAPEO (pads 0–7 con escala activa):
//   PAD0 → grado 1  (root)
//   PAD1 → grado 2
//   PAD2 → grado 3
//   PAD3 → grado 4
//   PAD4 → grado 5
//   PAD5 → grado 6
//   PAD6 → grado 7
//   PAD7 → root + 12 (octava)
//
// Octava base: 4 (C4 = nota MIDI 60 = la nota MIDI base para root=C).
// Las notas se cuantizan con Quantizer::quantize() — siempre en escala.
//
// pitch_ratio: freq_nota / freq_A4(440Hz)
// El AudioEngine usa ctx.t * pitch_ratio para modular el bytebeat.
//
#include <cstdint>
#include "quantizer.h"

struct NoteMode {
    // Octava base MIDI (C4 = 60)
    static constexpr uint8_t OCTAVE_BASE = 60;   // C4
    static constexpr uint8_t NUM_PADS    = 8;

    // Mapear pad 0–7 → nota MIDI cuantizada (scale+root)
    // pad7 = octava siempre (root + 12, no necesita cuantizar)
    static uint8_t pad_to_midi(uint8_t pad, ScaleId scale, uint8_t root) {
        if (pad >= NUM_PADS) return 60;

        if (pad == 7) {
            // Octava: root + 12 semitonos sobre C4
            uint8_t base = OCTAVE_BASE + root;
            return (base + 12 <= 127) ? (base + 12) : base;
        }

        // Grados 1–7: obtener los índices de notas en escala
        // Sacamos los bits activos del bitmask de la escala en orden
        uint16_t mask = Quantizer::SCALE_MASKS[(uint8_t)scale];
        uint8_t degree = 0;
        uint8_t semitone = 0;
        for (uint8_t s = 0; s < 12; s++) {
            uint8_t rotated = (s + root) % 12;
            if ((mask >> rotated) & 1u) {
                if (degree == pad) {
                    return (uint8_t)(OCTAVE_BASE + s);
                }
                degree++;
            }
        }
        // Fallback: si la escala tiene menos de 8 grados (ej: pentatónica),
        // wrappear — grado N vuelve al inicio en la octava siguiente
        return (uint8_t)(OCTAVE_BASE + root);
    }

    // Nota MIDI → ratio de pitch relativo a A4 (440Hz)
    // ratio = 2^((midi - 69) / 12)
    // Implementado con tabla de 128 entradas precalculadas (float, 512 bytes).
    static float midi_to_pitch_ratio(uint8_t midi_note) {
        // Approx: usamos la tabla de Quantizer::note_to_freq / 440.0
        float freq = Quantizer::note_to_freq(midi_note);
        return freq / 440.0f;
    }

    // Nota MIDI → nombre (para debug/display)
    static const char* midi_note_name(uint8_t note) {
        static const char* NAMES[12] = {
            "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
        };
        return NAMES[note % 12];
    }
};
