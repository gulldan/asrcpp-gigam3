# ASR Server (C++)

Сервер распознавания речи на базе [GigaAM v3](https://github.com/salute-developers/GigaAM) с HTTP, Realtime WebSocket и OpenAI-compatible API.

Основа стека:

- [GigaAM v3](https://github.com/salute-developers/GigaAM) в формате `sherpa-onnx`
- [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) для ASR
- [Silero VAD](https://github.com/snakers4/silero-vad) для детекции речи
- [Drogon](https://github.com/drogonframework/drogon) для HTTP/WebSocket
- [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) для метрик

## Что умеет

- `POST /recognize` для простого распознавания файлов
- `POST /v1/audio/transcriptions` и `/audio/transcriptions` для Whisper/OpenAI-compatible клиентов
- `WS /v1/realtime` для OpenAI Realtime-compatible клиентов
- VAD на сервере, авто-сегментация речи и внутренняя нарезка длинных файлов
- Prometheus-метрики на `/metrics`
- Встроенная демо-страница на `/`
- CPU и CUDA сборки

## Быстрый старт

### 1. Установите системные зависимости

Ubuntu / Debian:

```bash
sudo apt install build-essential cmake pkg-config libssl-dev zlib1g-dev libopus-dev
# Опционально: поддержка .opus файлов в HTTP API
sudo apt install libopusfile-dev
```

macOS:

```bash
brew install cmake pkg-config openssl opus
# Опционально: поддержка .opus файлов в HTTP API
brew install opusfile
```

Все остальные зависимости проект скачает сам через CMake `FetchContent`.

### 2. Скачайте модели

```bash
mkdir -p models

# 1) Скачайте и распакуйте GigaAM v3 в sherpa-onnx формате:
#    https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models
#    Директория по умолчанию:
#    models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16

# 2) Скачайте Silero VAD
curl -L https://github.com/snakers4/silero-vad/raw/master/files/silero_vad.onnx \
  -o models/silero_vad.onnx
```

### 3. Соберите проект

Release:

```bash
cmake --preset release
cmake --build --preset release --parallel
```

Debug с тестами:

```bash
cmake --preset debug
cmake --build --preset debug --parallel
```

CUDA:

```bash
cmake --preset cuda
cmake --build --preset cuda --parallel
```

Пресет `cuda` поддерживается только на Linux `x64`. На macOS CUDA сборка недоступна.

### 4. Запустите сервер

Если модели лежат в путях по умолчанию:

```bash
./build/release/asr-server
```

Если модели лежат в другом месте:

```bash
MODEL_DIR=/absolute/path/to/gigaam-model \
VAD_MODEL=/absolute/path/to/silero_vad.onnx \
./build/release/asr-server
```

По умолчанию сервер слушает `http://0.0.0.0:8081`.

### 5. Быстрая проверка

```bash
curl http://localhost:8081/healthz
curl http://localhost:8081/readyz
curl -F "file=@audio.wav" http://localhost:8081/recognize
```

Откройте `http://localhost:8081/` в браузере, если хотите проверить микрофон и загрузку файлов через встроенную demo-страницу.

## Основные маршруты

| Метод | Путь | Назначение |
|------|------|------------|
| `GET` | `/` | Встроенный web UI |
| `GET` | `/healthz` | Лёгкий liveness check |
| `GET` | `/readyz` | Проверка готовности recognizer + VAD |
| `GET` | `/health` | Backward-compatible alias к `/readyz` |
| `GET` | `/metrics` | Prometheus-метрики |
| `POST` | `/recognize` | Простой JSON API |
| `POST` | `/v1/audio/transcriptions` | Whisper/OpenAI-compatible API |
| `POST` | `/audio/transcriptions` | Алиас к `/v1/audio/transcriptions` |
| `WS` | `/v1/realtime` | Realtime-compatible API |

## Примеры API

### `GET /healthz`

```json
{"status":"ok","provider":"cpu","threads":4}
```

### `GET /readyz`

```json
{"status":"ok","provider":"cpu","recognizer_ready":true,"vad_ready":true}
```

### `GET /metrics`

Prometheus text format с префиксом `gigaam_`.

### `POST /recognize`

Самый простой способ получить текст из файла:

```bash
curl -F "file=@audio.wav" http://localhost:8081/recognize
```

Ответ:

```json
{"text":"распознанный текст","duration":2.5}
```

Поведение:

- `wav` поддерживается всегда
- `.opus` поддерживается, если проект собран с `libopusfile`
- многоканальные файлы автоматически downmix'ятся в mono перед ASR
- длинные файлы режутся внутри сервера на чанки примерно по 20 секунд
- runtime-зависимости от `ffmpeg` нет

Ошибки:

```json
{"detail":"описание ошибки"}
```

### `POST /v1/audio/transcriptions`

OpenAI/Whisper-compatible роут. Алиас: `POST /audio/transcriptions`.

Минимальный пример:

```bash
curl http://localhost:8081/v1/audio/transcriptions \
  -F "file=@voice.wav" \
  -F "model=whisper-1"
```

Поддерживаемые поля:

| Поле | Обязательно | Описание |
|------|-------------|----------|
| `file` | да | Аудиофайл |
| `model` | да | Любая строка модели |
| `language` | нет | Языковой hint, например `ru` |
| `prompt` | нет | Подсказка для стиля |
| `response_format` | нет | `json`, `text`, `srt`, `vtt`, `verbose_json` |
| `temperature` | нет | От `0` до `1` |
| `timestamp_granularities[]` | нет | `word` и/или `segment`, только для `verbose_json` |
| `stream` | нет | Не поддерживается, запрос будет отклонён |

Форматы:

- `wav` поддерживается всегда
- `.opus` поддерживается, если доступен `libopusfile`
- многоканальные файлы автоматически downmix'ятся в mono перед ASR

Ошибки возвращаются в OpenAI-compatible формате:

```json
{
  "error": {
    "message": "описание ошибки",
    "type": "invalid_request_error",
    "param": "file",
    "code": "invalid_audio"
  }
}
```

### `WS /v1/realtime`

OpenAI Realtime-compatible роут.

Поддерживаемые входные события:

- `session.update`
- `input_audio_buffer.append`
- `input_audio_buffer.commit`
- `input_audio_buffer.clear`
- `ping` и `noop`

Поддерживаемые входные аудиоформаты:

- `pcm16`
- `opus`

Минимальный flow:

```json
{"type":"session.update","event_id":"evt_1","session":{"input_audio_format":"pcm16","input_sample_rate":16000,"input_audio_transcription":{"model":"default","language":"ru"}}}
```

```json
{"type":"input_audio_buffer.append","event_id":"evt_2","audio":"<base64 pcm16>"}
```

```json
{"type":"input_audio_buffer.commit","event_id":"evt_3"}
```

Что важно знать:

- сервер сам отправляет `session.created` при подключении
- `input_audio_buffer.append` можно присылать и text JSON с base64, и binary frame
- для `opus` при переключении формата без `input_sample_rate` сервер использует `48000`
- если выбран `opus`, можно передавать raw Opus payload или RTP packet с Opus payload
- сервер поддерживает только `turn_detection.type = "server_vad"` и валидирует его параметры
- сервер эмитит `input_audio_buffer.speech_started`, `input_audio_buffer.speech_stopped`, `input_audio_buffer.committed`, `conversation.item.input_audio_transcription.completed` и `error`

## Настройки через переменные окружения

Ниже полная таблица переменных, которые реально читает сервер.

### Сервер

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `HOST` | `0.0.0.0` | Адрес привязки |
| `HTTP_PORT` | `8081` | Порт HTTP/WS |
| `THREADS` | число ядер | Потоки Drogon (`1..256`) |
| `IDLE_CONNECTION_TIMEOUT_SEC` | `0` | Idle timeout TCP-соединения, `0` = не закрывать |

### Модели и inference

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `MODEL_DIR` | `models/sherpa-onnx-nemo-transducer-punct-giga-am-v3-russian-2025-12-16` | Путь к модели GigaAM |
| `VAD_MODEL` | `models/silero_vad.onnx` | Путь к Silero VAD |
| `PROVIDER` | `cpu` | `cpu` или `cuda` |
| `NUM_THREADS` | `4` | Потоки распознавания (`1..128`) |
| `SAMPLE_RATE` | `16000` | Целевая частота ASR (`8000..48000`) |
| `FEATURE_DIM` | `64` | Размерность фичей |

### Параллелизм и лимиты

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `RECOGNIZER_POOL_SIZE` | `1` | Размер пула распознавателей (`1..256`) |
| `MAX_CONCURRENT_REQUESTS` | `RECOGNIZER_POOL_SIZE` | Лимит одновременных HTTP-запросов |
| `RECOGNIZER_WAIT_TIMEOUT_MS` | `30000` | Таймаут ожидания свободного recognizer slot |
| `MAX_WS_CONNECTIONS` | `0` | Лимит одновременных realtime WS-соединений, `0` = без лимита |
| `MAX_UPLOAD_BYTES` | `104857600` | Лимит файла для HTTP API |
| `MAX_WS_MESSAGE_BYTES` | `4194304` | Лимит одного realtime WS-сообщения |

### Аудио

| Переменная | По умолчанию | Описание |
|------------|-------------|----------|
| `SILENCE_THRESHOLD` | `0.008` | RMS-порог тишины |
| `MIN_AUDIO_SEC` | `0.5` | Минимальная длительность аудио |
| `VAD_THRESHOLD` | `0.5` | Порог вероятности речи |
| `VAD_MIN_SILENCE` | `0.5` | Минимальная тишина для конца сегмента, сек |
| `VAD_MIN_SPEECH` | `0.25` | Минимальная длина речи, сек |
| `VAD_MAX_SPEECH` | `20.0` | Максимальная длина сегмента, сек |
| `VAD_WINDOW_SIZE` | `512` | Размер окна VAD (`64..4096`) |
| `VAD_CONTEXT_SIZE` | `64` | Контекст VAD, должен быть меньше `VAD_WINDOW_SIZE` |

### Практические рекомендации для production

Обычно достаточно настроить только это:

- `MODEL_DIR` и `VAD_MODEL`, если модели не лежат в `models/`
- `RECOGNIZER_POOL_SIZE`, чтобы совпадал с доступным CPU budget
- `MAX_CONCURRENT_REQUESTS`, чтобы защитить HTTP API от перегруза
- `MAX_WS_CONNECTIONS`, чтобы защитить Realtime WebSocket API от OOM
- `MAX_UPLOAD_BYTES` и `MAX_WS_MESSAGE_BYTES`, если сервис смотрит наружу

## Docker

CPU:

```bash
docker build -f Dockerfile -t asr-server-cpp .
docker run --rm -p 8081:8081 asr-server-cpp
```

CUDA:

```bash
docker build -f Dockerfile.cuda -t asr-server-cpp:cuda .
docker run --rm -p 8081:8081 --gpus all -e PROVIDER=cuda asr-server-cpp:cuda
```

Замечания:

- CPU image собирается под текущую Docker platform
- CUDA image опирается на prebuilt ONNX Runtime только для Linux `x64`
- на ARM-хостах CUDA image нужно собирать с `--platform=linux/amd64`

## Тесты и проверки

Базовый цикл:

```bash
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Полезные команды:

```bash
cmake --build build/debug --target quality
cmake --build build/debug --target quality-full
./scripts/test-presets.sh
```

Подробности по quality workflow: [docs/QUALITY.md](docs/QUALITY.md)

## Ограничения и нюансы

- `.wav` работает всегда; `.opus` в HTTP API требует `libopusfile`
- file upload API автоматически сводит multi-channel аудио в mono
- realtime Opus через WebSocket работает через `libopus`
- сервер не зависит от `ffmpeg` во время выполнения
- длинные HTTP файлы обрабатываются внутренними чанками, но ответ возвращается как единый результат
- `WS /v1/realtime` совместим с основным OpenAI flow, но поддерживает только реализованные события из списка выше

## Лицензия

Модель GigaAM v3 распространяется под лицензией [CC BY-NC-SA 4.0](https://github.com/salute-developers/GigaAM/blob/main/LICENSE). Silero VAD распространяется под [MIT](https://github.com/snakers4/silero-vad/blob/master/LICENSE).
