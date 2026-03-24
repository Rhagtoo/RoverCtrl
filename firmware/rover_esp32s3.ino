// XIAO_ESP32S3_Motors_Laser_Servo_NEW_PCNT.ino

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>
#include "driver/pulse_cnt.h"

#define DEBUG_RX false
#define DEBUG_RPM true
#define DEBUG_ACK false

// ───────────────── WIFI ─────────────────
const char* ssid = "Rhanet";
const char* password = "Der1parol";

WiFiUDP udp;
const unsigned int udpPort = 4210;
IPAddress remoteIP; 
uint16_t remotePort = 4211;
bool haveRemote = true;

char packetBuffer[512];

// ───────────────── МОТОРЫ ─────────────────
#define IN1 6
#define IN2 7
#define PWM_L 4
#define PWM_R 5

const uint32_t PWM_FREQ = 25000;
const uint8_t PWM_RES = 10;
const uint32_t PWM_MAX = (1 << PWM_RES) - 1;

// ───────────────── ЭНКОДЕРЫ ─────────────────

#define ENC_L_A 35
#define ENC_L_B 36
#define ENC_R_A 38
#define ENC_R_B 37

#define PULSES_PER_REV 440

pcnt_unit_handle_t unitL = NULL;
pcnt_unit_handle_t unitR = NULL;

volatile float rpmL = 0;
volatile float rpmR = 0;

unsigned long lastTime = 0;
const int SAMPLE_TIME = 200;

// ───────────────── СЕРВО ─────────────────

Servo servoSteer;
Servo servoPan;
Servo servoTilt;

#define SERVO_STEER_PIN 13
#define SERVO_PAN_PIN 16
#define SERVO_TILT_PIN 11

#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500

int steerTarget = 90;
int panTarget = 90;
int tiltTarget = 90;

int steerNow = 90;
int panNow = 90;
int tiltNow = 90;

const uint32_t SERVO_UPDATE_INTERVAL = 12;
uint32_t lastServoUpdate = 0;

// ───────────────── ЛАЗЕР ─────────────────

#define LASER_PIN 17
bool laserEnabled = false;

// ───────────────── TELEMETRY ─────────────────

#define TELEM_PORT 4211
#define TELEM_PERIOD_MS 500

int lastAbsSpd = 0;

// ───────────────── МОТОРЫ ─────────────────

void setupMotors() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);

  ledcAttach(PWM_L, PWM_FREQ, PWM_RES);
  ledcAttach(PWM_R, PWM_FREQ, PWM_RES);

  ledcWrite(PWM_L, 0);
  ledcWrite(PWM_R, 0);
}

void setMotors(int left, int right, bool forward) {
  digitalWrite(IN1, forward);
  digitalWrite(IN2, !forward);

  ledcWrite(PWM_L, left);
  ledcWrite(PWM_R, right);
}

void stopMotors() {
  ledcWrite(PWM_L, 0);
  ledcWrite(PWM_R, 0);
}

// ───────────────── PCNT NEW API ─────────────────

pcnt_unit_handle_t createEncoderUnit(int pinA, int pinB) {
  pcnt_unit_config_t unit_config = {
    .low_limit = -32768,
    .high_limit = 32767,
  };

  pcnt_unit_handle_t unit;
  pcnt_new_unit(&unit_config, &unit);

  pcnt_chan_config_t chanA_config = {
    .edge_gpio_num = pinA,
    .level_gpio_num = pinB,
  };

  pcnt_channel_handle_t chanA;
  pcnt_new_channel(unit, &chanA_config, &chanA);

  pcnt_channel_set_edge_action(
    chanA,
    PCNT_CHANNEL_EDGE_ACTION_INCREASE,
    PCNT_CHANNEL_EDGE_ACTION_DECREASE);

  pcnt_channel_set_level_action(
    chanA,
    PCNT_CHANNEL_LEVEL_ACTION_KEEP,
    PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

  pcnt_chan_config_t chanB_config = {
    .edge_gpio_num = pinB,
    .level_gpio_num = pinA,
  };

  pcnt_channel_handle_t chanB;
  pcnt_new_channel(unit, &chanB_config, &chanB);

  pcnt_channel_set_edge_action(
    chanB,
    PCNT_CHANNEL_EDGE_ACTION_DECREASE,
    PCNT_CHANNEL_EDGE_ACTION_INCREASE);

  pcnt_channel_set_level_action(
    chanB,
    PCNT_CHANNEL_LEVEL_ACTION_KEEP,
    PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

  pcnt_unit_enable(unit);
  pcnt_unit_clear_count(unit);
  pcnt_unit_start(unit);

  return unit;
}

// ───────────────── СЕРВО ─────────────────

bool initServos() {
  servoSteer.setPeriodHertz(50);
  servoPan.setPeriodHertz(50);
  servoTilt.setPeriodHertz(50);

  if (!servoSteer.attach(SERVO_STEER_PIN, SERVO_MIN_US, SERVO_MAX_US)) return false;
  if (!servoPan.attach(SERVO_PAN_PIN, SERVO_MIN_US, SERVO_MAX_US)) return false;
  if (!servoTilt.attach(SERVO_TILT_PIN, SERVO_MIN_US, SERVO_MAX_US)) return false;

  servoSteer.write(90);
  servoPan.write(90);
  servoTilt.write(90);

  return true;
}

void updateServos() {
  if (millis() - lastServoUpdate < SERVO_UPDATE_INTERVAL) return;

  lastServoUpdate = millis();

  const int maxStep = 3;

  int d;

  d = constrain(steerTarget - steerNow, -maxStep, maxStep);
  steerNow += d;
  servoSteer.write(steerNow);

  d = constrain(panTarget - panNow, -maxStep, maxStep);
  panNow += d;
  servoPan.write(panNow);

  d = constrain(tiltTarget - tiltNow, -maxStep, maxStep);
  tiltNow += d;
  servoTilt.write(tiltNow);
}

// ───────────────── TELEMETRY ─────────────────

void sendTelemetry() {
  if (!haveRemote) return;

  char buf[160];

  snprintf(buf, sizeof(buf),
           "{\"bat\":100,\"yaw\":0.0,\"spd\":%d,\"pit\":0.0,\"rol\":0.0,"
           "\"rssi\":%d,\"rpmL\":%.1f,\"rpmR\":%.1f}",
           lastAbsSpd,
           WiFi.RSSI(),
           rpmL,
           rpmR);

  udp.beginPacket(remoteIP, TELEM_PORT);
  udp.write((uint8_t*)buf, strlen(buf));
  udp.endPacket();
}

// ───────────────── SETUP ─────────────────

void setup() {
  Serial.begin(115200);

  setupMotors();

  pinMode(LASER_PIN, OUTPUT);

  pinMode(ENC_L_A, INPUT_PULLUP);
  pinMode(ENC_L_B, INPUT_PULLUP);
  pinMode(ENC_R_A, INPUT_PULLUP);
  pinMode(ENC_R_B, INPUT_PULLUP);

  unitL = createEncoderUnit(ENC_L_A, ENC_L_B);
  unitR = createEncoderUnit(ENC_R_A, ENC_R_B);

  lastTime = millis();

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println(WiFi.localIP());

  udp.begin(udpPort);

  initServos();
}

// ───────────────── LOOP ─────────────────

void loop() {
  int packetSize = udp.parsePacket();

  if (packetSize) {
    remoteIP = udp.remoteIP();
    remotePort = udp.remotePort();
    haveRemote = true;

    int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
    packetBuffer[len] = 0;

    int spd = 0, str = 0, fwd = 0, pan = 0, tilt = 0, laser = 0;

    char* p;

    if ((p = strstr(packetBuffer, "FWD:"))) fwd = atoi(p + 4);
    if ((p = strstr(packetBuffer, "STR:"))) str = atoi(p + 4);
    if ((p = strstr(packetBuffer, "PAN:"))) pan = atoi(p + 4);
    if ((p = strstr(packetBuffer, "TILT:"))) tilt = atoi(p + 5);
    if ((p = strstr(packetBuffer, "LASER:"))) laser = atoi(p + 6);

    bool forward = (fwd >= 0);
    int absSpd = constrain(abs(fwd), 0, 100);

    int duty = map(absSpd, 0, 100, 0, PWM_MAX);

    lastAbsSpd = absSpd;

    if (absSpd == 0) stopMotors();
    else setMotors(duty, duty, forward);

    steerTarget = map(str, -100, 100, 40, 140);
    panTarget = map(pan, -100, 100, 45, 135);
    tiltTarget = map(tilt, -100, 100, 20, 160);

    laserEnabled = (laser == 1);
    digitalWrite(LASER_PIN, laserEnabled);
  }

  unsigned long now = millis();

  if (now - lastTime >= SAMPLE_TIME) {
    int countL = 0;
    int countR = 0;

    pcnt_unit_get_count(unitL, &countL);
    pcnt_unit_get_count(unitR, &countR);

    pcnt_unit_clear_count(unitL);
    pcnt_unit_clear_count(unitR);

    float dt = (now - lastTime) / 1000.0;

    lastTime = now;

    rpmL = (countL / (float)PULSES_PER_REV) / dt * 60.0;
    rpmR = (countR / (float)PULSES_PER_REV) / dt * 60.0;

    if (abs(rpmL) < 1) rpmL = 0;
    if (abs(rpmR) < 1) rpmR = 0;

    if (DEBUG_RPM) {
      Serial.println(countL, countR);
    }
  }

  updateServos();

  static unsigned long lastTelem = 0;

  if (millis() - lastTelem >= TELEM_PERIOD_MS) {
    lastTelem = millis();
    sendTelemetry();
  }

  delay(2);
}