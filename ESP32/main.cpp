#include <PubSubClient.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <HardwareSerial.h>
#include <Adafruit_BMP085.h>
#define btn2 32
#define wifiSSID "---------"
#define wifiPassword "---------"
#define soilRHpin 39
#define OTAName "---------"
#define OTAPassword "---------"

#define MQTTServer "mqttgo.io"  //MQTT伺服器(使用台灣的免費伺服器，無加密功能)
#define MQTTPort 1883           //MQTT Port
// #define MQTTUser ""
// #define MQTTPassword "";
#define clintID "esp32-731444467145"


unsigned int rh = 0, pressure = 0;                //使用unsinged，避免溫度有負值
float temp = 0, tempB = 0;                        //溫度（PMS5003T、BMP180）
long pm1 = 0, pm25 = 0, pm10 = 0;                 //讀取出的PM1、PM2.5、PM10數值
float soilH = 0, oringalSoilH = 0;                //土壤濕度
int sel = 4;                                      //螢幕顯示的頁數
int updating = 0, otaProgress = 0, otaTotal = 0;  //OTA資訊，第一個為是否正在更新，後面兩個為計算百分比所需資料

String url = "https://api.thingspeak.com/update?api_key=", apiKey = "2QRDIJ54X2RUWC2P";
WiFiUDP ntpUDP;
HardwareSerial pms(2);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_BMP085 bmp;
WiFiClient WifiClient;                // 建立 WiFiClient 物件
PubSubClient MQTTClient(WifiClient);  // 基於 WiFiClient 物件，建立 MQTTClient 物件


//url可依照API Key不同自行修改
//PMS5003T使用了ESP32內建的UART，使用串口2
//SSD1306使用I2C連接，此處需定義螢幕長度、寬度（若無reset針腳則在最後填入-1）

TaskHandle_t uploadData;
TimerHandle_t timer;

void setup() {
  Serial.begin(115200);
  xTaskCreate(TaskReadPMS, "Read PMS Sensor", 1500, NULL, 1, NULL);
  xTaskCreate(TaskReadSoilrh, "Read Soil Sensor", 1200, NULL, 2, NULL);
  xTaskCreate(TaskUploadData, "Upload Data", 3500, NULL, 1, &uploadData);
  xTaskCreate(TaskWiFi, "WiFi connect", 3000, NULL, 1, NULL);
  xTaskCreate(TaskReadBMP, "Read BMP180", 3000, NULL, 2, NULL);

  xTaskCreatePinnedToCore(TaskOTA, "OTA", 2000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskDisp, "Screen", 3000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskButton, "Button", 2000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskMQTT, "MQTT Service", 4000, NULL, 1, NULL, 0);

  timer = xTimerCreate("WiFi connection timer", (180000 / portTICK_PERIOD_MS), pdFALSE, (void *)1, wifiRestart);
}
void loop() {
}

void TaskWiFi(void *pvParam) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPassword);
  while (1) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskSuspend(uploadData);
      xTimerStart(timer, 0);
      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
    }
    xTimerStop(timer, 0);
    vTaskResume(uploadData);
    vTaskDelay(20000 / portTICK_PERIOD_MS);
  }
  //WiFi連線任務，若超過三分鐘尚未連線則重新啟動連線，連接到WiFi前會先暫停資料上傳的任務加速連線速度
}
void TaskReadPMS(void *pvParam) {
  pms.begin(9600, SERIAL_8N1, 16, 17);
  while (1) {
    int count = 0;
    unsigned char c;
    unsigned char high;
    while (pms.available()) {
      c = pms.read();
      if ((count == 0 && c != 0x42) || (count == 1 && c != 0x4d)) {
        //Serial.println("check failed");
        break;
      }
      if (count > 27) {
        //Serial.println("Done!!");
        break;
      } else if (count == 10 || count == 12 || count == 14 || count == 24 || count == 26) {
        high = c;
      } else if (count == 11) {
        pm1 = 256 * high + c;
      } else if (count == 13) {
        pm25 = 256 * high + c;
      } else if (count == 15) {
        pm10 = 256 * high + c;
      } else if (count == 25) {
        temp = (256.0 * high + c) / 10.0;
        temp += 3;  //溫度測得約有3度偏差
        //使用BMP180的溫度較準確
      } else if (count == 27) {
        rh = (256 * high + c) / 10;
      }
      count++;
    }
    while (pms.available()) {
      pms.read();
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  //PMS5003T使用串口通訊，此專案使用串口2，在最上方使用HardwareSerial定義
  //接著依照Datasheet的資料順序解讀出所需資料並存入上傷宣告的全域變數
}
void TaskReadSoilrh(void *pvParam) {
  pinMode(soilRHpin, INPUT);  //土壤濕度感測器IO39
  while (1) {
    soilH = analogRead(soilRHpin);
    oringalSoilH = soilH;
    soilH = ((4095 - soilH) / 2495) * 100;
    if (soilH > 100) {
      soilH = 100;
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  //使用類比讀取土壤感應器濕度資料，經轉換後輸出為百分比，0為乾燥，100為潮濕
}

void TaskReadBMP(void *param) {
  bmp.begin();
  while (1) {
    tempB = bmp.readTemperature();
    pressure = bmp.readPressure();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  //BMP180大氣壓力與溫度感應器，每秒讀取一次
}
void TaskUploadData(void *pvParam) {
  HTTPClient http;
  while (1) {
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    if (temp != 0 || rh != 0) {
      String tmpurl = (url + apiKey) + "&field1=" + String(tempB, 1) + "&field2=" + String(rh) + "&field3=" + String(int(soilH)) + "&field4=" + String(pm1) + "&field5=" + String(pm25) + "&field6=" + String(pm10) + "&field7=" + String(pressure);
      http.begin(tmpurl);
      http.GET();
      http.end();
    }
    vTaskDelay(13000 / portTICK_PERIOD_MS);
  }
  //上傳資料至Thingspeak，可依照Field不同調整tmpurl中的順序，時間可在最後vTaskDelay調整
  //最上方預留2秒給其他感應器讀取數據，同時確保有讀取到資料才上傳，若溫度與濕度為0則不上傳（尚未讀取到資料）
  //！因Thingspeak限制資料傳輸間隔最少需要15秒，因此下方至少需13000(加上方2000)才可達到15秒間隔！
}

void TaskMQTT(void *pvParam) {
  MQTTClient.setServer(MQTTServer, MQTTPort);
  while (1) {
    if (!MQTTClient.connected()) {
      while (!MQTTClient.connected()) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
    } else {
      MQTTClient.publish("---------/esp32/temp", String(tempB, 1).c_str());
      MQTTClient.publish("---------/esp32/rh", String(rh).c_str());
      MQTTClient.publish("---------/esp32/soilH", String(soilH).c_str());
      MQTTClient.publish("---------/esp32/pm1", String(pm1).c_str());
      MQTTClient.publish("---------/esp32/pm25", String(pm25).c_str());
      MQTTClient.publish("---------/esp32/pm10", String(pm10).c_str());
      MQTTClient.publish("---------/esp32/pressure", String(pressure).c_str());
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

void TaskOTA(void *pvParam) {
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);
  ArduinoOTA.onStart(onStart);
  ArduinoOTA.onProgress(onProgress);
  ArduinoOTA.begin();
  while (1) {
    ArduinoOTA.handle();
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
  //OTA任務，vTaskDelay設定最少需1否則會引起WatchDog重啟
  //可自訂名稱與密碼，onStart與onProgress用於讓螢幕顯示OTA進度
}
void TaskButton(void *pvParam) {
  pinMode(btn2, INPUT_PULLUP);
  while (1) {
    if (digitalRead(btn2) == LOW) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      if (digitalRead(btn2) == LOW) {
        sel++;
      }
    }
    if (sel > 5) {
      sel = 1;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  //按鈕偵測任務，若偵測到按下按鈕，等待200毫秒避免抖動，若持續按下則為使用者觸發，讓sel（頁面）加1
  //sel若大於4則回到1(第一頁)
}
void TaskDisp(void *pvParam) {
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  while (1) {
    if (updating == 1) {
      display.clearDisplay();
      display.setTextSize(2);
      display.setTextColor(1);
      display.setCursor(0, 4);
      display.println("OTA Prog:");
      display.setCursor(0, 24);
      display.println(String(otaProgress / (otaTotal / 100)) + "%");
      display.display();
    } else {
      display.clearDisplay();
      if (sel == 1) {
        display.setTextSize(2);
        display.setTextColor(1);
        display.setCursor(0, 4);
        display.println("PM1  :" + String(pm1));
        display.setCursor(0, 24);
        display.println("PM2.5:" + String(pm25));
        display.setCursor(0, 44);
        display.println("PM10 :" + String(pm10));
      } else if (sel == 2) {
        display.setTextSize(2);
        display.setTextColor(1);
        display.setCursor(0, 4);
        display.println(String(tempB, 1) + "   C");
        display.setCursor(0, 24);
        display.println(String(rh) + "     RH");
        display.setCursor(0, 44);
        display.println(String(pressure) + " Pa");
      } else if (sel == 3) {
        display.setTextSize(2);
        display.setTextColor(1);
        display.setCursor(0, 4);
        display.println("Soil RH:");
        display.setCursor(0, 24);
        display.println(String(soilH) + "   %");
        display.setCursor(0, 44);
        display.println(String(oringalSoilH));
      } else if (sel == 4) {
        display.setTextSize(2);
        display.setTextColor(1);
        display.setCursor(0, 4);
        display.println("WiFi:" + String(WiFi.status()));
        display.setCursor(0, 24);
        display.println("RSSI:" + String(WiFi.RSSI()));
        display.setCursor(0, 44);
        display.println("MQTT:" + String(MQTTClient.state()));
      } else if (sel == 5) {
      }
      display.display();
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
  //螢幕顯示任務，外圈的if用來辨識是否在更新，若正在更新則顯示更新進度
  //若未在更新，則進入螢幕顯示，1為懸浮微粒資料，2為溫濕度資料，3為土壤濕度百分比與原始數據、4為WiFi連接狀態、強度與MQTT狀態，5為關閉螢幕
}

//ArduinoOTA螢幕顯示參數
void onStart() {
  updating = 1;
}
void onProgress(unsigned int progress, unsigned int total) {
  otaProgress = progress;
  otaTotal = total;
}
//WiFi計時器，若超過三分鐘未連線則重啟動
void wifiRestart(TimerHandle_t xTimer) {
  esp_restart();
}
