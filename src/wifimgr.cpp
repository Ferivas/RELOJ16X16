// wifimgr.cpp — Port de wifimgr.py (basado en tayfunulu/WiFiManager)
// Mantiene: perfiles en /wifi.dat (formato "ssid;password"), escaneo ordenado
// por RSSI, AP "RELOJ2023"/"reloj2023", portal web en 192.168.4.1 con las
// mismas páginas HTML y decodificación %3F / %21.

#include "wifimgr.h"
#include <WiFi.h>
#include <LittleFS.h>

static const char *AP_SSID = "RELOJ2023";
static const char *AP_PASSWORD = "reloj2023";
static const char *NETWORK_PROFILES = "/wifi.dat";

static WiFiServer server(80);

// --- Perfiles WiFi (ssid;password por línea) ---
static String read_profiles_raw() {
    if (!LittleFS.exists(NETWORK_PROFILES)) return String();
    File f = LittleFS.open(NETWORK_PROFILES, "r");
    if (!f) return String();
    String s = f.readString();
    f.close();
    return s;
}

static String profile_password_for(const String &ssid) {
    String data = read_profiles_raw();
    int start = 0;
    while (start < (int)data.length()) {
        int nl = data.indexOf('\n', start);
        if (nl < 0) nl = data.length();
        String line = data.substring(start, nl);
        int sep = line.indexOf(';');
        if (sep > 0 && line.substring(0, sep) == ssid) return line.substring(sep + 1);
        start = nl + 1;
    }
    return String();
}

static void write_profile(const String &ssid, const String &password) {
    String data = read_profiles_raw();
    String out;
    int start = 0;
    while (start < (int)data.length()) {
        int nl = data.indexOf('\n', start);
        if (nl < 0) nl = data.length();
        String line = data.substring(start, nl);
        int sep = line.indexOf(';');
        if (sep > 0 && line.substring(0, sep) != ssid && line.length() > 0) {
            out += line + "\n";
        }
        start = nl + 1;
    }
    out += ssid + ";" + password + "\n";
    File f = LittleFS.open(NETWORK_PROFILES, "w");
    if (f) {
        f.print(out);
        f.close();
    }
}

// --- Conexión ---
static bool do_connect(const String &ssid, const char *password) {
    if (WiFi.status() == WL_CONNECTED) return false;
    Serial.printf("Trying to connect to %s...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password);
    bool connected = false;
    for (int retry = 0; retry < 100; retry++) {
        if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
        delay(100);
        Serial.print('.');
    }
    if (connected) {
        Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.printf("\nFailed. Not Connected to: %s\n", ssid.c_str());
    }
    return connected;
}

// --- Servidor web del portal ---
static void send_header(WiFiClient &client, int status_code = 200, int content_length = -1) {
    client.printf("HTTP/1.0 %d OK\r\n", status_code);
    client.print("Content-Type: text/html\r\n");
    if (content_length >= 0) client.printf("Content-Length: %d\r\n", content_length);
    client.print("\r\n");
}

static void send_response(WiFiClient &client, const String &payload, int status_code = 200) {
    send_header(client, status_code, payload.length());
    if (payload.length() > 0) client.print(payload);
    client.stop();
}

static void handle_root(WiFiClient &client) {
    int n = WiFi.scanNetworks();
    send_header(client);
    client.print(R"HTML(
        <html>
            <h1 style="color: #5e9ca0; text-align: center;">
                <span style="color: #ff0000;">
                    Wi-Fi Client Setup
                </span>
            </h1>
            <form action="configure" method="post">
                <table style="margin-left: auto; margin-right: auto;">
                    <tbody>
    )HTML");
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        client.printf(R"HTML(
                        <tr>
                            <td colspan="2">
                                <input type="radio" name="ssid" value="%s" />%s
                            </td>
                        </tr>
        )HTML", ssid.c_str(), ssid.c_str());
    }
    client.printf(R"HTML(
                        <tr>
                            <td>Password:</td>
                            <td><input name="password" type="password" /></td>
                        </tr>
                    </tbody>
                </table>
                <p style="text-align: center;">
                    <input type="submit" value="Submit" />
                </p>
            </form>
            <p>&nbsp;</p>
            <hr />
            <h5>
                <span style="color: #ff0000;">
                    Your ssid and password information will be saved into the
                    "%s" file in your ESP module for future usage.
                    Be careful about security!
                </span>
            </h5>
            <hr />
            <h2 style="color: #2e6c80;">
                Some useful infos:
            </h2>
            <ul>
                <li>
                    Original code from <a href="https://github.com/cpopp/MicroPythonSamples"
                        target="_blank" rel="noopener">cpopp/MicroPythonSamples</a>.
                </li>
                <li>
                    This code available at <a href="https://github.com/tayfunulu/WiFiManager"
                        target="_blank" rel="noopener">tayfunulu/WiFiManager</a>.
                </li>
            </ul>
        </html>
    )HTML", NETWORK_PROFILES + 1);  // sin el '/' inicial
    client.stop();
}

// El portal solo decodifica %3F y %21, igual que la versión MicroPython.
static String portal_urldecode(String s) {
    s.replace("%3F", "?");
    s.replace("%21", "!");
    return s;
}

static bool handle_configure(WiFiClient &client, const String &request) {
    int si = request.indexOf("ssid=");
    if (si < 0) {
        send_response(client, "Parameters not found", 400);
        return false;
    }
    si += 5;
    int amp = request.indexOf('&', si);
    int pi = request.indexOf("password=", amp);
    if (amp < 0 || pi < 0) {
        send_response(client, "Parameters not found", 400);
        return false;
    }
    String ssid = portal_urldecode(request.substring(si, amp));
    String password = portal_urldecode(request.substring(pi + 9));

    if (ssid.length() == 0) {
        send_response(client, "SSID must be provided", 400);
        return false;
    }

    if (do_connect(ssid, password.c_str())) {
        String response = String(R"HTML(
            <html>
                <center>
                    <br><br>
                    <h1 style="color: #5e9ca0; text-align: center;">
                        <span style="color: #ff0000;">
                            ESP successfully connected to WiFi network )HTML") + ssid + R"HTML(.
                        </span>
                    </h1>
                    <br><br>
                </center>
            </html>
        )HTML";
        send_response(client, response);
        write_profile(ssid, password);
        delay(5000);
        return true;
    } else {
        String response = String(R"HTML(
            <html>
                <center>
                    <h1 style="color: #5e9ca0; text-align: center;">
                        <span style="color: #ff0000;">
                            ESP could not connect to WiFi network )HTML") + ssid + R"HTML(.
                        </span>
                    </h1>
                    <br><br>
                    <form>
                        <input type="button" value="Go back!" onclick="history.back()"></input>
                    </form>
                </center>
            </html>
        )HTML";
        send_response(client, response);
        return false;
    }
}

// --- Punto de entrada: igual que wifimgr.get_connection() ---
bool wifi_get_connection() {
    if (WiFi.status() == WL_CONNECTED) return true;

    WiFi.mode(WIFI_STA);
    WiFi.begin();  // reintenta credenciales previas del SDK
    delay(3000);
    if (WiFi.status() == WL_CONNECTED) return true;

    bool connected = false;

    // Escaneo de redes y prueba de perfiles conocidos
    int n = WiFi.scanNetworks();
    // Recorrido por RSSI descendente usando selección de máximo:
    bool *used = new bool[n]();
    for (int k = 0; k < n && !connected; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (!used[i] && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        String ssid = WiFi.SSID(best);
        int32_t rssi = WiFi.RSSI(best);
        uint8_t enc = WiFi.encryptionType(best);
        Serial.printf("ssid: %s rssi: %d authmode: %d\n", ssid.c_str(), (int)rssi, (int)enc);
        if (enc != WIFI_AUTH_OPEN) {
            String pass = profile_password_for(ssid);
            if (pass.length() > 0) {
                connected = do_connect(ssid, pass.c_str());
            } else {
                Serial.println("skipping unknown encrypted network");
            }
        } else {
            connected = do_connect(ssid, nullptr);
        }
    }
    delete[] used;

    if (connected) return true;

    // --- Modo AP + portal cautivo ---
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    server.begin();
    Serial.printf("Connect to WiFi ssid %s, default password: %s\n", AP_SSID, AP_PASSWORD);
    Serial.println("and access the ESP via your favorite web browser at 192.168.4.1.");

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            server.end();
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            return true;
        }
        WiFiClient client = server.available();
        if (!client) {
            delay(10);
            continue;
        }
        Serial.println("client connected");
        client.setTimeout(5);
        String request;
        unsigned long t0 = millis();
        while (millis() - t0 < 5000) {
            while (client.available()) {
                request += (char)client.read();
                t0 = millis();
            }
            if (request.indexOf("\r\n\r\n") >= 0) {
                // si es POST con body, seguir leyendo un poco más
                int cl = request.indexOf("Content-Length:");
                if (cl < 0) break;
                int bodyStart = request.indexOf("\r\n\r\n") + 4;
                int contentLen = request.substring(cl + 15, request.indexOf("\r\n", cl)).toInt();
                if (request.length() - bodyStart >= (size_t)contentLen) break;
            }
            delay(1);
        }

        if (request.indexOf("HTTP") < 0) { client.stop(); continue; }

        // Extraer URL: "(GET|POST) /url[?query] HTTP"
        int sp1 = request.indexOf(' ');
        int sp2 = request.indexOf(" HTTP", sp1);
        String url = request.substring(sp1 + 1, sp2);
        int q = url.indexOf('?');
        if (q >= 0) url = url.substring(0, q);
        if (url.startsWith("/")) url = url.substring(1);
        if (url.endsWith("/")) url.remove(url.length() - 1);
        Serial.println("URL is " + url);

        if (url == "") {
            handle_root(client);
        } else if (url == "configure") {
            if (handle_configure(client, request)) {
                // conectado: salir del portal
                server.end();
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                return true;
            }
        } else {
            send_response(client, "Path not found: " + url, 404);
        }
        client.stop();
    }
}
