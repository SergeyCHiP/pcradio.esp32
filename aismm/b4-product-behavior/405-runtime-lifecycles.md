<!-- AISMM:BEGIN -->
type: layer_document
model_instance_id: f4c43287-bd9a-437f-b1f2-f979edac7c62
product_id: 7dd96755-e1a9-42dc-8016-22d83f5a9f42
product_key: pcradio_esp32
layer_id: "405"
layer_key: state_machines_and_lifecycles
document_id: pcradio.b4.405.runtime_lifecycles
document_type: layer_document
module_scope: root
status: draft
spec_version: 1.0.0-lite
completion_status: partial
references:
  - pcradio.b2.201.system_architecture
  - pcradio.b7.702.risk_register
<!-- AISMM:META_END -->

# Жизненные циклы

## Текущий boot flow

```text
BOOT -> system info -> config/LittleFS/NVS -> Wi-Fi (blocking)
     -> NTP -> playlist -> HTTP server -> player init -> auto-play -> RUNNING
```

Ошибки LittleFS/config приводят к бесконечному циклу ожидания. Wi-Fi startup
ожидает соединение без конечного timeout. Это неявные состояния `HALTED` и
`WAITING_FOR_NETWORK`, которые сейчас не наблюдаемы через UI/API.

## Целевая машина состояния устройства

```text
BOOTING -> CONFIGURING -> CONNECTING -> READY <-> DEGRADED
    |           |             |          |
    +---------> RECOVERY <-----+----------+

READY/DEGRADED -> UPDATING -> REBOOTING
```

## Целевая машина состояния player

```text
STOPPED -> RESOLVING -> CONNECTING -> BUFFERING -> PLAYING
   ^           |            |             |          |
   |           +----------> RETRYING <-----+----------+
   |
   +---------------- STOPPING <-------------------------+
```

Инварианты:

- только player task меняет состояние и владеет stream/decoder handles;
- `STOPPING` всегда завершается за ограниченное время без forced task delete;
- смена станции создаёт новую session generation и отменяет старую;
- ошибка сети не разрушает валидный buffered PCM без явной политики;
- Web/API читает immutable snapshot, а не внутренние mutable pointers;
- переходы логируются и покрываются тестами.

## Основные сценарии для тестирования

1. cold boot с корректной и повреждённой конфигурацией;
2. отсутствие Wi-Fi и восстановление сети;
3. play/stop/rapid station switching;
4. поток с неверными headers, frames и ICY metadata;
5. reconnect во время заполненного/пустого audio buffer;
6. обновление playlist при ошибке сети или питания;
7. исчерпание PSRAM/internal heap;
8. reboot/OTA с сохранением пользовательских данных.

<!-- AISMM:END -->
