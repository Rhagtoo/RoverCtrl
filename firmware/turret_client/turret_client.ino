/**
 * turret_client.ino — Прошивка турели XIAO ESP32S3 Sense
 *
 * ИЗМЕНЕНИЯ:
 * 1. Турель подключается к WiFi AP ровера (Station mode)
 * 2. Фиксированный IP 192.168.4.2 через WiFi.config()
 * 3. Автореконнект при потере связи
 * 4. MJPEG стрим с камеры OV2640
 *
 * Сетевая архитектура:
 * - Ровер: AP mode, IP 192.168.4.1
 * - Турель (этот модуль): Station mode, IP 192.168.4.2
 * - Телефон: подключается к AP ровера
 *
 * Протокол UDP :4210 входящий:
 *   PAN:{};TILT:{}\n
 *
 * HTTP :81 исходящий:
 *   /stream — MJPEG стрим
 *   /capture — одиночный JPEG
 *   /status — JSON статус
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ══════════════════════════════════════════════════════════════════════════
// WiFi Configuration — подключение к AP ровера
// ══════════════════════════════════════════════════════════════════════════

const char* AP_SSID = "RoverAP";
const char* AP_PASS = "rover12345";

// Статический IP для турели
const IPAddress STATIC_IP(192, 168, 4, 2);
const IPAddress GATEWAY(192, 168, 4, 1);
const IPAddress SUBNET(255, 255, 255, 0);

// ══════════════════════════════════════════════════════════════════════════
// Pin Configuration — XIAO ESP32S3 Sense
// ══════════════════════════════════════════════════════════════════════════

// Servo pins
#define SERVO_PAN   4   // D3 (GPIO4)
#define SERVO_TILT  2   // D2 (GPIO2)

// Camera pins (XIAO ESP32S3 Sense built-in)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// ══════════════════════════════════════════════════════════════════════════
// Constants
// ══════════════════════════════════════════════════════════════════════════

#define UDP_PORT      4210
#define HTTP_PORT     81
#define RECONNECT_INTERVAL_MS  5000

// ══════════════════════════════════════════════════════════════════════════
// Global Objects
// ══════════════════════════════════════════════════════════════════════════

WiFiUDP udp;
Servo servoPan;
Servo servoTilt;
httpd_handle_t httpd = NULL;

// Состояние серво
int currentPan = 90;
int currentTilt = 90;
volatile bool panDirty = false;
volatile bool tiltDirty = false;
volatile int targetPan = 90;
volatile int targetTilt = 90;

// WiFi состояние
bool wifiConnected = false;
unsigned long lastReconnectAttempt = 0;

// ══════════════════════════════════════════════════════════════════════════
// Camera Setup
// ══════════════════════════════════════════════════════════════════════════

bool setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;  // 320x240
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }
    
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 0);
        s->set_gain_ctrl(s, 1);
        s->set_agc_gain(s, 0);
    }
    
    return true;
}

// ══════════════════════════════════════════════════════════════════════════
// HTTP Handlers
// ══════════════════════════════════════════════════════════════════════════

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t stream_handler(httpd_req_t* req) {
    esp_err_t res = ESP_OK;
    char partBuf[64];
    
    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    while (true) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        
        size_t hdrLen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, fb->len);
        
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, partBuf, hdrLen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        }
        
        esp_camera_fb_return(fb);
        
        if (res != ESP_OK) break;
        
        delay(30);  // ~30fps max
    }
    
    return res;
}

esp_err_t capture_handler(httpd_req_t* req) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, (const char*)fb->buf, fb->len);
    
    esp_camera_fb_return(fb);
    return ESP_OK;
}

esp_err_t status_handler(httpd_req_t* req) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"pan\":%d,\"tilt\":%d,\"heap\":%d,\"rssi\":%d}",
        currentPan, currentTilt, 
        ESP.getFreeHeap(),
        WiFi.RSSI()
    );
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

void startHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    
    if (httpd_start(&httpd, &config) == ESP_OK) {
        httpd_uri_t streamUri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(httpd, &streamUri);
        
        httpd_uri_t captureUri = {
            .uri = "/capture",
            .method = HTTP_GET,
            .handler = capture_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(httpd, &captureUri);
        
        httpd_uri_t statusUri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(httpd, &statusUri);
        
        Serial.printf("HTTP server started on port %d\n", HTTP_PORT);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Command Parsing
// ══════════════════════════════════════════════════════════════════════════

void parseCommand(const char* cmd) {
    int pan = 0, tilt = 0;
    bool hasPan = false, hasTilt = false;
    
    // Parse: PAN:{};TILT:{}\n
    char* ptr;
    
    if ((ptr = strstr(cmd, "PAN:")) != NULL) {
        pan = atoi(ptr + 4);
        hasPan = true;
    }
    if ((ptr = strstr(cmd, "TILT:")) != NULL) {
        tilt = atoi(ptr + 5);
        hasTilt = true;
    }
    
    if (hasPan) {
        // PAN: map(-90,90, 180,0) — ИНВЕРТИРОВАН
        pan = constrain(pan, -90, 90);
        int angle = map(pan, -90, 90, 180, 0);
        targetPan = angle;
        panDirty = true;
    }
    
    if (hasTilt) {
        // TILT: map(-90,90, 0,180) — прямой
        tilt = constrain(tilt, -90, 90);
        int angle = map(tilt, -90, 90, 0, 180);
        targetTilt = angle;
        tiltDirty = true;
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Servo Update (вызывается из loop на Core 1)
// ══════════════════════════════════════════════════════════════════════════

void updateServos() {
    if (panDirty) {
        servoPan.write(targetPan);
        currentPan = targetPan;
        panDirty = false;
    }
    if (tiltDirty) {
        servoTilt.write(targetTilt);
        currentTilt = targetTilt;
        tiltDirty = false;
    }
}

// ══════════════════════════════════════════════════════════════════════════
// WiFi Connection
// ══════════════════════════════════════════════════════════════════════════

void connectWiFi() {
    Serial.printf("Connecting to %s...\n", AP_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.config(STATIC_IP, GATEWAY, SUBNET);
    WiFi.begin(AP_SSID, AP_PASS);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println();
        Serial.print("Connected! IP: ");
        Serial.println(WiFi.localIP());
    } else {
        wifiConnected = false;
        Serial.println();
        Serial.println("Connection failed!");
    }
}

void checkWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiConnected) {
            Serial.println("WiFi disconnected!");
            wifiConnected = false;
        }
        
        unsigned long now = millis();
        if (now - lastReconnectAttempt > RECONNECT_INTERVAL_MS) {
            lastReconnectAttempt = now;
            Serial.println("Attempting reconnect...");
            WiFi.reconnect();
        }
    } else if (!wifiConnected) {
        wifiConnected = true;
        Serial.print("Reconnected! IP: ");
        Serial.println(WiFi.localIP());
    }
}

// ══════════════════════════════════════════════════════════════════════════
// UDP Task (Core 0)
// ══════════════════════════════════════════════════════════════════════════

void udpTask(void* param) {
    while (true) {
        if (wifiConnected) {
            int packetSize = udp.parsePacket();
            if (packetSize > 0) {
                char buf[128];
                int len = udp.read(buf, sizeof(buf) - 1);
                buf[len] = '\0';
                parseCommand(buf);
            }
        }
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Setup
// ══════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Turret XIAO ESP32S3 Sense ===");
    
    // Servo setup
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    
    servoPan.setPeriodHertz(50);
    servoPan.attach(SERVO_PAN, 500, 2400);
    servoPan.write(90);
    
    servoTilt.setPeriodHertz(50);
    servoTilt.attach(SERVO_TILT, 500, 2400);
    servoTilt.write(90);
    
    Serial.println("Servos initialized");
    
    // Camera setup
    if (!setupCamera()) {
        Serial.println("Camera setup failed!");
    } else {
        Serial.println("Camera initialized");
    }
    
    // WiFi connection
    connectWiFi();
    
    // UDP
    udp.begin(UDP_PORT);
    Serial.printf("UDP listening on port %d\n", UDP_PORT);
    
    // HTTP server
    startHttpServer();
    
    // UDP task on Core 0
    xTaskCreatePinnedToCore(
        udpTask,
        "udp_task",
        4096,
        NULL,
        1,
        NULL,
        0
    );
    
    Serial.println("Ready!");
}

// ══════════════════════════════════════════════════════════════════════════
// Main Loop (Core 1)
// ══════════════════════════════════════════════════════════════════════════

void loop() {
    // Check WiFi and reconnect if needed
    checkWiFi();
    
    // Update servos
    updateServos();
    
    delay(10);
}
