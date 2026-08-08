<!-- AISMM:BEGIN -->
type: decision_record
model_instance_id: f4c43287-bd9a-437f-b1f2-f979edac7c62
product_id: 7dd96755-e1a9-42dc-8016-22d83f5a9f42
product_key: pcradio_esp32
layer_id: "805"
layer_key: change_history_and_decision_log
document_id: pcradio.b8.805.change_history
document_type: change_and_decision_log
module_scope: root
status: active
spec_version: 1.0.0-lite
completion_status: partial
references:
  - pcradio.b0.001.product_definition
  - pcradio.b7.702.risk_register
  - pcradio.b8.802.review_refactoring_plan
  - pcradio.b9.901.traceability_index
<!-- AISMM:META_END -->

# История действий и решений

Журнал ведётся в хронологическом порядке. Запись рабочего цикла содержит:
контекст, действия, решения, изменённые артефакты, проверки и открытые вопросы.
Git-коммиты дополняют журнал, но не заменяют его: здесь сохраняется причина
изменения и результат проверки.

## 2026-08-08 — первичное изучение и загрузка

**Контекст:** требовалось скачать и объяснить проект PCRadio ESP32.

**Действия и результаты:**

- репозиторий клонирован в локальный каталог `pcradio.esp32`;
- изучены ветки `update` и `esp32s3n16r8`, README, firmware guide, GPIO map,
  OpenAPI, manifests, partition table и строки firmware image;
- установлено, что `update` содержит дистрибутив 2.2.7 без актуального source;
- установлено, что `esp32s3n16r8` содержит более старую C/ESP-IDF реализацию;
- проверены SHA-256 бинарных артефактов ветки `update` по манифесту;
- восстановлена разметка 16-МБ Flash из `partition-table.bin`.

**Решение D-001:** не считать исходники `esp32s3n16r8` реализацией firmware
2.2.7 и явно разделять факты этих двух baseline.

## 2026-08-08 — план ревью и метамодель

**Контекст:** требовалось подготовить проект к дальнейшей разработке.

**Действия и результаты:**

- рабочая копия переключена на `esp32s3n16r8@106140b`;
- изучена структура AISMM 3.1 из `orkestron-ai/software-meta-model`;
- создана упрощённая модель PCRadio из продуктового, архитектурного,
  реализационного, поведенческого, риск-, change- и traceability-слоёв;
- создан registry со стабильными UUID продукта и экземпляра модели;
- выполнен предварительный обзор 5911 строк C/header-кода;
- сформирован реестр из 14 рисков и пятиэтапный план hardening/refactoring;
- метаданные, marker blocks, registry coverage и локальные ссылки проверены
  автоматическим валидатором — ошибок не найдено.

**Решение D-002:** использовать AISMM-inspired lite model без заявления полного
соответствия AISMM; расширять её только слоями с практической ценностью.

## 2026-08-08 — начало baseline hardening

**Контекст:** начато выполнение этапов 0–1 плана.

**Действия и результаты:**

- SHA `a1a9f07ec77c68b4104d2766b02f9b4dcc897b3a` из OTA-манифеста 2.2.7
  проверен через локальный object database, remote refs, прямой fetch и GitHub;
- remote вернул `not our ref`, commit публично недоступен;
- создана рабочая ветка `review/baseline-hardening`;
- исправлен R-002: stack buffer в `audio_i2s_write` теперь живёт до завершения
  `i2s_channel_write`;
- Docker Desktop запущен для изолированной сборки;
- загружен официальный image `espressif/idf:v5.4.2`;
- первый clean build успешно завершён;
- обнаружено, что caret constraints подтягивали более новые зависимости;
- версии `esp_audio_codec 2.3.0`, `esp_audio_effects 1.1.0` и LittleFS 1.20.1
  закреплены точно; `dependencies.lock` переведён под контроль Git;
- повторный clean build с точными версиями успешно завершён;
- добавлены `DEVELOPMENT.md` и GitHub Actions workflow сборки;
- firmware имеет размер `0x110920`, свободно 47% минимального app partition;
- DIRAM usage по `idf.py size` — 78 078 байт, или 22.85%;
- `git diff --check`, workflow YAML и целостность метамодели проверены успешно.

**Решение D-003:** продолжать `esp32s3n16r8` только как review/hardening
baseline. Называть полученные сборки версией 2.2.7 запрещено.

**Решение D-004:** воспроизводимая сборка использует Docker image ESP-IDF
5.4.2, точные component constraints и committed lock file.

**Незакрыто:**

- R-002 требует проверки на физическом устройстве и длительного audio test;
- CI workflow создан локально, но ещё не исполнялся в GitHub Actions;
- исходники 2.2.7 по-прежнему отсутствуют;
- portable LittleFS image generation ещё не реализован;
- следующий P0 — R-003, ownership/cancellation player task и HTTP handle;
- локальные изменения пока не закоммичены.

## 2026-08-08 — публикация рабочей линии в форке

**Контекст:** принято решение вести дальнейшую разработку в персональном форке.

**Действия и результаты:**

- создан GitHub fork `SergeyCHiP/pcradio.esp32`;
- remotes приведены к стандартной схеме: `origin` указывает на форк,
  `upstream` — на `RootShell-coder/pcradio.esp32`;
- изменения разделены на тематические коммиты:
  - `98b7a68` — project meta-model and action history;
  - `bc3265b` — pinned dependencies, Docker build and CI;
  - `9b89a21` — audio processing buffer lifetime fix;
- перед публикацией повторно выполнен clean build ESP-IDF 5.4.2;
- ветка `review/baseline-hardening` отправлена в `origin`;
- внутри форка открыт draft PR
  `SergeyCHiP/pcradio.esp32#1` с базой `esp32s3n16r8`.
- первый запуск Actions показал дублирование push и pull_request builds;
  push trigger ограничен основными ветками, feature-ветки проверяются через PR.

**Решение D-005:** текущая разработка и review PR ведутся в форке. Upstream
остаётся источником исходных веток; отправка изменений владельцу upstream
потребует отдельного решения после стабилизации и проверки на устройстве.

## 2026-08-08 — cooperative lifecycle player task (R-003)

**Контекст:** исходный `player_stop` одновременно с player task закрывал и
освобождал HTTP client, опрашивал состояние task и после пяти секунд мог
принудительно удалить её. Это создавало use-after-free/double-cleanup и
оставляло decoder/audio resources в неопределённом состоянии.

**Действия и результаты:**

- по исходникам ESP-IDF 5.4.2 подтверждено, что один HTTP client handle нельзя
  использовать одновременно из нескольких execution contexts;
- подтверждены API `esp_http_client_set_timeout_ms` и возврат
  `-ESP_ERR_HTTP_EAGAIN` при timeout чтения без данных;
- добавлен completion semaphore: caller сигнализирует stop и ограниченно ждёт,
  а все stream/decoder resources очищает только player task;
- удалены внешний HTTP cleanup, polling `eTaskGetState` и forced
  `vTaskDelete(task_handle)`;
- connection/header timeout оставлен 20 секунд, streaming timeout установлен в
  1 секунду, `EAGAIN` включён в существующую 15-секундную no-data policy;
- чтение ICY metadata стало stop-aware и ограниченным по времени;
- пятисекундная пауза reconnect теперь прерывается stop-сигналом;
- бесконечное ожидание I²S write в MP3/AAC заменено timeout 1 секунда;
- выполнен `idf.py fullclean build` в `espressif/idf:v5.4.2`: build успешен,
  firmware `0x110c10`, свободно 47% минимального app partition;
- `git diff --check` и поисковые проверки запрещённых lifecycle-паттернов
  прошли успешно;
- реализация и связанные документы сохранены коммитом `adfeb2e` и отправлены
  в `origin/review/baseline-hardening`; draft PR `#1` дополнен результатами
  R-003, а GitHub Actions повторно запущен для опубликованного состояния.

**Решение D-006:** task, создавшая HTTP/decoder/session resources, является их
единственным владельцем до полного завершения cleanup. Timeout ожидания stop
возвращается вызывающему коду как ошибка и не даёт права принудительно удалять
task или освобождать её ресурсы.

**Незакрыто:**

- проверить на ESP32-S3 остановку в фазах connect, headers, stream read, ICY,
  decode и I²S, включая rapid station switching и недоступный сервер;
- измерить фактическую stop latency и heap/stack watermark в soak test;
- заменить набор boolean/handle полей явной player state machine и command
  queue; синхронизацию snapshot для Web/API вести как R-006.

## 2026-08-09 — удалённая проверка R-003

**Действия и результаты:**

- GitHub Actions run `31278172331` успешно собрал опубликованную ветку за
  2 минуты 47 секунд и загрузил firmware artifacts;
- обнаружено предупреждение runner о deprecated Node.js 20 в
  `actions/checkout@v4` и `actions/upload-artifact@v4`;
- через официальные GitHub releases подтверждены актуальные `v7.0.1` обоих
  actions и runtime Node.js 24; workflow переведён на стабильные major tags
  `actions/checkout@v7` и `actions/upload-artifact@v7`.

**Решение D-007:** поддерживать first-party GitHub Actions на актуальном
стабильном major, совместимом с runtime текущих hosted runners; обновление
major требует отдельного успешного CI run.

<!-- AISMM:END -->
