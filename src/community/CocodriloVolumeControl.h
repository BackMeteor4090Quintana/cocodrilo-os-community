#ifndef COCODRILO_VOLUME_CONTROL_H
#define COCODRILO_VOLUME_CONTROL_H

#include <Arduino.h>

namespace CocodriloVolumeControl {

// Volumen global de COCODRILO OS, compartido por launcher y Retro-Go.
// 0 = silencio, 10 = máximo.
uint8_t get();
void set(uint8_t paso);

} // namespace CocodriloVolumeControl

#endif
