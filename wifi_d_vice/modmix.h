#pragma once
#include <Arduino.h>

// Real 4-channel ProTracker MOD player: parses the 31-sample "M.K." format
// (headers, order table, 64-row patterns, and the 8-bit signed sample PCM)
// and software-mixes the four Paula channels into an 8-bit unsigned stream
// for the ESP32 DAC. Linear interpolation; effects 0/1/2/3/4/5/6/9/A/B/C/
// D/F (arpeggio, portamento, tone-porta, vibrato, sample offset, volume
// slide, position jump, set volume, pattern break, speed/tempo). The song
// loops forever.

bool modmixLoad(const uint8_t *data, size_t len);   // data may be in PROGMEM
void modmixStart();                                 // reset transport to order 0
void modmixFree();

// Render `n` mixed samples (8-bit unsigned, 128 == silence) into `out`,
// advancing the sequencer by that much time. Call from the audio task.
void modmixRender(uint8_t *out, int n);

// Output sample rate the mixer assumes -- match the DAC's configured rate.
void modmixSetRate(int hz);
