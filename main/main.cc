#include <stdio.h>
#include <string.h>
#include <cmath>
#include <vector>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio/esp32s3_audio_codec.h"

// Simplex模式：麦克风和扬声器使用独立的I2S通道和GPIO引脚
// 扬声器（MAX98357A等I2S放大器）
#define I2S_SPK_BCLK_PIN  GPIO_NUM_15  // 扬声器I2S位时钟 (MAX98357 BCLK)
#define I2S_SPK_WS_PIN    GPIO_NUM_16  // 扬声器I2S字选择 (MAX98357 LRC/LRCLK)
#define I2S_SPK_DOUT_PIN  GPIO_NUM_7   // 扬声器I2S数据输出 (MAX98357 DIN)
// MAX98357的其他引脚：
//   - GAIN: 可选连接GPIO（软件控制增益）或连接到3.3V(高增益15dB)或GND(默认9dB)
//   - SD: 连接到3.3V（启用）或通过GPIO控制（低电平关闭芯片省电）
//   - VIN: 连接到3.3V或5V电源
//   - GND: 连接到GND

// 麦克风（INMP441等I2S数字麦克风）
#define I2S_MIC_SCK_PIN   GPIO_NUM_5   // 麦克风I2S位时钟（SCK）
#define I2S_MIC_WS_PIN    GPIO_NUM_4   // 麦克风I2S字选择（WS）
#define I2S_MIC_DIN_PIN   GPIO_NUM_6   // 麦克风I2S数据输入（SD）

static const char *TAG = "MAIN";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "XiaoZhi ESP32S3 Audio Test (Simplex Mode)");
    ESP_LOGI(TAG, "========================================");
    
    // 打印芯片信息
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s, %d CPU core(s)", CONFIG_IDF_TARGET, chip_info.cores);
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "MAX98357 接线说明:");
    ESP_LOGI(TAG, "  BCLK  -> GPIO %d", I2S_SPK_BCLK_PIN);
    ESP_LOGI(TAG, "  LRC   -> GPIO %d", I2S_SPK_WS_PIN);
    ESP_LOGI(TAG, "  DIN   -> GPIO %d", I2S_SPK_DOUT_PIN);
    ESP_LOGI(TAG, "  GAIN  -> 悬空(9dB) 或 3.3V(15dB) 或 通过GPIO控制");
    ESP_LOGI(TAG, "  SD    -> 3.3V(启用) 或 通过GPIO控制");
    ESP_LOGI(TAG, "  VIN   -> 3.3V 或 5V");
    ESP_LOGI(TAG, "  GND   -> GND");
    
    // 创建音频编解码器（Simplex模式，独立通道）
    // 输入16kHz，输出24kHz（与原项目保持一致）
    Esp32S3AudioCodec codec(16000, 24000,
                            I2S_SPK_BCLK_PIN, I2S_SPK_WS_PIN, I2S_SPK_DOUT_PIN,
                            I2S_MIC_SCK_PIN, I2S_MIC_WS_PIN, I2S_MIC_DIN_PIN);
    
    codec.Start();
    ESP_LOGI(TAG, "Audio codec started");
    
    // ========== 测试1：音频输出（播放正弦波） ==========
    ESP_LOGI(TAG, "Test 1: Audio Output (Playing 440Hz tone for 3 seconds)");
    int output_sample_rate = codec.output_sample_rate();
    int frame_samples = output_sample_rate / 1000 * 30; // 30ms @ output_sample_rate
    std::vector<int16_t> audio_data(frame_samples);
    float frequency = 440.0; // A4音符
    float sample_rate = (float)output_sample_rate;
    
    for (int i = 0; i < 100; i++) { // 100帧 * 30ms = 3秒
        for (int j = 0; j < audio_data.size(); j++) {
            float sample = std::sin(2.0 * M_PI * frequency * (i * audio_data.size() + j) / sample_rate);
            audio_data[j] = (int16_t)(sample * 16000);
        }
        codec.OutputData(audio_data);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGI(TAG, "Output test completed");
    
    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒
    
    // ========== 测试2：音频输入（读取麦克风数据） ==========
    ESP_LOGI(TAG, "Test 2: Audio Input (Reading microphone for 3 seconds)");
    std::vector<int16_t> input_data;
    int frame_count = 0;
    int64_t total_samples = 0;
    int64_t total_energy = 0;
    
    for (int i = 0; i < 100; i++) { // 100帧 * 30ms = 3秒
        if (codec.InputData(input_data)) {
            frame_count++;
            total_samples += input_data.size();
            
            // 计算能量（RMS的平方）
            for (int16_t sample : input_data) {
                total_energy += (int64_t)sample * sample;
            }
            
            // 每10帧打印一次统计信息
            if (frame_count % 10 == 0) {
                float rms = std::sqrt((float)total_energy / total_samples);
                ESP_LOGI(TAG, "Input frame %d: samples=%zu, RMS=%.1f", 
                         frame_count, input_data.size(), rms);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    if (frame_count > 0) {
        float avg_rms = std::sqrt((float)total_energy / total_samples);
        ESP_LOGI(TAG, "Input test completed: %d frames, avg RMS=%.1f", 
                 frame_count, avg_rms);
    } else {
        ESP_LOGW(TAG, "Input test: No audio data received");
    }
    
    ESP_LOGI(TAG, "All audio tests completed");
    
    // 保持运行，可以继续测试
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "System running... Free heap: %lu bytes", esp_get_free_heap_size());
    }
}