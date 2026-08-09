# ESP32_PCRadio

Hardware

- ESP32-S3 N16R8
- CJMCU-1334

Libraries

- ESP_IDF 5.4.2
- littlefs 1.20.1
- esp_audio_codec 2.3.0
- esp_audio_effects 1.1.0

See [DEVELOPMENT.md](DEVELOPMENT.md) for the pinned Docker build and baseline
limitations.

Script `create_littlefs.cmd` for creating image and flashing data folder. (Uses mklittlefs.exe)

Config WiFi `./data/config.json`

```json
{
  "wifi": {
  "ssid": "SID",
  "password": "Pass"
  },
  "ntp":{
  "ntp_server": [
    "pool.ntp.org",
    "ntp.ix.ru",
    "ntp0.ntp-servers.net"
  ],
  "ntp_tz": "+0300"
  }
}
```

## Web interface

![alt text](tools/doc/screen_ESP32_Web_Radio.png)

## API

### PLAYLIST update

```bash
curl -X POST http://<ip>/api/playlist -H "Content-Type: application/json" -d "update"
```

### Channel Num

```bash
curl -X POST http://<ip>/api/channel -H "Content-Type: application/json" -d "18"
```

### VOLUME 0-100

```bash
curl -X POST http://<ip>/api/volume -H "Content-Type: application/json" -d "90"
```

### MUTE 1 - mute, 0 - unpute

```bash
curl -X POST http://<ip>/api/mute -H "Content-Type: application/json" -d "1"
```

### SET EQ 0-9

```bash
curl -X POST http://<ip>/api/eq -H "Content-Type: application/json" -d "5"
```

### PLAYER STOP

```bash
curl -X POST http://<ip>/api/player -H "Content-Type: application/json" -d "stop"
```

[3D ESP32 PCRadio box](https://www.thingiverse.com/thing:7091753)
