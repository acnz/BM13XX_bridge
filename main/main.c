#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_crc.h" 

#define SIM_TXD_PIN      CONFIG_UART_TXD
#define SIM_RXD_PIN      CONFIG_UART_RXD
#define SIM_UART_PORT    CONFIG_UART_PORT_NUM

#define FPGA_UART_PORT   UART_NUM_2
#define FPGA_TXD_PIN     5  
#define FPGA_RXD_PIN     23 
#define FPGA_BAUD_RATE   115200 

#define BUF_SIZE 1024

static const char *TAG = "BM13XX_FPGA_Miner";

static SemaphoreHandle_t uart_mutex;

volatile uint32_t g_job_version = 0;
uint8_t g_midstate_bytes[32];
uint8_t g_data_bytes[12];

uint8_t get_crc5(uint8_t *ptr, uint8_t len) {
    uint8_t crc = 0x1f;
    for (int i = 0; i < len; i++) {
        crc ^= ptr[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x05;
            else crc <<= 1;
        }
    }
    return ((crc >> 3) & 0x1f);
}

/**
 * @brief Inverte a ordem dos BYTES dentro de cada palavra de 32 bits.
 * Mantém a ordem das palavras intacta.
 */
void swap_bytes_in_32bit_words(const uint8_t *src, uint8_t *dst, size_t num_bytes) {
    size_t num_words = num_bytes / 4;
    for (size_t i = 0; i < num_words; i++) {
        dst[i * 4 + 0] = src[i * 4 + 3]; // O último byte da palavra vira o primeiro
        dst[i * 4 + 1] = src[i * 4 + 2];
        dst[i * 4 + 2] = src[i * 4 + 1];
        dst[i * 4 + 3] = src[i * 4 + 0]; // O primeiro byte vira o último
    }
}

/**
 * @brief Função auxiliar para imprimir o buffer como uma string Hex contínua (estilo FPGA)
 */
void log_fpga_format(const char *label, const uint8_t *buffer, size_t len) {
    printf("I (%lu) %s: %s = \"", esp_log_timestamp(), TAG, label);
    for (size_t i = 0; i < len; i++) {
        printf("%02X", buffer[i]);
    }
    printf("\"\n");
}

void send_bitmain_response(uint32_t nonce) {
    unsigned char buf_nonce[11] = {0xAA, 0x55, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0};
    buf_nonce[4] = (nonce >> 24) & 0xFF; 
    buf_nonce[5] = (nonce >> 16) & 0xFF;
    buf_nonce[6] = (nonce >> 8)  & 0xFF; 
    buf_nonce[7] = (nonce >> 0)  & 0xFF;
    buf_nonce[10] = get_crc5(buf_nonce, 10);
    
    if (xSemaphoreTake(uart_mutex, portMAX_DELAY)) {
        uart_write_bytes(SIM_UART_PORT, buf_nonce, 11);
        xSemaphoreGive(uart_mutex);
        ESP_LOGE(TAG, "=> SHARE REAL ENVIADO AO MICRO-STRATUM: %08lx", (unsigned long)nonce);
    }
}

static void miner_task(void *arg) {
    int core_id = (int)arg;
    if (core_id != 0) { vTaskDelay(portMAX_DELAY); }

    uart_config_t fpga_uart_config = {
        .baud_rate = FPGA_BAUD_RATE, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(FPGA_UART_PORT, &fpga_uart_config));
    ESP_ERROR_CHECK(uart_set_pin(FPGA_UART_PORT, FPGA_TXD_PIN, FPGA_RXD_PIN, -1, -1));
    ESP_ERROR_CHECK(uart_driver_install(FPGA_UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));

    uint32_t local_version = 0;
    uint8_t fpga_packet[60];
    uint8_t fpga_rx_buf[128];

    ESP_LOGI(TAG, "Link com a FPGA ativo. Aguardando a Pool enviar Jobs...");

    while(1) {
        if (g_job_version != local_version) {
            local_version = g_job_version;
            ESP_LOGW(TAG, "Novo Trabalho da Rede recebido! Atualizando FPGA...");

            memset(fpga_packet, 0, 60);
            fpga_packet[0] = 60; fpga_packet[3] = 0x04; // PUSH_JOB

            // NonceMax
            fpga_packet[4] = 0xFF; fpga_packet[5] = 0xFF; fpga_packet[6] = 0xFF; fpga_packet[7] = 0xFF;
            //1dac2b7c  satoshi
            // NonceMin
            fpga_packet[8] = 0x24; fpga_packet[9] = 0x11; fpga_packet[10] = 0x30; fpga_packet[11] = 0x08;

            uint8_t temp_data_bytes[12];
            // Executa a inversão do data
            swap_bytes_in_32bit_words(g_data_bytes, temp_data_bytes, sizeof(g_data_bytes));
            memcpy(&fpga_packet[12], temp_data_bytes, 12);
            log_fpga_format("FPGA Data", temp_data_bytes, sizeof(temp_data_bytes));
            //ESP_LOGW(TAG, "ffff001d29ab5f494b1e5e4a");

            uint8_t temp_midstate_bytes[32];
            // Executa a inversão do Midstate
            swap_bytes_in_32bit_words(g_midstate_bytes, temp_midstate_bytes, sizeof(g_midstate_bytes));
            memcpy(&fpga_packet[24], temp_midstate_bytes, 32);
            log_fpga_format("FPGA Midstate", temp_midstate_bytes, sizeof(temp_midstate_bytes));
            //ESP_LOGW(TAG, "4719F91B96B187364F0103C8C3C8D8E91E59CAA890CCAC7D6358BFF0BC909A33");

            uint32_t crc = esp_rom_crc32_le(0, fpga_packet, 56);
            memcpy(&fpga_packet[56], &crc, 4);

            uart_flush(FPGA_UART_PORT);
            uart_write_bytes(FPGA_UART_PORT, fpga_packet, 60);
        }

        // Fica ouvindo o retorno dos Nonces pela FPGA
        int len = uart_read_bytes(FPGA_UART_PORT, fpga_rx_buf, sizeof(fpga_rx_buf), 10 / portTICK_PERIOD_MS);
        if (len == 4) {
            // CORREÇÃO CRIME 2: A FPGA manda o LSB primeiro. Lemos na ordem exata para não desvirar!
            uint32_t found_nonce = ((uint32_t)fpga_rx_buf[3] << 24) | 
                                   ((uint32_t)fpga_rx_buf[2] << 16) |
                                   ((uint32_t)fpga_rx_buf[1] << 8)  | 
                                   ((uint32_t)fpga_rx_buf[0] << 0);
                                   
            ESP_LOGE(TAG, "!!! SUCESSO !!! A FPGA DESTRUIU O HASH: %08lx", (unsigned long)found_nonce);
            send_bitmain_response(found_nonce);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void uart_listener_task(void *arg) {
    uart_config_t uart_config = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(SIM_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SIM_UART_PORT, SIM_TXD_PIN, SIM_RXD_PIN, -1, -1));
    ESP_ERROR_CHECK(uart_driver_install(SIM_UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));

    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);

    while (1) {
        int len = uart_read_bytes(SIM_UART_PORT, data, 128, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            if (strstr((char*)data, "PING") != NULL) { 
                uart_write_bytes(SIM_UART_PORT, "PONG", 4); 
                continue; 
            }

            if (len >= 6 && data[2] == 0x21) { 
                uart_flush(SIM_UART_PORT);
                memcpy(g_midstate_bytes, &data[4], 32);
                memcpy(g_data_bytes, &data[36], 12);
                ESP_LOGW(TAG, "[midstate]");
                ESP_LOG_BUFFER_HEX(TAG, g_midstate_bytes, sizeof(g_midstate_bytes));
                ESP_LOGW(TAG, "[data]");
                ESP_LOG_BUFFER_HEX(TAG, g_data_bytes, sizeof(g_data_bytes));

                g_job_version++; 
            }
            else if (data[2] == 0x51 && data[5] == 0x28) { 
                uart_wait_tx_done(SIM_UART_PORT, 1000 / portTICK_PERIOD_MS);
                uart_set_baudrate(SIM_UART_PORT, 1000000);
            }
        }
    }
}

void app_main(void) {
    uart_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "=== MAESTRO DA FPGA INICIADO ===");
    xTaskCreatePinnedToCore(uart_listener_task, "uart_task", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(miner_task, "miner_core0", 8192, (void*)0, 10, NULL, 0);
    xTaskCreatePinnedToCore(miner_task, "miner_core1", 8192, (void*)1, 10, NULL, 1);
}