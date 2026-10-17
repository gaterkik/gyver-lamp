#pragma once
#include <Arduino.h>
#include <vector>

// Плоская статическая C-структура для нулевой фрагментации кучи
struct PaletteStop {
  uint8_t pos;
  uint8_t r, g, b;
};

struct ActiveModularEffect {
  bool valid;
  char id[24];
  char name[36];
  uint8_t speedMs;
  uint8_t scaleX;
  uint8_t scaleY;
  uint8_t speedTime;
  PaletteStop palette[8];
  uint8_t paletteCount;
};

struct InstalledFxItem {
  String id;
  String name;
  bool inBtn;
};
