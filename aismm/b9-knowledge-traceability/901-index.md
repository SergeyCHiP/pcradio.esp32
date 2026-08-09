<!-- AISMM:BEGIN -->
type: trace_link
model_instance_id: f4c43287-bd9a-437f-b1f2-f979edac7c62
product_id: 7dd96755-e1a9-42dc-8016-22d83f5a9f42
product_key: pcradio_esp32
layer_id: "901"
layer_key: knowledge_index_and_navigation
document_id: pcradio.b9.901.traceability_index
document_type: traceability_index
module_scope: root
status: draft
spec_version: 1.0.0-lite
completion_status: partial
references:
  - pcradio.b0.001.product_definition
  - pcradio.b2.201.system_architecture
  - pcradio.b3.302.code_map
  - pcradio.b4.405.runtime_lifecycles
  - pcradio.b7.702.risk_register
  - pcradio.b8.802.review_refactoring_plan
  - pcradio.b8.805.change_history
<!-- AISMM:META_END -->

# Индекс и трассировка

## Навигация изменения

```text
Product baseline (b0.001)
  -> affected architecture boundary (b2.201)
  -> implementation files (b3.302)
  -> lifecycle/state transition (b4.405)
  -> risks and controls (b7.702)
  -> review/refactoring stage (b8.802)
  -> executed actions and decisions (b8.805)
```

## Ключевые связи

| Risk | Компонент/файл | Поведение | Этап |
| --- | --- | --- | --- |
| R-001 | branches `update`, `esp32s3n16r8` | release/source identity | 0 |
| R-002 | `main/src/player/audio_i2s.c` | PCM processing/write | 2 |
| R-003, R-006 | `main/src/player/player.c`, `web/api.c` | play/stop/reconnect | 2–3 |
| R-004 | `main/src/player/playlist.c` | playlist download | 2 |
| R-005 | `main/src/player/playlist.c` | update/rollback | 2 |
| R-007, R-010 | `main/src/web/www.c`, `web/api.c` | HTTP request handling | 2 |
| R-008 | build root, `main/idf_component.yml`, `tools/` | build/release | 1 |
| R-009 | `config.c`, `wireless.c`, `main.c` | boot/recovery | 4 |
| R-011 | `web/metrics.c` | observability | 4 |
| R-012 | `audio_i2s.c` | real-time audio | 3–4 |
| R-013 | player/playlist/ICY/codecs | external stream parsing | 1–4 |

## Provenance confidence

- `high`: факт непосредственно подтверждён кодом текущей ветки;
- `medium`: вывод из документации или конфигурации;
- `needs_verification`: зависит от runtime/toolchain или отсутствующего source.

R-004 имеет статус `needs_verification`, пока TLS-поведение не подтверждено на
ESP-IDF 5.4.2. R-001 подтверждён сравнением содержимого двух веток.

## Обновление модели

При закрытии риска обновляются минимум: risk record, связанный lifecycle,
change plan, change history и эта таблица. После выбора source baseline
обновляются registry, b0.001 и вся карта совместимости.

<!-- AISMM:END -->
