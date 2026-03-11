#pragma once
// debug_log.h — Bytebeat Machine V1.6.1
// Macros de debug por dominio. DESACTIVADOS por defecto.
//
// Para activar, pasar flags al compilador en CMakeLists.txt:
//   target_compile_definitions(bytebeat_machine PRIVATE
//       DEBUG_AUDIO     # logs del audio engine, DSP, snapshots, flash
//       DEBUG_PADS      # logs de calibración y scan de pads capacitivos
//       DEBUG_CLOCK     # logs del clock in/out, BPM detection, MIDI clock
//   )
//
// Uso:
//   LOG_AUDIO("DSP drive=%f", drive_val);
//   LOG_PADS ("pad%02u baseline=%lu", idx, baseline);
//   LOG_CLOCK("BPM=%f source=EXT", bpm);
//
// LOG genérico (backward compat — se activa si cualquier dominio está activo)
//

#include <stdio.h>

#if defined(DEBUG_AUDIO)
  #define LOG_AUDIO(fmt, ...) printf("[AUDIO] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_AUDIO(fmt, ...) ((void)0)
#endif

#if defined(DEBUG_PADS)
  #define LOG_PADS(fmt, ...) printf("[PADS] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_PADS(fmt, ...) ((void)0)
#endif

#if defined(DEBUG_CLOCK)
  #define LOG_CLOCK(fmt, ...) printf("[CLOCK] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_CLOCK(fmt, ...) ((void)0)
#endif

// LOG genérico — compatible con código existente que usa LOG(...)
#if defined(DEBUG_AUDIO) || defined(DEBUG_PADS) || defined(DEBUG_CLOCK)
  #define LOG_ENABLE
  #define LOG(fmt, ...) printf("[LOG] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG(fmt, ...) ((void)0)
#endif
