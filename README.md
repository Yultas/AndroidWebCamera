# AndroidWebCamera

Приложение для **Android 12+ (API 31+)** с Material 3 интерфейсом для трансляции видео с камеры смартфона на ПК по **Wi-Fi** и **USB** с поддержкой работы **при заблокированном экране**, нативный плагин для **OBS Studio** (`obs-plugin/` для Linux и Windows x64), а также лёгкая кроссплатформенная утилита `awc-bridge` (`pc-bridge/`) для автоматического USB-туннелирования и виртуальной камеры Linux (`v4l2loopback`).

---

## Возможности

- **Работа при выключенном и заблокированном экране**: `Camera2`, кодировщик (`MediaCodec` H.264 / H.265 (HEVC) / AV1 и `ImageReader` MJPEG) и HTTP/WebSocket-сервер работают в `ForegroundService` (`foregroundServiceType="camera"` + `PARTIAL_WAKE_LOCK`). При нажатии кнопки питания превью корректно отключается, а трансляция продолжается в фоне без обрыва.
- **Выбор параметров потока**:
  - Переключение между всеми доступными камерами (основная, широкоугольная, телефото, фронтальная, внешняя USB).
  - Список реально поддерживаемых устройством разрешений и FPS (15 / 24 / 30 / 60 fps).
  - Четыре режима кодеков с автоматическим определением аппаратных возможностей устройства:
    - **H.264** — универсальный аппаратный кодек с минимальной задержкой.
    - **H.265 (HEVC)** — аппаратное сжатие нового поколения (меньше битрейт при том же качестве, идеально для 1080p/4K по Wi-Fi).
    - **AV1** — поддерживается автоматически на смартфонах с аппаратным (или Android 14+ системным) энкодером `video/av01` (передаётся в браузер по WebCodecs/MSE и в OBS/FFmpeg как IVF `/stream.av1`).
    - **MJPEG** — максимальная совместимость, настройка качества JPEG.
  - Поворот (`0°`, `90°`, `180°`, `270°`), зеркало и фонарик — меняются как с телефона, так и прямо из веб-интерфейса.
- **Смартфон как хост (Wi-Fi / USB)**:
  - **Wi-Fi (общая сеть)** — сервер доступен по локальному IP телефона (`http://<IP>:8080/`).
  - **Локальная точка доступа (Local-Only Hotspot)** — телефон сам поднимает Wi-Fi сеть без роутера и root, отображая SSID и пароль прямо на экране.
  - **USB через `adb forward`** (Linux / Win64) — проброс порта с помощью `awc-bridge` или одной команды `adb`.
  - **USB-модем (RNDIS)** — включите «USB-модем» в настройках Android и открывайте адрес USB-интерфейса в браузере вообще без установки программ на ПК.
- **Полностью офлайн Веб-интерфейс (`http://<ip>:8080/`)**:
  - **WebCodecs** по WebSocket (`/ws/video`) для H.264, H.265 (HEVC) и AV1 с минимальной задержкой.
  - Автоматический **MSE fMP4** декодер в браузере (`avcC` / `hvcC` / `av1C`) для обычного HTTP по Wi-Fi с автоматической очисткой буфера.
  - Режим **MJPEG** (`/stream.mjpg`) и прямые ссылки для **OBS Studio / VLC / FFmpeg** (`/stream.h264`, `/stream.h265`, `/stream.av1`, `/stream.mjpg`).
- **Дизайн Material 3**: Jetpack Compose + Dynamic Color (Android 12+).

---

## Архитектура

```
┌───────────────────────────────────────────────┐
│               Android Smartphone              │
│                                               │
│  Camera2 ─► H.264 / H.265 / AV1 / MJPEG       │
│               │                               │
│               ▼                               │
│     CameraStreamService (Foreground + Wake)   │
│               │                               │
│     StreamHttpServer (:8080)                  │
│      ├── /             (Offline Web UI)       │
│      ├── /ws/video     (WebCodecs / MSE)      │
│      ├── /stream.h264  (H.264 Annex-B)        │
│      ├── /stream.h265  (H.265 / HEVC Annex-B) │
│      ├── /stream.av1   (AV1 IVF)              │
│      ├── /stream.mjpg  (MJPEG multipart)      │
│      └── /api/status   (JSON state & control) │
└───────────────┬───────────────────────────────┘
                │ Wi-Fi / Hotspot / USB (ADB / RNDIS)
                ▼
┌───────────────────────────────────────────────┐
│                 PC (Linux / Win64)            │
│                                               │
│  1) Нативный плагин OBS Studio (obs-plugin):  │
│      ├── Источник «AndroidWebCamera»          │
│      ├── USB: автопоиск и adb forward         │
│      ├── Wi-Fi: прямое подключение по IP      │
│      ├── FFmpeg: H.264/H.265/AV1/MJPEG low-lag│
│      └── Управление камерой/фонариком из OBS  │
│  2) Браузер: http://<ip>:8080/                │
│  3) awc-bridge (Go):                          │
│      ├── USB: adb forward tcp:8080 tcp:8080   │
│      └── Linux: ffmpeg -> v4l2loopback        │
│                 (/dev/videoX для Zoom/Meet)   │
└───────────────────────────────────────────────┘
```

---

## Сборка проекта

### 1. Android APK (`android/`)

Проект использует **AGP 8.7.3**, **Kotlin 2.0.21**, **Gradle 8.11.1**, `minSdk = 31` (Android 12), `targetSdk = 35`.

**Вариант A — через Android Studio:**
1. Откройте папку `android/` в Android Studio (Ladybug или новее).
2. Подключите телефон или нажмите **Build → Build Bundle(s) / APK(s) → Build APK(s)**.

**Вариант B — через командную строку (JDK 17 + Android SDK):**
```bash
export ANDROID_HOME=/path/to/Android/Sdk
cd android && ./gradlew testDebugUnitTest lintDebug assembleDebug
# Готовый APK: android/app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

> Также в репозитории настроен GitHub Actions workflow `.github/workflows/build.yml`, который автоматически собирает Debug APK, бинарники `awc-bridge` и нативный плагин OBS Studio (`obs-android-webcamera`) под Linux и Windows x64.

---

### 2. Нативный плагин для OBS Studio (`obs-plugin/`)

Плагин регистрирует источник **«AndroidWebCamera»** прямо в OBS Studio (`Источники → +`), автоматически поднимает `adb forward` в режиме USB, декодирует H.264 / H.265 / AV1 / MJPEG с минимальной задержкой через FFmpeg и позволяет менять камеру, разрешение, FPS, кодек, поворот, зеркало и фонарик прямо из свойств источника OBS.

**Сборка и установка на Linux:**
```bash
# Требуются g++/cmake и dev-пакеты FFmpeg (libavformat, libavcodec, libavutil, libswscale)
# Заголовки libobs подхватываются из системы или через встроенную ABI-совместимую обёртку
cd obs-plugin
make test          # компиляция и прогон unit + FFmpeg E2E тестов
make build         # сборка dist/obs-android-webcamera.so и data/
make install-user  # установка в ~/.config/obs-studio/plugins/obs-android-webcamera
```

**Сборка через CMake (Linux / Windows x64):**
```bash
cmake -S obs-plugin -B obs-plugin/build -DCMAKE_BUILD_TYPE=Release
cmake --build obs-plugin/build --config Release
ctest --test-dir obs-plugin/build -C Release --output-on-failure
```
На Windows x64 готовый архив `obs-android-webcamera-windows-x64.zip` из GitHub Actions распаковывается в `%ProgramData%\obs-studio\plugins\obs-android-webcamera\` (или `%APPDATA%\obs-studio\plugins\obs-android-webcamera\`).

---

### 3. ПК-мост `awc-bridge` (`pc-bridge/`)

Требуется только Go 1.22+ (внешних Go-зависимостей нет):

```bash
cd pc-bridge
make test       # vet + unit + FFmpeg E2E тесты
make build-all  # сборка dist/awc-bridge-linux-amd64 и dist/awc-bridge-windows-amd64.exe
```

---

## Использование

### Блокировка экрана при трансляции
1. Откройте **AndroidWebCamera**, выдайте разрешение на камеру (и уведомления на Android 13+).
2. Нажмите **«Начать трансляцию»**.
3. Появится постоянное системное уведомление с кнопкой **«Остановить»**. После этого экран можно сразу выключить кнопкой питания — поток не прервётся.
4. Для многочасовых трансляций нажмите на баннер вверху приложения и разрешите работу без оптимизации батареи.

---

### Подключение на Linux (включая виртуальную камеру)

#### 1. Просто в браузер или OBS Studio
- **По Wi-Fi**: откройте URL вида `http://192.168.x.x:8080/` в браузере, либо добавьте `http://192.168.x.x:8080/stream.h264` как *Источник медиа (Media Source)* в OBS.
- **По USB (с `awc-bridge`)**:
  ```bash
  ./pc-bridge/dist/awc-bridge-linux-amd64 --mode usb --open
  ```

#### 2. Как системная веб-камера (`/dev/videoX`) для Zoom / Meet / Discord / Telegram
Установите `v4l2loopback` и `ffmpeg`:
- **Arch Linux**: `sudo pacman -S v4l2loopback-dkms ffmpeg android-tools`
- **Ubuntu / Debian**: `sudo apt install v4l2loopback-dkms ffmpeg adb`

Загрузите модуль виртуальной камеры:
```bash
sudo modprobe v4l2loopback exclusive_caps=1 card_label="AndroidWebCamera"
```

Запустите мост по USB (или по Wi-Fi):
```bash
# Через USB (автоматически поднимет adb forward и направит поток в v4l2loopback)
./pc-bridge/dist/awc-bridge-linux-amd64 --mode usb --v4l2 auto

# Или по Wi-Fi / локальной точке доступа телефона
./pc-bridge/dist/awc-bridge-linux-amd64 --mode wifi --url http://192.168.x.x:8080 --v4l2 auto
```
В браузере, Discord, Zoom или Telegram выберите камеру **AndroidWebCamera**.

---

### Подключение на Win64

1. **Как системная виртуальная камера (`win-vcam/`) для VTube Studio / Discord / Zoom / OBS (без OBS Studio)**:
   - Скачайте архив `android-webcamera-win-vcam-x64.zip` из GitHub Actions (или соберите `win-vcam/` через CMake + MSVC) и распакуйте в постоянную папку (например, `C:\AndroidWebCamera-WinVCam`).
   - Один раз запустите **`install-vcam.bat` от имени администратора** — зарегистрируются DirectShow-фильтры `awc-vcam-filter64.dll` и `awc-vcam-filter32.dll` с именем камеры **`AndroidWebCamera`** (форматы `RGB24` и `YUY2`, режимы `1280x720@60/30`, `1920x1080@60/30`, `640x480@60/30`).
   - Запустите автономный хост (внешний `ffmpeg.exe` не требуется):
     ```powershell
     # По USB (автоматически поднимет adb forward)
     .\awc-vcam.exe --mode usb

     # Или по Wi-Fi / локальной точке доступа
     .\awc-vcam.exe --mode wifi --host 192.168.x.x
     ```
   - Откройте **VTube Studio** → настройки камеры → выберите **`AndroidWebCamera`** (`1280x720`, `60 FPS`).
   - Для удаления виртуальной камеры из системы запустите `uninstall-vcam.bat` от имени администратора.
2. **Через браузер / Wi-Fi / Точку доступа**: откройте `http://<IP-телефона>:8080/`.
3. **Через USB (`awc-bridge-windows-amd64.exe`)**:
   ```powershell
   .\awc-bridge-windows-amd64.exe --mode usb --open
   ```
4. **Через OBS Studio**: установите нативный плагин `obs-android-webcamera-windows-x64.zip` из GitHub Actions (или добавьте *Источник медиа* `http://127.0.0.1:8080/stream.h264`).
