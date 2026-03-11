# Bytebeat Machine — Firmware V1.14

Sintetizador generativo DIY basado en RP2040. Genera audio mediante evaluación sample a sample de fórmulas bytebeat representadas como árboles AST aleatorios.

## Features

| Feature | Estado |
|---|---|
| Dual-core: Core0 audio @ 44100 Hz, Core1 control | Activo |
| 15 pads capacitivos (matriz 3×5) | Activo |
| 6 potenciómetros via 74HC4051 MUX | Activo |
| 8 snapshots con persistencia en flash | Activo |
| DSP: Reverb + Chorus + HP Filter + Grain Freeze + Snap Gate | Activo |
| MIDI IN/OUT + Clock IN/OUT Eurorack | Activo |
| Note Mode (pads = grados de escala) | Activo |
| Encoder global: BPM / Swing / Root / Scale / Mutate | Activo |
| Swing real en el clock interno | Activo |
| Mutate de snapshot + secuencia generativa | Activo |
| Spread estéreo optimizado por segmentos | Activo |

## Hardware

- **MCU**: RP2040 (Raspberry Pi Pico o Ultimate Pico 16MB)
- **MUX**: 74HC4051 para 6 potenciómetros RV09
- **Audio**: PWM estéreo GP10/GP11 → filtro RC 3 polos → jack 3.5mm
- **MIDI IN**: 6N138 optoacoplador (pin 8 a 3V3 obligatorio)
- **MIDI OUT**: BC547 NPN
- **Clock I/O**: BAT43 clamp en GP16, 470Ω en GP17
- **Pads**: 1MΩ por fila, 100nF desacople, sin pull interno en COL pins

## Compilar

### GitHub Actions (recomendado)

Push al repositorio → el workflow `.github/workflows/build.yml` genera el UF2 automáticamente. Descargar desde la pestaña **Actions → Artifacts**.

### Local

```bash
# 1. Clonar Pico SDK
git clone --depth 1 --branch 2.1.0 https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..

# 2. Exportar path
export PICO_SDK_PATH=$(pwd)/pico-sdk

# 3. Compilar
mkdir build && cd build
cmake .. && make -j$(nproc)

# 4. Flash — modo BOOTSEL
cp bytebeat_machine.uf2 /media/$(whoami)/RPI-RP2/
```

## Flashear

1. Mantener **BOOTSEL** presionado mientras conectas USB
2. Aparece como unidad `RPI-RP2`
3. Copiar `bytebeat_machine.uf2` a esa unidad
4. La placa reinicia automáticamente

**Clone Ultimate Pico (16MB)**: si no aparece como RPI-RP2, mantener BOOTSEL más tiempo o usar `picotool load`.

## Mapeo de Controles

### Pots (3 capas)

| POT | Normal | SHIFT | SHIFT + REC |
|-----|--------|-------|-------------|
| 0 | Macro | Glide | Reverb Room |
| 1 | Tonal | Env Attack | Reverb Wet |
| 2 | Spread | Env Release | Chorus |
| 3 | Drive | Stutter Rate | Drum Decay |
| 4 | Time Div | Grain | Drum Color |
| 5 | Snap Gate | HP Filter | Duck Amount |

### Gestos

- **SHIFT + REC + PLAY**: Flash Save
- **SHIFT + PLAY**: Toggle Note Mode
- **Pads C/F/A/D/G/B/E/H**: snapshots 1–8
- **SHIFT + C/F/A/D/G/B/E**: punch FX momentáneos
- **SHIFT + H**: snapshot arp momentáneo
- **SHIFT + MUTE**: toggle Env Loop
- **MUTE + KICK/SNARE/HAT**: mute por drum
- **SHIFT + KICK + SNARE**: randomize / mutate global

## Changelog

- **V1.14**: encoder global, capas de potes reordenadas, swing real, mutate estabilizado, spread estéreo optimizado
- **V1.10**: envelope AR con loop, flash store, sexta capa de control consolidada
- **V1.7.2**: chorus, HP filter, grain freeze y snap gate
- **V1.7**: Note Mode y control performático de drums
- **V1.0**: base dual-core, pads, MIDI y clock I/O


## Encoder page feedback

- No hay LED RGB dedicado para el encoder.
- La barra WS2812 muestra la página del encoder durante ~1 segundo al cambiar de modo.
- Colores: BPM=verde, Swing=amarillo, Root=azul, Scale=violeta, Mutate=rojo.


## Stage 22
- Note Mode steps now record as note events per step (not snapshot triggers).
- Sequencer playback emits NOTE ON/OFF per logical pad lane, preserving free-length step capture and overdub.


Sequencer edit: in step-write/overdub, go to the MUTATE encoder page, then SHIFT + encoder adjusts the chance/probability of the current step. SHIFT + encoder click on MUTATE resets that step to 100%.


## Groove Engine
- Encoder page SWING: rotate = swing amount
- SHIFT + rotate on SWING page = groove template (Straight / MPC / Shuffle / Broken / Triplet)
- Groove is lane-aware: drums swing most, notes slightly, snapshots stay mostly straight.

- In step-write/overdub, hold SHIFT + a snapshot pad to copy the current 8-step page.
- In step-write/overdub, hold SHIFT + PLAY + a snapshot pad to paste the copied 8-step page.


## FX feel improvements
- Repeat 4/8/16 ahora usa loops más dedicados y entradas/salidas más suaves.
- Freeze punch usa grain hold real sin depender del pote.
- Vibrato y cambios de octava tienen rampas cortas para sonar más musicales.
- Chorus y reverb recibieron curvas internas más musicales.


## Salida de audio actual

El firmware ahora arranca por default con backend **PCM5102 I2S** usando PIO del RP2040.

Cableado propuesto:
- `GP10 -> BCK`
- `GP11 -> LCK / LRCK / WS`
- `GP12 -> DIN`
- `3V3 o 5V -> VIN` (según tu módulo)
- `GND -> GND`

Notas:
- el backend PWM original sigue presente en el proyecto como fallback rápido
- el PCM5102 no necesita MCLK para el uso básico del proyecto
- esta integración usa un stream estéreo de 16 bits por canal sobre PIO


## Macro Motion
- Macro sigue modulando el Bytebeat Graph, pero ahora también actúa como profundidad de auto-modulación derivada del propio bytebeat.
- Moduladores internos: **Rhythm**, **Chaos** y **Density**.
- Primera aplicación: paneo dinámico, drive, grain, chorus y reverb send modulados por el patrón matemático del snapshot.
