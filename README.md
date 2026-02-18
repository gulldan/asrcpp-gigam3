# ASR Server (C++)

Сервер распознавания речи на базе [GigaAM v3](https://github.com/salute-developers/GigaAM) с HTTP и WebSocket интерфейсами.

Используемые технологии:

- **ASR-модель**: [GigaAM v3](https://github.com/salute-developers/GigaAM) — модель распознавания русской речи от SberDevices (NeMo Transducer). Портирована в [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) формат.
- **Inference runtime**: [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (часть проекта [k2-fsa / next-gen Kaldi](https://github.com/k2-fsa)) — C/C++ библиотека для оффлайн/онлайн распознавания на базе ONNX Runtime.
- **Voice Activity Detection**: [Silero VAD](https://github.com/snakers4/silero-vad) — модель определения речевой активности, запускается через ONNX Runtime.

## Возможности

- Потоковое распознавание через WebSocket (float32 аудио)
- Распознавание файлов через HTTP POST (WAV)
- Voice Activity Detection (Silero VAD)
- Prometheus-метрики (`/metrics`)
- Web-интерфейс для записи с микрофона и загрузки файлов
- CPU и CUDA (GPU) провайдеры
- Graceful shutdown по SIGINT/SIGTERM

## Требования

- CMake >= 3.24
- C++17 компилятор (GCC 10+ / Clang 14+)
- Системные библиотеки: libssl, zlib, libjsoncpp, uuid
- Модели:
  - [sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16](https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models)
  - [silero_vad.onnx](https://github.com/snakers4/silero-vad/tree/master/files)

Все остальные зависимости (drogon, sherpa-onnx, onnxruntime, prometheus-cpp, spdlog, nlohmann_json, libsamplerate, googletest) скачиваются автоматически через CMake FetchContent.

### Установка системных зависимостей

**Arch Linux:**

```bash
sudo pacman -S cmake clang openssl zlib jsoncpp util-linux-libs cppcheck clang-tools-extra
paru -S include-what-you-use
```

**Ubuntu / Debian:**

```bash
# Сборка и запуск
sudo apt install cmake g++ libssl-dev zlib1g-dev libjsoncpp-dev uuid-dev

# Инструменты качества кода
sudo apt install cppcheck clang-tidy clang-format iwyu
```

### Загрузка моделей

```bash
# GigaAM v3 (sherpa-onnx формат)
# Скачайте из https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models
# Распакуйте в models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16/

# Silero VAD
wget -O models/silero_vad.onnx \
  https://github.com/snakers4/silero-vad/raw/master/files/silero_vad.onnx
```

## Сборка

```bash
# Release
cmake --preset release
cmake --build build/release -j$(nproc)

# Debug с тестами
cmake --preset debug
cmake --build build/debug -j$(nproc)

# CUDA (GPU)
cmake --preset cuda
cmake --build build/cuda -j$(nproc)
```

### Все пресеты

| Пресет | Тип | Тесты | Описание |
|--------|-----|-------|----------|
| `release` | Release + LTO | нет | Продакшен |
| `debug` | Debug | да | Разработка |
| `asan` | Debug + ASan/UBSan | да | Поиск ошибок памяти |
| `tsan` | Debug + TSan | да | Поиск гонок |
| `coverage` | Debug + gcov | да | Покрытие кода |
| `cuda` | Release + LTO + CUDA | нет | GPU-ускорение |

## Запуск

```bash
# Из директории сборки
./build/release/asr-server

# Или с указанием путей к моделям
MODEL_DIR=models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16 \
VAD_MODEL=models/silero_vad.onnx \
./build/release/asr-server
```

Сервер запустится на `http://0.0.0.0:8081`. Для остановки — `Ctrl+C` (SIGINT) или `kill <pid>` (SIGTERM).

## Docker

```bash
# CPU
docker build -f Dockerfile -t asr-server-cpp .
docker run -p 8081:8081 asr-server-cpp

# CUDA
docker build -f Dockerfile.cuda -t asr-server-cpp:cuda .
docker run -p 8081:8081 --gpus all -e PROVIDER=cuda asr-server-cpp:cuda
```

Dockerfile уже разбит на этапы `deps-builder` и `builder`, поэтому при изменении только `src/` или `include/` тяжёлые third-party зависимости переиспользуются из кэша слоёв и не пересобираются с нуля.

## Переменные окружения

### Сервер

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `HOST` | `0.0.0.0` | Адрес привязки |
| `HTTP_PORT` | `8081` | TCP-порт |
| `THREADS` | кол-во ядер | Потоки HTTP-сервера (1–256) |

### Модели

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `MODEL_DIR` | `models/sherpa-onnx-nemo-...` | Директория модели sherpa-onnx |
| `VAD_MODEL` | `models/silero_vad.onnx` | Файл модели VAD |
| `PROVIDER` | `cpu` | `cpu` или `cuda` |
| `NUM_THREADS` | `4` | Потоки декодера (1–128) |

### Аудио

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `SAMPLE_RATE` | `16000` | Частота дискретизации (8000–48000) |
| `FEATURE_DIM` | `64` | Размерность фичей |
| `SILENCE_THRESHOLD` | `0.008` | Порог тишины (RMS) |
| `MIN_AUDIO_SEC` | `0.5` | Минимальная длительность аудио |
| `MAX_AUDIO_SEC` | `30.0` | Максимальная длительность аудио |
| `MAX_UPLOAD_BYTES` | `104857600` | Лимит загрузки файла (100 МБ) |
| `MAX_WS_MESSAGE_BYTES` | `4194304` | Лимит размера одного WS-сообщения (4 МБ) |

### VAD

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `VAD_THRESHOLD` | `0.5` | Порог вероятности речи (0.01–0.99) |
| `VAD_MIN_SILENCE` | `0.5` | Мин. тишина для завершения сегмента (сек) |
| `VAD_MIN_SPEECH` | `0.25` | Мин. длительность речи (сек) |
| `VAD_MAX_SPEECH` | `20.0` | Макс. длительность сегмента (сек) |
| `VAD_WINDOW_SIZE` | `512` | Размер окна VAD (64–4096) |
| `VAD_CONTEXT_SIZE` | `64` | Контекст VAD (< window_size) |

## API

### `GET /health`

```json
{"status": "ok", "provider": "cpu", "threads": 4}
```

### `GET /metrics`

Prometheus-метрики (text format). Префикс: `gigaam_`.

### `POST /recognize`

Загрузка аудиофайла (multipart/form-data):

```bash
curl -F "file=@audio.wav" http://localhost:8081/recognize
```

```json
{"text": "распознанный текст", "duration": 2.5}
```

Ошибки возвращаются с соответствующим HTTP-кодом и JSON:

```json
{"detail": "описание ошибки"}
```

| Код | Описание |
|-----|----------|
| 200 | Успех |
| 400 | Некорректный файл (не WAV, стерео, пустой) |
| 413 | Файл превышает лимит `MAX_UPLOAD_BYTES` |
| 500 | Внутренняя ошибка |

### `WS /ws`

WebSocket для потокового распознавания:

| Направление | Тип | Содержимое |
|-------------|-----|-----------|
| Клиент → Сервер | binary | float32 PCM-сэмплы (16 кГц, моно) |
| Клиент → Сервер | text | `"RECOGNIZE"` — завершить сессию |
| Клиент → Сервер | text | `"RESET"` — сбросить сессию |
| Сервер → Клиент | text | `{"type": "interim", "duration": 1.5, "rms": 0.05, "is_speech": true}` |
| Сервер → Клиент | text | `{"type": "final", "text": "...", "duration": 2.5}` |
| Сервер → Клиент | text | `{"type": "done"}` |

## Архитектура

```
Audio → [Resampling] → [VAD (Silero)] → [ASR (GigaAM/sherpa-onnx)] → Text
          audio.cpp       vad.cpp            recognizer.cpp
```

- **VAD** сегментирует аудиопоток на фрагменты речи, отбрасывая тишину
- **ASR** распознаёт каждый сегмент в текст (offline transducer)
- **Метрики** отслеживают TTFR, RTF, длительность, ошибки и RMS

## Тесты

```bash
# Сборка и запуск
cmake --preset debug
cmake --build build/debug -j$(nproc)
LD_LIBRARY_PATH=build/debug/_deps/onnxruntime/lib build/debug/tests/asr_tests

# Или через ctest
ctest --preset debug
```

## Качество кода

```bash
# Форматирование
cmake --build build/debug --target format        # применить
cmake --build build/debug --target format-check  # проверить

# Статический анализ
cmake --build build/debug --target lint          # clang-tidy
cmake --build build/debug --target cppcheck      # cppcheck
cmake --build build/debug --target iwyu          # include-what-you-use

# Всё вместе
cmake --build build/debug --target quality       # format-check + lint + cppcheck
cmake --build build/debug --target quality-full  # + iwyu

# Или через скрипты
./scripts/check-all.sh build/debug               # format + lint + cppcheck
./scripts/check-all.sh build/debug --with-iwyu   # + iwyu (если установлен)
```

Необходимые инструменты: `clang-format`, `clang-tidy`, `cppcheck`, `include-what-you-use` (опционально).

## Структура проекта

```
asr-cpp/
├── include/asr/        # Заголовки
│   ├── config.h        # Конфигурация (env vars, валидация)
│   ├── server.h        # HTTP/WebSocket сервер (drogon)
│   ├── handler.h       # Сессия распознавания (VAD + ASR pipeline)
│   ├── recognizer.h    # Обёртка sherpa-onnx (thread-safe)
│   ├── vad.h           # Voice Activity Detection (Silero VAD + ONNX Runtime)
│   ├── audio.h         # Декодирование WAV (dr_wav), ресемплинг (libsamplerate)
│   ├── metrics.h       # Prometheus-метрики
│   └── span.h          # C++17 span polyfill
├── src/                # Реализация
├── tests/              # GoogleTest тесты
├── static/             # Web-интерфейс
├── third_party/        # dr_wav.h
├── scripts/            # Скрипты качества кода
├── docs/               # Документация
├── CMakeLists.txt
├── CMakePresets.json
├── Dockerfile          # CPU (distroless)
└── Dockerfile.cuda     # CUDA (nvidia runtime)
```

## Ссылки

- [GigaAM v3](https://github.com/salute-developers/GigaAM) — модель распознавания русской речи
- [sherpa-onnx (k2-fsa)](https://github.com/k2-fsa/sherpa-onnx) — inference runtime для ASR/TTS
- [Silero VAD](https://github.com/snakers4/silero-vad) — Voice Activity Detection
- [ONNX Runtime](https://github.com/microsoft/onnxruntime) — inference engine
- [Drogon](https://github.com/drogonframework/drogon) — HTTP/WebSocket framework
- [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) — Prometheus клиент

## Лицензия

Модель GigaAM v3 распространяется под лицензией [Creative Commons Attribution-NonCommercial-ShareAlike 4.0](https://github.com/salute-developers/GigaAM/blob/main/LICENSE). Silero VAD — под [MIT](https://github.com/snakers4/silero-vad/blob/master/LICENSE).
