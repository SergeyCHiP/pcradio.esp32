<!-- AISMM:BEGIN -->
type: layer_document
model_instance_id: f4c43287-bd9a-437f-b1f2-f979edac7c62
product_id: 7dd96755-e1a9-42dc-8016-22d83f5a9f42
product_key: pcradio_esp32
layer_id: "802"
layer_key: planning_and_delivery_flow
document_id: pcradio.b8.802.review_refactoring_plan
document_type: change_plan
module_scope: root
status: proposed
spec_version: 1.0.0-lite
completion_status: partial
references:
  - pcradio.b0.001.product_definition
  - pcradio.b7.702.risk_register
<!-- AISMM:META_END -->

# План ревью и подготовки к развитию

## Текущий прогресс — 2026-08-08

- source commit 2.2.7 из OTA-манифеста проверен и недоступен в публичном Git;
- создана рабочая ветка `review/baseline-hardening` от `esp32s3n16r8`;
- подтверждён clean Docker build на ESP-IDF 5.4.2;
- managed dependencies закреплены точными версиями и lock-файлом;
- добавлен CI workflow сборки и локальный Docker-рецепт;
- исправлена кодовая часть R-002; target audio test ещё требуется.

## Этап 0 — определить настоящую базовую линию (P0)

1. Найти исходники бинарной версии 2.2.7 и историю их сборки.
2. Выбрать один путь:
   - продолжать 2.2.7 source baseline;
   - развивать `esp32s3n16r8` как новую линию и портировать возможности 2.x.
3. Создать feature/architecture decision matrix: форматы, OTA, captive portal,
   alarms, GPIO/IR/WLED, WebUI/API, partition/data migrations.
4. Зафиксировать release/branch policy и ADR с принятым решением.

**Критерий выхода:** один репозиторий/commit собирается в идентифицируемый
firmware, а backlog не смешивает две реализации.

## Этап 1 — воспроизводимая сборка и safety net (P0/P1)

1. Зафиксировать ESP-IDF, managed components и generator LittleFS.
2. Добавить `sdkconfig.defaults`, build/flash инструкции и clean build CI.
3. Включить warnings-as-errors для собственного кода, clang-format,
   cppcheck/clang static analyzer и dependency/license inventory.
4. Создать host-test harness для URL, M3U, ICY, codec detection и pure DSP.
5. Создать target smoke test: boot, Wi-Fi, play/stop, audio output, heap watermark.

**Критерий выхода:** clean checkout автоматически собирается; повторная сборка
имеет объяснимые артефакты; базовые тесты проходят локально и в CI.

## Этап 2 — устранить блокирующие дефекты (P0/P1)

Порядок: R-002 buffer lifetime → R-003 ownership/cancellation → R-004 TLS →
R-005 atomic playlist → R-007 static path → R-010 request parsing.

Для каждого исправления обязательны regression test, target smoke test и
проверка heap/stack watermark. Не совмещать эти изменения с новым UI или DSP.

**Критерий выхода:** нет forced task delete и незащищённого shared handle;
сетевые/файловые операции имеют отрицательные тесты и восстановление.

## Этап 3 — архитектурное разделение

1. Описать enum states/events/errors и public player contract.
2. Сделать player task единственным владельцем connection/decoder/session.
3. Отделить `stream_reader`, `playlist_repository`, `decoder`, `audio_sink`.
4. Разделить `audio_i2s_write` на предвыделенный PCM pipeline и I²S adapter.
5. Выделить configuration repository со schema version и migrations.
6. Перевести API/GPIO на command queue и immutable state snapshots.

**Критерий выхода:** модули тестируются через порты; high-level код не зависит
от ESP-IDF handles; state transitions детерминированы.

## Этап 4 — надёжность и эксплуатация

1. Recovery/captive mode вместо бесконечных boot loops.
2. Реальные network/audio/heap metrics и structured error counters.
3. Soak tests 24–72 часа, fault injection сети, malformed streams, low memory.
4. OTA/partition/userdata compatibility и rollback tests.
5. Threat model: local API, OTA, playlist supply chain, secrets in Flash.

**Критерий выхода:** измерены reconnect time, underruns, heap trend и recovery;
обновление/откат не теряют пользовательские данные.

## Этап 5 — только теперь продуктовые доработки

Каждая функция должна связывать requirement → component/API → risk → tests →
release. Первый разумный feature milestone выбирается после gap analysis с
2.2.7, а не до него.

## Формат code review

- Проход A: correctness/memory/resource ownership.
- Проход B: concurrency/FreeRTOS/real-time behavior.
- Проход C: external input/security/storage/OTA.
- Проход D: module boundaries/API/data contracts.
- Проход E: tests/observability/build/release.

Вывод каждого прохода: findings с severity, file:line, failure scenario,
minimal fix, architectural fix, verification and owner.

<!-- AISMM:END -->
