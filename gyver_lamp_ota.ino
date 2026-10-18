#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
#include <LittleFS.h>
#include <vector>
#include "modular_effects.h"
#include "audio_analyzer.h"

#define FIRMWARE_VERSION  "1.2.0"

// ================= НАСТРОЙКИ ОБОРУДОВАНИЯ =================
#define PIN_MATRIX        2      // Пин данных матрицы WS2812B
#define PIN_BUTTON        1      // Пин сенсорной кнопки TTP223
#define PIN_I2S_SCK       4      // I2S BCLK микрофона INMP441
#define PIN_I2S_WS        5      // I2S LRCLK микрофона INMP441
#define PIN_I2S_SD        6      // I2S DOUT микрофона INMP441
#define MATRIX_WIDTH      16     // Ширина матрицы
#define MATRIX_HEIGHT     16     // Высота матрицы
#define NUM_LEDS          (MATRIX_WIDTH * MATRIX_HEIGHT) // 256 диодов

// Защита по питанию: максимальный ток в миллиамперах (2.0А = безопасно для любого БП)
#define MAX_CURRENT_MA    2000   
// Ток холостого хода одного чипа WS2812B (~1 мА)
#define LED_IDLE_MA       1      
// Максимальный ток одного диода на полной яркости белого (~55 мА)
#define LED_FULL_MA       55     

// Ориентация матрицы
#define MATRIX_ZIGZAG     true   // Зигзагообразная разводка рядов
#define MATRIX_MIRROR_H   false  // Зеркалирование по горизонтали
#define MATRIX_MIRROR_V   false  // Зеркалирование по вертикали

// ================= ОБЪЕКТЫ СИСТЕМЫ =================
Adafruit_NeoPixel matrix(NUM_LEDS, PIN_MATRIX, NEO_GRB + NEO_KHZ800);
WebServer server(80);
DNSServer dnsServer;
Preferences prefsLamp;
Preferences prefsWifi;

// ================= СОСТОЯНИЕ СЕТИ =================
bool apMode = false;
const byte DNS_PORT = 53;
String currentSSID = "";
String currentPass = "";

// ================= СОСТОЯНИЕ ЛАМПЫ =================
bool lampOn = true;
uint8_t lampBrightness = 70;    // 5..255
uint8_t lampEffect = 0;        // Индекс текущего встроенного эффекта
String currentEffectId = "fire"; // Активный строковый ID ("fire", "matrix", "fireflies", "solid", "aurora"...)
bool isModularActive = false;   // true, если сейчас играет скачанный из LittleFS эффект
uint32_t solidColor = 0xFFAA33; // Цвет для ночника (янтарный)

// Отложенное сохранение в NVS (чтобы не убивать флеш частыми записями)
bool nvsSavePending = false;
unsigned long nvsSaveTimer = 0;

// ================= ФАЙЛОВОЕ ХРАНИЛИЩЕ И МОДУЛЬНЫЕ ЭФФЕКТЫ =================
bool fsAvailable = false;
bool isEffectDownloading = false;
String effectsCatalogUrl = "https://raw.githubusercontent.com/gaterkik/gyver-lamp/main/effects/index.json";
String effectsBaseUrl = "https://raw.githubusercontent.com/gaterkik/gyver-lamp/main/effects/";

// Высокоскоростной 8-битный шум Перлина (Fixed-Point, без FPU)
static const uint8_t p_table[256] = {
  151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
  8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
  35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
  134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
  55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
  18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,
  250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,
  189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,
  172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,
  228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,
  107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
  138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

uint8_t fast_noise8(uint16_t x, uint16_t y) {
  uint8_t X = (x >> 8) & 255;
  uint8_t Y = (y >> 8) & 255;
  uint8_t xf = x & 255;
  uint8_t yf = y & 255;

  uint8_t A = p_table[X] + Y;
  uint8_t B = p_table[X + 1] + Y;

  uint8_t p00 = p_table[A];
  uint8_t p10 = p_table[B];
  uint8_t p01 = p_table[A + 1];
  uint8_t p11 = p_table[B + 1];

  uint16_t u = ((uint32_t)xf * xf * (765 - 2 * xf)) >> 16;
  uint16_t v = ((uint32_t)yf * yf * (765 - 2 * yf)) >> 16;

  uint16_t x1 = p00 + (((int16_t)(p10 - p00) * u) >> 8);
  uint16_t x2 = p01 + (((int16_t)(p11 - p01) * u) >> 8);
  return x1 + (((int16_t)(x2 - x1) * v) >> 8);
}

ActiveModularEffect currentModularFx;
AudioAnalyzer audio;

// Битовая маска базовых эффектов в кнопке TTP223: bit0:fire, bit1:matrix, bit2:fireflies, bit3:solid, bit4:sound_pulse (0x1F = все включены)
uint8_t btnBuiltinMask = 0x1F;

bool isBuiltinInBtn(const String& id) {
  if (id == "fire") return (btnBuiltinMask & 0x01) != 0;
  if (id == "matrix") return (btnBuiltinMask & 0x02) != 0;
  if (id == "fireflies") return (btnBuiltinMask & 0x04) != 0;
  if (id == "solid") return (btnBuiltinMask & 0x08) != 0;
  if (id == "sound_pulse") return (btnBuiltinMask & 0x10) != 0;
  return false;
}

void toggleBuiltinInBtn(const String& id) {
  if (id == "fire") btnBuiltinMask ^= 0x01;
  else if (id == "matrix") btnBuiltinMask ^= 0x02;
  else if (id == "fireflies") btnBuiltinMask ^= 0x04;
  else if (id == "solid") btnBuiltinMask ^= 0x08;
  else if (id == "sound_pulse") btnBuiltinMask ^= 0x10;
  
  Preferences p;
  p.begin("lamp_cfg", false);
  p.putUChar("btn_builtin", btnBuiltinMask);
  p.end();
  Serial.printf("[Кнопка] Базовый эффект '%s' переключен, маска=0x%02X\n", id.c_str(), btnBuiltinMask);
}

// ================= ОБЛАЧНЫЕ ОБНОВЛЕНИЯ (GITHUB CLOUD OTA) =================
String cloudManifestUrl = "https://raw.githubusercontent.com/gaterkik/gyver-lamp/main/version.json";
bool autoOtaEnabled = true;
bool cloudUpdateAvailable = false;
String cloudVersion = "";
String cloudBinUrl = "";
String cloudChangelog = "";
bool isUpdatingCloudOta = false;
unsigned long lastCloudCheck = 0;
const unsigned long CLOUD_CHECK_INTERVAL = 24UL * 60UL * 60UL * 1000UL; // Раз в 24 часа
bool initialCloudCheckDone = false;

// Массив тепла для эффекта Огня
byte heat[MATRIX_WIDTH][MATRIX_HEIGHT];
// Параметры капель для эффекта Матрицы (разная длина линий)
int8_t matrixDropY[MATRIX_WIDTH];
uint8_t matrixDropLen[MATRIX_WIDTH];
// Фаза для эффекта Радуги
uint16_t rainbowHue = 0;
// Чистый буфер несжатого кадра (для чистого расчета эффектов и точного ограничения тока)
uint32_t canvas[NUM_LEDS];

// ================= ПРЕОБРАЗОВАНИЕ КООРДИНАТ =================
uint16_t getPixelIndex(int8_t x, int8_t y) {
  if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT) return 0;
  
  int8_t rx = MATRIX_MIRROR_H ? (MATRIX_WIDTH - 1 - x) : x;
  int8_t ry = MATRIX_MIRROR_V ? (MATRIX_HEIGHT - 1 - y) : y;

  if (MATRIX_ZIGZAG) {
    if (ry % 2 == 0) {
      return ry * MATRIX_WIDTH + rx;
    } else {
      return ry * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - rx);
    }
  } else {
    return ry * MATRIX_WIDTH + rx;
  }
}

void setPixel(int8_t x, int8_t y, uint32_t color) {
  if (x >= 0 && x < MATRIX_WIDTH && y >= 0 && y < MATRIX_HEIGHT) {
    canvas[getPixelIndex(x, y)] = color;
  }
}

// ================= РАБОТА С МОДУЛЬНЫМИ ЭФФЕКТАМИ И LITTLEFS =================
String extractJsonField(const String& json, const String& field); // Forward declaration

uint32_t getPaletteColor(uint8_t val, const PaletteStop* pal, uint8_t count) {
  if (count == 0) return 0;
  if (count == 1 || val <= pal[0].pos) return matrix.Color(pal[0].r, pal[0].g, pal[0].b);
  if (val >= pal[count - 1].pos) return matrix.Color(pal[count - 1].r, pal[count - 1].g, pal[count - 1].b);

  for (uint8_t i = 0; i < count - 1; i++) {
    if (val >= pal[i].pos && val <= pal[i + 1].pos) {
      uint16_t span = pal[i + 1].pos - pal[i].pos;
      if (span == 0) return matrix.Color(pal[i].r, pal[i].g, pal[i].b);
      uint16_t dist = val - pal[i].pos;
      uint8_t r = pal[i].r + (((int16_t)pal[i + 1].r - pal[i].r) * dist) / span;
      uint8_t g = pal[i].g + (((int16_t)pal[i + 1].g - pal[i].g) * dist) / span;
      uint8_t b = pal[i].b + (((int16_t)pal[i + 1].b - pal[i].b) * dist) / span;
      return matrix.Color(r, g, b);
    }
  }
  return matrix.Color(pal[count - 1].r, pal[count - 1].g, pal[count - 1].b);
}

void renderModularEffect() {
  if (!currentModularFx.valid) return;
  static uint16_t noiseTime = 0;
  noiseTime += currentModularFx.speedTime;

  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      uint16_t nx = x * currentModularFx.scaleX * 16;
      uint16_t ny = y * currentModularFx.scaleY * 16;
      uint8_t val = fast_noise8(nx, ny + noiseTime);
      canvas[getPixelIndex(x, y)] = getPaletteColor(val, currentModularFx.palette, currentModularFx.paletteCount);
    }
  }
}

bool parseModularEffect(const String& content, ActiveModularEffect& outFx) {
  memset(&outFx, 0, sizeof(outFx));

  String id = extractJsonField(content, "id");
  String name = extractJsonField(content, "name");
  String speedStr = extractJsonField(content, "speed");
  String sxStr = extractJsonField(content, "scaleX");
  String syStr = extractJsonField(content, "scaleY");
  String stStr = extractJsonField(content, "speedTime");

  if (id.length() == 0 || name.length() == 0) return false;

  strncpy(outFx.id, id.c_str(), sizeof(outFx.id) - 1);
  strncpy(outFx.name, name.c_str(), sizeof(outFx.name) - 1);
  outFx.speedMs = speedStr.length() > 0 ? constrain(speedStr.toInt(), 15, 255) : 35;
  outFx.scaleX = sxStr.length() > 0 ? constrain(sxStr.toInt(), 5, 200) : 30;
  outFx.scaleY = syStr.length() > 0 ? constrain(syStr.toInt(), 5, 200) : 45;
  outFx.speedTime = stStr.length() > 0 ? constrain(stStr.toInt(), 1, 100) : 15;

  int palIdx = content.indexOf("\"palette\"");
  if (palIdx < 0) return false;

  int curPos = palIdx;
  outFx.paletteCount = 0;
  while (outFx.paletteCount < 8) {
    int posTag = content.indexOf("\"pos\"", curPos);
    if (posTag < 0) break;
    int colon = content.indexOf(":", posTag);
    if (colon < 0) break;
    int comma = content.indexOf(",", colon);
    if (comma < 0) comma = content.indexOf("}", colon);
    if (comma < 0) break;

    int stopPos = content.substring(colon + 1, comma).toInt();

    int colorTag = content.indexOf("\"color\"", comma);
    if (colorTag < 0) break;
    int colorColon = content.indexOf(":", colorTag);
    if (colorColon < 0) break;
    int q1 = content.indexOf("\"", colorColon);
    if (q1 < 0) break;
    int q2 = content.indexOf("\"", q1 + 1);
    if (q2 < 0) break;

    String hexCol = content.substring(q1 + 1, q2);
    hexCol.trim();
    if (hexCol.startsWith("#")) hexCol = hexCol.substring(1);
    uint32_t rgb = (uint32_t)strtol(hexCol.c_str(), NULL, 16);

    outFx.palette[outFx.paletteCount].pos = constrain(stopPos, 0, 255);
    outFx.palette[outFx.paletteCount].r = (rgb >> 16) & 0xFF;
    outFx.palette[outFx.paletteCount].g = (rgb >> 8) & 0xFF;
    outFx.palette[outFx.paletteCount].b = rgb & 0xFF;
    outFx.paletteCount++;

    curPos = q2 + 1;
  }

  if (outFx.paletteCount < 2) {
    outFx.palette[0] = {0, 0, 255, 100};
    outFx.palette[1] = {255, 0, 100, 255};
    outFx.paletteCount = 2;
  }

  outFx.valid = true;
  return true;
}

std::vector<InstalledFxItem> getInstalledModularList() {
  std::vector<InstalledFxItem> list;
  if (!fsAvailable) return list;
  if (!LittleFS.exists("/effects/installed.json")) return list;

  File f = LittleFS.open("/effects/installed.json", "r");
  if (!f) return list;
  String content = f.readString();
  f.close();

  int curPos = 0;
  while (true) {
    int idTag = content.indexOf("\"id\"", curPos);
    if (idTag < 0) break;
    int q1 = content.indexOf("\"", idTag + 4);
    if (q1 < 0) break;
    int q2 = content.indexOf("\"", q1 + 1);
    if (q2 < 0) break;
    String id = content.substring(q1 + 1, q2);

    int nameTag = content.indexOf("\"name\"", q2);
    String name = id;
    if (nameTag >= 0) {
      int nq1 = content.indexOf("\"", nameTag + 6);
      int nq2 = content.indexOf("\"", nq1 + 1);
      if (nq1 >= 0 && nq2 > nq1) name = content.substring(nq1 + 1, nq2);
    }

    int btnTag = content.indexOf("\"in_btn\"", q2);
    bool inBtn = true;
    if (btnTag >= 0 && btnTag < content.indexOf("}", q2)) {
      int bcolon = content.indexOf(":", btnTag);
      if (bcolon >= 0) {
        String val = content.substring(bcolon + 1, bcolon + 6);
        if (val.indexOf("false") >= 0) inBtn = false;
      }
    }

    InstalledFxItem item;
    item.id = id;
    item.name = name;
    item.inBtn = inBtn;
    list.push_back(item);

    curPos = content.indexOf("}", q2);
    if (curPos < 0) break;
    curPos++;
  }
  return list;
}

void saveInstalledModularList(const std::vector<InstalledFxItem>& list) {
  if (!fsAvailable) return;
  File f = LittleFS.open("/effects/installed.json", "w");
  if (!f) return;
  String json = "[\n";
  for (size_t i = 0; i < list.size(); i++) {
    json += "  {\"id\":\"" + list[i].id + "\",\"name\":\"" + list[i].name + "\",\"in_btn\":" + (list[i].inBtn ? "true" : "false") + "}";
    if (i + 1 < list.size()) json += ",";
    json += "\n";
  }
  json += "]";
  f.print(json);
  f.close();
}

bool activateEffect(String id) {
  id.trim();
  if (id == "fire" || id == "0") {
    lampEffect = 0;
    isModularActive = false;
    currentEffectId = "fire";
    return true;
  }
  if (id == "matrix" || id == "1") {
    lampEffect = 1;
    isModularActive = false;
    currentEffectId = "matrix";
    return true;
  }
  if (id == "fireflies" || id == "2") {
    lampEffect = 2;
    isModularActive = false;
    currentEffectId = "fireflies";
    return true;
  }
  if (id == "solid" || id == "3") {
    lampEffect = 3;
    isModularActive = false;
    currentEffectId = "solid";
    return true;
  }
  if (id == "sound_pulse" || id == "4") {
    lampEffect = 4;
    isModularActive = false;
    currentEffectId = "sound_pulse";
    return true;
  }

  if (fsAvailable) {
    String path = "/effects/" + id + ".fx";
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      if (f) {
        String content = f.readString();
        f.close();
        ActiveModularEffect newFx;
        if (parseModularEffect(content, newFx)) {
          currentModularFx = newFx;
          isModularActive = true;
          currentEffectId = id;
          Serial.printf("[Effects] Активирован модульный эффект: %s (%s)\n", currentModularFx.name, currentModularFx.id);
          return true;
        }
      }
    }
  }

  Serial.printf("[Effects] Эффект '%s' не найден. Безопасный откат на Огонь.\n", id.c_str());
  lampEffect = 0;
  isModularActive = false;
  currentEffectId = "fire";
  return false;
}

void nextButtonEffect() {
  std::vector<String> btnFxList;
  if (isBuiltinInBtn("fire")) btnFxList.push_back("fire");
  if (isBuiltinInBtn("matrix")) btnFxList.push_back("matrix");
  if (isBuiltinInBtn("fireflies")) btnFxList.push_back("fireflies");
  if (isBuiltinInBtn("solid")) btnFxList.push_back("solid");
  if (isBuiltinInBtn("sound_pulse")) btnFxList.push_back("sound_pulse");

  if (fsAvailable) {
    auto modular = getInstalledModularList();
    for (const auto& item : modular) {
      if (item.inBtn) {
        btnFxList.push_back(item.id);
      }
    }
  }

  // Защитный фоллбек: если пользователь снял галочки вообще со всех эффектов
  if (btnFxList.empty()) {
    btnFxList.push_back("fire");
  }

  int curIdx = -1;
  for (size_t i = 0; i < btnFxList.size(); i++) {
    if (btnFxList[i] == currentEffectId) {
      curIdx = i;
      break;
    }
  }

  int nextIdx = (curIdx + 1) % btnFxList.size();
  activateEffect(btnFxList[nextIdx]);
  markSettingsChanged();
  Serial.printf("[Кнопка] Двойной клик: Эффект -> %s (всего в кнопке: %d)\n", currentEffectId.c_str(), (int)btnFxList.size());
}

// ================= ЭНЕРГОНЕЗАВИСИМАЯ ПАМЯТЬ (NVS / PREFERENCES) =================
void loadLampSettings() {
  prefsLamp.begin("lamp_cfg", false); // read/write для миграции
  lampOn = prefsLamp.getBool("power", true);
  lampBrightness = prefsLamp.getUChar("bright", 70);
  solidColor = prefsLamp.getUInt("color", 0xFFAA33);
  cloudManifestUrl = prefsLamp.getString("ota_url", "https://raw.githubusercontent.com/gaterkik/gyver-lamp/main/version.json");
  autoOtaEnabled = prefsLamp.getBool("auto_ota", true);
  btnBuiltinMask = prefsLamp.getUChar("btn_builtin", 0x1F);
  uint8_t sens = prefsLamp.getUChar("audio_sens", 50);
  audio.setSensitivity(sens);

  // Бесшовная миграция со старого числового индекса на строковый ID
  if (prefsLamp.isKey("active_fx")) {
    currentEffectId = prefsLamp.getString("active_fx", "fire");
  } else {
    uint8_t oldEffect = prefsLamp.getUChar("effect", 0);
    if (oldEffect == 1) currentEffectId = "matrix";
    else if (oldEffect == 2) currentEffectId = "fireflies";
    else if (oldEffect == 3) currentEffectId = "solid";
    else if (oldEffect == 4) currentEffectId = "sound_pulse";
    else currentEffectId = "fire";
    prefsLamp.putString("active_fx", currentEffectId);
    Serial.printf("[NVS Migration] Преобразован старый индекс %u -> '%s'\n", oldEffect, currentEffectId.c_str());
  }
  prefsLamp.end();

  // Активируем сохраненный эффект
  activateEffect(currentEffectId);

  Serial.printf("[NVS] Загружено: Лампа=%s, Яркость=%u, Эффект=%s, Цвет=#%06X\n",
                lampOn ? "ВКЛ" : "ВЫКЛ", lampBrightness, currentEffectId.c_str(), solidColor);
  Serial.printf("[NVS] Облачный OTA URL: %s (Авто: %s)\n", cloudManifestUrl.c_str(), autoOtaEnabled ? "ДА" : "НЕТ");
}

void saveOtaSettings(String newUrl, bool newAuto) {
  cloudManifestUrl = newUrl;
  autoOtaEnabled = newAuto;
  prefsLamp.begin("lamp_cfg", false);
  prefsLamp.putString("ota_url", cloudManifestUrl);
  prefsLamp.putBool("auto_ota", autoOtaEnabled);
  prefsLamp.end();
  Serial.println("[NVS] Настройки Cloud OTA сохранены.");
}

void markSettingsChanged() {
  nvsSavePending = true;
  nvsSaveTimer = millis(); // откладываем запись на 2 секунды (Debounce)
}

void processNvsSaving() {
  if (nvsSavePending && (millis() - nvsSaveTimer > 2000)) {
    nvsSavePending = false;
    prefsLamp.begin("lamp_cfg", false); // read/write
    prefsLamp.putBool("power", lampOn);
    prefsLamp.putUChar("bright", lampBrightness);
    prefsLamp.putUChar("effect", lampEffect); // для обратной совместимости
    prefsLamp.putString("active_fx", currentEffectId);
    prefsLamp.putUInt("color", solidColor);
    prefsLamp.end();
    Serial.printf("[NVS] Настройки лампы сохранены (Эффект=%s, Яркость=%u)\n", currentEffectId.c_str(), lampBrightness);
  }
}

// ================= ЭФФЕКТЫ ЛАМПЫ =================

// 1. Огонь
uint32_t getFireColor(byte heatVal) {
  if (heatVal < 85) {
    return matrix.Color((heatVal * 3), 0, 0);
  } else if (heatVal < 170) {
    byte val = (heatVal - 85) * 3;
    return matrix.Color(255, val, 0);
  } else {
    byte val = (heatVal - 170) * 3;
    return matrix.Color(255, 255, val);
  }
}

void effectFire() {
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      int cooldown = random(0, 15);
      heat[x][y] = (cooldown >= heat[x][y]) ? 0 : heat[x][y] - cooldown;
    }
  }
  for (int y = MATRIX_HEIGHT - 1; y >= 2; y--) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      int left = (x > 0) ? x - 1 : MATRIX_WIDTH - 1;
      int right = (x < MATRIX_WIDTH - 1) ? x + 1 : 0;
      heat[x][y] = (heat[left][y - 1] + heat[x][y - 1] + heat[right][y - 1] + heat[x][y - 2]) / 4;
    }
  }
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    if (random(0, 100) < 65) heat[x][0] = random(180, 255);
    if (random(0, 100) < 40) heat[x][1] = random(150, 230);
  }
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      setPixel(x, y, getFireColor(heat[x][y]));
    }
  }
}

// 2. Матрица (Цифровой дождь с линиями разной длины)
static uint8_t getRandomMatrixLength() {
  uint8_t r = random(0, 100);
  if (r < 30) {
    return random(3, 6);   // Короткие стрелы: 3..5 пикселей
  } else if (r < 75) {
    return random(6, 10);  // Средние линии: 6..9 пикселей
  } else {
    return random(10, 15); // Длинные спадающие потоки: 10..14 пикселей
  }
}

void effectMatrix() {
  static bool matrixInitialized = false;
  if (!matrixInitialized) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      matrixDropY[x] = random(-10, MATRIX_HEIGHT);
      matrixDropLen[x] = getRandomMatrixLength();
    }
    matrixInitialized = true;
  }

  // Очищаем буфер кадра (каждая линия четко сохраняет свою индивидуальную длину)
  for (int i = 0; i < NUM_LEDS; i++) canvas[i] = 0;

  for (int x = 0; x < MATRIX_WIDTH; x++) {
    int8_t headY = matrixDropY[x];
    uint8_t len = matrixDropLen[x];

    // Проверяем, видна ли хоть какая-то часть линии на матрице
    if (headY + len > 0 && headY < MATRIX_HEIGHT + 2) {
      for (uint8_t k = 0; k < len; k++) {
        int8_t y = headY + k;
        if (y >= 0 && y < MATRIX_HEIGHT) {
          if (k == 0) {
            // Голова линии — яркая бело-зеленая искра
            setPixel(x, y, matrix.Color(200, 255, 200));
          } else if (k == 1) {
            // Первый пиксель за головой — насыщенный неоновый зеленый
            setPixel(x, y, matrix.Color(0, 255, 30));
          } else {
            // Тело хвоста: градиентное затухание от 230 вниз до 25
            uint8_t g = 230 - ((k - 1) * 205 / (len > 2 ? len - 2 : 1));
            // Цифровой глитч: случайная вспышка меняющегося символа (4%)
            if (random(0, 100) < 4) {
              g = (g > 175) ? 255 : g + 80;
            }
            setPixel(x, y, matrix.Color(0, g, 0));
          }
        }
      }
      // Смещаем линию вниз на 1 пиксель
      matrixDropY[x]--;
    } else {
      // Линия полностью ушла за нижний край — шанс появления новой линии со случайной длиной
      if (random(0, 100) < 12) {
        matrixDropY[x] = MATRIX_HEIGHT - 1;
        matrixDropLen[x] = getRandomMatrixLength();
      }
    }
  }
}

// 3. Радуга
void effectRainbow() {
  rainbowHue += 256;
  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    uint32_t color = matrix.ColorHSV(rainbowHue + (y * 2200), 255, 255);
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      setPixel(x, y, color);
    }
  }
}

// 4. Светлячки
void effectFireflies() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = canvas[i];
    uint8_t r = ((c >> 16) & 0xFF) * 0.88;
    uint8_t g = ((c >> 8) & 0xFF) * 0.88;
    uint8_t b = (c & 0xFF) * 0.88;
    canvas[i] = matrix.Color(r, g, b);
  }
  if (random(0, 100) < 25) {
    setPixel(random(0, MATRIX_WIDTH), random(0, MATRIX_HEIGHT),
             matrix.Color(random(150, 255), random(200, 255), random(0, 70)));
  }
}

// 5. Ночник / Светильник
void effectSolid() {
  for (int i = 0; i < NUM_LEDS; i++) {
    canvas[i] = solidColor;
  }
}

// 6. Светомузыка / Пульс в такт музыке (INMP441)
uint32_t colorWheel(uint8_t wheelPos) {
  wheelPos = 255 - wheelPos;
  if (wheelPos < 85) {
    return matrix.Color(255 - wheelPos * 3, 0, wheelPos * 3);
  }
  if (wheelPos < 170) {
    wheelPos -= 85;
    return matrix.Color(0, wheelPos * 3, 255 - wheelPos * 3);
  }
  wheelPos -= 170;
  return matrix.Color(wheelPos * 3, 255 - wheelPos * 3, 0);
}

void effectAudioPulse() {
  uint8_t vol = audio.getVolume();          // 0..255
  uint8_t beat = audio.getBeatIntensity();   // 0..255

  static uint8_t baseHue = 0;
  static uint8_t waveRadius = 0;
  static bool waveActive = false;

  // Медленный дрейф базовой палитры + ускорение при ударах баса
  if (beat > 200) {
    baseHue += 16;
    waveActive = true;
    waveRadius = 0;
  } else {
    baseHue += 1;
  }

  if (waveActive) {
    waveRadius += 16;
    if (waveRadius > 250) {
      waveActive = false;
    }
  }

  // Радиус светового ядра от громкости (от 20 до 220)
  uint8_t currentRadius = map(vol, 0, 255, 20, 220);

  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      uint8_t d = distCenter[y][x]; // 0 (центр) .. 255 (края)

      if (d <= currentRadius) {
        // Внутри пульсирующего звукового ядра
        uint8_t localBright = 255 - ((d * 180) / currentRadius);
        uint8_t pixelHue = baseHue + (d >> 2);
        uint32_t col = colorWheel(pixelHue);

        // Вспышка на ударе баса
        if (beat > 100 && d < 70) {
          uint8_t r = min(255, (int)(((col >> 16) & 0xFF) + (beat >> 1)));
          uint8_t g = min(255, (int)(((col >> 8) & 0xFF) + (beat >> 2)));
          uint8_t b = min(255, (int)((col & 0xFF) + (beat >> 2)));
          col = matrix.Color(r, g, b);
        }

        uint8_t r = (((col >> 16) & 0xFF) * localBright) >> 8;
        uint8_t g = (((col >> 8) & 0xFF) * localBright) >> 8;
        uint8_t b = ((col & 0xFF) * localBright) >> 8;
        setPixel(x, y, matrix.Color(r, g, b));
      } else {
        // Прохождение взрывной волны бита
        if (waveActive && abs((int)d - (int)waveRadius) < 25) {
          uint8_t waveIntensity = 255 - abs((int)d - (int)waveRadius) * 10;
          uint32_t waveCol = colorWheel(baseHue + 128);
          uint8_t r = (((waveCol >> 16) & 0xFF) * waveIntensity) >> 8;
          uint8_t g = (((waveCol >> 8) & 0xFF) * waveIntensity) >> 8;
          uint8_t b = ((waveCol & 0xFF) * waveIntensity) >> 8;
          setPixel(x, y, matrix.Color(r, g, b));
        } else {
          // Мягкое угасание хвоста волн
          uint32_t cur = canvas[getPixelIndex(x, y)];
          uint8_t r = ((cur >> 16) & 0xFF) * 70 >> 7;
          uint8_t g = ((cur >> 8) & 0xFF) * 70 >> 7;
          uint8_t b = (cur & 0xFF) * 70 >> 7;
          setPixel(x, y, matrix.Color(r, g, b));
        }
      }
    }
  }
}

// ================= МОДУЛЬНАЯ ТАБЛИЦА ЭФФЕКТОВ =================
typedef void (*EffectFunction)();

struct EffectDef {
  const char* id;
  const char* name;
  EffectFunction func;
  uint8_t speedMs;
};

const EffectDef effectsList[] = {
  {"fire",        "🔥 Огонь",        effectFire,       30},
  {"matrix",      "🟢 Матрица",      effectMatrix,     45},
  {"fireflies",   "✨ Светлячки",    effectFireflies,  35},
  {"solid",       "💡 Ночник/Свет",  effectSolid,      50},
  {"sound_pulse", "🎵 Пульс музыки", effectAudioPulse, 25}
};

const uint8_t TOTAL_EFFECTS = sizeof(effectsList) / sizeof(effectsList[0]);

// ================= БЕГУЩАЯ СТРОКА IP-АДРЕСА =================
const uint16_t FONT_3x5[10] = {
  0x7B6F, // 0 (111 101 101 101 111)
  0x2492, // 1 (010 010 010 010 010)
  0x73E7, // 2 (111 001 111 100 111)
  0x73CF, // 3 (111 001 111 001 111)
  0x5DE9, // 4 (101 101 111 001 001)
  0x79CF, // 5 (111 100 111 001 111)
  0x79EF, // 6 (111 100 111 101 111)
  0x7249, // 7 (111 001 001 001 001)
  0x7BEF, // 8 (111 101 111 101 111)
  0x7BCF  // 9 (111 101 111 001 111)
};

bool isScrollingText = false;
String scrollTextStr = "";
int scrollTextX = 0;
int totalScrollWidth = 0;
unsigned long lastScrollStep = 0;

int getCharWidth(char c) {
  if (c == '.') return 2;
  if (c == ' ') return 2;
  return 4; // 3 пикселя символ + 1 пиксель пробел
}

int getTextWidth(const String& str) {
  int w = 0;
  for (unsigned int i = 0; i < str.length(); i++) {
    w += getCharWidth(str[i]);
  }
  return w;
}

void drawChar3x5(char c, int startX, int startY, uint32_t color) {
  if (c >= '0' && c <= '9') {
    uint16_t glyph = FONT_3x5[c - '0'];
    for (int row = 0; row < 5; row++) {
      for (int col = 0; col < 3; col++) {
        int bitIdx = 14 - (row * 3 + col);
        if ((glyph >> bitIdx) & 1) {
          setPixel(startX + col, startY + row, color);
        }
      }
    }
  } else if (c == '.') {
    setPixel(startX, startY + 4, color);
  }
}

void drawScrollingText(const String& str, int offset, uint32_t color) {
  int curX = offset;
  int startY = 5; // Вертикальный центр (строки 5..9 из 0..15)
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str[i];
    if (curX + 3 >= 0 && curX < MATRIX_WIDTH) {
      drawChar3x5(c, curX, startY, color);
    }
    curX += getCharWidth(c);
  }
}

void triggerIpScroll() {
  if (WiFi.status() == WL_CONNECTED) {
    scrollTextStr = "  " + WiFi.localIP().toString() + "  ";
    totalScrollWidth = getTextWidth(scrollTextStr);
    scrollTextX = MATRIX_WIDTH;
    isScrollingText = true;
    lastScrollStep = millis();
    Serial.printf("[Экран] Запуск бегущей строки IP: %s\n", scrollTextStr.c_str());
  }
}

// Индикация достижения границы яркости (моргание без блокировки процессора)
bool isEdgeBlinking = false;
unsigned long edgeBlinkUntil = 0;
uint8_t edgeBlinkMode = 0; // 0: нырок в темноту (на максимуме), 1: яркая вспышка (на минимуме)

void renderLamp() {
  if (isUpdatingCloudOta) return; // Во время облачной прошивки матрица занята мягким прогресс-баром

  if (!lampOn) {
    matrix.clear();
    matrix.show();
    return;
  }

  // 1. Отрисовка кадра: бегущая строка IP либо текущий эффект
  if (isScrollingText) {
    for (int i = 0; i < NUM_LEDS; i++) canvas[i] = 0;
    drawScrollingText(scrollTextStr, scrollTextX, matrix.Color(0, 180, 240)); // Мягкий бирюзовый текст
  } else {
    if (isModularActive) {
      renderModularEffect();
    } else {
      if (lampEffect < TOTAL_EFFECTS) {
        effectsList[lampEffect].func();
      } else {
        lampEffect = 0;
        effectsList[0].func();
      }
    }
  }

  // 2. Расчет потребления тока несжатого кадра (в миллиамперах)
  uint32_t fullPowerCurrent = NUM_LEDS * LED_IDLE_MA;
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = canvas[i];
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >> 8) & 0xFF;
    uint8_t b = c & 0xFF;
    fullPowerCurrent += ((uint32_t)(r + g + b) * LED_FULL_MA) / 765;
  }

  // 3. Максимально допустимая яркость для жесткого ограничения тока в 2.0А
  uint8_t maxAllowed = 255;
  if (fullPowerCurrent > MAX_CURRENT_MA) {
    maxAllowed = (MAX_CURRENT_MA * 255) / fullPowerCurrent;
  }

  // 4. Определение целевой яркости (с учетом индикации упора)
  uint8_t targetReq = lampBrightness;
  if (isEdgeBlinking) {
    if (millis() < edgeBlinkUntil) {
      targetReq = (edgeBlinkMode == 0) ? 0 : 255; // 0: нырок в темноту, 255: яркая вспышка
    } else {
      isEdgeBlinking = false;
    }
  }

  // 5. Пропорциональное масштабирование без «мертвого» диапазона:
  // targetReq (0..255) плавно отображается в 0..maxAllowed
  uint8_t scaledBrightness = ((uint32_t)targetReq * maxAllowed) / 255;
  if (targetReq > 0 && scaledBrightness == 0) {
    scaledBrightness = 1;
  }

  // 6. Вывод в матрицу с применением масштабирования
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = canvas[i];
    uint8_t r = (((c >> 16) & 0xFF) * scaledBrightness) >> 8;
    uint8_t g = (((c >> 8) & 0xFF) * scaledBrightness) >> 8;
    uint8_t b = ((c & 0xFF) * scaledBrightness) >> 8;
    matrix.setPixelColor(i, r, g, b);
  }
  matrix.show();
}

// ================= СЕНСОРНАЯ КНОПКА (TTP223) =================
void handleButton() {
  static int lastRawState = LOW;
  static int stableState = LOW;
  static unsigned long lastDebounceTime = 0;

  static unsigned long pressStartTime = 0;
  static unsigned long releaseTime = 0;
  static int clickCount = 0;
  static bool isHolding = false;
  static bool brightDirection = true;
  static unsigned long lastBrightStep = 0;
  static bool edgeBlinkDone = false;

  int rawState = digitalRead(PIN_BUTTON);

  // 1. Аппаратный/программный антидребезг (25 мс)
  if (rawState != lastRawState) {
    lastDebounceTime = millis();
    lastRawState = rawState;
  }

  if ((millis() - lastDebounceTime) > 25) {
    if (rawState != stableState) {
      stableState = rawState;

      // === НАЖАТИЕ (LOW -> HIGH) ===
      if (stableState == HIGH) {
        pressStartTime = millis();
        isHolding = false;
        edgeBlinkDone = false;
      }

      // === ОТПУСКАНИЕ (HIGH -> LOW) ===
      if (stableState == LOW) {
        if (isHolding) {
          // Завершили удержание
          isHolding = false;
          if (lampBrightness >= 250) {
            brightDirection = false;
          } else if (lampBrightness <= 15) {
            brightDirection = true;
          } else {
            brightDirection = !brightDirection;
          }
          clickCount = 0; // Сбрасываем счетчик кликов после удержания
        } else {
          // Короткий клик (< 350 мс)
          clickCount++;
          releaseTime = millis();

          // ДВОЙНОЙ КЛИК: срабатывает мгновенно в момент отпускания второго тапа!
          if (clickCount >= 2) {
            clickCount = 0;
            if (!lampOn) lampOn = true;
            nextButtonEffect();
          }
        }
      }
    }
  }

  // 2. ОБРАБОТКА УДЕРЖАНИЯ (пока кнопка зажата)
  if (stableState == HIGH) {
    if (!isHolding && (millis() - pressStartTime > 350)) {
      isHolding = true;
      clickCount = 0; // Отменяем регистрацию одиночного клика при переходе в удержание
    }

    if (isHolding && lampOn) {
      if (millis() - lastBrightStep >= 25) {
        lastBrightStep = millis();

        if (brightDirection) {
          if (lampBrightness < 255) {
            lampBrightness = (lampBrightness + 2 > 255) ? 255 : lampBrightness + 2;
            markSettingsChanged();
          } else if (!edgeBlinkDone) {
            // Уперлись в максимум: короткий заметный нырок в темноту и остановка
            edgeBlinkDone = true;
            isEdgeBlinking = true;
            edgeBlinkMode = 0; // нырок (0)
            edgeBlinkUntil = millis() + 120;
            // Направление не меняем, пока палец не будет отпущен!
          }
        } else {
          if (lampBrightness > 10) {
            lampBrightness = (lampBrightness - 2 < 10) ? 10 : lampBrightness - 2;
            markSettingsChanged();
          } else if (!edgeBlinkDone) {
            // Уперлись в минимум: короткая яркая вспышка и остановка
            edgeBlinkDone = true;
            isEdgeBlinking = true;
            edgeBlinkMode = 1; // вспышка (255)
            edgeBlinkUntil = millis() + 120;
            // Направление не меняем, пока палец не будет отпущен!
          }
        }
      }
    }
  }

  // 3. ОБРАБОТКА ОДИНОЧНОГО КЛИКА (только когда кнопка стабильно отпущена и истекло окно второго тапа)
  if (stableState == LOW && clickCount == 1) {
    if (millis() - releaseTime > 380) {
      clickCount = 0;
      lampOn = !lampOn;
      markSettingsChanged();
      Serial.printf("[Кнопка] Одинарный клик: Лампа -> %s\n", lampOn ? "ВКЛ" : "ВЫКЛ");
    }
  }
}

// ================= ОБЛАЧНЫЕ ОБНОВЛЕНИЯ (GITHUB CLOUD OTA) =================

// Сравнение семантических версий "X.Y.Z". Возвращает true, если remote новее current
bool isNewerVersion(String remote, String current) {
  int rMaj = 0, rMin = 0, rPat = 0;
  int cMaj = 0, cMin = 0, cPat = 0;
  sscanf(remote.c_str(), "%d.%d.%d", &rMaj, &rMin, &rPat);
  sscanf(current.c_str(), "%d.%d.%d", &cMaj, &cMin, &cPat);
  if (rMaj != cMaj) return rMaj > cMaj;
  if (rMin != cMin) return rMin > cMin;
  return rPat > cPat;
}

// Извлечение строкового поля из JSON {"field": "value"}
String extractJsonField(const String& json, const String& field) {
  String key = "\"" + field + "\"";
  int keyPos = json.indexOf(key);
  if (keyPos < 0) return "";
  int colonPos = json.indexOf(':', keyPos + key.length());
  if (colonPos < 0) return "";
  int startQuote = json.indexOf('\"', colonPos);
  if (startQuote < 0) return "";
  int endQuote = json.indexOf('\"', startQuote + 1);
  if (endQuote < 0) return "";
  return json.substring(startQuote + 1, endQuote);
}

void performCloudUpdate(bool silent = false); // прототип

// Проверка наличия свежей прошивки в облаке
bool checkCloudUpdate(bool triggerUpdateIfStandby) {
  if (WiFi.status() != WL_CONNECTED || isUpdatingCloudOta) return false;

  Serial.println("[Cloud OTA] Проверка обновлений на GitHub...");
  WiFiClientSecure client;
  client.setInsecure(); // Пропуск валидации сертификатов для бессрочной работы
  client.setTimeout(8000);

  HTTPClient http;
  if (!http.begin(client, cloudManifestUrl)) {
    Serial.println("[Cloud OTA] Ошибка подключения к серверу манифеста.");
    return false;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();

    String ver = extractJsonField(payload, "version");
    String url = extractJsonField(payload, "url");
    String changelog = extractJsonField(payload, "changelog");

    if (ver.length() > 0 && url.length() > 0) {
      cloudVersion = ver;
      cloudBinUrl = url;
      cloudChangelog = (changelog.length() > 0) ? changelog : "Улучшения стабильности и новые эффекты";

      if (isNewerVersion(cloudVersion, FIRMWARE_VERSION)) {
        cloudUpdateAvailable = true;
        Serial.printf("[Cloud OTA] Найдено обновление: v%s (текущая: v%s)\n", cloudVersion.c_str(), FIRMWARE_VERSION);
        Serial.printf("[Cloud OTA] Описание от создателя: %s\n", cloudChangelog.c_str());

        // Продуктовое правило: если лампа ВЫКЛЮЧЕНА (standby) и включено автообновление -> ТИХОЕ обновление
        if (triggerUpdateIfStandby && autoOtaEnabled && !lampOn) {
          Serial.println("[Cloud OTA] Лампа выключена (standby). Запуск полностью тихого фонового обновления...");
          performCloudUpdate(true); // silent = true: матрица остается абсолютно темной
        }
        return true;
      } else {
        cloudUpdateAvailable = false;
        Serial.printf("[Cloud OTA] Установлена актуальная версия v%s.\n", FIRMWARE_VERSION);
      }
    }
  } else {
    Serial.printf("[Cloud OTA] HTTP ответ: %d\n", httpCode);
    http.end();
  }
  return false;
}

// Запуск процесса загрузки и прошивки
void performCloudUpdate(bool silent) {
  if (cloudBinUrl.length() == 0 || isUpdatingCloudOta) return;
  if (WiFi.status() != WL_CONNECTED) return;

  isUpdatingCloudOta = true;
  Serial.printf("[Cloud OTA] Старт загрузки прошивки v%s (silent=%s)...\n", cloudVersion.c_str(), silent ? "true" : "false");

  if (silent) {
    matrix.clear();
    matrix.show();
  }

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false); // [КРИТИЧНО] Запрещаем автоматический reboot внутри httpUpdate, чтобы сработал зеленый всполох!

  // Мягкий неслепящий прогресс-бар на матрице показываем ТОЛЬКО в интерактивном режиме (ручной запуск)
  if (!silent) {
    httpUpdate.onProgress([](int cur, int total) {
      if (total > 0) {
        int ledsToLight = ((int64_t)cur * NUM_LEDS) / total;
        for (int i = 0; i < NUM_LEDS; i++) {
          if (i < ledsToLight) {
            matrix.setPixelColor(i, matrix.Color(0, 30, 45)); // Мягкий бирюзовый
          } else {
            matrix.setPixelColor(i, 0);
          }
        }
        matrix.show();
      }
    });
  } else {
    httpUpdate.onProgress(nullptr);
  }

  t_httpUpdate_return ret = httpUpdate.update(client, cloudBinUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[Cloud OTA] Сбой: (%d): %s\n", 
                    httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      if (!silent) {
        // Мягкая красная индикация сбоя только в интерактивном режиме
        for (int i = 0; i < NUM_LEDS; i++) matrix.setPixelColor(i, matrix.Color(50, 0, 0));
        matrix.show();
        delay(1500);
      }
      matrix.clear();
      matrix.show();
      isUpdatingCloudOta = false;
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[Cloud OTA] Нет обновлений.");
      isUpdatingCloudOta = false;
      break;

    case HTTP_UPDATE_OK:
      Serial.println("[Cloud OTA] Успех! Перезагрузка в новый раздел...");
      if (!silent) {
        // Яркий красивый изумрудно-зеленый всполох на всей матрице (1200 мс)
        for (int i = 0; i < NUM_LEDS; i++) matrix.setPixelColor(i, matrix.Color(0, 140, 30));
        matrix.show();
        delay(1200);
      } else {
        matrix.clear();
        matrix.show();
        delay(100);
      }
      ESP.restart();
      break;
  }
}

// ================= WI-FI И CAPTIVE PORTAL =================
String cachedWifiOptions = "<option value=''>Поиск сетей... Обновите страницу через 2 сек</option>";
bool scanInProgress = false;

void triggerWifiScan() {
  if (!scanInProgress) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true); // [QA AUDIT] Асинхронное сканирование (0% блокировки loop)
    scanInProgress = true;
  }
}

void checkWifiScan() {
  if (scanInProgress) {
    int16_t n = WiFi.scanComplete();
    if (n >= 0) {
      cachedWifiOptions = "";
      cachedWifiOptions.reserve(1024);
      for (int i = 0; i < n; ++i) {
        cachedWifiOptions += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
      }
      WiFi.scanDelete();
      scanInProgress = false;
      Serial.printf("[AP] Асинхронное сканирование: найдено %d сетей\n", n);
    }
  }
}

void startAccessPoint() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Lamp-Setup");
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  
  dnsServer.start(DNS_PORT, "*", apIP); // Перенаправлять все DNS на 192.168.4.1
  triggerWifiScan(); // Запуск неблокирующего сканирования эфира
  Serial.println("\n[AP] Запущена точка настройки: Lamp-Setup");
  Serial.println("[AP] Подключитесь и перейдите на: http://192.168.4.1");
}

void loadWifiCredentials() {
  prefsWifi.begin("wifi_cfg", true);
  currentSSID = prefsWifi.getString("ssid", "gwfap");
  currentPass = prefsWifi.getString("pass", "06081987");
  prefsWifi.end();
}

void saveWifiCredentials(String newSsid, String newPass) {
  prefsWifi.begin("wifi_cfg", false);
  prefsWifi.putString("ssid", newSsid);
  prefsWifi.putString("pass", newPass);
  prefsWifi.end();
  Serial.printf("[Wi-Fi] Новые данные сохранены: %s\n", newSsid.c_str());
}

bool connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("lamp"); // Имя устройства для роутера
  WiFi.setAutoReconnect(true); // [QA FIX #3] Автоматическое переподключение
  WiFi.begin(currentSSID.c_str(), currentPass.c_str());
  Serial.printf("[Wi-Fi] Подключение к сети: %s\n", currentSSID.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] Подключено к Wi-Fi! IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("\n[!] Не удалось подключиться к Wi-Fi.");
  return false;
}

// ================= ВЕБ-ИНТЕРФЕЙС И РОУТЫ =================
String getUptime() {
  unsigned long sec = millis() / 1000;
  unsigned long min = sec / 60;
  unsigned long hr  = min / 60;
  char buf[32];
  snprintf(buf, sizeof(buf), "%02lu ч %02lu мин %02lu сек", hr, min % 60, sec % 60);
  return String(buf);
}

void handleRoot() {
  if (apMode) {
    String html = "";
    html.reserve(2560); // [QA FIX #5] Защита от фрагментации кучи
    html += R"rawliteral(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Настройка Wi-Fi | GyverLamp</title>
<style>
body { background:#0f172a; color:#f8fafc; font-family:sans-serif; padding:20px; display:flex; justify-content:center; }
.card { background:#1e293b; border-radius:14px; padding:24px; max-width:400px; width:100%; box-shadow:0 10px 20px rgba(0,0,0,0.4); }
h2 { color:#f97316; margin-bottom:12px; }
label { display:block; margin:12px 0 6px; font-size:0.9rem; color:#94a3b8; }
select, input { width:100%; padding:10px; border-radius:8px; border:1px solid #334155; background:#0f172a; color:#fff; box-sizing:border-box; }
.btn { margin-top:16px; background:#f97316; border:none; padding:12px; width:100%; border-radius:8px; font-weight:bold; color:#fff; cursor:pointer; }
.rescan-link { display:inline-block; margin-top:8px; font-size:0.85rem; color:#f97316; text-decoration:none; }
</style></head><body>
<div class='card'>
  <h2>🔥 Настройка сети Wi-Fi</h2>
  <p style='font-size:0.85rem;color:#94a3b8;'>Выберите вашу домашнюю сеть (2.4 ГГц):</p>
  <form action='/save_wifi' method='POST'>
    <label>Сеть Wi-Fi:</label>
    <select name='ssid'>)rawliteral";
    html += cachedWifiOptions; // [QA AUDIT FIX] Мгновенная отдача без заморозки loop()
    html += R"rawliteral(</select>
    <a href='/rescan' class='rescan-link'>🔄 Обновить список сетей</a>
    <label>Пароль:</label>
    <input type='password' name='pass' placeholder='Пароль от Wi-Fi' required>
    <button class='btn' type='submit'>Сохранить и подключить</button>
  </form>
</div></body></html>)rawliteral";
    server.send(200, "text/html", html);
    return;
  }

  // [QA FIX #5] Защита от фрагментации кучи
  String effectOptions = "";
  effectOptions.reserve(1024);
  effectOptions += "<optgroup label=\"Базовые эффекты\">";
  effectOptions += "<option value='fire'";
  if (currentEffectId == "fire") effectOptions += " selected";
  effectOptions += ">🔥 Огонь</option>";

  effectOptions += "<option value='matrix'";
  if (currentEffectId == "matrix") effectOptions += " selected";
  effectOptions += ">🟢 Матрица</option>";

  effectOptions += "<option value='fireflies'";
  if (currentEffectId == "fireflies") effectOptions += " selected";
  effectOptions += ">✨ Светлячки</option>";

  effectOptions += "<option value='solid'";
  if (currentEffectId == "solid") effectOptions += " selected";
  effectOptions += ">💡 Ночник/Свет</option>";

  effectOptions += "<option value='sound_pulse'";
  if (currentEffectId == "sound_pulse") effectOptions += " selected";
  effectOptions += ">🎵 Пульс музыки</option>";
  effectOptions += "</optgroup>";

  if (fsAvailable) {
    auto modularList = getInstalledModularList();
    if (!modularList.empty()) {
      effectOptions += "<optgroup label=\"Модульные эффекты (LittleFS)\">";
      for (const auto& item : modularList) {
        effectOptions += "<option value='" + item.id + "'" + (currentEffectId == item.id ? " selected" : "") + ">" + item.name + "</option>";
      }
      effectOptions += "</optgroup>";
    }
  }

  String html = "";
  html.reserve(8192);
  html += R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Огненная Лампа C6</title>
  <style>
    :root { --bg:#0f172a; --card:#1e293b; --accent:#f97316; --text:#f8fafc; --muted:#94a3b8; --border:#334155; --success:#22c55e; --error:#ef4444; }
    * { box-sizing:border-box; margin:0; padding:0; font-family:-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background:var(--bg); color:var(--text); padding:16px; display:flex; justify-content:center; }
    .container { max-width:480px; width:100%; display:flex; flex-direction:column; gap:16px; }
    .card { background:var(--card); border:1px solid var(--border); border-radius:16px; padding:20px; box-shadow:0 10px 25px rgba(0,0,0,0.3); }
    h1 { font-size:1.4rem; color:var(--accent); margin-bottom:4px; }
    h2 { font-size:1.05rem; margin-bottom:12px; border-bottom:1px solid var(--border); padding-bottom:6px; }
    .row { display:flex; justify-content:space-between; font-size:0.85rem; margin-bottom:6px; color:var(--muted); }
    .val { color:var(--text); font-weight:600; }
    .btn { background:var(--accent); color:white; border:none; padding:12px; border-radius:10px; font-size:1rem; font-weight:600; cursor:pointer; width:100%; transition:0.2s; }
    .btn-power { background:#334155; }
    .btn-power.on { background:var(--success); }
    .grp { margin-bottom:16px; }
    label { display:block; font-size:0.85rem; color:var(--muted); margin-bottom:6px; }
    select, input[type="range"] { width:100%; }
    select { background:#0f172a; color:var(--text); border:1px solid var(--border); padding:10px; border-radius:8px; font-size:0.95rem; }
    input[type="range"] { height:8px; border-radius:4px; background:#0f172a; accent-color:var(--accent); cursor:pointer; }
    .file-box { margin:12px 0; border:2px dashed var(--border); border-radius:12px; padding:16px; text-align:center; cursor:pointer; }
    .progress-bg { width:100%; height:10px; background:#0f172a; border-radius:5px; overflow:hidden; margin-top:6px; border:1px solid var(--border); }
    .progress { width:0%; height:100%; background:linear-gradient(90deg, #f97316, #eab308); }
  </style>
</head>
<body>
  <div class="container">
    <div class="card">
      <h1>🔥 GyverLamp C6</h1>
      <p style="color:var(--muted); font-size:0.8rem; margin-bottom:16px;">WS2812B 16×16 • Лимит тока 2.0А • Память NVS</p>
      
      <div class="grp">
        <button id="pwr-btn" class="btn btn-power )rawliteral";
  html += (lampOn ? "on" : "");
  html += R"rawliteral(" onclick="togglePower()">)rawliteral";
  html += (lampOn ? "Питание: ВКЛ" : "Питание: ВЫКЛ");
  html += R"rawliteral(</button>
      </div>

      <div class="grp">
        <label>Эффект:</label>
        <select id="eff-sel" onchange="setEffect(this.value)">)rawliteral";
  html += effectOptions;
  html += R"rawliteral(</select>
      </div>

      <div class="grp">
        <div style="display:flex; justify-content:space-between;">
          <label>Яркость:</label>
          <span id="brt-val" style="font-size:0.85rem; font-weight:600;">)rawliteral";
  html += String((int)round((lampBrightness / 255.0) * 100));
  html += R"rawliteral(%</span>
        </div>
        <input type="range" min="5" max="255" value=")rawliteral";
  html += String(lampBrightness);
  html += R"rawliteral(" oninput="setBrightness(this.value)">
      </div>

      <div class="grp">
        <label>Цвет ночника (для режима "Ночник"):</label>
        <input type="color" value="#)rawliteral";
  html += String(solidColor, HEX);
  html += R"rawliteral(" onchange="setColor(this.value)" style="width:100%; height:38px; border:none; border-radius:8px; cursor:pointer;">
      </div>
    </div>

    <div class="card">
      <h2>🎤 Микрофон INMP441 (Светомузыка)</h2>
      <p style="font-size:0.8rem; color:var(--muted); margin-bottom:12px;">
        Цифровой I2S микрофон. При звучании музыки лампа пульсирует световыми кольцами и реагирует на басы. Если микрофон не подключен или вокруг тишина, эффект плавно переходит в мягкое дыхание.
      </p>

      <div class="row"><span>Статус микрофона:</span><span class="val" id="mic-status">Проверка...</span></div>
      <div class="row"><span>Детекция звука:</span><span class="val" id="mic-sound">--</span></div>

      <div class="grp" style="margin-top:12px;">
        <div style="display:flex; justify-content:space-between;">
          <label>Чувствительность микрофона:</label>
          <span id="sens-val" style="font-size:0.85rem; font-weight:600;">)rawliteral";
  html += String(audio.getSensitivity());
  html += R"rawliteral(%</span>
        </div>
        <input type="range" id="sens-slider" min="10" max="100" value=")rawliteral";
  html += String(audio.getSensitivity());
  html += R"rawliteral(" onchange="setAudioSens(this.value)" oninput="document.getElementById('sens-val').innerText = this.value + '%'">
      </div>

      <div style="margin-top:8px;">
        <label>Индикатор громкости (VU Meter):</label>
        <div style="width:100%; height:12px; background:#0f172a; border-radius:6px; overflow:hidden; border:1px solid var(--border); margin-top:4px;">
          <div id="vu-bar" style="width:0%; height:100%; background:linear-gradient(90deg, #22c55e, #eab308, #ef4444); transition:width 0.15s ease;"></div>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>🔘 Ротация эффектов (кнопка TTP223)</h2>
      <p style="font-size:0.8rem; color:var(--muted); margin-bottom:12px;">
        Отметьте галочками эффекты, которые будут переключаться двойным нажатием сенсорной кнопки. Все эффекты сохраняются в памяти и могут быть включены кнопкой «Включить» или через выпадающий список выше.
      </p>

      <div id="installed-list" style="display:flex; flex-direction:column; gap:8px; margin-bottom:12px;">
        <div style="font-size:0.8rem; color:var(--muted); text-align:center;">Загрузка списка эффектов...</div>
      </div>

      <div style="margin-bottom:14px; font-size:0.75rem; color:var(--muted);" id="fs-info">
        💾 Память LittleFS
      </div>

      <div style="border-top:1px solid var(--border); padding-top:12px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
          <h3 style="font-size:0.9rem; color:#38bdf8;">🌐 Скачать новые эффекты (GitHub):</h3>
          <button class="btn" style="width:auto; padding:5px 12px; font-size:0.75rem; background:#0284c7;" onclick="loadCatalog()">🔄 Каталог</button>
        </div>
        <div id="catalog-list" style="display:flex; flex-direction:column; gap:8px;">
          <div style="font-size:0.8rem; color:var(--muted); text-align:center; padding:6px;">Нажмите «Каталог», чтобы открыть магазин эффектов</div>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>Статус</h2>
      <div class="row"><span>Версия прошивки:</span><span class="val">v)rawliteral" FIRMWARE_VERSION R"rawliteral(</span></div>
      <div class="row"><span>IP адрес:</span><span class="val">)rawliteral";
  html += WiFi.localIP().toString();
  html += R"rawliteral(</span></div>
      <div class="row"><span>mDNS адрес:</span><span class="val"><a href="http://lamp.local" style="color:#f97316; text-decoration:none;">http://lamp.local</a></span></div>
      <div class="row"><span>Wi-Fi сеть:</span><span class="val">)rawliteral";
  html += WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)";
  html += R"rawliteral(</span></div>
      <div class="row"><span>Время работы:</span><span class="val">)rawliteral";
  html += getUptime();
  html += R"rawliteral(</span></div>
      <div style="margin-top:10px;">
        <button class="btn" style="background:#334155; padding:8px; font-size:0.85rem;" onclick="fetch('/api/show_ip')">📟 Показать IP на лампе бегущей строкой</button>
      </div>
    </div>

    <div class="card">
      <h2>🌐 Обновление прошивки (GitHub Cloud OTA)</h2>
      <div class="row"><span>Установленная версия:</span><span class="val">v)rawliteral" FIRMWARE_VERSION R"rawliteral(</span></div>
      
      <div id="ota-card" style="margin-top:12px;">
)rawliteral";

  if (cloudUpdateAvailable) {
    html += R"rawliteral(
        <div style="background:#14532d; border:1px solid #22c55e; border-radius:10px; padding:14px; margin-bottom:12px;">
          <div style="font-weight:bold; color:#4ade80; font-size:1rem; margin-bottom:6px;">🎉 Доступна новая версия v)rawliteral";
    html += cloudVersion;
    html += R"rawliteral(!</div>
          <div style="font-size:0.85rem; color:#dcfce7; margin-bottom:10px; line-height:1.4;">Сообщение от создателя:<br><b>")rawliteral";
    html += cloudChangelog;
    html += R"rawliteral("</b></div>
          <button class="btn" style="background:#22c55e; font-size:1rem;" onclick="startCloudOta()">Установить обновление из интернета</button>
        </div>)rawliteral";
  } else {
    html += R"rawliteral(
        <p style="font-size:0.85rem; color:var(--muted); margin-bottom:12px;">Установлена актуальная версия. Лампа автоматически проверяет обновления раз в сутки.</p>
        <button class="btn" id="chk-btn" onclick="checkCloudOta()">Проверить обновления в облаке</button>)rawliteral";
  }

  html += R"rawliteral(
        <div id="ota-stat-msg" style="margin-top:10px; font-size:0.85rem; text-align:center; color:var(--muted);"></div>
      </div>

      <details style="margin-top:16px; font-size:0.85rem; border-top:1px solid var(--border); padding-top:10px; color:var(--muted);">
        <summary style="cursor:pointer; font-weight:600; color:var(--muted);">⚙️ Инженерные настройки</summary>
        <div style="margin-top:10px;">
          <label>URL манифеста (version.json):</label>
          <input type="text" id="ota-url" value=")rawliteral";
  html += cloudManifestUrl;
  html += R"rawliteral(" style="background:#0f172a; color:#fff; padding:8px; border:1px solid var(--border); border-radius:6px; width:100%; font-size:0.8rem; margin-bottom:10px;">
          
          <label style="display:flex; align-items:center; gap:8px; cursor:pointer;">
            <input type="checkbox" id="ota-auto" style="width:auto;" )rawliteral";
  html += (autoOtaEnabled ? "checked" : "");
  html += R"rawliteral(>
            <span>Авто-установка, когда лампа выключена</span>
          </label>
          
          <button class="btn" style="background:#475569; margin-top:10px; padding:8px;" onclick="saveOtaConfig()">Сохранить настройки GitHub</button>
        </div>
      </details>
    </div>

    <div class="card">
      <h2>⚡ Ручная прошивка файла (Локальный Web OTA)</h2>
      <div class="file-box" onclick="document.getElementById('f-file').click()">
        <span id="f-label" style="font-size:0.85rem; color:var(--muted);">Выберите локальный файл .bin</span>
        <input type="file" id="f-file" accept=".bin" style="display:none;" onchange="onFileSelected()">
      </div>
      <button class="btn" id="up-btn" onclick="startUpload()" disabled>Залить новую прошивку</button>
      <div id="p-box" style="margin-top:10px; display:none;">
        <div style="display:flex; justify-content:space-between; font-size:0.8rem;"><span id="p-stat">Загрузка...</span><span id="p-pct">0%</span></div>
        <div class="progress-bg"><div class="progress" id="p-bar"></div></div>
      </div>
      <div id="s-msg" style="margin-top:10px; text-align:center; font-size:0.9rem;"></div>
    </div>
  </div>

  <script>
    let pwr = )rawliteral";
  html += (lampOn ? "true" : "false");
  html += R"rawliteral(;
    function togglePower() {
      pwr = !pwr;
      const b = document.getElementById('pwr-btn');
      b.className = pwr ? "btn btn-power on" : "btn btn-power";
      b.innerText = pwr ? "Питание: ВКЛ" : "Питание: ВЫКЛ";
      fetch('/api/set?power=' + (pwr ? '1' : '0'));
    }
    function setEffect(val) {
      const sel = document.getElementById('eff-sel');
      if (sel) sel.value = val;
      fetch('/api/set?effect=' + encodeURIComponent(val)).then(() => loadInstalledFx());
    }
    function setBrightness(val) {
      document.getElementById('brt-val').innerText = Math.round((val/255)*100) + '%';
      fetch('/api/set?bright=' + val);
    }
    function setColor(hex) { fetch('/api/set?color=' + hex.replace('#','')); }

    function loadInstalledFx() {
      fetch('/api/effects/list')
        .then(r => r.json())
        .then(d => {
          const c = document.getElementById('installed-list');
          c.innerHTML = '';
          d.builtin.forEach(b => {
            const row = document.createElement('div');
            row.style = 'background:#0f172a; padding:8px 12px; border-radius:8px; display:flex; justify-content:space-between; align-items:center; border:1px solid var(--border);';
            row.innerHTML = `<label style="display:flex; align-items:center; gap:10px; font-size:0.85rem; font-weight:600; cursor:pointer; margin:0;">
                               <input type="checkbox" id="chk-${b.id}" ${b.in_btn ? 'checked' : ''} onchange="toggleBtnFx('${b.id}')" style="width:18px; height:18px; accent-color:var(--accent); cursor:pointer;">
                               <span>${b.name} <span style="font-size:0.7rem; color:var(--muted); font-weight:normal;">(базовый)</span></span>
                             </label>
                             <button class="btn" style="width:auto; padding:5px 10px; font-size:0.75rem; ${d.active === b.id ? 'background:var(--success);' : 'background:#334155;'}" onclick="setEffect('${b.id}')">${d.active === b.id ? 'Активен' : 'Включить'}</button>`;
            c.appendChild(row);
          });
          d.modular.forEach(m => {
            const row = document.createElement('div');
            row.style = 'background:#0f172a; padding:8px 12px; border-radius:8px; display:flex; justify-content:space-between; align-items:center; border:1px solid var(--border);';
            row.innerHTML = `<label style="display:flex; align-items:center; gap:10px; font-size:0.85rem; font-weight:600; cursor:pointer; margin:0;">
                               <input type="checkbox" id="chk-${m.id}" ${m.in_btn ? 'checked' : ''} onchange="toggleBtnFx('${m.id}')" style="width:18px; height:18px; accent-color:var(--accent); cursor:pointer;">
                               <span>${m.name}</span>
                             </label>
                             <div style="display:flex; gap:6px;">
                               <button class="btn" style="width:auto; padding:5px 10px; font-size:0.75rem; ${d.active === m.id ? 'background:var(--success);' : 'background:#334155;'}" onclick="setEffect('${m.id}')">${d.active === m.id ? 'Активен' : 'Включить'}</button>
                               <button class="btn" style="width:auto; padding:5px 8px; font-size:0.75rem; background:var(--error);" onclick="deleteFx('${m.id}', '${m.name}')">🗑</button>
                             </div>`;
            c.appendChild(row);
          });

          const kbUsed = Math.round(d.fs_used / 1024);
          const kbTotal = Math.round(d.fs_total / 1024);
          const pct = kbTotal > 0 ? Math.round((d.fs_used / d.fs_total) * 100) : 0;
          document.getElementById('fs-info').innerText = `💾 Память LittleFS: ${kbUsed} КБ из ${kbTotal} КБ (занято ${pct}%)`;
        });
    }

    function toggleBtnFx(id) {
      fetch('/api/effects/toggle_btn?id=' + encodeURIComponent(id), { method: 'POST' });
    }

    function deleteFx(id, name) {
      if (!confirm(`Удалить эффект "${name}" из памяти лампы?`)) return;
      fetch('/api/effects/delete?id=' + encodeURIComponent(id), { method: 'POST' })
        .then(r => r.json())
        .then(d => {
          if (d.success) {
            loadInstalledFx();
            location.reload();
          }
        });
    }

    function loadCatalog() {
      const c = document.getElementById('catalog-list');
      c.innerHTML = '<div style="font-size:0.8rem; color:var(--muted); text-align:center; padding:8px;">⏳ Загрузка каталога с GitHub...</div>';
      
      Promise.all([fetch('/api/effects/catalog').then(r => r.json()), fetch('/api/effects/list').then(r => r.json())])
        .then(([cat, inst]) => {
          c.innerHTML = '';
          const installedIds = new Set(inst.modular.map(m => m.id));
          if (!cat.effects || cat.effects.length === 0) {
            c.innerHTML = '<div style="font-size:0.8rem; color:var(--muted); text-align:center; padding:8px;">Каталог пуст</div>';
            return;
          }
          cat.effects.forEach(e => {
            const isInst = installedIds.has(e.id);
            const card = document.createElement('div');
            card.style = 'background:#0f172a; padding:10px 12px; border-radius:8px; border:1px solid #1e3a5f; display:flex; justify-content:space-between; align-items:center;';
            card.innerHTML = `<div style="max-width:70%;">
                                <div style="font-size:0.85rem; font-weight:bold; color:#f8fafc;">${e.name} <span style="font-size:0.7rem; color:var(--muted); font-weight:normal;">v${e.ver || '1.0'}</span></div>
                                <div style="font-size:0.75rem; color:var(--muted); margin-top:2px; line-height:1.3;">${e.desc}</div>
                              </div>
                              <div>
                                ${isInst 
                                  ? '<span style="font-size:0.75rem; color:#4ade80; font-weight:600;">✓ Скачан</span>'
                                  : `<button class="btn" id="btn-dl-${e.id}" style="width:auto; padding:6px 10px; font-size:0.75rem; background:#0284c7;" onclick="installFx('${e.id}', this)">📥 Скачать</button>`
                                }
                              </div>`;
            c.appendChild(card);
          });
        })
        .catch(err => {
          c.innerHTML = '<div style="font-size:0.8rem; color:var(--error); text-align:center; padding:8px;">❌ Ошибка загрузки каталога</div>';
        });
    }

    function installFx(id, btn) {
      btn.disabled = true;
      btn.innerText = "⏳...";
      fetch('/api/effects/install?id=' + encodeURIComponent(id), { method: 'POST' })
        .then(r => r.json())
        .then(d => {
          if (d.success) {
            btn.outerHTML = '<span style="font-size:0.75rem; color:#4ade80; font-weight:600;">✓ Скачан</span>';
            loadInstalledFx();
            location.reload();
          } else {
            alert("Ошибка скачивания: " + (d.error || "Неизвестная ошибка"));
            btn.disabled = false;
            btn.innerText = "📥 Скачать";
          }
        })
        .catch(e => {
          alert("Сбой соединения при скачивании");
          btn.disabled = false;
          btn.innerText = "📥 Скачать";
        });
    }
    function onFileSelected() {
      const f = document.getElementById('f-file');
      if (f.files.length > 0) {
        document.getElementById('f-label').innerText = f.files[0].name;
        document.getElementById('up-btn').disabled = false;
      }
    }
    function checkCloudOta() {
      const b = document.getElementById('chk-btn');
      const m = document.getElementById('ota-stat-msg');
      if (b) b.disabled = true;
      m.innerText = "⏳ Подключение к GitHub...";
      fetch('/api/ota/check')
        .then(r => r.json())
        .then(d => {
          if (b) b.disabled = false;
          if (d.available) {
            location.reload();
          } else {
            m.innerText = "✅ Установлена самая свежая версия!";
          }
        })
        .catch(e => {
          if (b) b.disabled = false;
          m.innerText = "❌ Ошибка проверки связи с GitHub.";
        });
    }
    function startCloudOta() {
      if (!confirm("Запустить обновление прошивки из интернета? Лампа скачает файл и перезагрузится.")) return;
      const m = document.getElementById('ota-stat-msg');
      m.innerText = "⏳ Загрузка прошивки из интернета... Пожалуйста, не выключайте лампу.";
      fetch('/api/ota/start', { method: 'POST' })
        .then(r => {
          setTimeout(() => {
            m.innerText = "Лампа обновляется и перезагружается. Страница обновится через 15 секунд...";
            setTimeout(() => location.reload(), 15000);
          }, 3000);
        });
    }
    function saveOtaConfig() {
      const u = document.getElementById('ota-url').value;
      const a = document.getElementById('ota-auto').checked ? '1' : '0';
      fetch('/api/ota/config?url=' + encodeURIComponent(u) + '&auto_ota=' + a, { method: 'POST' })
        .then(r => alert("Настройки GitHub сохранены!"));
    }
    function startUpload() {
      const f = document.getElementById('f-file').files[0];
      const data = new FormData();
      data.append('update', f);
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);
      document.getElementById('p-box').style.display = 'block';
      document.getElementById('up-btn').disabled = true;
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const p = Math.round((e.loaded / e.total) * 100);
          document.getElementById('p-bar').style.width = p + '%';
          document.getElementById('p-pct').innerText = p + '%';
        }
      };
      xhr.onload = () => {
        if (xhr.status === 200 && xhr.responseText === 'OK') {
          document.getElementById('s-msg').innerHTML = '<span style="color:var(--success)">✅ Готово! Перезагрузка...</span>';
          setTimeout(() => location.reload(), 6000);
        } else {
          document.getElementById('s-msg').innerHTML = '<span style="color:var(--error)">❌ Ошибка: ' + xhr.responseText + '</span>';
        }
      };
      xhr.send(data);
    }
    function setAudioSens(val) {
      fetch('/api/audio/set?sens=' + val, { method: 'POST' });
    }
    function updateAudioStatus() {
      fetch('/api/audio/status')
        .then(r => r.json())
        .then(d => {
          const st = document.getElementById('mic-status');
          const snd = document.getElementById('mic-sound');
          const vu = document.getElementById('vu-bar');
          if (st) {
            st.innerHTML = d.ready 
              ? '<span style="color:var(--success);">✓ Подключен (I2S OK)</span>' 
              : '<span style="color:var(--muted);">Демо-режим (I2S не активен)</span>';
          }
          if (snd) {
            snd.innerHTML = d.sound 
              ? '<span style="color:var(--success); font-weight:bold;">Звук обнаружен 🎵</span>' 
              : '<span style="color:var(--muted);">Тишина / Демо-дыхание</span>';
          }
          if (vu) {
            const pct = Math.min(100, Math.round((d.vol / 255) * 100));
            vu.style.width = pct + '%';
          }
        })
        .catch(() => {});
    }
    loadInstalledFx();
    updateAudioStatus();
    setInterval(updateAudioStatus, 1500);
  </script>
</body></html>)rawliteral";

  server.send(200, "text/html", html);
}

// [QA FIX #6] Валидация непустого SSID
void handleSaveWifi() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    String s = server.arg("ssid");
    String p = server.arg("pass");
    s.trim();
    if (s.length() == 0) {
      server.send(400, "text/html", "<h3>Ошибка: Имя сети (SSID) не может быть пустым.</h3>");
      return;
    }
    saveWifiCredentials(s, p);
    server.send(200, "text/html", "<h3>Настройки сохранены!</h3><p>Лампа перезагружается и подключается к сети " + s + "...</p>");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleApiSet() {
  if (server.hasArg("power")) {
    lampOn = (server.arg("power") == "1");
    markSettingsChanged();
  }
  if (server.hasArg("effect")) {
    String ef = server.arg("effect");
    activateEffect(ef);
    markSettingsChanged();
  }
  if (server.hasArg("bright")) {
    lampBrightness = constrain(server.arg("bright").toInt(), 5, 255);
    markSettingsChanged();
  }
  if (server.hasArg("color")) {
    String c = server.arg("color");
    c.trim();
    if (c.startsWith("#")) c = c.substring(1); // [QA AUDIT FIX] Безопасная обработка HEX цвета с # или без
    solidColor = (uint32_t)strtol(c.c_str(), NULL, 16);
    markSettingsChanged();
  }
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("  GyverLamp ESP32-C6 (Production Build) ");
  Serial.println("========================================");

  // [QA FIX #4] Внутренняя подтяжка к земле (исключает ложные нажатия от наводок)
  pinMode(PIN_BUTTON, INPUT_PULLDOWN);

  // Проверка удержания кнопки при старте (для принудительного входа в AP-режим)
  bool forceAP = false;
  if (digitalRead(PIN_BUTTON) == HIGH) {
    Serial.println("[Кнопка] Обнаружено касание сенсора при включении питания...");
    delay(2000);
    if (digitalRead(PIN_BUTTON) == HIGH) {
      Serial.println("[Кнопка] Активирован принудительный режим настройки Wi-Fi (AP)!");
      forceAP = true;
    }
  }

  // 2. Инициализация матрицы WS2812B
  matrix.begin();
  matrix.setBrightness(255); // 1:1 прямой вывод, яркость масштабируется математически в renderLamp
  matrix.clear();
  matrix.show();

  // Инициализация файловой системы LittleFS (хранилище модульных эффектов)
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] ОШИБКА: LittleFS не смонтировался. Аварийный режим.");
    fsAvailable = false;
  } else {
    Serial.println("[FS] LittleFS успешно смонтирован.");
    fsAvailable = true;
    if (!LittleFS.exists("/effects")) {
      LittleFS.mkdir("/effects");
    }
    if (!LittleFS.exists("/effects/installed.json")) {
      File f = LittleFS.open("/effects/installed.json", "w");
      if (f) {
        f.print("[]");
        f.close();
      }
    }
  }

  // 3. Инициализация цифрового микрофона INMP441
  audio.begin(PIN_I2S_SCK, PIN_I2S_WS, PIN_I2S_SD);

  // 4. Загрузка сохраненных настроек лампы из NVS
  loadLampSettings();

  // 5. Подключение к Wi-Fi или запуск точки доступа
  loadWifiCredentials();
  if (forceAP || !connectToWifi()) {
    startAccessPoint();
  } else {
    if (MDNS.begin("lamp")) {
      Serial.println("[OK] mDNS: http://lamp.local");
    }
    ArduinoOTA.setHostname("lamp");
    ArduinoOTA.begin();
    if (lampOn) {
      triggerIpScroll(); // Запуск бегущей строки IP-адреса на матрице при старте только если лампа включена
    }
  }

  // 6. Роуты веб-сервера
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save_wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/set", HTTP_GET, handleApiSet);
  server.on("/api/show_ip", HTTP_GET, []() {
    triggerIpScroll();
    server.send(200, "text/plain", "OK");
  });
  server.on("/rescan", HTTP_GET, []() {
    triggerWifiScan();
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  // Роуты модульного магазина эффектов (LittleFS & GitHub)
  server.on("/api/effects/list", HTTP_GET, []() {
    String json = "{";
    json += "\"active\":\"" + currentEffectId + "\",";
    json += "\"builtin\":[";
    json += "{\"id\":\"fire\",\"name\":\"🔥 Огонь\",\"in_btn\":" + String(isBuiltinInBtn("fire") ? "true" : "false") + "},";
    json += "{\"id\":\"matrix\",\"name\":\"🟢 Матрица\",\"in_btn\":" + String(isBuiltinInBtn("matrix") ? "true" : "false") + "},";
    json += "{\"id\":\"fireflies\",\"name\":\"✨ Светлячки\",\"in_btn\":" + String(isBuiltinInBtn("fireflies") ? "true" : "false") + "},";
    json += "{\"id\":\"solid\",\"name\":\"💡 Ночник\",\"in_btn\":" + String(isBuiltinInBtn("solid") ? "true" : "false") + "},";
    json += "{\"id\":\"sound_pulse\",\"name\":\"🎵 Пульс музыки\",\"in_btn\":" + String(isBuiltinInBtn("sound_pulse") ? "true" : "false") + "}";
    json += "],";
    json += "\"modular\":[";
    if (fsAvailable) {
      auto list = getInstalledModularList();
      for (size_t i = 0; i < list.size(); i++) {
        json += "{\"id\":\"" + list[i].id + "\",\"name\":\"" + list[i].name + "\",\"in_btn\":" + (list[i].inBtn ? "true" : "false") + "}";
        if (i + 1 < list.size()) json += ",";
      }
    }
    json += "],";
    size_t totalBytes = fsAvailable ? LittleFS.totalBytes() : 0;
    size_t usedBytes = fsAvailable ? LittleFS.usedBytes() : 0;
    json += "\"fs_total\":" + String(totalBytes) + ",";
    json += "\"fs_used\":" + String(usedBytes);
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/effects/catalog", HTTP_GET, []() {
    if (WiFi.status() != WL_CONNECTED) {
      server.send(503, "application/json", "{\"error\":\"Wi-Fi не подключен\"}");
      return;
    }
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(4);
    HTTPClient http;
    if (http.begin(client, effectsCatalogUrl)) {
      http.setTimeout(4000);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        http.end();
        server.send(200, "application/json", payload);
        return;
      }
      http.end();
    }
    server.send(502, "application/json", "{\"error\":\"Не удалось получить каталог с GitHub\"}");
  });

  server.on("/api/effects/install", HTTP_POST, []() {
    if (!server.hasArg("id")) {
      server.send(400, "application/json", "{\"error\":\"Отсутствует id эффекта\"}");
      return;
    }
    if (!fsAvailable) {
      server.send(500, "application/json", "{\"error\":\"Файловая система недоступна\"}");
      return;
    }
    if (isEffectDownloading) {
      server.send(409, "application/json", "{\"error\":\"Уже идет скачивание другого эффекта\"}");
      return;
    }

    String id = server.arg("id");
    id.trim();

    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeBytes < 2048) {
      server.send(507, "application/json", "{\"error\":\"Недостаточно свободной памяти на лампе\"}");
      return;
    }

    isEffectDownloading = true;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(4);
    HTTPClient http;
    String fxUrl = effectsBaseUrl + id + ".fx";

    bool success = false;
    String errorMsg = "";

    if (http.begin(client, fxUrl)) {
      http.setTimeout(4000);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        http.end();

        ActiveModularEffect testFx;
        if (parseModularEffect(payload, testFx)) {
          File f = LittleFS.open("/effects/" + id + ".fx", "w");
          if (f) {
            f.print(payload);
            f.close();

            auto list = getInstalledModularList();
            bool found = false;
            for (auto& item : list) {
              if (item.id == id) {
                item.name = String(testFx.name);
                found = true;
                break;
              }
            }
            if (!found) {
              InstalledFxItem newItem;
              newItem.id = id;
              newItem.name = String(testFx.name);
              newItem.inBtn = true;
              list.push_back(newItem);
            }
            saveInstalledModularList(list);
            success = true;
          } else {
            errorMsg = "Ошибка записи файла в LittleFS";
          }
        } else {
          errorMsg = "Невалидный формат файла эффекта";
        }
      } else {
        http.end();
        errorMsg = "HTTP ошибка скачивания: " + String(httpCode);
      }
    } else {
      errorMsg = "Не удалось подключиться к GitHub";
    }

    isEffectDownloading = false;

    if (success) {
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(500, "application/json", "{\"error\":\"" + errorMsg + "\"}");
    }
  });

  server.on("/api/effects/delete", HTTP_POST, []() {
    if (!server.hasArg("id")) {
      server.send(400, "application/json", "{\"error\":\"Отсутствует id эффекта\"}");
      return;
    }
    if (!fsAvailable) {
      server.send(500, "application/json", "{\"error\":\"Файловая система недоступна\"}");
      return;
    }
    String id = server.arg("id");
    id.trim();

    if (currentEffectId == id) {
      activateEffect("fire");
      markSettingsChanged();
    }

    String path = "/effects/" + id + ".fx";
    if (LittleFS.exists(path)) {
      LittleFS.remove(path);
    }

    auto list = getInstalledModularList();
    std::vector<InstalledFxItem> updated;
    for (const auto& item : list) {
      if (item.id != id) {
        updated.push_back(item);
      }
    }
    saveInstalledModularList(updated);

    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/effects/toggle_btn", HTTP_POST, []() {
    if (!server.hasArg("id")) {
      server.send(400, "application/json", "{\"error\":\"Отсутствует id\"}");
      return;
    }
    String id = server.arg("id");
    id.trim();

    if (id == "fire" || id == "matrix" || id == "fireflies" || id == "solid" || id == "sound_pulse") {
      toggleBuiltinInBtn(id);
      server.send(200, "application/json", "{\"success\":true}");
      return;
    }

    if (fsAvailable) {
      auto list = getInstalledModularList();
      for (auto& item : list) {
        if (item.id == id) {
          item.inBtn = !item.inBtn;
          break;
        }
      }
      saveInstalledModularList(list);
      server.send(200, "application/json", "{\"success\":true}");
      return;
    }

    server.send(404, "application/json", "{\"error\":\"Эффект не найден\"}");
  });

  // [QA FIX #2] Роуты перехвата для всплывающего окна Captive Portal на iOS и Android
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/canonical.html", HTTP_GET, handleRoot);

  server.onNotFound([]() {
    if (apMode) {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  // Обработчик Web OTA обновления
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    delay(500);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[Web OTA] Запись файла: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("[Web OTA] Успешно прошито: %u байт\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  // 6. Роуты облачного OTA (GitHub)
  server.on("/api/ota/status", HTTP_GET, []() {
    String json = "{";
    json += "\"current\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"available\":" + String(cloudUpdateAvailable ? "true" : "false") + ",";
    json += "\"cloud_version\":\"" + cloudVersion + "\",";
    json += "\"changelog\":\"" + cloudChangelog + "\",";
    json += "\"url\":\"" + cloudManifestUrl + "\",";
    json += "\"auto_ota\":" + String(autoOtaEnabled ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/ota/check", HTTP_GET, []() {
    bool hasUpdate = checkCloudUpdate(false);
    String json = "{";
    json += "\"success\":true,";
    json += "\"available\":" + String(hasUpdate ? "true" : "false") + ",";
    json += "\"version\":\"" + cloudVersion + "\",";
    json += "\"changelog\":\"" + cloudChangelog + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/ota/start", HTTP_POST, []() {
    if (cloudUpdateAvailable && cloudBinUrl.length() > 0) {
      server.send(200, "text/plain", "OK");
      performCloudUpdate(false); // Ручной интерактивный запуск: показывать мягкий прогресс-бар
    } else {
      server.send(400, "text/plain", "No update available");
    }
  });

  server.on("/api/ota/config", HTTP_POST, []() {
    if (server.hasArg("url")) {
      String url = server.arg("url");
      bool autoOta = server.hasArg("auto_ota") && (server.arg("auto_ota") == "1");
      saveOtaSettings(url, autoOta);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Bad Request");
    }
  });

  // 7. Роуты микрофона INMP441 (Светомузыка)
  server.on("/api/audio/status", HTTP_GET, []() {
    String json = "{";
    json += "\"ready\":" + String(audio.isReady() ? "true" : "false") + ",";
    json += "\"sound\":" + String(audio.isSound() ? "true" : "false") + ",";
    json += "\"vol\":" + String(audio.getVolume()) + ",";
    json += "\"raw\":" + String(audio.getRawVolume()) + ",";
    json += "\"beat\":" + String(audio.getBeatIntensity()) + ",";
    json += "\"sens\":" + String(audio.getSensitivity());
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/audio/set", HTTP_POST, []() {
    if (server.hasArg("sens")) {
      uint8_t sens = constrain(server.arg("sens").toInt(), 10, 100);
      audio.setSensitivity(sens);
      Preferences p;
      p.begin("lamp_cfg", false);
      p.putUChar("audio_sens", sens);
      p.end();
      server.send(200, "application/json", "{\"success\":true,\"sens\":" + String(sens) + "}");
    } else {
      server.send(400, "application/json", "{\"error\":\"Missing sens parameter\"}");
    }
  });

  server.begin();
  Serial.println("[OK] Сервер запущен.");
}

void loop() {
  // Неблокирующий опрос микрофона INMP441 через I2S DMA
  static unsigned long lastAudioUpdate = 0;
  if (millis() - lastAudioUpdate >= 15) {
    lastAudioUpdate = millis();
    audio.update();
  }

  if (apMode) {
    dnsServer.processNextRequest();
    checkWifiScan(); // [QA AUDIT FIX] Неблокирующий опрос сканера сетей
  } else {
    ArduinoOTA.handle();

    // [QA FIX #3] Мониторинг разрыва Wi-Fi (например, перезагрузка домашнего роутера)
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 15000) {
      lastWifiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Wi-Fi] Связь потеряна! Авто-переподключение...");
        WiFi.reconnect();
      }
    }

    // Подтверждение стабильности прошивки (Rollback) через 30 секунд после старта
    static bool otaMarkedValid = false;
    if (!otaMarkedValid && millis() > 30000) {
      otaMarkedValid = true;
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("[OTA Rollback] Прошивка отработала 30 сек без сбоев. Раздел подтвержден как стабильный.");
    }

    // Фоновая проверка облачных обновлений раз в сутки
    if (WiFi.status() == WL_CONNECTED && !isUpdatingCloudOta) {
      if (!initialCloudCheckDone && millis() > 30000) {
        initialCloudCheckDone = true;
        lastCloudCheck = millis();
        checkCloudUpdate(true); // true = запустить тихое обновление, только если лампа выключена
      } else if (initialCloudCheckDone && (millis() - lastCloudCheck > CLOUD_CHECK_INTERVAL)) {
        lastCloudCheck = millis();
        checkCloudUpdate(true);
      }
    }
  }
  
  // Шаг сдвига бегущей строки IP (каждые 45 мс)
  if (isScrollingText && (millis() - lastScrollStep >= 45)) {
    lastScrollStep = millis();
    scrollTextX--;
    if (scrollTextX < -totalScrollWidth) {
      isScrollingText = false;
      Serial.println("[Экран] Бегущая строка IP завершена.");
    }
  }

  server.handleClient();
  handleButton();
  processNvsSaving();

  // Отрисовка кадров с индивидуальной скоростью эффекта
  static unsigned long lastFrameTime = 0;
  uint8_t speed = isModularActive ? currentModularFx.speedMs : ((lampEffect < TOTAL_EFFECTS) ? effectsList[lampEffect].speedMs : 30);
  if (millis() - lastFrameTime >= speed) {
    lastFrameTime = millis();
    renderLamp();
  }
}
