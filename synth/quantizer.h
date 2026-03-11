#pragma once
// quantizer.h — Bytebeat Machine V1.6
// Cuantizador de pitch por escalas diatónicas.
// Completamente CPU-barato: bitmask de 12 bits por escala.
// Sin malloc, sin float pesado, sin dependencias externas.
//
// USO:
//   uint8_t midi_note = Quantizer::quantize(pitch_raw, scale_id, root);
//   float   freq_hz   = Quantizer::note_to_freq(midi_note);
//
// LEAD TONAL:
//   El quantizer convierte el byte alto del bytebeat (pitch_raw 0-127)
//   a la nota MIDI más cercana de la escala. El oscilador de lead
//   usa esa frecuencia para generar una onda senoidal limpia.
//
// AMOUNT (crossfade bytebeat <-> lead tonal):
//   amount=0.0 → solo bytebeat libre
//   amount=0.5 → mezcla 50/50
//   amount=1.0 → solo lead tonal (cuantizado puro)
//
#include <cstdint>
#include <cmath>

// ── Escalas (bitmask 12 bits, bit0=C, bit1=C#, ... bit11=B) ──
enum class ScaleId : uint8_t {
    CHROMATIC   = 0,   // todos los semitonos
    MAJOR       = 1,   // C D E F G A B
    NAT_MINOR   = 2,   // C D Eb F G Ab Bb
    DORIAN      = 3,   // C D Eb F G A Bb
    PHRYGIAN    = 4,   // C Db Eb F G Ab Bb
    PENTA_MAJ   = 5,   // C D E G A
    PENTA_MIN   = 6,   // C Eb F G Bb
    WHOLETONE   = 7,   // C D E F# G# A# (alien)
    NUM_SCALES  = 8
};

struct Quantizer {
    // Bitmasks de cada escala (bit0=C, bit11=B)
    // Chromatic: todos los 12 semitonos
    // Cada 1 = nota en la escala
    static constexpr uint16_t SCALE_MASKS[8] = {
        0b111111111111,  // CHROMATIC   (12 notas)
        0b101011010101,  // MAJOR       C D E F G A B
        0b101101011010,  // NAT_MINOR   C D Eb F G Ab Bb  (corregido)
        0b101101010110,  // DORIAN      C D Eb F G A Bb
        0b110101011010,  // PHRYGIAN    C Db Eb F G Ab Bb (corregido)
        0b100010100101,  // PENTA_MAJ   C D E G A
        0b100101010010,  // PENTA_MIN   C Eb F G Bb       (corregido)
        0b010101010101,  // WHOLETONE   C D E F# G# A#
    };

    // Tabla de frecuencias MIDI 0-127 (Hz × 1000 para int, pero usamos float)
    // A4=69=440Hz. freq = 440 * 2^((note-69)/12)
    // Precalculada para notas 24-96 (rango útil para el lead)
    // Fuera de ese rango: clamp.
    static float note_to_freq(uint8_t note) {
        if (note > 127) note = 127;
        // 440 * 2^((note-69)/12)
        // Aproximación: tabla de lookup implícita via pow2 barato
        // En RP2040 con FPU disponible, un powf es OK (no en hot path)
        int8_t  delta = (int8_t)note - 69;
        float   freq  = 440.0f;
        // Multiplicar/dividir por 2^(1/12) iterativamente — más barato que powf
        // Para delta positivo: subir semitonos
        // Para delta negativo: bajar semitonos
        // Usamos la constante 2^(1/12) ≈ 1.05946f
        if (delta > 0) {
            for (int8_t i = 0; i < delta; i++) freq *= 1.05946f;
        } else {
            for (int8_t i = 0; i > delta; i--) freq /= 1.05946f;
        }
        return freq;
    }

    // Cuantizar: dado un pitch_raw 0-127 (nota MIDI libre),
    // devuelve la nota más cercana EN la escala (con root transpuesto).
    // Operación O(12) — siempre 12 iteraciones max.
    static uint8_t quantize(uint8_t pitch_raw, ScaleId scale, uint8_t root) {
        if (scale == ScaleId::CHROMATIC) return pitch_raw;

        uint16_t mask = SCALE_MASKS[(uint8_t)scale];
        uint8_t  note = pitch_raw;

        // Transponer root: girar el bitmask root posiciones
        // Escala relativa al root: semitono = (note - root + 12) % 12
        uint8_t  octave   = note / 12;
        uint8_t  semitone = (note - root + 120) % 12;  // relativo al root

        // Buscar el semitono más cercano en la escala
        // Primero buscamos hacia abajo, luego hacia arriba, tomamos el más cercano
        uint8_t  best      = semitone;
        uint8_t  best_dist = 12;

        for (int8_t d = 0; d <= 6; d++) {
            // hacia abajo
            uint8_t dn = (semitone - d + 12) % 12;
            if (mask & (1u << dn)) {
                if ((uint8_t)d < best_dist) { best = dn; best_dist = d; }
                break;
            }
        }
        for (int8_t d = 1; d <= 6; d++) {
            // hacia arriba
            uint8_t up = (semitone + d) % 12;
            if (mask & (1u << up)) {
                if ((uint8_t)d < best_dist) { best = up; best_dist = d; }
                break;
            }
        }

        // Reconstruir nota absoluta: octave + root + best_semitone
        uint8_t quantized = (uint8_t)(octave * 12 + root + best);
        // Clamp a rango MIDI válido
        if (quantized > 127) quantized -= 12;
        if (quantized < 24)  quantized += 12;
        return quantized;
    }

    // Crossfade entre pitch libre y pitch cuantizado.
    // amount=0 → pitch_raw (bytebeat puro, mapeado a nota MIDI)
    // amount=1 → pitch_quantized (cuantizado limpio)
    // Retorna frecuencia Hz para el oscilador de lead.
    static float blend_freq(uint8_t pitch_raw, uint8_t pitch_q, float amount) {
        float f_raw = note_to_freq(pitch_raw);
        float f_q   = note_to_freq(pitch_q);
        return f_raw + (f_q - f_raw) * amount;
    }

    // Curva de amount desde pot (0.0-1.0 → quant_amount efectivo)
    // 0.00-0.25 → 0.0    (zona libre/atonal — no cuantiza)
    // 0.25-0.60 → 0.0-0.6 (cuantizado suave, lineal)
    // 0.60-1.00 → 0.6-1.0 (cuantizado fuerte, acelerado)
    static float pot_to_amount(float pot) {
        if (pot < 0.25f) return 0.0f;
        if (pot < 0.60f) return (pot - 0.25f) / 0.35f * 0.6f;
        return 0.6f + (pot - 0.60f) / 0.40f * 0.4f;
    }

    // Root favoritos para SHIFT+SNAP A-E
    // C=0, D#=3, F=5, G=7, A#=10
    static constexpr uint8_t FAVORITE_ROOTS[5] = { 0, 3, 5, 7, 10 };

    // Nombres para display/debug
    static const char* scale_name(ScaleId s) {
        switch (s) {
        case ScaleId::CHROMATIC:  return "CHROM";
        case ScaleId::MAJOR:      return "MAJOR";
        case ScaleId::NAT_MINOR:  return "MINOR";
        case ScaleId::DORIAN:     return "DORIA";
        case ScaleId::PHRYGIAN:   return "PHRYG";
        case ScaleId::PENTA_MAJ:  return "PMAJ";
        case ScaleId::PENTA_MIN:  return "PMIN";
        case ScaleId::WHOLETONE:  return "WHOLE";
        default:                   return "?";
        }
    }

    static const char* root_name(uint8_t root) {
        static const char* names[] = {
            "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
        };
        return names[root % 12];
    }
};
