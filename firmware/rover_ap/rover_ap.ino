/**
 * rover_ap.ino — Прошивка ровера XIAO ESP32S3 с WiFi Access Point
 *
 * ИЗМЕНЕНИЯ:
 * 1. Ровер создаёт собственную WiFi точку доступа (AP mode)
 * 2. Добавлен motor watchdog: если нет пакета >500ms → остановка моторов
 * 3. Исправлен маппинг STR: map(-100,100, 40,140) — согласовано с документацией
 * 4. Улучшена обработка телеметрии
 *
 * Сетевая архитектура:
 * - Ровер (этот модуль): AP mode, IP 192.168.4.1
 * - XIAO турель: подключается к AP, получает IP 192.168.4.2
 * - Телефон Android: подключается к AP
 *
 * Протокол UDP :4210 входящий:
 *   SPD:{};STR:{};FWD:{};LASER:{}\n
 *
 * Телеметрия UDP :4211 исходящий:
 *   {"bat":N,"yaw":F,"spd":N,"pit":F,"rol":F,"rssi":N,"rpmL":F,"rpmR":F}
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>
#include "driver/pcnt.h"

// ══════════════════════════════════════════════════════════════════════════
// WiFi Access Point Configuration
// ══════════════════════════════════════════════════════════════════════════

const char* AP_SSID = "RoverAP";
const char* AP_PASS = "rover12345";  // минимум 8 символов
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

// ══════════════════════════════════════════════════════════════════════════
// Pin Configuration
// ══════════════════════════════════════════════════════════════════════════

// Motor driver
#define MOTOR_IN1 6
#define MOTOR_IN2 7
#define PWM_LEFT  4
#define PWM_RIGHT 5

// Encoders (квадратурные)
#define ENC_L_A 12
#define ENC_L_B 11
#define ENC_R_A 38
#define ENC_R_B 37

// Servo & Laser
#define SERVO_STEER 13
#define LASER_PIN   17

// ══════════════════════════════════════════════════════════════════════════
// Constants
// ══════════════════════════════════════════════════════════════════════════

#define CMD_PORT      4210
#define TELEM_PORT    4211
#define MOTOR_WATCHDOG_MS  500   // ИСПРАВЛЕНИЕ: watchdog таймаут
#define TELEM_INTERVAL_MS  500
#define ENCODER_PPR        440   // пульсов на оборот
#define RPM_SAMPLE_MS      200

// ══════════════════════════════════════════════════════════════════════════
// Global Objects
// ══════════════════════════════════════════════════════════════════════════

WiFiUDP udp;
WiFiUDP udpTelem;
Servo steerServo;

// Состояние
volatile int16_t encCountL = 0;
volatile int16_t encCountR = 0;
float rpmL = 0.0f;
float rpmR = 0.0f;
int lastFwd = 0;
int lastStr = 0;
bool laserOn = false;

// Телеметрия
IPAddress remoteIP;
uint16_t remotePort = 0;
bool haveRemote = false;

// Timing
unsigned long lastCmdTime = 0;
unsigned long lastTelemTime = 0;
unsigned long lastRpmSampleTime = 0;
int16_t prevEncL = 0;
int16_t prevEncR = 0;

// ══════════════════════════════════════════════════════════════════════════
// PCNT Encoder Setup
// ══════════════════════════════════════════════════════════════════════════

void setupEncoder(pcnt_unit_t unit, int pinA, int pinB) {
    pcnt_config_t cfg = {};
    cfg.pulse_gpio_num = pinA;
    cfg.ctrl_gpio_num = pinB;
    cfg.channel = PCNT_CHANNEL_0;
    cfg.unit = unit;
    cfg.pos_mode = PCNT_COUNT_INC;
    cfg.neg_mode = PCNT_COUNT_DEC;
    cfg.lctrl_mode = PCNT_MODE_REVERSE;
    cfg.hctrl_mode = PCNT_MODE_KEEP;
    cfg.counter_h_lim = 32767;
    cfg.counter_l_lim = -32768;
    
    pcnt_unit_config(&cfg);
    pcnt_set_filter_value(unit, 100);
    pcnt_filter_enable(unit);
    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
    pcnt_counter_resume(unit);
}

void readEncoders() {
    int16_t countL, countR;
    pcnt_get_counter_value(PCNT_UNIT_0, &countL);
    pcnt_get_counter_value(PCNT_UNIT_1, &countR);
    encCountL = countL;
    encCountR = countR;
}

void updateRpm() {
    unsigned long now = millis();
    unsigned long dt = now - lastRpmSampleTime;
    
    if (dt >= RPM_SAMPLE_MS) {
        readEncoders();
        
        int16_t deltaL = encCountL - prevEncL;
        int16_t deltaR = encCountR - prevEncR;
        
        // RPM = (pulses / PPR) * (60000 / dt_ms)
        float factor = 60000.0f / (ENCODER_PPR * dt);
        rpmL = deltaL * factor;
        rpmR = deltaR * factor;
        
        prevEncL = encCountL;
        prevEncR = encCountR;
        lastRpmSampleTime = now;
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Motor Control
// ══════════════════════════════════════════════════════════════════════════

void setMotor(int fwd) {
    int pwmVal = abs(fwd) * 255 / 100;
    pwmVal = constrain(pwmVal, 0, 255);
    
    if (fwd > 0) {
        digitalWrite(MOTOR_IN1, HIGH);
        digitalWrite(MOTOR_IN2, LOW);
    } else if (fwd < 0) {
        digitalWrite(MOTOR_IN1, LOW);
        digitalWrite(MOTOR_IN2, HIGH);
    } else {
        digitalWrite(MOTOR_IN1, LOW);
        digitalWrite(MOTOR_IN2, LOW);
    }
    
    analogWrite(PWM_LEFT, pwmVal);
    analogWrite(PWM_RIGHT, pwmVal);
}

void stopMotors() {
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, LOW);
    analogWrite(PWM_LEFT, 0);
    analogWrite(PWM_RIGHT, 0);
    lastFwd = 0;
}

// ══════════════════════════════════════════════════════════════════════════
// Command Parsing
// ══════════════════════════════════════════════════════════════════════════

void parseCommand(const char* cmd) {
    int spd = 0, str = 0, fwd = 0, laser = 0;
    
    // Parse: SPD:{};STR:{};FWD:{};LASER:{}\n
    char* ptr = (char*)cmd;
    
    if (strstr(ptr, "SPD:")) {
        ptr = strstr(ptr, "SPD:") + 4;
        spd = atoi(ptr);
    }
    if (strstr(ptr, "STR:")) {
        ptr = strstr(ptr, "STR:") + 4;
        str = atoi(ptr);
    }
    if (strstr(ptr, "FWD:")) {
        ptr = strstr(ptr, "FWD:") + 4;
        fwd = atoi(ptr);
    }
    if (strstr(ptr, "LASER:")) {
        ptr = strstr(ptr, "LASER:") + 6;
        laser = atoi(ptr);
    }
    
    // Apply steering
    // ИСПРАВЛЕНИЕ: map(-100,100, 40,140) — согласовано с документацией
    str = constrain(str, -100, 100);
    int steerAngle = map(str, -100, 100, 40, 140);
    steerServo.write(steerAngle);
    lastStr = str;
    
    // Apply motor
    fwd = constrain(fwd, -100, 100);
    setMotor(fwd);
    lastFwd = fwd;
    
    // Apply laser
    laserOn = (laser == 1);
    digitalWrite(LASER_PIN, laserOn ? HIGH : LOW);
    
    // Update watchdog
    lastCmdTime = millis();
}

// ══════════════════════════════════════════════════════════════════════════
// Motor Watchdog
// ══════════════════════════════════════════════════════════════════════════

void checkWatchdog() {
    unsigned long now = millis();
    
    // ИСПРАВЛЕНИЕ: Если нет команд >500ms → аварийная остановка
    if (lastCmdTime > 0 && (now - lastCmdTime) > MOTOR_WATCHDOG_MS) {
        if (lastFwd != 0) {
            Serial.println("WATCHDOG: No command received, stopping motors!");
            stopMotors();
            laserOn = false;
            digitalWrite(LASER_PIN, LOW);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Telemetry
// ══════════════════════════════════════════════════════════════════════════

void sendTelemetry() {
    if (!haveRemote) return;
    
    unsigned long now = millis();
    if (now - lastTelemTime < TELEM_INTERVAL_MS) return;
    lastTelemTime = now;
    
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"bat\":100,\"yaw\":0.0,\"spd\":%d,\"pit\":0.0,\"rol\":0.0,"
        "\"rssi\":%d,\"rpmL\":%.1f,\"rpmR\":%.1f}",
        abs(lastFwd),
        WiFi.RSSI(),
        rpmL, rpmR
    );
    
    udpTelem.beginPacket(remoteIP, TELEM_PORT);
    udpTelem.write((const uint8_t*)buf, strlen(buf));
    udpTelem.endPacket();
}

// ══════════════════════════════════════════════════════════════════════════
// Setup
// ══════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== RoverCtrl ESP32S3 AP Mode ===");
    
    // GPIO setup
    pinMode(MOTOR_IN1, OUTPUT);
    pinMode(MOTOR_IN2, OUTPUT);
    pinMode(PWM_LEFT, OUTPUT);
    pinMode(PWM_RIGHT, OUTPUT);
    pinMode(LASER_PIN, OUTPUT);
    
    stopMotors();
    digitalWrite(LASER_PIN, LOW);
    
    // Servo
    ESP32PWM::allocateTimer(0);
    steerServo.setPeriodHertz(50);
    steerServo.attach(SERVO_STEER, 500, 2400);
    steerServo.write(90);  // center
    
    // Encoders
    setupEncoder(PCNT_UNIT_0, ENC_L_A, ENC_L_B);
    setupEncoder(PCNT_UNIT_1, ENC_R_A, ENC_R_B);
    
    // WiFi Access Point
    Serial.println("Starting WiFi AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASS);
    
    Serial.print("AP SSID: ");
    Serial.println(AP_SSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    
    // UDP
    udp.begin(CMD_PORT);
    udpTelem.begin(TELEM_PORT);
    
    Serial.printf("Listening on UDP port %d\n", CMD_PORT);
    Serial.printf("Telemetry on UDP port %d\n", TELEM_PORT);
    Serial.println("Ready!");
}

// ══════════════════════════════════════════════════════════════════════════
// Main Loop
// ══════════════════════════════════════════════════════════════════════════

void loop() {
    // Check for incoming commands
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        char buf[256];
        int len = udp.read(buf, sizeof(buf) - 1);
        buf[len] = '\0';
        
        // Save remote address for telemetry
        remoteIP = udp.remoteIP();
        remotePort = udp.remotePort();
        haveRemote = true;
        
        // Parse and execute command
        parseCommand(buf);
    }
    
    // Update RPM from encoders
    updateRpm();
    
    // ИСПРАВЛЕНИЕ: Motor watchdog
    checkWatchdog();
    
    // Send telemetry
    sendTelemetry();
    
    // Small delay to prevent busy-loop
    delay(5);
}

// ══════════════════════════════════════════════════════════════════════════
// Debug Commands (Serial)
// ══════════════════════════════════════════════════════════════════════════

void serialEvent() {
    while (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        if (cmd == "status") {
            Serial.printf("Clients connected: %d\n", WiFi.softAPgetStationNum());
            Serial.printf("RPM L=%.1f R=%.1f\n", rpmL, rpmR);
            Serial.printf("FWD=%d STR=%d LASER=%d\n", lastFwd, lastStr, laserOn);
            Serial.printf("Watchdog: %lu ms since last cmd\n", 
                lastCmdTime > 0 ? millis() - lastCmdTime : 0);
        }
        else if (cmd == "stop") {
            stopMotors();
            Serial.println("Motors stopped");
        }
        else if (cmd == "help") {
            Serial.println("Commands: status, stop, help");
        }
    }
}
