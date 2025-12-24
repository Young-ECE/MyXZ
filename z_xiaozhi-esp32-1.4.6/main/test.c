#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <math.h>

#define TAG "AudioTest"

// bread-compact-wifi 配置
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4    // 字时钟
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5    // 位时钟  
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6    // 数据输入

// 音频录制配置
#define RECORD_DURATION_SEC     3             // 录制时长3秒
#define SAMPLES_PER_SEC         AUDIO_INPUT_SAMPLE_RATE
#define TOTAL_SAMPLES           (SAMPLES_PER_SEC * RECORD_DURATION_SEC)  // 48000个采样点
#define FRAME_SIZE              240           // 每次读取240个采样点 (15ms)

typedef struct {
    i2s_chan_handle_t rx_handle;
    bool running;
    int16_t* audio_buffer;                   // 存储录制的音频数据
    int current_sample;                      // 当前录制到的采样点
} audio_test_t;

static audio_test_t audio_test;

/**
 * 音频质量分析结构
 */
typedef struct {
    float rms;                  // RMS值
    float snr;                  // 信噪比估算
    int16_t peak_positive;      // 正峰值
    int16_t peak_negative;      // 负峰值
    float dc_offset;            // 直流偏移
    int zero_crossing_rate;     // 过零率
    float dynamic_range;        // 动态范围
    int clipping_count;         // 削波计数
    float frequency_content[8]; // 简单频率分析(8个频段)
} audio_quality_t;

/**
 * 初始化I2S音频采集
 */
static esp_err_t audio_test_init(void) {
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initializing I2S audio input...");
    ESP_LOGI(TAG, "Sample Rate: %d Hz", AUDIO_INPUT_SAMPLE_RATE);
    ESP_LOGI(TAG, "Recording Duration: %d seconds", RECORD_DURATION_SEC);
    ESP_LOGI(TAG, "Total Samples: %d", TOTAL_SAMPLES);

    // 分配音频缓冲区
    audio_test.audio_buffer = malloc(TOTAL_SAMPLES * sizeof(int16_t));
    if (!audio_test.audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer (%zu bytes)", TOTAL_SAMPLES * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    audio_test.current_sample = 0;

    // 创建I2S通道
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = FRAME_SIZE,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    
    ret = i2s_new_channel(&chan_cfg, NULL, &audio_test.rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        free(audio_test.audio_buffer);
        return ret;
    }

    // 配置I2S
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_INPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_MIC_GPIO_SCK,
            .ws = AUDIO_I2S_MIC_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_I2S_MIC_GPIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ret = i2s_channel_init_std_mode(audio_test.rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(audio_test.rx_handle);
        free(audio_test.audio_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "I2S audio input initialized successfully");
    return ESP_OK;
}

/**
 * 启动音频采集
 */
static esp_err_t audio_test_start(void) {
    esp_err_t ret = i2s_channel_enable(audio_test.rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待I2S稳定
    audio_test.running = true;
    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

/**
 * 停止音频采集
 */
static esp_err_t audio_test_stop(void) {
    audio_test.running = false;
    
    esp_err_t ret = i2s_channel_disable(audio_test.rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Audio capture stopped");
    return ret;
}

/**
 * 读取音频数据
 */
static int audio_read_frame(int16_t* dest, int samples) {
    size_t bytes_read = 0;
    int32_t* bit32_buffer = malloc(samples * sizeof(int32_t));
    
    if (!bit32_buffer) {
        ESP_LOGE(TAG, "Failed to allocate 32-bit buffer");
        return 0;
    }

    esp_err_t ret = i2s_channel_read(audio_test.rx_handle, bit32_buffer, 
                                     samples * sizeof(int32_t), &bytes_read, 
                                     pdMS_TO_TICKS(100));
    
    if (ret != ESP_OK || bytes_read == 0) {
        free(bit32_buffer);
        return 0;
    }

    int actual_samples = bytes_read / sizeof(int32_t);
    
    // 32位转16位
    for (int i = 0; i < actual_samples; i++) {
        int32_t value = bit32_buffer[i] >> 16;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        dest[i] = (int16_t)value;
    }
    
    free(bit32_buffer);
    return actual_samples;
}

/**
 * 简单的频率分析 (8个频段)
 */
static void analyze_frequency_content(int16_t* data, int samples, float* freq_bands) {
    // 简单的能量分布分析，分为8个频段
    // 频段: 0-1k, 1k-2k, 2k-3k, 3k-4k, 4k-5k, 5k-6k, 6k-7k, 7k-8k Hz
    
    memset(freq_bands, 0, 8 * sizeof(float));
    
    // 简化的频率分析：基于样本值的变化率
    for (int band = 0; band < 8; band++) {
        int band_start = (samples * band) / 8;
        int band_end = (samples * (band + 1)) / 8;
        
        float energy = 0.0f;
        for (int i = band_start; i < band_end - 1; i++) {
            float diff = (float)(data[i + 1] - data[i]);
            energy += diff * diff;
        }
        freq_bands[band] = sqrtf(energy / (band_end - band_start));
    }
}

/**
 * 音频质量分析
 */
static void analyze_audio_quality(int16_t* data, int samples, audio_quality_t* quality) {
    memset(quality, 0, sizeof(audio_quality_t));
    
    if (samples == 0) return;
    
    // 基本统计
    float sum = 0.0f;
    float sum_squares = 0.0f;
    int zero_crossings = 0;
    int clipping = 0;
    
    quality->peak_positive = INT16_MIN;
    quality->peak_negative = INT16_MAX;
    
    for (int i = 0; i < samples; i++) {
        int16_t sample = data[i];
        
        // 累加计算
        sum += sample;
        sum_squares += (float)sample * sample;
        
        // 峰值检测
        if (sample > quality->peak_positive) quality->peak_positive = sample;
        if (sample < quality->peak_negative) quality->peak_negative = sample;
        
        // 削波检测
        if (sample >= 32767 || sample <= -32767) clipping++;
        
        // 过零率计算
        if (i > 0 && ((data[i-1] >= 0 && sample < 0) || (data[i-1] < 0 && sample >= 0))) {
            zero_crossings++;
        }
    }
    
    // 计算统计值
    quality->dc_offset = sum / samples;
    quality->rms = sqrtf(sum_squares / samples);
    quality->zero_crossing_rate = zero_crossings;
    quality->clipping_count = clipping;
    quality->dynamic_range = 20.0f * log10f((float)(quality->peak_positive - quality->peak_negative) / 65536.0f);
    
    // 估算信噪比 (简化方法)
    float signal_power = quality->rms;
    float noise_floor = 100.0f; // 假设噪声底限
    quality->snr = 20.0f * log10f(signal_power / noise_floor);
    
    // 频率分析
    analyze_frequency_content(data, samples, quality->frequency_content);
}

/**
 * 输出音频质量报告
 */
static void print_quality_report(audio_quality_t* quality) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========== 音频质量分析报告 ==========");
    ESP_LOGI(TAG, "📊 基本参数:");
    ESP_LOGI(TAG, "   RMS值: %.1f", quality->rms);
    ESP_LOGI(TAG, "   峰值: %d 到 %d", quality->peak_negative, quality->peak_positive);
    ESP_LOGI(TAG, "   直流偏移: %.1f", quality->dc_offset);
    ESP_LOGI(TAG, "   动态范围: %.1f dB", quality->dynamic_range);
    ESP_LOGI(TAG, "   估算SNR: %.1f dB", quality->snr);
    
    ESP_LOGI(TAG, "🔊 信号特征:");
    ESP_LOGI(TAG, "   过零率: %d 次/3秒", quality->zero_crossing_rate);
    ESP_LOGI(TAG, "   削波计数: %d", quality->clipping_count);
    
    // 音频质量评估
    ESP_LOGI(TAG, "✅ 质量评估:");
    if (quality->rms > 5000) {
        ESP_LOGI(TAG, "   📢 信号强度: 很强 (可能过载)");
    } else if (quality->rms > 1000) {
        ESP_LOGI(TAG, "   📢 信号强度: 强");
    } else if (quality->rms > 100) {
        ESP_LOGI(TAG, "   📢 信号强度: 中等");
    } else if (quality->rms > 10) {
        ESP_LOGI(TAG, "   📢 信号强度: 弱");
    } else {
        ESP_LOGI(TAG, "   📢 信号强度: 很弱 (可能无信号)");
    }
    
    if (quality->clipping_count > 0) {
        ESP_LOGW(TAG, "   ⚠️  检测到 %d 个削波点，可能存在失真", quality->clipping_count);
    } else {
        ESP_LOGI(TAG, "   ✅ 无削波，信号清洁");
    }
    
    if (fabsf(quality->dc_offset) > 1000) {
        ESP_LOGW(TAG, "   ⚠️  直流偏移较大: %.1f", quality->dc_offset);
    } else {
        ESP_LOGI(TAG, "   ✅ 直流偏移正常");
    }
    
    // 频率分析
    ESP_LOGI(TAG, "🎵 频率分析 (8个频段):");
    const char* band_names[] = {"0-1k", "1-2k", "2-3k", "3-4k", "4-5k", "5-6k", "6-7k", "7-8k"};
    for (int i = 0; i < 8; i++) {
        ESP_LOGI(TAG, "   %s Hz: %.1f", band_names[i], quality->frequency_content[i]);
    }
    
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "");
}

/**
 * 录制音频任务
 */
static void record_audio_task(void* arg) {
    ESP_LOGI(TAG, "开始录制音频...");
    ESP_LOGI(TAG, "请对着麦克风说话或制造声音...");
    
    int16_t frame_buffer[FRAME_SIZE];
    int frames_recorded = 0;
    int total_frames = (TOTAL_SAMPLES + FRAME_SIZE - 1) / FRAME_SIZE; // 向上取整
    
    while (audio_test.running && audio_test.current_sample < TOTAL_SAMPLES) {
        int samples_to_read = FRAME_SIZE;
        if (audio_test.current_sample + samples_to_read > TOTAL_SAMPLES) {
            samples_to_read = TOTAL_SAMPLES - audio_test.current_sample;
        }
        
        int samples_read = audio_read_frame(frame_buffer, samples_to_read);
        
        if (samples_read > 0) {
            // 复制到主缓冲区
            memcpy(&audio_test.audio_buffer[audio_test.current_sample], 
                   frame_buffer, samples_read * sizeof(int16_t));
            audio_test.current_sample += samples_read;
            frames_recorded++;
            
            // 进度显示
            if (frames_recorded % 20 == 0) { // 每0.3秒显示一次进度
                float progress = (float)audio_test.current_sample / TOTAL_SAMPLES * 100.0f;
                ESP_LOGI(TAG, "录制进度: %.1f%% (%d/%d 采样点)", 
                         progress, audio_test.current_sample, TOTAL_SAMPLES);
            }
        } else {
            ESP_LOGW(TAG, "未读取到数据，跳过此帧");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    ESP_LOGI(TAG, "录制完成！总共录制了 %d 个采样点", audio_test.current_sample);
    
    // 分析音频质量
    ESP_LOGI(TAG, "开始分析音频质量...");
    audio_quality_t quality;
    analyze_audio_quality(audio_test.audio_buffer, audio_test.current_sample, &quality);
    print_quality_report(&quality);
    
    // 输出部分原始数据用于调试
    ESP_LOGI(TAG, "原始数据样本 (前20个):");
    for (int i = 0; i < 20 && i < audio_test.current_sample; i++) {
        printf("%d ", audio_test.audio_buffer[i]);
        if ((i + 1) % 10 == 0) printf("\n");
    }
    printf("\n");
    
    vTaskDelete(NULL);
}

/**
 * 主测试函数
 */
void app_main(void) {
    ESP_LOGI(TAG, "=== 3秒音频录制与质量分析测试 ===");
    ESP_LOGI(TAG, "Board: bread-compact-wifi");
    ESP_LOGI(TAG, "Microphone: INMP441");
    ESP_LOGI(TAG, "录制参数:");
    ESP_LOGI(TAG, "  采样率: %d Hz", AUDIO_INPUT_SAMPLE_RATE);
    ESP_LOGI(TAG, "  录制时长: %d 秒", RECORD_DURATION_SEC);
    ESP_LOGI(TAG, "  总采样点: %d", TOTAL_SAMPLES);
    ESP_LOGI(TAG, "  数据大小: %zu 字节", TOTAL_SAMPLES * sizeof(int16_t));
    
    ESP_LOGI(TAG, "硬件连接:");
    ESP_LOGI(TAG, "  INMP441 SCK  -> GPIO %d", AUDIO_I2S_MIC_GPIO_SCK);
    ESP_LOGI(TAG, "  INMP441 WS   -> GPIO %d", AUDIO_I2S_MIC_GPIO_WS);
    ESP_LOGI(TAG, "  INMP441 SD   -> GPIO %d", AUDIO_I2S_MIC_GPIO_DIN);
    ESP_LOGI(TAG, "  INMP441 VDD  -> 3.3V");
    ESP_LOGI(TAG, "  INMP441 GND  -> GND");
    ESP_LOGI(TAG, "  INMP441 L/R  -> GND");
    
    // 初始化音频系统
    if (audio_test_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败!");
        return;
    }
    
    // 启动音频采集
    if (audio_test_start() != ESP_OK) {
        ESP_LOGE(TAG, "音频启动失败!");
        return;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "🎤 系统准备就绪，3秒后开始录制...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 创建录制任务
    xTaskCreate(record_audio_task, "audio_record", 8192, NULL, 5, NULL);
    
    // 主循环监控
    int seconds = 0;
    while (audio_test.current_sample < TOTAL_SAMPLES) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        seconds++;
        
        if (seconds > RECORD_DURATION_SEC + 5) {
            ESP_LOGW(TAG, "录制超时，强制停止");
            break;
        }
    }
    
    audio_test_stop();
    ESP_LOGI(TAG, "测试完成！");
    
    // 清理资源
    if (audio_test.audio_buffer) {
        free(audio_test.audio_buffer);
        ESP_LOGI(TAG, "资源清理完成");
    }
}