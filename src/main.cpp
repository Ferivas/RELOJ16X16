// main.cpp — Reloj 16x16 (4 matrices 8x8) para ESP32, port de mainani.py
// Conserva: multiplexado por timer de 2 ms con SPI HW, 3 fuentes, animación
// de cambio de minuto, 3 niveles de brillo, botones con config persistente
// (config.json en LittleFS), sincronización NTP periódica (N horas + 30 s),
// parpadeo de dos puntos e icono WiFi parpadeante en matriz 0 si no hay red.

#include <Arduino.h>
#include <SPI.h>
#include <LittleFS.h>
#include <time.h>
#include "wifimgr.h"

// --- GESTIÓN DE CONFIGURACIÓN JSON ---
static const char *CONFIG_FILE = "/config.json";

struct Config {
    int fuente_actual = 0;
    int pin_fuente = 18;
    int pin_brillo = 19;
    int nivel_brillo = 2;
    int n_horas = 6;  // Intervalo N en horas para la sincronización NTP
    long gmt_offset = -18000;  // Offset horario en segundos (UTC-5 por defecto)
};

static Config config;

// Parser mínimo: extrae enteros "clave": N del JSON guardado.
static int json_get_int(const String &json, const char *key, int defval) {
    String k = String("\"") + key + "\"";
    int i = json.indexOf(k);
    if (i < 0) return defval;
    i = json.indexOf(':', i + k.length());
    if (i < 0) return defval;
    return json.substring(i + 1).toInt();
}

static void guardar_config() {
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        Serial.println("Error guardando config.json");
        return;
    }
    f.printf("{\"fuente_actual\": %d, \"pin_fuente\": %d, \"pin_brillo\": %d, "
             "\"nivel_brillo\": %d, \"n_horas\": %d, \"gmt_offset\": %ld}\n",
             config.fuente_actual, config.pin_fuente, config.pin_brillo,
             config.nivel_brillo, config.n_horas, config.gmt_offset);
    f.close();
}

static void cargar_config() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("No se encontró config.json. Usando valores predeterminados...");
        guardar_config();
        return;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) { guardar_config(); return; }
    String json = f.readString();
    f.close();
    config.fuente_actual = json_get_int(json, "fuente_actual", 0);
    config.pin_fuente    = json_get_int(json, "pin_fuente", 18);
    config.pin_brillo    = json_get_int(json, "pin_brillo", 19);
    config.nivel_brillo  = json_get_int(json, "nivel_brillo", 2);
    config.n_horas       = json_get_int(json, "n_horas", 6);
    config.gmt_offset    = json_get_int(json, "gmt_offset", -18000);
}

// --- HARDWARE ---
#define NUMMATRIZ 4
#define LONGBUF (NUMMATRIZ * 8)

#define VERSION_HW 2

#if VERSION_HW == 1
static const int LEDPIN = 5;
static const int PIN_OE = 27;
static const int PIN_DATA = 26, PIN_CLK = 25, PIN_LOAD = 33, PIN_MISO = 15;
#elif VERSION_HW == 2
static const int LEDPIN = 2;
static const int PIN_OE = 27;
static const int PIN_DATA = 14, PIN_CLK = 12, PIN_LOAD = 13, PIN_MISO = 15;
#endif

static uint8_t buffram[LONGBUF + 1];

static volatile bool flaginternet = false;
static const uint8_t columnas[8] = {0b00000001, 0b00000010, 0b00000100, 0b00001000,
                                    0b00010000, 0b00100000, 0b01000000, 0b10000000};
static const int tblposdig[6] = {10, 2, 23, 16, 25, 31};

static volatile int NIVEL_BRILLO = 3;
static int ptrcol = 0;
static volatile bool interruptCounter = false;
static int ap_blink_counter = 0;
static bool wifi_icon_on = false;

// Icono "sin WiFi" 8x8 para la matriz 0 (column-major: cada byte es una
// columna, bit 0 = fila superior). Punto inferior + 2 arcos:
//   .######.  #......#  .##..##.  ...##...  ...##...  ...##... (punto)
static const uint8_t ICONO_WIFI[8] = {
    0b00000100, 0b00001010, 0b00001010, 0b10110010,
    0b10110010, 0b00001010, 0b00001010, 0b00000100
};
#define ICONO_WIFI_LEN 8

static SPIClass *spi = nullptr;
static hw_timer_t *timer = nullptr;

// --- ISR de barrido (equivalente a handleInterrupt del timer de 2 ms) ---
void IRAM_ATTR handleInterrupt() {
    // 1. Apagar inmediatamente (blanking)
    digitalWrite(PIN_OE, HIGH);

    // 2. Cargar la nueva columna en los 4 módulos
    uint8_t col = columnas[ptrcol];
    uint8_t spi_buf[8] = {
        col, buffram[ptrcol],
        col, buffram[ptrcol + 8],
        col, buffram[ptrcol + 16],
        col, buffram[ptrcol + 24]
    };

    digitalWrite(PIN_LOAD, LOW);
    spi->writeBytes(spi_buf, 8);
    digitalWrite(PIN_LOAD, HIGH);
    digitalWrite(PIN_LOAD, LOW);

    // 3. Pequeña pausa de blanking para que los datos se estabilicen
    //    y se disipe la carga parasita de la columna anterior
    delayMicroseconds(10);

    // 4. Encender el display
    digitalWrite(PIN_OE, LOW);

    // 5. Control de brillo: ancho del pulso de encendido
    if (NIVEL_BRILLO == 1) {
        delayMicroseconds(150);
        digitalWrite(PIN_OE, HIGH);
    } else if (NIVEL_BRILLO == 2) {
        delayMicroseconds(500);
        digitalWrite(PIN_OE, HIGH);
    } else if (NIVEL_BRILLO == 3) {
        // Antes no había apagado; ahora dejamos encendido 1.5 ms
        // y apagado ~0.49 ms hasta la siguiente interrupción (2 ms total)
        delayMicroseconds(1500);
        digitalWrite(PIN_OE, HIGH);
    }

    ptrcol = (ptrcol + 1) & 7;
    interruptCounter = true;

    // Icono "sin WiFi" parpadeante en la matriz 0 (bytes 0..7) a ~1 s.
    // Solo activo mientras no hay conexión (modo portal AP).
    if (!flaginternet) {
        ap_blink_counter++;
        if (ap_blink_counter >= 500) {  // 500 ticks x 2 ms = 1 s
            ap_blink_counter = 0;
            wifi_icon_on = !wifi_icon_on;
            for (int i = 0; i < ICONO_WIFI_LEN; i++) {
                buffram[i] = wifi_icon_on ? ICONO_WIFI[i] : 0x00;
            }
        }
    }
}

// --- DEFINICIÓN DE LAS 3 FUENTES ---
static const uint8_t TBL_ORIGINAL[66] = {
0B01111100, 0B11111110, 0B10000010, 0B10000010, 0B11111110, 0B01111100,
0B00000000, 0B10000100, 0B11111110, 0B11111110, 0B10000000, 0B00000000,
0B10000100, 0B11000010, 0B11100010, 0B10110010, 0B10011110, 0B10001100,
0B10000010, 0B10000010, 0B10010010, 0B10010010, 0B11111110, 0B01101100,
0B00111100, 0B00111100, 0B00100000, 0B11111110, 0B11111110, 0B00100000,
0B01011110, 0B11011110, 0B10010010, 0B10010010, 0B11110010, 0B01100010,
0B01111100, 0B11111110, 0B10010010, 0B10010010, 0B11110010, 0B01100000,
0B00000010, 0B11100010, 0B11110010, 0B00011010, 0B00001110, 0B00000110,
0B01101100, 0B11111110, 0B10010010, 0B10010010, 0B11111110, 0B01101100,
0B00001100, 0B10011110, 0B10010010, 0B10010010, 0B11111110, 0B01111100,
0B00000000, 0B00000000, 0B00000000, 0B00000000, 0B00000000, 0B00000000,
};

static const uint8_t TBL_MODERNA[66] = {
0B01111100,
0B10000010,
0B10000010,
0B10000010,
0B10000010,
0B01111100,


0B00000000,
0B00001000,
0B10000100,
0B11111110,
0B10000000,
0B00000000,


0B10000100,
0B11000010,
0B10100010,
0B10010010,
0B10010010,
0B10001100,


0B10000010,
0B10000010,
0B10010010,
0B10010010,
0B10010010,
0B01101100,


0B00111100,
0B00100000,
0B00100000,
0B00100000,
0B11111110,
0B00100000,


0B10011110,
0B10010010,
0B10010010,
0B10010010,
0B10010010,
0B01100010,


0B01111100,
0B10010010,
0B10010010,
0B10010010,
0B10010010,
0B01100000,


0B00000010,
0B00000010,
0B11100010,
0B00010010,
0B00001010,
0B00000110,


0B01101100,
0B10010010,
0B10010010,
0B10010010,
0B10010010,
0B01101100,


0B00001100,
0B10010010,
0B10010010,
0B10010010,
0B10010010,
0B01111100,


0B00000000,
0B00000000,
0B00000000,
0B00000000,
0B00000000,
0B00000000,


};

static const uint8_t TBL_RETRO[66] = {
0B00000000, 0B01111100, 0B10000010, 0B10000010, 0B10000010, 0B01111100,
0B00000000, 0B00000000, 0B10000100, 0B11111110, 0B10000000, 0B00000000,
0B00000000, 0B10000100, 0B11000010, 0B10100010, 0B10010010, 0B10001100,
0B00000000, 0B01000010, 0B10000010, 0B10001010, 0B10010110, 0B01100010,
0B00000000, 0B00110000, 0B00101000, 0B00100100, 0B11111110, 0B00100000,
0B00000000, 0B01001110, 0B10001010, 0B10001010, 0B10001010, 0B01110010,
0B00000000, 0B01111000, 0B10010100, 0B10010010, 0B10010010, 0B01100000,
0B00000000, 0B00000110, 0B11000010, 0B00100010, 0B00010010, 0B00001110,
0B00000000, 0B01101100, 0B10010010, 0B10010010, 0B10010010, 0B01101100,
0B00000000, 0B00001100, 0B00010010, 0B10010010, 0B10010010, 0B01111100,
0B00000000, 0B00000000, 0B00000000, 0B00000000, 0B00000000, 0B00000000,
};

static const uint8_t *FUENTES[3] = {TBL_ORIGINAL, TBL_MODERNA, TBL_RETRO};
#define NUM_FUENTES 3

static int fuente_actual = 0;

static void gendig(int valdig, int posdig) {
    int ptrdig = valdig * 6;
    int ptrpos = tblposdig[posdig];
    const uint8_t *tabla = FUENTES[fuente_actual];
    for (int i = 0; i < 6; i++) {
        buffram[ptrpos + i] = tabla[ptrdig + i];
    }
}

// --- NTP: offset horario configurable vía "gmt_offset" en config.json ---
// (por defecto -18000 = UTC-5, igual que time.time() - 18000 del original)
static const char *NTP_HOST = "pool.ntp.org";

static void getntptime() {
    Serial.println("Sincronizando hora con NTP...");
    configTime(config.gmt_offset, 0, NTP_HOST);
    struct tm tval;
    if (getLocalTime(&tval, 10000)) {
        Serial.println("Sincronización NTP exitosa.");
    } else {
        Serial.println("Error en NTP");
    }
}

void setup() {
    Serial.begin(115200);

    if (!LittleFS.begin(true)) {
        Serial.println("Error montando LittleFS");
    }

     // -----------------------------------------

    cargar_config();
    // --- Mostrar contenido de config.json ---
    File f = LittleFS.open("/config.json", "r");
    if (f) {
        Serial.println("--- Contenido de /config.json ---");
        while (f.available()) {
            Serial.write(f.read());
        }
        Serial.println("\n--- Fin ---");
        f.close();
    } else {
        Serial.println("No se pudo abrir /config.json para lectura");
    }    

    fuente_actual = config.fuente_actual;
    NIVEL_BRILLO = config.nivel_brillo;

    pinMode(config.pin_fuente, INPUT_PULLUP);
    pinMode(config.pin_brillo, INPUT_PULLUP);
    pinMode(LEDPIN, OUTPUT);
    pinMode(PIN_OE, OUTPUT);
    pinMode(PIN_LOAD, OUTPUT);

    spi = new SPIClass(HSPI);
    spi->begin(PIN_CLK, PIN_MISO, PIN_DATA, -1);
    spi->beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

    digitalWrite(PIN_LOAD, LOW);
    digitalWrite(PIN_OE, LOW);

// --- REEMPLAZA ESTO (core 3.x) ---
// timer = timerBegin(1000000);
// timerAttachInterrupt(timer, &handleInterrupt);
// timerAlarm(timer, 2000, true, 0);

// --- POR ESTO (core 2.x) ---
timer = timerBegin(0, 80, true);                    // Timer 0, prescaler 80 → 1 MHz, count up
timerAttachInterrupt(timer, &handleInterrupt, true); // true = edge triggered
timerAlarmWrite(timer, 2000, true);                 // 2000 ticks @ 1 MHz = 2 ms, auto-reload
timerAlarmEnable(timer);                            // Activar la alarma

    // Conexión WiFi (STA o portal AP de configuración)
    if (!wifi_get_connection()) {
        Serial.println("No se pudo inicializar la conexión de red.");
        while (true) { delay(1000); }
    }

    flaginternet = true;
    // Limpiar la ventana del icono "sin WiFi" (matriz 0, bytes 0..7) para
    // que el reloj arranque limpio aunque el icono quedara encendido.
    for (int i = 0; i < ICONO_WIFI_LEN; i++) buffram[i] = 0;
    getntptime();
}

void loop() {
    static unsigned long INTERVALO_NTP_SEG = (unsigned long)config.n_horas * 3600UL + 30UL;
    static time_t ultimo_ntp_tick = time(nullptr);

    static int dhoraant = 99, uhoraant = 99, dminant = 99, uminant = 99;
    static bool newd = false;
    static int tled = 0;
    static int btn_fuente_prev = 1, btn_brillo_prev = 1;

    static const unsigned long ANIM_FRAME_TIME = 200;
    static bool is_animating = false;
    static int anim_frame = 0;
    static unsigned long last_anim_tick = 0;

    static int curr_dhora = 0, curr_uhora = 0, curr_dmin = 0, curr_umin = 0;
    static int next_dhora = 0, next_uhora = 0, next_dmin = 0, next_umin = 0;
    static int tick = 0;
    static bool newdig = false;
    static int cntrdig = 0;

    if (interruptCounter) {
        interruptCounter = false;
        tled = (tled + 1) % 1600;
        digitalWrite(LEDPIN, tled < 100 ? HIGH : LOW);
        tick = (tick + 1) % 500;
        if (tick == 0) newdig = true;
    }

    // --- LECTURA DE BOTÓN FUENTE ---
    int btn_f_state = digitalRead(config.pin_fuente);
    if (btn_f_state == 0 && btn_fuente_prev == 1) {
        fuente_actual = (fuente_actual + 1) % NUM_FUENTES;
        config.fuente_actual = fuente_actual;
        guardar_config();
        newd = true;
        delay(200);
    }
    btn_fuente_prev = btn_f_state;

    // --- LECTURA DE BOTÓN BRILLO ---
    int btn_b_state = digitalRead(config.pin_brillo);
    if (btn_b_state == 0 && btn_brillo_prev == 1) {
        NIVEL_BRILLO = (NIVEL_BRILLO % 3) + 1;
        config.nivel_brillo = NIVEL_BRILLO;
        guardar_config();
        delay(200);
    }
    btn_brillo_prev = btn_b_state;

    unsigned long current_tick_ms = millis();

    // --- VERIFICACIÓN DE ACTUALIZACIÓN NTP PERIÓDICA ---
    time_t t_actual_epoch = time(nullptr);
    if ((unsigned long)(t_actual_epoch - ultimo_ntp_tick) >= INTERVALO_NTP_SEG) {
        ultimo_ntp_tick = t_actual_epoch;
        getntptime();
        newd = true;
    }

    if (is_animating) {
        if (current_tick_ms - last_anim_tick >= ANIM_FRAME_TIME) {
            anim_frame++;
            last_anim_tick = current_tick_ms;

            if (anim_frame > 8) {
                is_animating = false;
                newd = true;
            } else {
                // draw_animated_digit para las 4 posiciones
                int cur[4]  = {curr_umin, curr_dmin, curr_uhora, curr_dhora};
                int nxt[4]  = {next_umin, next_dmin, next_uhora, next_dhora};
                for (int pos = 0; pos < 4; pos++) {
                    int old_val = cur[pos], new_val = nxt[pos];
                    if (old_val == new_val || anim_frame == 0) {
                        gendig(old_val, pos);
                    } else if (anim_frame >= 8) {
                        gendig(new_val, pos);
                    } else {
                        int ptrdig_old = old_val * 6;
                        int ptrdig_new = new_val * 6;
                        int ptrpos = tblposdig[pos];
                        const uint8_t *tabla = FUENTES[fuente_actual];
                        for (int i = 0; i < 6; i++) {
                            uint8_t old_byte = tabla[ptrdig_old + i];
                            uint8_t new_byte = tabla[ptrdig_new + i];
                            uint8_t shifted_old = old_byte >> anim_frame;
                            uint8_t shifted_new = (new_byte << (8 - anim_frame)) & 0xFF;
                            buffram[ptrpos + i] = shifted_old | shifted_new;
                        }
                    }
                }
            }
        }
    }

    if (newdig) {
        newdig = false;
        cntrdig++;
        int tc = cntrdig % 2;
        if (tc == 0) {
            buffram[31] = 0b00000000;
            buffram[30] = 0b00000000;
        } else {
            buffram[31] = 0b01101100;
            buffram[30] = 0b01101100;
        }

        time_t tclk = time(nullptr);
        struct tm tval;
        localtime_r(&tclk, &tval);

        // --- CÁLCULO DE DÍGITOS ---
        curr_dhora = tval.tm_hour / 10;
        curr_uhora = tval.tm_hour % 10;
        curr_dmin  = tval.tm_min / 10;
        curr_umin  = tval.tm_min % 10;

        if (tval.tm_sec == 58 && !is_animating) {
            time_t tclk_next = tclk + 2;
            struct tm tval_next;
            localtime_r(&tclk_next, &tval_next);
            next_dhora = tval_next.tm_hour / 10;
            next_uhora = tval_next.tm_hour % 10;
            next_dmin  = tval_next.tm_min / 10;
            next_umin  = tval_next.tm_min % 10;

            if (curr_umin != next_umin) {
                is_animating = true;
                anim_frame = 0;
                last_anim_tick = current_tick_ms;
            }
        }

        if (!is_animating) {
            if (curr_dhora != dhoraant || curr_uhora != uhoraant ||
                curr_dmin != dminant || curr_umin != uminant || newd) {
                dhoraant = curr_dhora; uhoraant = curr_uhora;
                dminant = curr_dmin;   uminant = curr_umin;
                newd = false;
                gendig(curr_dhora, 3);
                gendig(curr_uhora, 2);
                gendig(curr_dmin, 1);
                gendig(curr_umin, 0);
            }
        }
    }

    delay(2);
}
