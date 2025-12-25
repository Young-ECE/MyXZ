#include <stdio.h>
#include <string.h>
#include <cmath>
#include <vector>
#include <queue>
#include <mutex>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
// 使用标准输出(stdout)发送数据，无需UART驱动
// #include "driver/uart.h"  // 不需要UART驱动，使用stdout即可
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
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

// 事件组定义
#define AUDIO_INPUT_READY_EVENT BIT0

// 音频数据包结构（用于队列传递）
struct AudioPacket {
    std::vector<int16_t> data;
};

// 全局变量
static EventGroupHandle_t event_group_ = nullptr;
static QueueHandle_t audio_queue_ = nullptr;
static Esp32S3AudioCodec* g_codec = nullptr;

// 通过标准输出发送音频数据到上位机（Vofa+ FireWater格式）
// FireWater格式：每行一个浮点数，逗号分隔（单通道），换行结尾
// 例如：1.234\n 或 1.234,5.678\n（多通道）
// ESP-IDF会自动将stdout路由到USB Serial/JTAG（当仅连接USB口时）
static void send_audio_to_host(const std::vector<int16_t>& audio_data) {
    // 将PCM16数据转换为归一化浮点数（-1.0到1.0范围）
    // 每个采样点发送一行，Vofa+可以直接读取和显示
    for (int16_t sample : audio_data) {
        // 归一化到-1.0到1.0范围（int16_t范围：-32768到32767）
        float normalized = (float)sample / 32768.0f;
        
        // 使用printf发送FireWater格式（每行一个浮点数，换行结尾）
        fprintf(stdout, "%.6f\n", normalized);
    }
    fflush(stdout); // 确保数据立即刷新到USB
}

// 发送任务：从队列中取出音频数据并发送到上位机
static void send_task(void* arg) {
    AudioPacket* packet = nullptr;
    
    ESP_LOGI(TAG, "Audio send task started");
    
    static int frame_count = 0;
    
    while (1) {
        // 从队列中获取音频数据（最多等待100ms）
        if (xQueueReceive(audio_queue_, &packet, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (packet != nullptr) {
                frame_count++;
                
                // 调试：每50帧打印一次数据包内容
                if (frame_count % 50 == 0 && !packet->data.empty()) {
                    ESP_LOGI(TAG, "=== Audio Packet Debug (Frame %d) ===", frame_count);
                    ESP_LOGI(TAG, "Packet size: %zu samples", packet->data.size());
                    // 打印前10个采样值
                    ESP_LOGI(TAG, "First 10 samples: ");
                    for (size_t i = 0; i < 10 && i < packet->data.size(); i++) {
                        ESP_LOGI(TAG, "  [%zu]: %d (0x%04x)", i, packet->data[i], (unsigned short)packet->data[i]);
                    }
                    
                    // 计算统计数据
                    int16_t max_val = INT16_MIN, min_val = INT16_MAX;
                    int64_t sum = 0, sum_sq = 0;
                    for (int16_t sample : packet->data) {
                        if (sample > max_val) max_val = sample;
                        if (sample < min_val) min_val = sample;
                        sum += sample;
                        sum_sq += (int64_t)sample * sample;
                    }
                    float mean = (float)sum / packet->data.size();
                    float rms = sqrtf((float)sum_sq / packet->data.size());
                    ESP_LOGI(TAG, "Stats: min=%d, max=%d, mean=%.2f, RMS=%.2f", 
                             min_val, max_val, mean, rms);
                }
                
                // 发送到上位机
                // send_audio_to_host(packet->data);
                
                // 释放内存
                delete packet;
                
                // 统计信息（每秒打印一次）
                static int64_t last_time = 0;
                int64_t current_time = esp_timer_get_time() / 1000000; // 转换为秒
                if (current_time - last_time >= 1) {
                    ESP_LOGI(TAG, "Sent %d frames in last second, heap: %lu bytes", 
                             frame_count, esp_get_free_heap_size());
                    frame_count = 0;
                    last_time = current_time;
                }
            }
        }
    }
}

// 主循环：处理音频输入事件
static void main_loop(void* arg) {
    ESP_LOGI(TAG, "Main loop started");
    
    std::vector<int16_t> input_data;
    input_data.reserve(16000 / 1000 * 30); // 预分配30ms的缓冲区
    
    while (1) {
        // 等待音频输入就绪事件
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            AUDIO_INPUT_READY_EVENT,
            pdTRUE,  // 清除事件位
            pdFALSE, // 不等待所有位
            portMAX_DELAY
        );
        
        if (bits & AUDIO_INPUT_READY_EVENT) {
            // 从音频编解码器读取数据
            if (g_codec && g_codec->InputData(input_data)) {
                // 检查数据是否有效
                if (input_data.empty()) {
                    continue;
                }
                
                // 在堆上分配数据包并复制数据
                AudioPacket* packet = new AudioPacket();
                packet->data = input_data; // 复制数据
                
                // 将数据包指针放入队列发送到上位机
                if (xQueueSend(audio_queue_, &packet, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Audio queue full, dropping frame");
                    delete packet; // 如果队列满，释放内存
                }
            }
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "XiaoZhi ESP32S3 Audio Test (Callback Mode)");
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
    
    // 使用标准输出发送数据（通过USB Serial/JTAG）
    // stdout会自动路由到USB Serial/JTAG，无需额外初始化
    ESP_LOGI(TAG, "Using stdout (USB Serial/JTAG) for audio data transmission");
    
    // 创建事件组
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }
    
    // 创建音频数据队列（最多缓存10帧，每帧约30ms）
    // 队列存储AudioPacket*指针
    audio_queue_ = xQueueCreate(20, sizeof(AudioPacket*));
    if (audio_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return;
    }
    
    // 创建音频编解码器（Simplex模式，独立通道）
    // 输入16kHz，输出24kHz（与原项目保持一致）
    static Esp32S3AudioCodec codec(16000, 24000,
                                   I2S_SPK_BCLK_PIN, I2S_SPK_WS_PIN, I2S_SPK_DOUT_PIN,
                                   I2S_MIC_SCK_PIN, I2S_MIC_WS_PIN, I2S_MIC_DIN_PIN);
    g_codec = &codec;
    
    // 设置输入就绪回调函数（在I2S中断中调用）
    codec.OnInputReady([]() {
        BaseType_t higher_priority_task_woken = pdFALSE;
        // 在中断中设置事件位
        xEventGroupSetBitsFromISR(event_group_, AUDIO_INPUT_READY_EVENT, &higher_priority_task_woken);
        // 如果唤醒了更高优先级的任务，需要执行任务切换
        return higher_priority_task_woken == pdTRUE;
    });
    
    // 启动音频编解码器
    codec.Start();
    ESP_LOGI(TAG, "Audio codec started with callback mode");
    ESP_LOGI(TAG, "DMA配置: dma_desc_num=6, dma_frame_num=240");
    
    // 创建主循环任务
    xTaskCreate(main_loop, "main_loop", 4096, nullptr, 5, nullptr);
    
    // 创建发送任务
    xTaskCreate(send_task, "send_task", 4096, nullptr, 4, nullptr);
    
    ESP_LOGI(TAG, "System initialized. Audio data will be sent via USB Serial/JTAG continuously.");
    ESP_LOGI(TAG, "Data format: Vofa+ FireWater format (one float per line, normalized -1.0 to 1.0)");
    ESP_LOGI(TAG, "Vofa+ settings: Protocol=FireWater, Channels=1, Sample Rate=%d Hz", 
             codec.input_sample_rate());
    
    // 保持运行
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "System running... Free heap: %lu bytes", esp_get_free_heap_size());
    }
}