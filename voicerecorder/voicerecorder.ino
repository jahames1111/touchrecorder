#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <lvgl.h>
#include <TFT_eSPI.h>

#include "ui.h"

#define I2S_WS 5
#define I2S_SD 6
#define I2S_SCK 7
#define I2S_PORT I2S_NUM_0

#define SD_MISO 13
#define SD_MOSI 11
#define SD_SCLK 12
#define SD_CS   10

#define RECORD_BUTTON_PIN 0
#define BUFFER_SIZE 2048
#define QUEUE_LEN 15

QueueHandle_t audioQueue;
TFT_eSPI tft = TFT_eSPI();
SPIClass sdSPI = SPIClass(HSPI);

static const uint16_t screenWidth = 240;
static const uint16_t screenHeight = 135;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

volatile bool isSystemRecording = false;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

void audioCaptureTask(void *pvParameters) {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  
  uint8_t rawAudioChunk[BUFFER_SIZE];
  size_t readBytesCount;

  while (1) {
    if (isSystemRecording) {
      i2s_read(I2S_PORT, &rawAudioChunk, BUFFER_SIZE, &readBytesCount, portMAX_DELAY);
      if (readBytesCount > 0) {
        xQueueSend(audioQueue, &rawAudioChunk, 0);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void storageAndUITask(void *pvParameters) {
  uint8_t dataToDiskBuffer[BUFFER_SIZE];
  File activeWavFile;
  bool lastRecordingState = false;

  tft.init();
  tft.setRotation(1);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);
  
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  ui_init();

  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 40000000)) {
    if (ui_errorbox) {
      lv_label_set_text(ui_errorbox, "SD INIT FAIL");
    }
  } else {
    if (ui_errorbox) {
      lv_label_set_text(ui_errorbox, "SYSTEM OK");
    }
  }

  while (1) {
    uint32_t currentTickCount = millis();

    if (isSystemRecording != lastRecordingState) {
      if (isSystemRecording) {
        xQueueReset(audioQueue);
        activeWavFile = SD.open("/classroom_rec.raw", FILE_WRITE);
        if (!activeWavFile) {
          isSystemRecording = false;
        } else {
          lv_scr_load(ui_Screen3);
        }
      } else {
        if (activeWavFile) {
          activeWavFile.close();
        }
        lv_scr_load(ui_Screen1);
        if (ui_errorbox) {
          lv_label_set_text(ui_errorbox, "SYSTEM OK\nREC STOPPED");
        }
      }
      lastRecordingState = isSystemRecording;
    }

    if (isSystemRecording && activeWavFile) {
      while (xQueueReceive(audioQueue, &dataToDiskBuffer, 0) == pdTRUE) {
        activeWavFile.write(dataToDiskBuffer, BUFFER_SIZE);
      }
      
      extern lv_obj_t * ui_mic_icon;
      lv_obj_t * targetMicObj = NULL;
      if (typeof(ui_mic_icon) != void) { targetMicObj = ui_mic_icon; }
      else if (typeof(ui_Image1) != void) { targetMicObj = ui_Image1; }
      
      if (targetMicObj) {
        float mathRadianValue = (currentTickCount % 1000) * (2.0 * PI / 1000.0);
        uint8_t computedOpacityValue = 127 + (int)(128.0 * sin(mathRadianValue));
        lv_obj_set_style_img_opa(targetMicObj, computedOpacityValue, LV_PART_MAIN);
      }
    }

    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void setup() {
  Serial.begin(111200);
  pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);
  
  audioQueue = xQueueCreate(QUEUE_LEN, BUFFER_SIZE);

  xTaskCreatePinnedToCore(audioCaptureTask, "AudioCap", 8192, NULL, 10, NULL, 0);
  xTaskCreatePinnedToCore(storageAndUITask, "StorageUI", 24576, NULL, 2, NULL, 1);
}

void loop() {
  static bool previousPhysicalButtonState = HIGH;
  bool currentPhysicalButtonState = digitalRead(RECORD_BUTTON_PIN);

  if (currentPhysicalButtonState == LOW && previousPhysicalButtonState == HIGH) {
    isSystemRecording = !isSystemRecording;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  previousPhysicalButtonState = currentPhysicalButtonState;
  vTaskDelay(pdMS_TO_TICKS(10));
}
