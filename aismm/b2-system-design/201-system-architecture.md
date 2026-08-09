<!-- AISMM:BEGIN -->
type: layer_document
model_instance_id: f4c43287-bd9a-437f-b1f2-f979edac7c62
product_id: 7dd96755-e1a9-42dc-8016-22d83f5a9f42
product_key: pcradio_esp32
layer_id: "201"
layer_key: applications_and_system_architecture
document_id: pcradio.b2.201.system_architecture
document_type: layer_document
module_scope: root
status: draft
spec_version: 1.0.0-lite
completion_status: partial
references:
  - pcradio.b3.302.code_map
  - pcradio.b4.405.runtime_lifecycles
<!-- AISMM:META_END -->

# Слоистая архитектура

## Текущее устройство

```text
WebUI / HTTP client / user
             │
             ▼
HTTP API + static file server + Prometheus endpoint
             │
             ▼
main + player + playlist + configuration orchestration
             │
             ▼
ICY + MP3/AAC codecs + volume/EQ/ALC/audio processing
             │
             ▼
ESP-IDF adapters: Wi-Fi, HTTP, NTP, LittleFS, NVS, I²S, GPIO
             │
             ▼
ESP32-S3 + PSRAM/Flash + UDA1334A + network services
```

Поток аудио:

```text
M3U station URL -> HTTP/ICY reader -> codec detection -> MP3/AAC decoder
                -> PCM processing -> I²S DMA -> UDA1334A
```

## Целевые слои

| Слой | Ответственность | Допустимые зависимости |
| --- | --- | --- |
| Interface | HTTP API, WebUI, GPIO/IR commands | Application contracts |
| Application | player use cases, command serialization, lifecycle | Domain + ports |
| Domain | station, playlist, player states, invariants, error taxonomy | C standard library only where возможно |
| Media | stream parsing, ICY, codecs, PCM/DSP pipeline | Domain contracts + platform ports |
| Platform | ESP-IDF Wi-Fi/HTTP/FS/NVS/I²S/clock adapters | ESP-IDF and hardware |

Зависимости направляются сверху вниз через явные интерфейсы. HTTP handlers не
должны напрямую управлять task handles, декодерами или файловой системой.

## Главные архитектурные границы

- Единственный владелец изменяемого состояния player — player task/state machine.
- Команды UI/GPIO поступают через очередь; ответы API отражают accepted/result.
- Сетевой reader, decoder и audio sink имеют независимые контракты и ошибки.
- Конфигурация валидируется до применения и записывается атомарно.
- Hardware-specific GPIO/I²S детали изолируются в platform/HAL.
- WebUI и firmware версионируются как совместимая пара.

## Внешние интерфейсы текущей ветки

- HTTP: `/api/channel`, `/api/volume`, `/api/mute`, `/api/eq`,
  `/api/playlist`, `/api/player`, `/metrics`, static WebUI.
- Network: HTTP(S) streams, GitHub-hosted M3U, NTP.
- Hardware: I²S0 и mute GPIO для UDA1334A.

Интерфейсы текущей ветки не равны API бинарной версии 2.2.7; совместимость
должна быть оформлена отдельным решением после выбора baseline.

<!-- AISMM:END -->
