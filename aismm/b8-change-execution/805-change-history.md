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

**Решение D-005:** текущая разработка и review PR ведутся в форке. Upstream
остаётся источником исходных веток; отправка изменений владельцу upstream
потребует отдельного решения после стабилизации и проверки на устройстве.

<!-- AISMM:END -->
