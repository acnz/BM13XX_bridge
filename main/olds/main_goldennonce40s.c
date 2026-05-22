#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_crc.h" // Algoritmo de CRC32 nativo da ROM do ESP32

// Configurações da UART0 (Comunicação com o PC / Proxy Stratum)
#define SIM_TXD_PIN      CONFIG_UART_TXD
#define SIM_RXD_PIN      CONFIG_UART_RXD
#define SIM_UART_PORT    CONFIG_UART_PORT_NUM

// Configurações da UART1 (Comunicação com a sua FPGA Gowin)
#define FPGA_UART_PORT   UART_NUM_2
#define FPGA_TXD_PIN     5  // Conecte ao pino URX da FPGA
#define FPGA_RXD_PIN     23  // Conecte ao pino UTX da FPGA
#define FPGA_BAUD_RATE   115200 // Deve bater com o baud_clk da FPGA

#define BUF_SIZE 1024

static const char *TAG = "BM13XX_FPGA_Test";

// Mutex para evitar colisões na transmissão da Serial com o PC
static SemaphoreHandle_t uart_mutex;

// ============================================================================
// FUNÇÕES AUXILIARES DE PROTOCOLO (BITMAIN COMPATIBLE)
// ============================================================================

// Checksum CRC5 para empacotamento da resposta da Bitmain para o PC
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

// Envia o share/nonce encontrado para o PC
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
        ESP_LOGE(TAG, "=> SHARE ENVIADO AO PC (Aguardando recusa da Pool): %08lx", (unsigned long)nonce);
    }
}

// ============================================================================
// LOOP DE INJEÇÃO EXTRA TREMO (CORE 0 - GERENCIADOR DA FPGA)
// ============================================================================
static void miner_task(void *arg) {
    int core_id = (int)arg;
    
    // Deixamos apenas o Core 0 cuidando da FPGA para evitar dupla transmissão serial
    if (core_id != 0) {
        vTaskDelay(portMAX_DELAY);
    }

    // Inicialização da UART com a FPGA
    uart_config_t fpga_uart_config = {
        .baud_rate = FPGA_BAUD_RATE, 
        .data_bits = UART_DATA_8_BITS, 
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, 
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(FPGA_UART_PORT, &fpga_uart_config));
    ESP_ERROR_CHECK(uart_set_pin(FPGA_UART_PORT, FPGA_TXD_PIN, FPGA_RXD_PIN, -1, -1));
    ESP_ERROR_CHECK(uart_driver_install(FPGA_UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));

    uint8_t fpga_packet[60];
    uint8_t fpga_rx_buf[128];
    uint32_t last_send_tick = 0;

    // ------------------------------------------------------------------------
    // CONSTRUÇÃO DO SEU VETOR DE TESTE COM CORREÇÃO DE ALINHAMENTO VERILOG
    // ------------------------------------------------------------------------
    memset(fpga_packet, 0, 60);
    fpga_packet[0] = 60;   // msg_length total
    fpga_packet[1] = 0x00; // Padding
    fpga_packet[2] = 0x00; // Padding
    fpga_packet[3] = 0x04; // msg_type = MSG_PUSH_JOB

    // ------------------------------------------------------------------------
    // CONSTRUÇÃO DO SEU VETOR DE TESTE (AGORA PADRONIZADO!)
    // ------------------------------------------------------------------------
    memset(fpga_packet, 0, 60);
    fpga_packet[0] = 60;   // msg_length total 
    fpga_packet[1] = 0x00; // Padding
    fpga_packet[2] = 0x00; // Padding
    fpga_packet[3] = 0x04; // msg_type = MSG_PUSH_JOB 

    // Alvo: tx_noncemax = 32'h0E33356E
    // Invertemos dinamicamente (LSB primeiro)
    uint8_t raw_noncemax[4] = {0x0E, 0x33, 0x35, 0x6E};
    for (int i = 0; i < 4; i++) fpga_packet[4 + i] = raw_noncemax[3 - i];
    
    // tx_noncemin = 32'h00000000
    uint8_t raw_noncemin[4] = {0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 4; i++) fpga_packet[8 + i] = raw_noncemin[3 - i];

    // tx_data = 96'h2194261a9395e64dbed17115
    // Escrito na ORDEM NORMAL de leitura (MSB -> LSB)
    uint8_t raw_data[12] = {
        0x21, 0x94, 0x26, 0x1A, 0x93, 0x95, 0xE6, 0x4D, 0xBE, 0xD1, 0x71, 0x15
    };
    // Laço for inverte a ordem (0x15 vai para o índice 12 do pacote)
    for (int i = 0; i < 12; i++) {
        fpga_packet[12 + i] = raw_data[11 - i];
    }

    // tx_midstate = 256'h228ea4732a3c9ba860c009cda7252b9161a5e75ec8c582a5f106abb3af41f790
    // Escrito na ORDEM NORMAL de leitura (MSB -> LSB)
    uint8_t raw_midstate[32] = {
        0x22, 0x8E, 0xA4, 0x73, 0x2A, 0x3C, 0x9B, 0xA8,
        0x60, 0xC0, 0x09, 0xCD, 0xA7, 0x25, 0x2B, 0x91,
        0x61, 0xA5, 0xE7, 0x5E, 0xC8, 0xC5, 0x82, 0xA5,
        0xF1, 0x06, 0xAB, 0xB3, 0xAF, 0x41, 0xF7, 0x90
    };
    // Laço for inverte a ordem (0x90 vai para o índice 24 do pacote)
    for (int i = 0; i < 32; i++) {
        fpga_packet[24 + i] = raw_midstate[31 - i];
    }


    // Cálculo dinâmico do Checksum CRC32 sobre os cabeçalhos e payloads
    uint32_t crc = esp_rom_crc32_le(0, fpga_packet, 56);
    memcpy(&fpga_packet[56], &crc, 4);

    ESP_LOGW(TAG, "Driver de Teste Periódico Rodando. Injetando bloco a cada 40s...");

    while(1) {
        uint32_t current_tick = xTaskGetTickCount();

        // Disparador Temporal: Envia o PUSH_JOB a cada 10.000 milissegundos
        if (last_send_tick == 0 || (current_tick - last_send_tick) >= (40000 / portTICK_PERIOD_MS)) {
            last_send_tick = current_tick;
            
            // Limpa lixos residuais da UART antes de mandar para evitar dessincronização
            uart_flush(FPGA_UART_PORT);
            
            uart_write_bytes(FPGA_UART_PORT, fpga_packet, 60);
            ESP_LOGW(TAG, ">> [Loop 40s] Bloco de Teste enviado para a FPGA!");
        }

        // Fica escutando as respostas elétricas vindas do pino UTX da FPGA
        int len = uart_read_bytes(FPGA_UART_PORT, fpga_rx_buf, sizeof(fpga_rx_buf), 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            ESP_LOGI(TAG, "FPGA respondeu com %d bytes!", len);
            
            // Se a UART da FPGA encontrou e cuspiu o pacote com o Nonce correto:
            if (len == 4) {
                uint32_t found_nonce = ((uint32_t)fpga_rx_buf[3] << 24) |
                                       ((uint32_t)fpga_rx_buf[2] << 16) |
                                       ((uint32_t)fpga_rx_buf[1] << 8)  |
                                       ((uint32_t)fpga_rx_buf[0] << 0);
                
                ESP_LOGE(TAG, "!!! TICKET DOURADO CAPTURADO DA FPGA !!! Nonce encontrado: %08lx", (unsigned long)found_nonce);
                
                // Repassa o Nonce para o PC para fechar o teste de mesa completo
                send_bitmain_response(found_nonce);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// MAESTRO CORE 1: MANTÉM A CONEXÃO SERIAL DO PC ATIVA
// ============================================================================
static void uart_listener_task(void *arg) {
    uart_config_t uart_config = {
        .baud_rate = 115200, 
        .data_bits = UART_DATA_8_BITS, 
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, 
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(SIM_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SIM_UART_PORT, SIM_TXD_PIN, SIM_RXD_PIN, -1, -1));
    ESP_ERROR_CHECK(uart_driver_install(SIM_UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));

    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);
    ESP_LOGI(TAG, "Maestro de Comunicação Conectado. Mantendo canal do PC aberto...");

    while (1) {
        int len = uart_read_bytes(SIM_UART_PORT, data, 128, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            // Responde aos PINGs do computador automaticamente para travar o software em modo ativo
            if (strstr((char*)data, "PING") != NULL) { 
                uart_write_bytes(SIM_UART_PORT, "PONG", 4); 
                continue; 
            }

            // Ignoramos o PUSH_JOB real do PC por enquanto para focar estritamente no nosso loop de 10s
            if (len >= 6 && data[2] == 0x21) {
                ESP_LOGI(TAG, "Trabalho real do PC ignorado (Modo Loop de Teste FPGA ativo).");
                uart_flush(SIM_UART_PORT);
            }
            // Negociação automática de velocidade exigida pelo CGMiner/NerdMiner
            else if (data[2] == 0x51 && data[5] == 0x28) {
                uart_wait_tx_done(SIM_UART_PORT, 1000 / portTICK_PERIOD_MS);
                uart_set_baudrate(SIM_UART_PORT, 1000000);
            }
        }
    }
}

// ============================================================================
// ENTRADA DO PROGRAMA ESP-IDF
// ============================================================================
void app_main(void) {
    uart_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, " INICIANDO COMUNICAÇÃO DE BANCADA ESP32<->FPGA");
    ESP_LOGI(TAG, "=============================================");

    // Inicializa o Maestro para manter o canal do PC aceso no Core 1
    xTaskCreatePinnedToCore(uart_listener_task, "uart_task", 8192, NULL, 5, NULL, 1);
    
    // Inicializa o injetor de teste e escuta da FPGA no Core 0
    xTaskCreatePinnedToCore(miner_task, "miner_core0", 8192, (void*)0, 10, NULL, 0);
    xTaskCreatePinnedToCore(miner_task, "miner_core1", 8192, (void*)1, 10, NULL, 1);
}