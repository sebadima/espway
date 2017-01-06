#include <Arduino.h>
#include <Hash.h>
#include <Wire.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <I2Cdev.h>
#include <MPU6050.h>
#include <ESPAsyncWebServer.h>
#include <MadgwickAHRS_fix.h>
#include <NeoPixelBus.h>


enum mode { LOG_FREQ, LOG_RAW, LOG_GRAVXY, LOG_NONE };

const float BETA = 0.05f;
const int MPU_RATE = 0;
const mode MYMODE = LOG_NONE;
const int N_SAMPLES = 1000;
const int QUAT_DELAY = 100;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

MPU6050 mpu;
int16_t ax, ay, az, gx, gy, gz;
quaternion_fix quat = { Q16_MULTIPLIER, 0, 0, 0 };
q16 beta, gyroIntegrationFactor;

unsigned long lastSentQuat = 0;
bool sendQuat = false;
AsyncWebSocketClient *wsclient = NULL;
unsigned long lastTime = 0;
int sampleCounter = 0;

NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> eyes(2);
RgbColor black(0);
RgbColor red(180, 0, 0);
RgbColor yellow(180, 180, 0);


void wsCallback(AsyncWebSocket * server, AsyncWebSocketClient * client,
    AwsEventType type, void * arg, uint8_t *data, size_t len) {
    // Received data frame from websocket, mark that we should send back the quaternion

    sendQuat = true;
    wsclient = client;
}


void calculateIMUCoeffs() {
    float sampleTime = (1.0f + MPU_RATE) / 1000.0f;
    float gyroScale = 2.0f * M_PI / 180.0f * 2000.0f;
    gyroIntegrationFactor = float_to_q16(0.5f * gyroScale * sampleTime);
    beta = float_to_q16(BETA / (0.5f * gyroScale));
}


volatile bool intFlag = false;
void mpuInterrupt() {
    intFlag = true;
}


void setup() {
    Serial.begin(115200);

    // NeoPixel eyes initialization
    eyes.Begin();
    eyes.SetPixelColor(0, red);
    eyes.SetPixelColor(1, yellow);
    eyes.Show();

    // I2C initialization
    Wire.begin(0, 5);
    Wire.setClock(400000);
    // MPU6050 initialization
    calculateIMUCoeffs();
    mpu.initialize();
    mpu.setRate(MPU_RATE);
    mpu.setDLPFMode(MPU6050_DLPF_BW_188);
    mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
    mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_2000);
    mpu.setIntDataReadyEnabled(true);
    mpu.setInterruptMode(MPU6050_INTMODE_ACTIVELOW);
    mpu.setInterruptDrive(MPU6050_INTDRV_PUSHPULL);
    mpu.setIntEnabled(true);
    attachInterrupt(4, mpuInterrupt, RISING);

    // WiFi soft AP init
    WiFi.softAP("ESPway");
    // ArduinoOTA init
    ArduinoOTA.begin();

    SPIFFS.begin();

    ws.onEvent(wsCallback);
    server.addHandler(&ws);

    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
    server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404);
    });
    server.begin();
}


void loop() {
    if (!intFlag) {
        ArduinoOTA.handle();
        return;
    }

    // Perform MPU quaternion update
    intFlag = false;
    mpu.getIntStatus();
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
    MadgwickAHRSupdateIMU_fix(beta, gyroIntegrationFactor,
        ax, ay, az, gx, gy, gz, &quat);

    if (MYMODE == LOG_RAW) {
        Serial.print(ax); Serial.print(",");
        Serial.print(ay); Serial.print(",");
        Serial.print(az); Serial.print(",");
        Serial.print(gx); Serial.print(",");
        Serial.print(gy); Serial.print(",");
        Serial.println(gz);
    } else if (MYMODE == LOG_GRAVXY) {
    	q16 half_gravx = q16_mul(quat.q1, quat.q3) - q16_mul(quat.q0, quat.q2);
    	q16 half_gravy = q16_mul(quat.q0, quat.q1) + q16_mul(quat.q2, quat.q3);
        Serial.printf("%d, %d\n", half_gravx, half_gravy);
    } else if (MYMODE == LOG_FREQ) {
        unsigned long ms = millis();
        if (++sampleCounter == N_SAMPLES) {
            Serial.println(N_SAMPLES * 1000 / (ms - lastTime));
            sampleCounter = 0;
            lastTime = ms;
        }
    }

    unsigned long ms = millis();
    if (sendQuat && ms - lastSentQuat > QUAT_DELAY) {
        int16_t qdata[4];
        qdata[0] = quat.q0 / 2;
        qdata[1] = quat.q1 / 2;
        qdata[2] = quat.q2 / 2;
        qdata[3] = quat.q3 / 2;
        wsclient->binary((uint8_t *)qdata, 8);
        sendQuat = false;
        lastSentQuat = ms;
    }
}
