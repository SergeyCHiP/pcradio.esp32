<!-- AISMM:BEGIN -->
type: layer_document
model_instance_id: f4c43287-bd9a-437f-b1f2-f979edac7c62
product_id: 7dd96755-e1a9-42dc-8016-22d83f5a9f42
product_key: pcradio_esp32
layer_id: "302"
layer_key: code_and_implementation
document_id: pcradio.b3.302.code_map
document_type: layer_document
module_scope: root
status: draft
spec_version: 1.0.0-lite
completion_status: partial
references:
  - pcradio.b2.201.system_architecture
  - pcradio.b7.702.risk_register
<!-- AISMM:META_END -->

# Карта реализации

## Технологическая база

- C и ESP-IDF 5.4.2;
- FreeRTOS tasks, semaphores и timers;
- LittleFS/NVS для файлов и настроек;
- `esp_http_client`/`esp_http_server` для сети;
- `esp_audio_codec` 2.3.0 и `esp_audio_effects` 1.1.0;
- ESP32-S3, 16 МБ Flash, 8 МБ octal PSRAM.

## Модули

| Компонент | Файлы | Текущая роль | Зона ревью |
| --- | --- | --- | --- |
| Bootstrap | `main/src/main.c` | последовательная инициализация | ошибки init, recovery, dependency order |
| Player | `player/player.c` | сеть, task lifecycle, reconnect, decoder orchestration | ownership, races, state machine |
| Codecs | `codec_mp3.c`, `codec_aac.c`, `wrapper.c` | buffering/detection/decoding | bounds, malformed input, lifecycle |
| Audio | `audio_i2s.c`, `eq.c`, `alc.c`, `volume.c` | DSP и I²S | real-time safety, buffer lifetime, clipping |
| Playlist | `playlist.c` | download, file index, station lookup | TLS, atomicity, parser limits |
| Metadata | `icy.c` | ICY headers и metadata | bounds, encoding, malformed streams |
| Configuration | `utils/config.c` | LittleFS/NVS, Wi-Fi/NTP/player settings | schema, atomic writes, recovery |
| Network/time | `wireless.c`, `ntp.c` | Wi-Fi и SNTP | retries, offline mode, event lifecycle |
| Web | `web/api.c`, `web/www.c` | REST и static files | auth boundary, validation, traversal |
| Metrics | `web/metrics.c` | Prometheus text | correctness and concurrency |

## Structural hotspots

- `audio_i2s.c`: 888 строк и несколько DSP-стадий в одной функции.
- `player.c`: 821 строк, shared mutable singleton и управление ресурсами/task.
- `playlist.c`: network, storage и parser в одном модуле.
- `metrics.c`: 519 строк, часть метрик вычисляется синтетически.
- тестовые каталоги, CI и статический анализ отсутствуют.

## Build baseline

В репозитории зафиксирован полный `sdkconfig`, но нет `sdkconfig.defaults`, CI
recipe или переносимого генератора LittleFS. Доступный `tools/mklittlefs.exe`
ориентирован на Windows. Первый clean build выполнен в Docker на ESP-IDF 5.4.2;
для воспроизводимости версии managed components закреплены точно, а
`dependencies.lock` переведён под контроль Git.

<!-- AISMM:END -->
