#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_random.h"

// ============================================================================
// CONFIGURAÇÕES DE PINOS E BARRAMENTOS
// ============================================================================
#define SIM_TXD_PIN CONFIG_UART_TXD
#define SIM_RXD_PIN CONFIG_UART_RXD
#define SIM_UART_PORT CONFIG_UART_PORT_NUM
#define BUF_SIZE 1024

#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#define SPI_SPEED_HZ (5 * 1000 * 1000) 

static const char *TAG = "BM13XX_FPGA_MASTER";

static SemaphoreHandle_t uart_mutex;
spi_device_handle_t fpga_spi;

volatile uint32_t g_job_version = 0;
uint8_t g_midstate_raw[32];
uint8_t g_data_raw[12];

// ============================================================================
// FUNÇÕES AUXILIARES DE CRC
// ============================================================================

uint8_t calculate_spi_crc5(uint8_t *data, int num_bits) {
    uint8_t crc = 0x1F;
    for (int i = 0; i < num_bits; i++) {
        uint8_t byte = data[i / 8];
        uint8_t bit = (byte >> (7 - (i % 8))) & 1;
        uint8_t inv = bit ^ ((crc >> 4) & 1);
        crc = (crc << 1) & 0x1F;
        if (inv) crc ^= 0x05;
    }
    return crc;
}

uint8_t get_bitmain_crc5(uint8_t *ptr, uint8_t len) {
    uint8_t crc = 0x1F;
    for (int i = 0; i < len; i++) {
        crc ^= ptr[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x05;
            else crc <<= 1;
        }
    }
    return ((crc >> 3) & 0x1F);
}

void send_bitmain_response(uint32_t nonce) {
    unsigned char buf_nonce[11] = {0xAA, 0x55, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0};
    buf_nonce[4] = (nonce >> 24) & 0xFF; 
    buf_nonce[5] = (nonce >> 16) & 0xFF;
    buf_nonce[6] = (nonce >> 8)  & 0xFF; 
    buf_nonce[7] = (nonce >> 0)  & 0xFF;
    buf_nonce[10] = get_bitmain_crc5(buf_nonce, 10);
    
    if (xSemaphoreTake(uart_mutex, portMAX_DELAY)) {
        uart_write_bytes(SIM_UART_PORT, buf_nonce, 11);
        xSemaphoreGive(uart_mutex);
    }
}

// ============================================================================
// TAREFAS MULTICORE
// ============================================================================

// TAREFA MAESTRO: Fica ouvindo a UART, simulando o chip BM13xx
static void uart_listener_task(void *arg) {
    uart_config_t uart_config = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(SIM_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SIM_UART_PORT, SIM_TXD_PIN, SIM_RXD_PIN, -1, -1));
    ESP_ERROR_CHECK(uart_driver_install(SIM_UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));

    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);
    ESP_LOGI(TAG, "Músculo (Emulador BM13xx) Iniciado. Aguardando Trabalho da UART...");

    while (1) {
        int len = uart_read_bytes(SIM_UART_PORT, data, 128, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            if (strstr((char*)data, "PING") != NULL) { 
                uart_write_bytes(SIM_UART_PORT, "PONG", 4); 
                continue; 
            }

            if (len >= 6 && data[2] == 0x21) {
                uart_flush(SIM_UART_PORT);

                // Copia os dados intocados diretamente para as variáveis
                memcpy(g_midstate_raw, &data[4], 32);
                memcpy(g_data_raw, &data[36], 12);
                
                g_job_version++; 
                ESP_LOGI(TAG, "=> Novo Bloco (Job %lu) recebido da UART!", (unsigned long)g_job_version);
            }
            else if (data[2] == 0x51 && data[5] == 0x28) {
                uart_wait_tx_done(SIM_UART_PORT, 1000 / portTICK_PERIOD_MS);
                uart_set_baudrate(SIM_UART_PORT, 1000000);
            }
        }
    }
}

// TAREFA FPGA: Interliga o FPGA via SPI
static void fpga_miner_task(void *arg) {
    ESP_LOGI(TAG, "Bancada de Teste do FPGA Iniciada! Lendo a saída do SHA-256...");
    
    uint8_t genesis_midstate[32] = {
        0xBC, 0x90, 0x9A, 0x33, 0x63, 0x58, 0xBF, 0xF0, 
        0x90, 0xCC, 0xAC, 0x7D, 0x1E, 0x59, 0xCA, 0xA8, 
        0xC3, 0xC8, 0xD8, 0xE9, 0x4F, 0x01, 0x03, 0xC8, 
        0x96, 0xB1, 0x87, 0x36, 0x47, 0x19, 0xF9, 0x1B
    };

    // DADOS CORRIGIDOS: Exatamente os bytes originais do Cabeçalho do Bitcoin!
    uint8_t genesis_data[12] = {
        0x4A, 0x5E, 0x1E, 0x4B, 
        0x29, 0xAB, 0x5F, 0x49, 
        0xFF, 0xFF, 0x00, 0x1D  
    };

    bool job_enviado = false;

    while (1) {
        if (!job_enviado) {
            uint8_t tx_buf[47] = {0};
            tx_buf[0] = 0x01; 
            tx_buf[1] = 0x01; // ID 1 (Dispara o Motor)
            memcpy(&tx_buf[2], genesis_midstate, 32);
            memcpy(&tx_buf[34], genesis_data, 12);
            tx_buf[46] = calculate_spi_crc5(tx_buf, 46 * 8);

            spi_transaction_t t;
            memset(&t, 0, sizeof(t));
            t.length = 47 * 8; 
            t.tx_buffer = tx_buf;
            
            spi_device_transmit(fpga_spi, &t);
            ESP_LOGI(TAG, "=> Pacote injetado! Motor a calcular e a CONGELAR...");
            job_enviado = true;
        }

        // POLLING
        uint8_t poll_cmd[7] = {0x02, 0, 0, 0, 0, 0, 0}; 
        uint8_t poll_rx[7]  = {0};

        spi_transaction_t t_poll;
        memset(&t_poll, 0, sizeof(t_poll));
        t_poll.length = 7 * 8; 
        t_poll.tx_buffer = poll_cmd;
        t_poll.rx_buffer = poll_rx;

        spi_device_transmit(fpga_spi, &t_poll);

        uint8_t status = poll_rx[1];
        if (status == 0x01) { 
            uint32_t hash_h0 = (poll_rx[2] << 24) | (poll_rx[3] << 16) | (poll_rx[4] << 8) | poll_rx[5];
            uint8_t fpga_crc = poll_rx[6];
            uint8_t calc_crc = calculate_spi_crc5(&poll_rx[1], 5 * 8);

            if (fpga_crc == calc_crc) {
                // Comparamos com H0 esperado (0x6FE28C0A)
                if (hash_h0 == 0x6FE28C0A) {
                    ESP_LOGW(TAG, ">>>> MATEMÁTICA PERFEITA! HASH GERADO: 0x%08X <<<<", (unsigned int)hash_h0);
                } else {
                    ESP_LOGE(TAG, "[DEBUG] Hash Incorreto: 0x%08X (Esperado: 0x6FE28C0A)", (unsigned int)hash_h0);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}


void app_main(void) {
    uart_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Inicializando Barramento SPI VSPI (Modo Mestre)...");
    
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 100 
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_SPEED_HZ,
        .mode = 0,                  
        .spics_io_num = PIN_NUM_CS, 
        .queue_size = 1,            
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &fpga_spi));
    ESP_LOGI(TAG, "SPI Pronto a %d Hz!", SPI_SPEED_HZ);

    xTaskCreatePinnedToCore(uart_listener_task, "uart_maestro", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(fpga_miner_task, "fpga_miner", 4096, NULL, 4, NULL, 1);
}