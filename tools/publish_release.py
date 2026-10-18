import subprocess
import json
import urllib.request
import urllib.error
import sys
import os

# 1. Get GitHub token from git credential helper
def get_token():
    p = subprocess.Popen(["git", "credential", "fill"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, _ = p.communicate(input="protocol=https\nhost=github.com\n")
    for line in out.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise Exception("Token not found in git credentials")

token = get_token()
repo = "gaterkik/gyver-lamp"

def create_or_update_release(tag, name, body, bin_path=None, bin_name=None):
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github+json",
        "User-Agent": "Antigravity-Release-Tool"
    }
    
    # Check if release exists
    req = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases/tags/{tag}", headers=headers)
    release = None
    try:
        with urllib.request.urlopen(req) as resp:
            release = json.loads(resp.read().decode())
            print(f"Release {tag} already exists (id: {release['id']})")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            print(f"Creating release for {tag}...")
            post_data = json.dumps({
                "tag_name": tag,
                "name": name,
                "body": body,
                "draft": False,
                "prerelease": False
            }).encode('utf-8')
            create_req = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases", data=post_data, headers={**headers, "Content-Type": "application/json"})
            with urllib.request.urlopen(create_req) as resp:
                release = json.loads(resp.read().decode())
                print(f"Release {tag} created successfully (id: {release['id']})")
        else:
            raise

    # Upload binary asset if provided
    if bin_path and os.path.exists(bin_path):
        upload_url = release["upload_url"].split("{")[0]
        actual_name = bin_name or os.path.basename(bin_path)
        
        # Check existing assets
        for asset in release.get("assets", []):
            if asset["name"] == actual_name:
                print(f"Deleting existing asset {actual_name} (id: {asset['id']})...")
                del_req = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases/assets/{asset['id']}", headers=headers, method="DELETE")
                urllib.request.urlopen(del_req)
                break
        
        print(f"Uploading {actual_name} to release {tag}...")
        with open(bin_path, "rb") as f:
            data = f.read()
        
        upload_req = urllib.request.Request(
            f"{upload_url}?name={actual_name}",
            data=data,
            headers={
                **headers,
                "Content-Type": "application/octet-stream"
            }
        )
        with urllib.request.urlopen(upload_req) as resp:
            asset_res = json.loads(resp.read().decode())
            print(f"Asset uploaded: {asset_res['browser_download_url']} ({len(data)} bytes)")

releases = [
    {
        "tag": "v1.1.0",
        "name": "v1.1.0 — Модульная система эффектов LittleFS и Галерея",
        "body": """### 🎨 Release v1.1.0: Модульная система эффектов (LittleFS)

- 🎨 **Встроенный магазин «Галерея эффектов»**: Онлайн-каталог шейдеров из GitHub прямо в веб-интерфейсе `lamp.local`.
- ⚡ **Установка в 1 клик**: Скачивание процедурных шейдеров (`.fx`) напрямую в память LittleFS лампы без перекомпиляции и без перезагрузки ядра.
- 🔘 **Настраиваемая ротация**: Возможность отмечать галочкой, какие эффекты участвуют в переключении двойным тапом сенсора TTP223.
- 🚀 **Шейдерный движок 60 FPS**: Высокопроизводительный целочисленный 8-битный шум Перлина (`fast_noise8` + `fast_sin8`) без использования FPU на ядре RISC-V ESP32-C6.
- 🛡️ **Неприкосновенное ядро**: 4 базовых эффекта (Огонь, Матрица v1.0.4 с линиями разной длины, Светлячки, Ночник) жестко зашиты в Flash и работают автономно даже при повреждении файловой системы.""",
        "bin_name": "gyver_lamp_v1.1.0.bin"
    },
    {
        "tag": "v1.1.1",
        "name": "v1.1.1 — Полностью тихий режим ночного автообновления (Silent Auto-OTA)",
        "body": """### 🌙 Release v1.1.1: Полностью тихий режим ночного автообновления

- 🤫 **Silent Auto-OTA**: Когда лампа выключена (ночной режим ожидания), регламентное автообновление происходит **в полной темноте** — матрица остаётся абсолютно тёмной, синий прогресс-бар и световые вспышки отключены.
- 💤 **Бегущая строка IP**: При стартовой перезагрузке бегущая строка IP-адреса не запускается, если лампа была выключена, чтобы не тревожить сон в спальне.
- 🔘 **Интерактивный режим**: При ручном запуске обновления из веб-интерфейса мягкий бирюзовый прогресс-бар и индикация сохраняются.""",
        "bin_name": "gyver_lamp_v1.1.1.bin"
    },
    {
        "tag": "v1.1.2",
        "name": "v1.1.2 — Зелёный всполох успеха и универсальные галочки ротации кнопки",
        "body": """### 🟢 Release v1.1.2: Зелёный всполох успеха и универсальные галочки ротации кнопки

- 🟢 **Зелёный всполох завершения прошивки**: Отключен мгновенный автоматический перезапуск внутри библиотеки `HTTPUpdate` (`httpUpdate.rebootOnUpdate(false)`). При успешном окончании прошивки матрица 16×16 загорается красивым изумрудно-зелёным светом на 1.2 секунды, подтверждая успех перед перезагрузкой.
- 🔘 **Галочки ротации для ВСЕХ эффектов**: В карточке ротации сенсорной кнопки TTP223 теперь каждый эффект (и базовые: Огонь, Матрица, Светлячки, Ночник, и модульные `.fx`) снабжён чекбоксом:
  - **Отмеченные**: переключаются по циклу двойным тапом по сенсору.
  - **Неотмеченные**: пропускаются кнопкой, но остаются в памяти и могут быть включены в любой момент через веб-интерфейс.
  - Настройки базовых эффектов сохраняются в NVS (`btn_builtin` bitmask), а модульных — в LittleFS (`installed.json`).
  - Добавлен защитный fallback: если снять все галочки, кнопка безопасно переключается на эффект Огня.""",
        "bin_name": "gyver_lamp_v1.1.2.bin"
    },
    {
        "tag": "v1.2.0",
        "name": "v1.2.0 — Поддержка цифрового I2S микрофона INMP441 и Светомузыка",
        "body": """### 🎤 Release v1.2.0: Поддержка цифрового I2S микрофона INMP441 и Светомузыка

- 🎤 **Аппаратная поддержка цифрового MEMS-микрофона INMP441**:
  - Высококачественный 24-битный I2S аудиопоток без аналоговых наводок.
  - Подключение по 3 линиям: `GPIO 4` (SCK/BCLK), `GPIO 5` (WS/LRCLK), `GPIO 6` (SD/DOUT).
  - Питание 3.3V, вывод L/R к GND (левый моно-канал).
- 🎵 **Новый встроенный эффект «Пульс музыки» (Светомузыка)**:
  - Центробежные световые волны и пульсирующее цветовое ядро (0 float, быстрые целочисленные таблицы расстояний).
  - Адаптивное АРУ (Automatic Gain Control) с фильтром отсечки постоянной составляющей (DC-blocker).
  - Двойной фильтр скользящего среднего для детекции басовых ударов (битов).
- 🛡️ **Строгая защита от отсутствия микрофона (Zero-Freeze & Graceful Fallback)**:
  - Неблокирующее DMA чтение буфера (таймаут 0 мс): `loop()` никогда не блокируется и не зависает.
  - Если микрофон не припаян или вокруг тишина, лампа автоматически и бесшовно переходит в режим мягкого медитативного дыхания.
- 🎛️ **Управление в веб-интерфейсе `lamp.local`**:
  - Новая карточка «Микрофон INMP441»: статус подключения, детекция звука в реальном времени.
  - Индикатор уровня звука (VU Meter) с плавной анимацией.
  - Ползунок настройки чувствительности микрофона (10..100%) с сохранением в NVS память (`audio_sens`).
  - Чекбокс включения/исключения светомузыки из ротации сенсорной кнопки TTP223.""",
        "bin_name": "gyver_lamp_v1.2.0.bin"
    },
    {
        "tag": "v1.2.1",
        "name": "v1.2.1 — Исправление шкалы громкости VU Meter и аппаратная подтяжка I2S",
        "body": """### 🎛️ Release v1.2.1: Исправление шкалы громкости VU Meter и аппаратная подтяжка I2S

- 🔇 **Строгий 0% на шкале громкости (VU Meter) в тишине**:
  - Устранена ложная анимация шкалы громкости в веб-интерфейсе при неподключенном микрофоне или в тишине.
  - Аудиоанализатор теперь строго разделяет **физический аудиопоток** (реальные замеры амплитуды микрофона) и **визуальный рендеринг** эффекта.
  - В тишине или при отсутствии сигнала с микрофона уровень громкости в API `/api/audio/status` и на шкале VU Meter равен строго **0%**, а статус отображает *«Тишина / Микрофон не подключен»*.
- 🛡️ **Аппаратная подтяжка к земле (INPUT_PULLDOWN)**:
  - Пин данных I2S `GPIO 6` (SD/DOUT) инициализируется с внутренней подтяжкой к земле `INPUT_PULLDOWN`.
  - Это исключает любые высокочастотные наводки и ложные шумы на висящем в воздухе проводе до того, как пользователь припаяет физический микрофон.
- 🧘 **Автономный рендеринг медитативного дыхания**:
  - Когда звук отсутствует, эффект «Пульс музыки» на самой лампе плавно переходит в мягкое синусоидальное дыхание, а при появлении музыки мгновенно взрывается световыми кольцами в такт ударным.""",
        "bin_name": "gyver_lamp_v1.2.1.bin"
    }
]

cur_bin = "/Users/oleg/.gemini/antigravity/scratch/github_release/gyver_lamp_ota.ino.bin"

for r in releases:
    create_or_update_release(r["tag"], r["name"], r["body"], bin_path=cur_bin, bin_name=r["bin_name"])

print("All releases created successfully!")
