<!-- AISMM:BEGIN -->
type: risk_record
model_instance_id: f4c43287-bd9a-437f-b1f2-f979edac7c62
product_id: 7dd96755-e1a9-42dc-8016-22d83f5a9f42
product_key: pcradio_esp32
layer_id: "702"
layer_key: risk_management
document_id: pcradio.b7.702.risk_register
document_type: risk_register
module_scope: root
status: draft
spec_version: 1.0.0-lite
completion_status: partial
references:
  - pcradio.b3.302.code_map
  - pcradio.b8.802.review_refactoring_plan
<!-- AISMM:META_END -->

# Реестр рисков исходной ветки

Приоритет: P0 — нельзя безопасно продолжать; P1 — до новых функций; P2 — в
первом цикле улучшений; P3 — плановый долг.

| ID | P | Риск и свидетельство | План проверки/закрытия |
| --- | --- | --- | --- |
| R-001 | P0 | Source baseline не соответствует дистрибутиву 2.2.7 | получить актуальные исходники или утвердить новую линию и gap analysis |
| R-002 | P0 | `audio_i2s_write`: `data_to_write` сохранял адрес `stack_buffer` после выхода из его scope (`audio_i2s.c:649-793`) | lifetime исправлен в `review/baseline-hardening`; остаются build + target soak/audio test |
| R-003 | P0 | `player_stop` закрывает общий HTTP handle параллельно player task и затем может принудительно удалить task (`player.c:441-474`) | single-owner state machine, cooperative cancellation, concurrency tests |
| R-004 | P1 | Проверка TLS для playlist выглядит отключённой: нет CA/bundle, включён `skip_cert_common_name_check` (`playlist.c:60-68`) | подтвердить поведением ESP-IDF 5.4.2; включить cert bundle и negative TLS tests |
| R-005 | P1 | Playlist пишется прямо в рабочий файл; rollback индекса не восстанавливает старый файл (`playlist.c:113-190`, `271-301`) | temp file + validation + fsync + atomic rename + power-loss tests |
| R-006 | P1 | Shared globals player/playlist/ICY читаются HTTP handlers без общей ownership/snapshot модели | TSAN host model или stress tests; очередь команд и immutable snapshots |
| R-007 | P1 | Static file path строится непосредственно из URI (`www.c:75-99`) | reject traversal/encoded traversal; canonical allowlisted paths; tests |
| R-008 | P1 | Первый Docker build подтверждён, но ещё нет CI, unit/integration tests и проверки детерминизма артефактов | toolchain/deps закреплены; добавить CI, static analysis, tests и сравнение artifact hashes |
| R-009 | P2 | Ошибки config/LittleFS и ожидание Wi-Fi могут навсегда остановить boot | recovery/captive mode, bounded retries, health state |
| R-010 | P2 | HTTP body читается одним `httpd_req_recv`, нет общего limit/complete-body helper | централизованный parser, Content-Length limits, fuzz/negative tests |
| R-011 | P2 | Метрики RX/TX синтетические, зависят от uptime/request count (`metrics.c:127-180`) | убрать или переименовать; использовать реальные счётчики |
| R-012 | P2 | Audio path выделяет heap buffer во время playback и смешивает DSP/I²S/locking | preallocated buffers, bounded work, profiling underruns/latency |
| R-013 | P2 | Парсеры URL/M3U/ICY/codec frames обрабатывают внешние недоверенные данные без fuzz corpus | host adapters + fuzzing + malformed-stream corpus |
| R-014 | P3 | API, data formats и compatibility policy не версионированы в source branch | OpenAPI/schema, migration/version policy, contract tests |

## Security boundary

API текущей ветки не имеет аутентификации. Минимальная модель угроз должна
считать всю локальную сеть потенциально недоверенной: любой клиент может
переключать станции, менять настройки и нагружать память/Flash. Решение
«trusted LAN only» допустимо только как явно принятый продуктовый риск.

<!-- AISMM:END -->
