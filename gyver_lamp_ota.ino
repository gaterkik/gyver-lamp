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

#define FIRMWARE_VERSION  "1.0.1"

// ================= НАСТРОЙКИ ОБОРУДОВАНИЯ =================
#define PIN_MATRIX        2      // Пин данных матрицы WS2812B
#define PIN_BUTTON        1      // Пин сенсорной кнопки TTP223
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
uint8_t lampEffect = 0;        // Индекс текущего эффекта
uint32_t solidColor = 0xFFAA33; // Цвет для ночника (янтарный)

// Отложенное сохранение в NVS (чтобы не убивать флеш частыми записями)
bool nvsSavePending = false;
unsigned long nvsSaveTimer = 0;

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
// Координаты капель для эффекта Матрицы
int8_t matrixDrops[MATRIX_WIDTH];
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

// ================= ЭНЕРГОНЕЗАВИСИМАЯ ПАМЯТЬ (NVS / PREFERENCES) =================
void loadLampSettings() {
  prefsLamp.begin("lamp_cfg", true); // readonly
  lampOn = prefsLamp.getBool("power", true);
  lampBrightness = prefsLamp.getUChar("bright", 70);
  lampEffect = prefsLamp.getUChar("effect", 0);
  solidColor = prefsLamp.getUInt("color", 0xFFAA33);
  cloudManifestUrl = prefsLamp.getString("ota_url", "https://raw.githubusercontent.com/oleg-dev/gyver-lamp/main/version.json");
  autoOtaEnabled = prefsLamp.getBool("auto_ota", true);
  prefsLamp.end();
  Serial.printf("[NVS] Загружено: Лампа=%s, Яркость=%u, Эффект=%u, Цвет=#%06X\n",
                lampOn ? "ВКЛ" : "ВЫКЛ", lampBrightness, lampEffect, solidColor);
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
    prefsLamp.putUChar("effect", lampEffect);
    prefsLamp.putUInt("color", solidColor);
    prefsLamp.end();
    Serial.println("[NVS] Настройки лампы сохранены во флеш-память.");
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

// 2. Матрица
void effectMatrix() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = canvas[i];
    uint8_t g = (c >> 8) & 0xFF;
    g = (g > 16) ? g - 16 : 0;
    canvas[i] = matrix.Color(0, g, 0);
  }
  for (int x = 0; x < MATRIX_WIDTH; x++) {
    if (matrixDrops[x] < 0) {
      if (random(0, 100) < 8) matrixDrops[x] = MATRIX_HEIGHT - 1;
    } else {
      setPixel(x, matrixDrops[x], matrix.Color(180, 255, 180));
      if (matrixDrops[x] + 1 < MATRIX_HEIGHT) {
        setPixel(x, matrixDrops[x] + 1, matrix.Color(0, 255, 0));
      }
      matrixDrops[x]--;
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

// ================= МОДУЛЬНАЯ ТАБЛИЦА ЭФФЕКТОВ =================
typedef void (*EffectFunction)();

struct EffectDef {
  const char* id;
  const char* name;
  EffectFunction func;
  uint8_t speedMs;
};

const EffectDef effectsList[] = {
  {"fire",      "🔥 Огонь",        effectFire,      30},
  {"matrix",    "🟢 Матрица",      effectMatrix,    45},
  {"rainbow",   "🌈 Радуга",       effectRainbow,   20},
  {"fireflies", "✨ Светлячки",    effectFireflies, 35},
  {"solid",     "💡 Ночник/Свет",  effectSolid,     50}
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
    if (lampEffect < TOTAL_EFFECTS) {
      effectsList[lampEffect].func();
    } else {
      lampEffect = 0;
      effectsList[0].func();
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
            lampEffect = (lampEffect + 1) % TOTAL_EFFECTS;
            markSettingsChanged();
            Serial.printf("[Кнопка] Двойной клик: Эффект -> %s\n", effectsList[lampEffect].name);
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

void performCloudUpdate(); // прототип

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

        // Продуктовое правило: если лампа ВЫКЛЮЧЕНА (standby) и включено автообновление
        if (triggerUpdateIfStandby && autoOtaEnabled && !lampOn) {
          Serial.println("[Cloud OTA] Лампа выключена (standby). Запуск фонового обновления...");
          performCloudUpdate();
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
void performCloudUpdate() {
  if (cloudBinUrl.length() == 0 || isUpdatingCloudOta) return;
  if (WiFi.status() != WL_CONNECTED) return;

  isUpdatingCloudOta = true;
  Serial.printf("[Cloud OTA] Старт загрузки прошивки v%s...\n", cloudVersion.c_str());

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Мягкий неслепящий прогресс-бар на матрице (10-15% яркости)
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

  t_httpUpdate_return ret = httpUpdate.update(client, cloudBinUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[Cloud OTA] Сбой: (%d): %s\n", 
                    httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      // Мягкая красная индикация сбоя
      for (int i = 0; i < NUM_LEDS; i++) matrix.setPixelColor(i, matrix.Color(40, 0, 0));
      matrix.show();
      delay(1500);
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
      // Мягкая зеленая индикация успеха
      for (int i = 0; i < NUM_LEDS; i++) matrix.setPixelColor(i, matrix.Color(0, 45, 0));
      matrix.show();
      delay(800);
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
  effectOptions.reserve(512);
  for (uint8_t i = 0; i < TOTAL_EFFECTS; i++) {
    effectOptions += "<option value='" + String(i) + "'" + (lampEffect == i ? " selected" : "") + ">" + String(effectsList[i].name) + "</option>";
  }

  String html = "";
  html.reserve(6144);
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
    function setEffect(val) { fetch('/api/set?effect=' + val); }
    function setBrightness(val) {
      document.getElementById('brt-val').innerText = Math.round((val/255)*100) + '%';
      fetch('/api/set?bright=' + val);
    }
    function setColor(hex) { fetch('/api/set?color=' + hex.replace('#','')); }
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
    lampEffect = server.arg("effect").toInt() % TOTAL_EFFECTS;
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

  // 3. Загрузка сохраненных настроек лампы из NVS
  loadLampSettings();

  // 4. Подключение к Wi-Fi или запуск точки доступа
  loadWifiCredentials();
  if (forceAP || !connectToWifi()) {
    startAccessPoint();
  } else {
    if (MDNS.begin("lamp")) {
      Serial.println("[OK] mDNS: http://lamp.local");
    }
    ArduinoOTA.setHostname("lamp");
    ArduinoOTA.begin();
    triggerIpScroll(); // Запуск бегущей строки IP-адреса на матрице при подключении к Wi-Fi
  }

  // 5. Роуты веб-сервера
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
      delay(200);
      performCloudUpdate();
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

  server.begin();
  Serial.println("[OK] Сервер запущен.");
}

void loop() {
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
  uint8_t speed = (lampEffect < TOTAL_EFFECTS) ? effectsList[lampEffect].speedMs : 30;
  if (millis() - lastFrameTime >= speed) {
    lastFrameTime = millis();
    renderLamp();
  }
}
