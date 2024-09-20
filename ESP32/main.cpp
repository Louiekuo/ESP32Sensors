#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <HardwareSerial.h>
#define btn2 36

unsigned int rh = 0;                              //使用unsinged，避免溫度有負值
float temp = 0;                                   //溫度
long pm1 = 0, pm25 = 0, pm10 = 0;                 //讀取出的PM1、PM2.5、PM10數值
float soilH = 0;                                  //土壤濕度
int sel = 1;                                      //螢幕顯示的頁數
int updating = 0, otaProgress = 0, otaTotal = 0;  //OTA資訊，第一個為是否正在更新，後面兩個為計算百分比所需資料

String url = "https://api.thingspeak.com/update?api_key=", apiKey = "---------";
WiFiUDP ntpUDP;
HardwareSerial pms(2);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
//url可依照API Key不同自行修改
//PMS5003T使用了ESP32內建的UART，使用串口2
//SSD1306使用I2C連接，此處需定義螢幕長度、寬度（若無reset針腳則在最後填入-1）

void setup() {
  Serial.begin(115200);
  xTaskCreate(TaskReadPMS, "Read PMS Sensor", 1500, NULL, 1, NULL);
  xTaskCreate(TaskReadSoilrh, "Read Soil Sensor", 1200, NULL, 2, NULL);
  xTaskCreate(TaskUploadData, "Upload Data", 3500, NULL, 1, NULL);
  xTaskCreate(TaskWiFi, "WiFi connect", 3000, NULL, 1, NULL);

  xTaskCreatePinnedToCore(TaskOTA, "OTA", 2000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskDisp, "Screen", 3000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskButton, "Button", 2000, NULL, 1, NULL, 0);
  delay(10000);
}
void loop() {
}

void TaskWiFi(void *pvParam) {
  WiFi.mode(WIFI_STA);
  WiFi.begin("---------", "---------");
  while (1) {
    if (WiFi.status() != WL_CONNECTED) {
      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        //Serial.print(".");
      }
      // Serial.println("");
      // Serial.println("WiFi connected.");
    }
    vTaskDelay(20000 / portTICK_PERIOD_MS);
  }
  //WiFi連線任務
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
  pinMode(39, INPUT);  //土壤濕度感測器IO39
  while (1) {
    soilH = analogRead(39);
    soilH = ((4095 - soilH) / 1695) * 100;
    if (soilH > 100) {
      soilH = 100;
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  //使用類比讀取土壤感應器濕度資料，經轉換後輸出為百分比，0為乾燥，100為潮濕
}
void TaskUploadData(void *pvParam) {
  HTTPClient http;
  while (1) {
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    String tmpurl = (url + apiKey) + "&field1=" + String(temp, 1) + "&field2=" + String(rh) + "&field3=" + String(int(soilH)) + "&field4=" + String(pm1) + "&field5=" + String(pm25) + "&field6=" + String(pm10);
    http.begin(tmpurl);
    Serial.println("result = " + String(http.GET()));
    http.end();
    vTaskDelay(13000 / portTICK_PERIOD_MS);
  }
  //上傳資料至Thingspeak，可依照Field不同調整tmpurl中的順序，時間可在最後vTaskDelay調整
  //最上方預留2秒給其他感應器讀取數據
  //！因Thingspeak限制資料傳輸間隔最少需要15秒，因此下方至少需13000(加上方2000)才可達到15秒間隔！
}
void TaskOTA(void *pvParam) {
  ArduinoOTA.setHostname("---------");
  ArduinoOTA.setPassword("---------");
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
  pinMode(btn2, INPUT);
  while (1) {
    if (digitalRead(btn2) == LOW) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      if (digitalRead(btn2) == LOW) {
        sel++;
      }
    }
    if (sel > 4) {
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
      if (sel == 1) {
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(1);
        display.setCursor(0, 4);
        display.println("PM1  :" + String(pm1));
        display.setCursor(0, 24);
        display.println("PM2.5:" + String(pm25));
        display.setCursor(0, 44);
        display.println("PM10 :" + String(pm10));
        display.display();
      } else if (sel == 2) {
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(1);
        display.setCursor(0, 4);
        display.println(String(temp, 1) + " C");
        display.setCursor(0, 24);
        display.println(String(rh) + "  RH");
        display.display();
      } else if (sel == 3) {
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(1);
        display.setCursor(0, 4);
        display.println("WiFi:" + String(WiFi.status()));
        display.setCursor(0, 24);
        display.println("RSSI:" + String(WiFi.RSSI()));
        display.setCursor(0, 44);
        display.println("RAM:" + String(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
        display.display();
      } else if (sel == 4) {
        display.clearDisplay();
        display.display();
      }
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
  //螢幕顯示任務，外圈的if用來辨識是否在更新，若正在更新則顯示更新進度
  //若未在更新，則進入螢幕顯示，1為懸浮微粒資料，2為溫濕度資料，3為WiFi連接狀態、訊息與記憶體剩餘大小，4為關閉螢幕
}

//ArduinoOTA螢幕顯示參數
void onStart() {
  updating = 1;
}
void onProgress(unsigned int progress, unsigned int total) {
  otaProgress = progress;
  otaTotal = total;
}
