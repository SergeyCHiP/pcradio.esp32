# Development

## Reproducible build

The firmware baseline uses ESP-IDF 5.4.2. Managed component versions are pinned
in `main/idf_component.yml` and resolved hashes are committed in
`dependencies.lock`.

Build without installing ESP-IDF on the host:

```bash
docker run --rm \
  -v "$PWD":/project \
  -w /project \
  espressif/idf:v5.4.2 \
  idf.py build
```

Perform a clean build after changing the toolchain, component manifest, lock
file, partition table, or `sdkconfig`:

```bash
docker run --rm \
  -v "$PWD":/project \
  -w /project \
  espressif/idf:v5.4.2 \
  idf.py fullclean build
```

The main outputs are:

- `build/ESP32-PCRadio.bin`;
- `build/bootloader/bootloader.bin`;
- `build/partition_table/partition-table.bin`;
- `build/flash_args`.

The current LittleFS generation helper is Windows-only. Replacing it with a
portable, version-pinned build step remains part of the reproducibility backlog.

## Baseline limitations

This branch is based on the published `esp32s3n16r8` sources. It is not the
source of the distributed firmware 2.2.7. The commit referenced by the 2.2.7
OTA manifest is not available from the public Git remote.

Do not describe artifacts built from this branch as firmware 2.2.7.

## Before submitting a change

1. Run a clean Docker build.
2. Check `git diff --check`.
3. Record target-device tests when the change touches FreeRTOS lifecycle,
   network behavior, Flash storage, GPIO, or the audio path.
4. Update the relevant risk and traceability records under `aismm/`.
