#pragma once
#include <Arduino.h>
#include <ESP_I2S.h>

// Точная таблица расстояний от центра (7.5, 7.5) матрицы 16х16, масштабированная 0..255
const uint8_t distCenter[16][16] = {
  {255, 239, 224, 210, 199, 190, 184, 181, 181, 184, 190, 199, 210, 224, 239, 255},
  {239, 221, 205, 190, 177, 167, 160, 157, 157, 160, 167, 177, 190, 205, 221, 239},
  {224, 205, 187, 171, 157, 145, 137, 133, 133, 137, 145, 157, 171, 187, 205, 224},
  {210, 190, 171, 153, 137, 124, 114, 109, 109, 114, 124, 137, 153, 171, 190, 210},
  {199, 177, 157, 137, 119, 103,  92,  85,  85,  92, 103, 119, 137, 157, 177, 199},
  {190, 167, 145, 124, 103,  85,  70,  61,  61,  70,  85, 103, 124, 145, 167, 190},
  {184, 160, 137, 114,  92,  70,  51,  38,  38,  51,  70,  92, 114, 137, 160, 184},
  {181, 157, 133, 109,  85,  61,  38,  17,  17,  38,  61,  85, 109, 133, 157, 181},
  {181, 157, 133, 109,  85,  61,  38,  17,  17,  38,  61,  85, 109, 133, 157, 181},
  {184, 160, 137, 114,  92,  70,  51,  38,  38,  51,  70,  92, 114, 137, 160, 184},
  {190, 167, 145, 124, 103,  85,  70,  61,  61,  70,  85, 103, 124, 145, 167, 190},
  {199, 177, 157, 137, 119, 103,  92,  85,  85,  92, 103, 119, 137, 157, 177, 199},
  {210, 190, 171, 153, 137, 124, 114, 109, 109, 114, 124, 137, 153, 171, 190, 210},
  {224, 205, 187, 171, 157, 145, 137, 133, 133, 137, 145, 157, 171, 187, 205, 224},
  {239, 221, 205, 190, 177, 167, 160, 157, 157, 160, 167, 177, 190, 205, 221, 239},
  {255, 239, 224, 210, 199, 190, 184, 181, 181, 184, 190, 199, 210, 224, 239, 255}
};

class AudioAnalyzer {
public:
  AudioAnalyzer() 
    : initialized(false), soundDetected(false),
      volume(0), rawVolume(0), beatIntensity(0),
      lastBeatTime(0), lastSoundTime(0),
      sensitivity(50), maxLevel(400),
      lastRaw(0), lastFiltered(0),
      fastAvg(0), slowAvg(0) {}

  bool begin(int8_t bclk, int8_t ws, int8_t din) {
    _bclk = bclk;
    _ws = ws;
    _din = din;

    // Внутренняя подтяжка к земле на случай, если микрофон ещё не припаян
    // Предотвращает ложный шум и наводки на висящем в воздухе пине данных
    pinMode(_din, INPUT_PULLDOWN);

    i2s.setPins(_bclk, _ws, -1, _din);
    // 16 kHz, 32-bit slot, Stereo Philips format
    bool ok = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    if (ok) {
      initialized = true;
      Serial.printf("[Audio] I2S INMP441 инициализирован (SCK=%d, WS=%d, SD=%d)\n", _bclk, _ws, _din);
    } else {
      Serial.println("[Audio] Ошибка инициализации I2S!");
      initialized = false;
    }
    return ok;
  }

  void update() {
    if (!initialized) {
      soundDetected = false;
      volume = 0;
      beatIntensity = 0;
      return;
    }

    // 32 стерео-сэмпла (64 x 32-бит = 256 байт буфер)
    int32_t samples[64];
    size_t bytesRead = 0;

    // Чтение DMA с таймаутом 0 мс (не блокирует процессор)
    esp_err_t err = i2s_channel_read(i2s.rxChan(), samples, sizeof(samples), &bytesRead, 0);

    if (err == ESP_OK && bytesRead >= sizeof(int32_t) * 2) {
      size_t numSamples = bytesRead / (sizeof(int32_t) * 2);
      uint32_t sumAmp = 0;

      for (size_t i = 0; i < numSamples; i++) {
        // Левый канал (L/R соединен с GND)
        int32_t raw = samples[i * 2] >> 14;

        // Фильтр отсечки постоянной составляющей (DC blocking)
        int32_t filtered = raw - lastRaw + ((lastFiltered * 124) >> 7);
        lastRaw = raw;
        lastFiltered = filtered;

        sumAmp += abs(filtered);
      }

      uint32_t blockAvg = numSamples > 0 ? (sumAmp / numSamples) : 0;
      rawVolume = constrain(blockAvg, 0, 1023);

      // Проверка наличия звука (порог шума наводок)
      if (rawVolume > 15) {
        soundDetected = true;
        lastSoundTime = millis();
      } else if (millis() - lastSoundTime > 2000) {
        soundDetected = false;
      }

      if (soundDetected) {
        // Адаптивное АРУ (Automatic Gain Control)
        if (rawVolume > maxLevel) {
          maxLevel = rawVolume;
        } else {
          static unsigned long lastAgcDecay = 0;
          if (millis() - lastAgcDecay > 40) {
            lastAgcDecay = millis();
            if (maxLevel > 60) maxLevel = (maxLevel * 254) >> 8;
          }
        }

        uint32_t scaled = (rawVolume * 255UL * sensitivity) / (max(maxLevel, 60UL) * 50UL);
        volume = constrain(scaled, 0, 255);

        // Детектор битов (акцент на басовых ударах)
        fastAvg = (fastAvg * 3 + volume) / 4;
        slowAvg = (slowAvg * 31 + volume) / 32;

        unsigned long now = millis();
        if (fastAvg > ((slowAvg * 12) / 10) && fastAvg > 35 && (now - lastBeatTime > 160)) {
          lastBeatTime = now;
          beatIntensity = 255;
        }
      } else {
        // В тишине или без звука: громкость строго 0!
        volume = 0;
        beatIntensity = 0;
      }
    } else {
      soundDetected = false;
      volume = 0;
      beatIntensity = 0;
    }

    // Экспоненциальное затухание энергии удара
    if (beatIntensity > 0) {
      beatIntensity = (beatIntensity * 210) >> 8;
    }
  }

  static uint8_t sin8(uint8_t angle) {
    static const uint8_t sinTable[64] = {
      128, 140, 153, 165, 177, 188, 199, 209, 218, 226, 234, 240, 245, 250, 253, 254,
      255, 254, 253, 250, 245, 240, 234, 226, 218, 209, 199, 188, 177, 165, 153, 140,
      128, 115, 102, 90,  78,  67,  56,  46,  37,  29,  21,  15,  10,  5,   2,   1,
      0,   1,   2,   5,   10,  15,  21,  29,  37,  46,  56,  67,  78,  90,  102, 115
    };
    return sinTable[(angle >> 2) & 63];
  }

  uint8_t getVolume() const { return soundDetected ? volume : 0; }
  uint16_t getRawVolume() const { return soundDetected ? rawVolume : 0; }
  uint8_t getBeatIntensity() const { return soundDetected ? beatIntensity : 0; }
  bool isSound() const { return soundDetected; }
  bool isReady() const { return initialized; }

  void setSensitivity(uint8_t s) { sensitivity = constrain(s, 10, 100); }
  uint8_t getSensitivity() const { return sensitivity; }

private:
  I2SClass i2s;
  int8_t _bclk, _ws, _din;
  bool initialized;
  bool soundDetected;

  uint8_t volume;
  uint16_t rawVolume;
  uint8_t beatIntensity;
  unsigned long lastBeatTime;
  unsigned long lastSoundTime;

  uint8_t sensitivity;
  uint32_t maxLevel;

  int32_t lastRaw;
  int32_t lastFiltered;

  uint32_t fastAvg;
  uint32_t slowAvg;
};

extern AudioAnalyzer audio;
