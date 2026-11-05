#pragma once
// Datos PCM del decodificador dr_mp3 para la salida I2S legacy.
// Definidos en una cabecera independiente para el preprocesador Arduino .ino.
struct PCMFrameInfoCommunity {
  int samprate;
  int nChans;
  int bitrate;
};
