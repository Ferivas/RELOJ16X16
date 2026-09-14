# RELOJ16X16

Reloj digital sobre matriz LED de 16×16 (4 módulos de 8×8) controlado por un ESP32. Proyecto PlatformIO + Arduino: multiplexado por timer de hardware con SPI, 3 fuentes, animación de cambio de minuto, brillo regulable, botones con configuración persistente, sincronización NTP y portal WiFi de configuración con icono de aviso en la matriz cuando no hay red.

## Hardware

| Elemento | Detalle |
|---|---|
| MCU | ESP32 (`esp32dev`), 240 MHz, 4 MB Flash |
| Display | 4 matrices 8×8 (16×16 píxeles), barrido por columnas vía SPI HW |
| Revisión | `VERSION_HW 2` (definido en `src/main.cpp`) |

Pines según revisión (`src/main.cpp`):

| Señal | HW v1 | HW v2 |
|---|---|---|
| `LEDPIN` | 5 | 2 |
| `PIN_OE` | 27 | 27 |
| `PIN_DATA` | 26 | 14 |
| `PIN_CLK` | 25 | 12 |
| `PIN_LOAD` | 33 | 13 |
| `PIN_MISO` | 15 | 15 |
| Botón fuente | 18 (configurable) | 18 (configurable) |
| Botón brillo | 19 (configurable) | 19 (configurable) |

## Funcionalidades del firmware

- **Reloj HH:MM** con parpadeo de dos puntos (1 s) y animación de desplazamiento vertical al cambiar de minuto.
- **3 fuentes** seleccionables con el botón de fuente (`pin_fuente`, por defecto 18). La selección persiste en LittleFS.
- **3 niveles de brillo** con el botón de brillo (`pin_brillo`, por defecto 19). El brillo se controla con el ancho del pulso de `PIN_OE` dentro de la ISR de 2 ms.
- **Sincronización NTP** (`pool.ntp.org`) con offset horario configurable y re-sincronización periódica cada `n_horas` (+30 s).
- **Portal WiFi**: si no hay red conocida, levanta el AP `RELOJ2023` (clave `reloj2023`) con portal web en `192.168.4.1` para configurar la red. Las credenciales se guardan en `/wifi.dat` (`ssid;password` por línea).
- **Icono "sin WiFi"**: mientras no hay conexión, la primera matriz muestra un icono WiFi (arcos + punto) parpadeando cada ~1 s. Al conectar, la zona se limpia y arranca el reloj.

## Configuración (`/config.json` en LittleFS)

| Clave | Por defecto | Descripción |
|---|---|---|
| `fuente_actual` | 0 | Fuente activa (0–2) |
| `pin_fuente` | 18 | GPIO del botón de fuente |
| `pin_brillo` | 19 | GPIO del botón de brillo |
| `nivel_brillo` | 2 | Nivel de brillo (1–3) |
| `n_horas` | 6 | Intervalo de re-sincronización NTP en horas (requiere reinicio al cambiarlo) |
| `gmt_offset` | -18000 | Offset horario en segundos (UTC-5 por defecto) |

`data/config.json` es la plantilla que se graba al dispositivo con `uploadfs`. Si el equipo ya tiene un `/config.json` anterior, conserva sus valores hasta reformatear o re-subir el filesystem.

## Comandos

```bash
pio run -e esp32dev                 # compilar
pio run -e esp32dev -t upload      # subir firmware
pio run -e esp32dev -t uploadfs    # subir filesystem (data/ -> LittleFS, necesario tras cambiar data/config.json)
pio device monitor -b 115200       # monitor serie
pio run -e esp32dev -t erase       # borrado total (firmware + NVS + LittleFS, útil para pruebas sin credenciales)
```

## Estructura

```
src/main.cpp     # reloj: ISR de barrido 2 ms, fuentes, animación, botones, NTP, config
src/wifimgr.cpp  # WiFiManager: perfiles /wifi.dat, escaneo por RSSI, portal AP
src/wifimgr.h
data/config.json # plantilla LittleFS
platformio.ini   # env esp32dev, espressif32@6.8.1, LittleFS
```

## Notas técnicas

- `platformio.ini` fija `espressif32@6.8.1` (Arduino core **2.x**): el timer usa `timerBegin(0, 80, true)` / `timerAttachInterrupt(..., true)` / `timerAlarmWrite(timer, 2000, true)` / `timerAlarmEnable`. No migrar a la API 3.x.
- La ISR `handleInterrupt` corre cada 2 ms: mantenerla `IRAM_ATTR`, sin `Serial`, reservas de memoria ni `String`.
- El ESP32 guarda además las credenciales STA en NVS: `WiFi.begin()` sin argumentos reconecta solo. Para probar el modo sin-WiFi hay que borrar NVS (`erase`) además de `/wifi.dat`.
- El portal solo decodifica `%3F` (`?`) y `%21` (`!`) en SSID/password.
- En el bitmap del icono WiFi, el **bit 0 de cada byte es la fila superior** (verificado en hardware).
