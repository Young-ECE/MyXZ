#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "XiaoZhi ESP32S3 Test Program");
    ESP_LOGI(TAG, "========================================");
    
    // 打印芯片信息
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s, %d CPU core(s), WiFi%s%s",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
    ESP_LOGI(TAG, "Silicon revision: %d", chip_info.revision);
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    
    // 简单的闪烁任务（如果有LED）
    int count = 0;
    while (1) {
        ESP_LOGI(TAG, "Running... Count: %d, Free heap: %lu", 
                 count++, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒打印一次
    }
}