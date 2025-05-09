/*
 * BOARD TTGO T7 V1.3  mini32
 */
#include <SD.h>                /*@3.2.0*/
//#include <FS.h>              /*@3.2.0*/
#include <driver/uart.h>
#include <AsyncTCP.h>          /* https://github.com/me-no-dev/AsyncTCP @1.1.4*/
#include <ESPAsyncWebServer.h> /* https://github.com/me-no-dev/ESPAsyncWebServer @2.9.4 by ESP32Async*/
#include <WebSocketsClient.h>  /* https://github.com/Links2004/arduinoWebSockets @2.6.1*/
#include <dsmr.h>              /* https://github.com/matthijskooijman/arduino-dsmr @0.1*/
#include <esp32-hal-log.h>
/*
Using library SD at version 3.2.0 in folder: ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/SD 
Using library FS at version 3.2.0 in folder: ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/FS 
Using library SPI at version 3.2.0 in folder: ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/SPI 
Using library Async TCP at version 3.3.2 in folder: ~/Downloads/projects/Arduino/libraries/Async_TCP 
Using library ESP Async WebServer at version 3.6.2 in folder: ~/Downloads/projects/Arduino/libraries/ESP_Async_WebServer 
Using library WiFi at version 3.2.0 in folder: ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/WiFi 
Using library Networking at version 3.2.0 in folder: ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/Network 
Using library WebSockets at version 2.6.1 in folder: ~/Downloads/projects/Arduino/libraries/WebSockets 
Using library NetworkClientSecure at version 3.2.0 in folder: ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/NetworkClientSecure 
Using library Dsmr at version 0.1 in folder: ~/Downloads/projects/Arduino/libraries/arduino-dsmr 
Using library ESP8266 and ESP32 OLED driver for SSD1306 displays at version 4.6.1 in folder: ~/Downloads/projects/Arduino/libraries/ESP8266_and_ESP32_OLED_driver_for_SSD1306_displays 
Using library Wire at version 3.2.0 in folder: ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/Wire 
*/
#include "setup.h"
#include "index_htm_gz.h"
#include "dagelijks_htm_gz.h"

#if defined(SH1106_OLED)
#include <SH1106.h> /* Install via 'Manage Libraries' in Arduino IDE -> https://github.com/ThingPulse/esp8266-oled-ssd1306 @4.6.1 */
#else
#include <SSD1306.h> /* In same library as SH1106 */
#endif

#define SAVE_TIME_MIN (1) /* data save interval in minutes */

const char* WS_RAW_URL = "/raw";
const char* WS_EVENTS_URL = "/events";

WebSocketsClient ws_bridge;
AsyncWebServer http_server(80);
AsyncWebSocket ws_server_raw(WS_RAW_URL);
AsyncWebSocket ws_server_events(WS_EVENTS_URL);
HardwareSerial smartMeter(UART_NR);

#if defined(SH1106_OLED)
SH1106 oled(OLED_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);
#else
SSD1306 oled(OLED_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);
#endif

time_t bootTime;
bool oledFound{false};
static uint8_t xOrigin;
static uint8_t yOrigin;
static uint8_t yOffset;
static char buffer[17];

const char* HEADER_MODIFIED_SINCE = "If-Modified-Since";

static inline __attribute__((always_inline)) bool htmlUnmodified(const AsyncWebServerRequest* request, const char* date) {
    return request->hasHeader(HEADER_MODIFIED_SINCE) && request->header(HEADER_MODIFIED_SINCE).equals(date);
}

void connectToWebSocketBridge() {
    ws_bridge.onEvent(ws_bridge_onEvents);
    ws_bridge.begin(WS_BRIDGE_HOST, WS_BRIDGE_PORT, WS_BRIDGE_URL);
}

const char* CACHE_CONTROL_HEADER{ "Cache-Control" };
const char* CACHE_CONTROL_NOCACHE{ "no-store, max-age=0" };

void updateFileHandlers(const tm& now) {
    static char path[100];
    snprintf(path, sizeof(path), "/%i/%i/%i.log", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);

    log_d("Current logfile: %s", path);

    static AsyncCallbackWebHandler* currentLogFileHandler;
    http_server.removeHandler(currentLogFileHandler);
    currentLogFileHandler = &http_server.on(path, HTTP_GET, [](AsyncWebServerRequest* const request) {
        if (!SD.exists(path)) return request->send(404);
        AsyncWebServerResponse* const response = request->beginResponse(SD, path, asyncsrv::empty);
        response->addHeader(CACHE_CONTROL_HEADER, CACHE_CONTROL_NOCACHE);
        request->send(response);
        log_d("Request for current logfile");
    });

    static AsyncStaticWebHandler* staticFilesHandler;
    http_server.removeHandler(staticFilesHandler);
    staticFilesHandler = &http_server.serveStatic("/", SD, "/").setCacheControl("public, max-age=604800, immutable");
}

void setup() {
    Serial.begin(115200);
    Serial.println("smartMeterLogger-esp32");

    if (!SD.begin())
        Serial.println("SD card mount failed");
    else
        Serial.println("SD card mounted");

    /* check if oled display is present */
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.beginTransmission(OLED_ADDRESS);
    const uint8_t error = Wire.endTransmission();
    if (error)
        Serial.println("no SSD1306/SH1106 oled found.");
    else {
        Serial.println("SSD1306/SH1106 oled found.");
        oledFound = true;
        oled.init();
        oled.flipScreenVertically();
        oled.setContrast(10, 5, 0);
        oled.setFont(ArialMT_Plain_10);
        /*
         * Note: You should know the Grove - OLED Display 0.66" (SSD1306) screen is based on the 128×64 resolution screen. 
         * When you want to display by U8g2 SSD 128*64 drive, you may need to start the point at (31,16) instead of (0,0). 
         * The range is from (31,16) to (95,63).
         */
        xOrigin=31;
        yOrigin=16;
        yOffset=16;
        oled.drawString(xOrigin, yOrigin, "Connecting..");
        oled.display();
        Serial.printf("oled.width %i\n", oled.width());
    }

    sprintf(buffer, "%s", STATIC_IP.toString().c_str());

    Serial.printf("try to set static IP %s\n", buffer);
    if (SET_STATIC_IP && !WiFi.config(STATIC_IP, GATEWAY, SUBNET, PRIMARY_DNS, SECONDARY_DNS))
        Serial.printf("Setting static IP %s failed\n", buffer);
    else
        Serial.printf("Setting static IP %s succeeded\n", buffer);

    Serial.printf("connecting to %s...\n", WIFI_NETWORK);
    WiFi.setHostname("SmartMeter");    
    WiFi.begin(WIFI_NETWORK, WIFI_PASSWORD);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    while (!WiFi.isConnected())
        delay(10);
    Serial.printf("connected to '%s' as %s\n", WIFI_NETWORK, WiFi.localIP().toString().c_str());

    if (oledFound) {
        oled.clear();
        oled.drawString(xOrigin, yOrigin, WiFi.localIP().toString());
        oled.drawString(xOrigin, yOrigin+yOffset, "Syncing NTP...");
        oled.display();
    }
    Serial.println("syncing NTP");

    /* sync the clock with ntp */
    configTzTime(TIMEZONE, NTP_POOL);

    tm now{};

    while (!getLocalTime(&now, 0))
        delay(10);

    if (oledFound) {
        oled.clear();
        oled.setFont(ArialMT_Plain_16);
        strftime(buffer,sizeof(buffer),"%y-%m-%d",&now);
        oled.drawString(xOrigin, yOrigin, buffer);
        strftime(buffer,sizeof(buffer),"%H:%M:%S",&now);
        oled.drawString(xOrigin, yOrigin+yOffset, buffer);
        oled.display();
    }

    /* websocket setup */
    ws_server_raw.onEvent(ws_server_onEvent);
    http_server.addHandler(&ws_server_raw);

    ws_server_events.onEvent(ws_server_onEvent);
    http_server.addHandler(&ws_server_events);


    /* webserver setup */
    time(&bootTime);

    static char modifiedDate[30];
    strftime(modifiedDate, sizeof(modifiedDate), "%a, %d %b %Y %X GMT", gmtime(&bootTime));

    static const char* HTML_MIMETYPE{ "text/html" };
    static const char* HEADER_LASTMODIFIED{ "Last-Modified" };
    static const char* CONTENT_ENCODING_HEADER{ "Content-Encoding" };
    static const char* CONTENT_ENCODING_GZIP{ "gzip" };

    http_server.on("/robots.txt", HTTP_GET, [](AsyncWebServerRequest* const request) {
        request->send(200, HTML_MIMETYPE, "User-agent: *\nDisallow: /\n");
    });

    http_server.on("/", HTTP_GET, [](AsyncWebServerRequest* const request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, HTML_MIMETYPE, index_htm_gz, index_htm_gz_len);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->addHeader(CONTENT_ENCODING_HEADER, CONTENT_ENCODING_GZIP);
        request->send(response);
    });

    http_server.on("/daggrafiek", HTTP_GET, [](AsyncWebServerRequest* const request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, HTML_MIMETYPE, dagelijks_htm_gz, dagelijks_htm_gz_len);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->addHeader(CONTENT_ENCODING_HEADER, CONTENT_ENCODING_GZIP);
        request->send(response);
    });

    http_server.on("/jaren", HTTP_GET, [](AsyncWebServerRequest* const request) {
        File root = SD.open("/");
        // TODO: check that the folders are at least plausibly named for a /year thing
        if (!root || !root.isDirectory()) return request->send(503);
        File item = root.openNextFile();
        if (!item) return request->send(404);
        AsyncResponseStream* const response = request->beginResponseStream(HTML_MIMETYPE);
        while (item) {
            if (item.isDirectory())
                response->printf("%s\n", item.name());
            item = root.openNextFile();
        }
        response->addHeader(CACHE_CONTROL_HEADER, CACHE_CONTROL_NOCACHE);
        request->send(response);
    });

    http_server.on("/maanden", HTTP_GET, [](AsyncWebServerRequest* const request) {
        const char* year{ "jaar" };
        if (!request->hasArg(year)) return request->send(400);
        char requestStr[request->arg(year).length() + 3];
        snprintf(requestStr, sizeof(requestStr), "/%s", request->arg(year).c_str());
        if (!SD.exists(requestStr)) return request->send(404);
        // TODO: check that the folders are at least plausibly named for a /year/month thing
        File path = SD.open(requestStr);
        if (!path || !path.isDirectory()) return request->send(503);
        File item = path.openNextFile();
        if (!item) return request->send(404);
        AsyncResponseStream* const response = request->beginResponseStream(HTML_MIMETYPE);
        while (item) {
            if (item.isDirectory())
                response->printf("%s\n", item.name());
            item = path.openNextFile();
        }
        response->addHeader(CACHE_CONTROL_HEADER, CACHE_CONTROL_NOCACHE);
        request->send(response);
    });

    http_server.on("/dagen", HTTP_GET, [](AsyncWebServerRequest* const request) {
        const char* month{ "maand" };
        if (!request->hasArg(month)) return request->send(400);
        if (!SD.exists(request->arg(month))) return request->send(404);
        // TODO: check that the file is at least plausibly named for a /year/month/day thing
        File path = SD.open(request->arg(month));
        if (!path || !path.isDirectory()) return request->send(503);
        File item = path.openNextFile();
        if (!item) return request->send(404);
        AsyncResponseStream* const response = request->beginResponseStream(HTML_MIMETYPE);
        while (item) {
            if (!item.isDirectory())
                response->printf("%s\n", item.name());
            item = path.openNextFile();
        }
        response->addHeader(CACHE_CONTROL_HEADER, CACHE_CONTROL_NOCACHE);
        request->send(response);
    });

    updateFileHandlers(now);

    http_server.onNotFound([](AsyncWebServerRequest* const request) {
        request->send(404);
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    http_server.begin();

    if (USE_WS_BRIDGE)
        connectToWebSocketBridge();
    else {
        smartMeter.begin(BAUDRATE, SERIAL_8N1, RXD_PIN);
        Serial.printf("listening for smartMeter RXD_PIN = %i baudrate = %i\n", RXD_PIN, BAUDRATE);
    }

    Serial.printf("saving average use every %i minutes\n", SAVE_TIME_MIN);
}

static int32_t average{ 0 };
static uint32_t numberOfSamples{ 0 };
struct {
    uint32_t low;
    uint32_t high;
    uint32_t rlow;
    uint32_t rhigh;
    uint32_t gas;
    uint32_t voltage;
} currentDTO;

void saveAverage(const tm& timeinfo) {
    String message(time(NULL));
    if (average >= 0){
      message += " " + String(average / numberOfSamples);
    } else {
      message += " -" + String((-average) / numberOfSamples);
    }
    ws_server_events.textAll("electric_saved\n" + message);
    String path{ '/' + String(timeinfo.tm_year + 1900) }; /* add the current year to the path */
    File folder = SD.open(path);
    if (!folder && !SD.mkdir(path)) {
        log_e("could not create folder %s", path);
    }
    path.concat("/" + String(timeinfo.tm_mon + 1)); /* add the current month to the path */
    folder = SD.open(path);
    if (!folder && !SD.mkdir(path)) {
        log_e("could not create folder %s", path);
    }
    path.concat("/" + String(timeinfo.tm_mday) + ".log"); /* add the filename to the path */
    static bool booted{ true };
    const String current { String(currentDTO.low) + " " + currentDTO.high + " " + currentDTO.gas + " " + currentDTO.rlow + " " + currentDTO.rhigh + " " + currentDTO.voltage};
    if (booted || !SD.exists(path)) {
        const String startHeader{ "#" + String(bootTime) + " " + current };
        log_d("writing start header '%s' to '%s'", startHeader.c_str(), path.c_str());
        appendToFile(path.c_str(), startHeader.c_str());
        booted = false;
    }
    log_d("%i samples - saving '%s' to file '%s'", numberOfSamples, message.c_str(), path.c_str());
    const String msg { message + " " + current };
    appendToFile(path.c_str(), msg.c_str());
    average = 0;
    numberOfSamples = 0;
}

static unsigned long lastMessageMs = millis();

void loop() {
    ws_server_raw.cleanupClients();
    ws_server_events.cleanupClients();

    static tm now;
    getLocalTime(&now);
    if ((59 == now.tm_sec) && !(now.tm_min % SAVE_TIME_MIN) && (numberOfSamples > 2))
        saveAverage(now);

    static uint8_t currentMonthDay = now.tm_mday;
    if (currentMonthDay != now.tm_mday) {
        updateFileHandlers(now);
        currentMonthDay = now.tm_mday;
    }

    if (USE_WS_BRIDGE) {
        ws_bridge.loop();
        static const auto TIMEOUT_MS = 8000;
        if (ws_bridge.isConnected() && millis() - lastMessageMs > TIMEOUT_MS) {
            log_w("WebSocket bridge has received no data for %.2f seconds - reconnecting...", TIMEOUT_MS / 1000.0);
            ws_bridge.disconnect();
            lastMessageMs = millis();
        }
    } else if (smartMeter.available()) {
        static const auto BUFFERSIZE = 1024;
        static char telegram[BUFFERSIZE];
        
        const unsigned long START_MS = millis();
        static const auto TIMEOUT_MS = 100;
        int size = 0;
        auto bytes = smartMeter.available();
        
        while (millis() - START_MS < TIMEOUT_MS && size + bytes < BUFFERSIZE) {
            size += bytes ? smartMeter.read(telegram + size, bytes) : 0;
            delay(5);
            bytes = smartMeter.available();
        }
        
        log_d("telegram received - %i bytes:\n%s", size, telegram);
        process(telegram, size);
    }
    delay(1);
}

char currentUseString[200];

void ws_server_onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {

        case WS_EVT_CONNECT:
            log_d("[%s][%u] connect", server->url(), client->id());
            if (0 == strcmp(WS_EVENTS_URL, server->url()))
                client->text(currentUseString);
            break;

        case WS_EVT_DISCONNECT:
            log_d("[%s][%u] disconnect", server->url(), client->id());
            break;

        case WS_EVT_ERROR:
            log_e("[%s][%u] error(%u): %s", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
            break;

        case WS_EVT_DATA:
            {
                AwsFrameInfo* info = (AwsFrameInfo*)arg;
                // here all data is contained in a single packet - and since we only listen for packets <= 1024 bytes we do not check for multi-packet or multi-frame telegrams
                if (info->final && info->index == 0 && info->len == len) {
                    if (info->opcode == WS_TEXT) {
                        data[len] = 0;
                        log_d("ws message from client %i: %s", client->id(), reinterpret_cast<char*>(data));
                    }
                }
            }
            break;

        default: log_e("unhandled ws event type");
    }
}

void ws_bridge_onEvents(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_CONNECTED:
            Serial.printf("connected to websocket bridge 'ws://%s:%i%s'\n", WS_BRIDGE_HOST, WS_BRIDGE_PORT, WS_BRIDGE_URL);
            lastMessageMs = millis();
            break;

        case WStype_DISCONNECTED:
            Serial.println("websocket bridge down - reconnecting");
            connectToWebSocketBridge();
            break;

        case WStype_TEXT:
            log_d("payload: %s", payload);
            process(reinterpret_cast<char*>(payload), length);
            lastMessageMs = millis();
            break;

        case WStype_ERROR:
            log_e("websocket bridge error");
            break;

        case WStype_PING:
            log_d("received ping");
            break;

        case WStype_PONG:
            log_d("received pong");
            break;

        default: log_e("unhandled websocket bridge event");
    }
}

bool appendToFile(const char* path, const char* message) {
    log_d("appending to file: %s", path);

    File file = SD.open(path, FILE_APPEND);
    if (!file) {
        log_d("failed to open %s for appending", path);
        return false;
    }
    if (!file.println(message)) {
        log_d("failed to write %s", path);
        return false;
    }

    file.close();
    return true;
}

void process(const char* telegram, const int size) {

    using decodedFields = ParsedData<
        /* FixedValue */ energy_delivered_tariff1,
        /* FixedValue */ energy_delivered_tariff2,
        /* FixedValue */ energy_returned_tariff1,
        /* FixedValue */ energy_returned_tariff2,
        /* String */ electricity_tariff,
        /* FixedValue */ power_delivered,
        /* FixedValue */ power_returned,
        /* TimestampedFixedValue */ gas_delivered,
        /* FixedValue */ voltage_l1>;
    decodedFields data;
    const ParseResult<void> res = P1Parser::parse(&data, telegram, size);
    /*
        if (res.err)
        log_e("Error decoding telegram\n%s", res.fullError(telegram, telegram + size);

        if (!data.all_present())
        log_e("Could not decode all fields");
    */
    if (res.err || !data.all_present())
        return;

    ws_server_raw.textAll(telegram);

    static struct {
        uint32_t t1Start;
        uint32_t t2Start;
        uint32_t r1Start;
        uint32_t r2Start;
        uint32_t gasStart;
    } today;

    currentDTO = { data.energy_delivered_tariff1.int_val(),
                data.energy_delivered_tariff2.int_val(),
                data.energy_returned_tariff1.int_val(),
                data.energy_returned_tariff2.int_val(),
                data.gas_delivered.int_val(),
                data.voltage_l1.int_val()};

    /* out of range value to make sure the next check updates the first time */
    static uint8_t currentMonthDay{ 40 };

    static struct tm timeinfo;
    getLocalTime(&timeinfo);

    /* check if we changed day and update starter values if so */
    if (currentMonthDay != timeinfo.tm_mday) {
        today.t1Start = data.energy_delivered_tariff1.int_val();
        today.t2Start = data.energy_delivered_tariff2.int_val();
        today.r1Start = data.energy_returned_tariff1.int_val();
        today.r2Start = data.energy_returned_tariff2.int_val();
        today.gasStart = data.gas_delivered.int_val();
        currentMonthDay = timeinfo.tm_mday;
    }

    average += data.power_delivered.int_val();
    average -= data.power_returned.int_val();
    numberOfSamples++;

    snprintf(currentUseString, sizeof(currentUseString), "current\n%i\n%i\n%i\n%i\n%i\n%i\n%i\n%s\n%i\n%i\n%i\n%i\n%i",
             data.power_delivered.int_val(),
             data.energy_delivered_tariff1.int_val(),
             data.energy_delivered_tariff2.int_val(),
             data.gas_delivered.int_val(),
             data.energy_delivered_tariff1.int_val() - today.t1Start,
             data.energy_delivered_tariff2.int_val() - today.t2Start,
             data.gas_delivered.int_val() - today.gasStart,
             (data.electricity_tariff.equals("0001")) ? "laag" : "hoog",
             data.energy_returned_tariff1.int_val(),
             data.energy_returned_tariff2.int_val(),
             data.energy_returned_tariff1.int_val() - today.r1Start,
             data.energy_returned_tariff2.int_val() - today.r2Start,
             data.power_returned.int_val()
             );

    ws_server_events.textAll(currentUseString);

    if (oledFound) {
        oled.clear();
        oled.setFont(ArialMT_Plain_16);
        if (timeinfo.tm_sec < 9) {
            oled.drawString(xOrigin, yOrigin, "in lo");
            oled.drawString(xOrigin, yOrigin+yOffset, String(data.energy_delivered_tariff1.int_val()));
        } else if (timeinfo.tm_sec < 18) {
            oled.drawString(xOrigin, yOrigin, "in hi");
            oled.drawString(xOrigin, yOrigin+yOffset, String(data.energy_delivered_tariff2.int_val()));
        } else if (timeinfo.tm_sec < 27) {
            oled.drawString(xOrigin, yOrigin, "out lo");
            oled.drawString(xOrigin, yOrigin+yOffset, String(data.energy_returned_tariff1.int_val()));
        } else if (timeinfo.tm_sec < 36) {
            oled.drawString(xOrigin, yOrigin, "out hi");
            oled.drawString(xOrigin, yOrigin+yOffset, String(data.energy_returned_tariff2.int_val()));
        } else if (timeinfo.tm_sec < 45) {
            oled.drawString(xOrigin, yOrigin, "gas");
            oled.drawString(xOrigin, yOrigin+yOffset, String(data.gas_delivered.int_val()));
        } else if (timeinfo.tm_sec < 54) {
          //46..54; 8 characters fit 192.168,xxx,yyy, The range is from (31,16) to (95,63). = 64, 1 ch = 8 x
            oled.drawString(xOrigin+8*(46-timeinfo.tm_sec), yOrigin, WiFi.localIP().toString());
            oled.drawString(xOrigin, yOrigin+yOffset, String(data.voltage_l1.int_val()) + "V");
        } else {
            strftime(buffer,sizeof(buffer),"%y-%m-%d",&timeinfo);
            oled.drawString(xOrigin, yOrigin, buffer);
            strftime(buffer,sizeof(buffer),"%H:%M:%S",&timeinfo);
            oled.drawString(xOrigin, yOrigin+yOffset, buffer);
        }
        uint32_t delivered = data.power_delivered.int_val();
        uint32_t returned = data.power_returned.int_val();
        oled.setFont(ArialMT_Plain_16);
        if (delivered >= returned) {
        oled.drawString(xOrigin, yOrigin+yOffset+yOffset, String(delivered - returned) + "W");
        } else {
        oled.drawString(xOrigin, yOrigin+yOffset+yOffset, "-" +String(returned - delivered) + "W");
        }
        oled.display();
    }
}
