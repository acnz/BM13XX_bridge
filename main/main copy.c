#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_crc.h" // Usa o algoritmo de CRC32 ultra-rápido da ROM do ESP32

// Configurações da UART0 (Comunicação com o PC / Stratum / NerdMiner)
#define SIM_TXD_PIN      CONFIG_UART_TXD
#define SIM_RXD_PIN      CONFIG_UART_RXD
#define SIM_UART_PORT    CONFIG_UART_PORT_NUM

// Configurações da UART1 (Comunicação Direta com a FPGA Tang Primer)
#define FPGA_UART_PORT   UART_NUM_2
#define FPGA_TXD_PIN     5  // Conecte ao pino URX da FPGA
#define FPGA_RXD_PIN     23  // Conecte ao pino UTX da FPGA
#define FPGA_BAUD_RATE   115200 // Deve bater exatamente com o baud_clk da FPGA

#define BUF_SIZE 1024

static const char *TAG = "BM13XX_FPGA_Driver";

// Mutex para proteção do barramento de transmissão com o PC
static SemaphoreHandle_t uart_mutex;

// Variáveis Globais do Trabalho (Armazenam os bytes brutos vindos do PC)
volatile uint32_t g_job_version = 0;
uint8_t g_midstate_bytes[32];
uint8_t g_data_bytes[12];

// ============================================================================
// FUNÇÕES AUXILIARES DE COMUNICAÇÃO
// ============================================================================

// Calcula o checksum CRC5 exigido pelo protocolo de resposta da Bitmain para o PC
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

// Envia o Nonce premiado de volta para o software de mineração do PC
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
        ESP_LOGE(TAG, "=> NONCE ENVIADO AO PC COM SUCESSO: %08lx", (unsigned long)nonce);
    }
}

// ============================================================================
// OPERÁRIO CORE 0: GERENCIADOR E DRIVER DA FPGA
// ============================================================================
static void miner_task(void *arg) {
    int core_id = (int)arg;
    
    // Como a FPGA faz o cálculo pesado sozinha, não precisamos de duas CPUs disputando a UART.
    // Colocamos o Core 1 em suspensão eterna para economizar energia e focar o Core 0 na FPGA.
    if (core_id != 0) {
        ESP_LOGW(TAG, "[Core %d] Entrando em modo passivo (FPGA gerenciada pelo Core 0).", core_id);
        while(1) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    uint32_t local_version = 0;
    uint8_t fpga_packet[60];
    uint8_t fpga_rx_buf[128];

    ESP_LOGI(TAG, "[Core 0] Inicializando fiação da UART1 com a FPGA...");

    // Configura e instala o driver da UART com a FPGA
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

    ESP_LOGI(TAG, "[Core 0] Conexão com a FPGA estabelecida! Aguardando ordens do PC...");

    while(1) {
        // Se o Maestro (Core 1) receber um bloco novo do PC, o Core 0 acorda para repassar à FPGA
        if (g_job_version != local_version) {
            local_version = g_job_version;
            ESP_LOGW(TAG, "Novo trabalho recebido do PC. Montando pacote MSG_PUSH_JOB (60 bytes)...");

            // Limpa o buffer do pacote
            memset(fpga_packet, 0, 60);
            
            // Montagem do Cabeçalho esperado pela máquina de estados do uart_comm.v
            fpga_packet[0] = 60;   // msg_length (60 bytes totais: 4 header + 52 payload + 4 crc)
            fpga_packet[1] = 0x00; // Padding interno
            fpga_packet[2] = 0x00; // Padding interno
            fpga_packet[3] = 0x04; // msg_type = MSG_PUSH_JOB (0x04)

            // NonceMax (Bytes 4..7) -> Varredura completa até o teto máximo de 32 bits
            fpga_packet[4] = 0xFF; fpga_packet[5] = 0xFF; fpga_packet[6] = 0xFF; fpga_packet[7] = 0xFF;
            
            // NonceMin (Bytes 8..11) -> Início do escaneamento elétrico a partir do zero
            fpga_packet[8] = 0x00; fpga_packet[9] = 0x00; fpga_packet[10] = 0x00; fpga_packet[11] = 0x00;


            // Data (Bytes 12..23) -> Injeção direta dos 12 bytes úteis (nTime, nBits, Merkle final)    
            // Laço for inverte a ordem (0x15 vai para o índice 12 do pacote)
            for (int i = 0; i < 12; i++) {
                fpga_packet[12 + i] = g_data_bytes[11 - i];
            }
            //memcpy(&fpga_packet[12], g_data_bytes, 12);

            // Midstate (Bytes 24..55) -> Injeção direta dos 32 bytes do Midstate pré-calculado
            // Laço for inverte a ordem (0x90 vai para o índice 24 do pacote)
            for (int i = 0; i < 32; i++) {
                fpga_packet[24 + i] = g_midstate_bytes[31 - i];
            }
            //memcpy(&fpga_packet[24], g_midstate_bytes, 32);

            // Calcula o CRC32 sobre os primeiros 56 bytes usando o hardware do ESP32
            uint32_t crc = esp_rom_crc32_le(0, fpga_packet, 56);
            memcpy(&fpga_packet[56], &crc, 4);

            // Envia os 60 bytes estruturados para o pino URX da FPGA
            uart_write_bytes(FPGA_UART_PORT, fpga_packet, 60);
            ESP_LOGI(TAG, "Pacote enviado à FPGA. Motor de hash ativado!");
        }

        // Fica escutando se a FPGA enviou alguma resposta pelo pino UTX
        int len = uart_read_bytes(FPGA_UART_PORT, fpga_rx_buf, sizeof(fpga_rx_buf), 10 / portTICK_PERIOD_MS);
        if (len > 0) {
            ESP_LOGI(TAG, "FPGA respondeu com %d bytes no barramento!", len);
            
            // Se você configurou sua FPGA para cuspir o Nonce premiado bruto em formato de 4 bytes:
            if (len == 4) {
                uint32_t found_nonce = ((uint32_t)fpga_rx_buf[3] << 24) |
                                       ((uint32_t)fpga_rx_buf[2] << 16) |
                                       ((uint32_t)fpga_rx_buf[1] << 8)  |
                                       ((uint32_t)fpga_rx_buf[0] << 0);
                
                ESP_LOGE(TAG, "!!! GOLDEN NONCE ENCONTRADO PELA FPGA !!! Valor: %08lx", (unsigned long)found_nonce);
                
                // Envia o ticket premiado de volta para o PC
                send_bitmain_response(found_nonce);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS); // Evita estouro de Watchdog no Core 0
    }
}

// ============================================================================
// MAESTRO CORE 1: ESCUTA A PORTA SERIAL DO PC (PROTOCOLO BITMAIN 0x21)
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
    ESP_LOGI(TAG, "Maestro do Sistema Ativo. Aguardando comando 0x21 do PC...");

    while (1) {
        int len = uart_read_bytes(SIM_UART_PORT, data, 128, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            // Responde às checagens de pulso (PING) do PC para não cair a conexão
            if (strstr((char*)data, "PING") != NULL) { 
                uart_write_bytes(SIM_UART_PORT, "PONG", 4); 
                continue; 
            }

            // Detecta o comando clássico da Bitmain: 0x21 (SEND_WORK)
            if (len >= 6 && data[2] == 0x21) {
                ESP_LOGE(TAG, "=> SEND_WORK Recebido do PC! Extraindo payloads brutos...");
                uart_flush(SIM_UART_PORT);

                // Captura os 32 bytes do Midstate pré-calculado (Vem a partir do byte index 4)
                memcpy(g_midstate_bytes, &data[4], 32);
                
                // Captura os 12 bytes restantes de dados do bloco (Vem a partir do byte index 36)
                memcpy(g_data_bytes, &data[36], 12);
                
                // Dispara o gatilho incrementando a versão do Job para acordar a tarefa da FPGA
                g_job_version++; 
            }
            // Trata a negociação automática de Baudrate padrão exigida pela Bitmain
            else if (data[2] == 0x51 && data[5] == 0x28) {
                uart_wait_tx_done(SIM_UART_PORT, 1000 / portTICK_PERIOD_MS);
                uart_set_baudrate(SIM_UART_PORT, 1000000);
                ESP_LOGW(TAG, "Baudrate alterado para 1.000.000 bps por ordem do PC.");
            }
        }
    }
}

// ============================================================================
// ENTRADA PRINCIPAL DO FIRMWARE
// ============================================================================
void app_main(void) {
    uart_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "   INICIANDO MAESTRO CO-PROCESSADOR DE FPGA      ");
    ESP_LOGI(TAG, "=================================================");

    // Cria a tarefa de escuta do PC (Maestro) presa no Core 1
    xTaskCreatePinnedToCore(uart_listener_task, "uart_listener", 8192, NULL, 5, NULL, 1);
    
    // Cria as tarefas dos operários (O Core 0 gerenciará o hardware, o Core 1 dormirá)
    xTaskCreatePinnedToCore(miner_task, "miner_core0", 8192, (void*)0, 10, NULL, 0);
    xTaskCreatePinnedToCore(miner_task, "miner_core1", 8192, (void*)1, 10, NULL, 1);
}