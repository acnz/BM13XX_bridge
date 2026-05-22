#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"

// Pinos físicos
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

// Barramento cravado em 5 MHz com margem de segurança
#define SPI_SPEED_HZ (5 * 1000 * 1000) 

static const char *TAG = "MINER_MASTER";

// =========================================================================
// FUNÇÃO 1: ENVIA UM TRABALHO NOVO (OPCODE 0x01)
// =========================================================================
void send_new_job(spi_device_handle_t spi, uint8_t job_id) {
    // 1 byte Opcode + 1 byte ID + 32 bytes Midstate + 12 bytes Data = 46 bytes
    uint8_t tx_buf[46] = {0};

    tx_buf[0] = 0x01;       // O nosso OPCODE mágico: WRITE JOB
    tx_buf[1] = job_id;     // ID da missão atual

    // Preenchendo com dados "Dummy" (Falsos) para testar o FSM do FPGA.
    // Quando você ligar o WiFi, esses dados virão do servidor da Pool (Stratum).
    for (int i = 2; i < 34; i++) tx_buf[i]  = 0xAA; // Midstate Falso
    for (int i = 34; i < 46; i++) tx_buf[i] = 0xBB; // Dados Falsos

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 46 * 8; // Tamanho exato em bits (368 bits)
    t.tx_buffer = tx_buf;
    t.rx_buffer = NULL; // Não precisamos ler nada do FPGA nesta transação

    esp_err_t ret = spi_device_transmit(spi, &t);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "=> Novo Job Enviado! Job ID: %d", job_id);
    } else {
        ESP_LOGE(TAG, "Erro ao enviar Job!");
    }
}

// =========================================================================
// FUNÇÃO 2: PERGUNTA SE O FPGA ACHOU UM NONCE (OPCODE 0x02)
// =========================================================================
void poll_nonce(spi_device_handle_t spi) {
    // Comando 0x02 + 5 bytes de Zeros para manter o clock batendo
    uint8_t tx_buf[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00}; 
    uint8_t rx_buf[6] = {0};

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 6 * 8; // Tamanho exato em bits (48 bits)
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;

    esp_err_t ret = spi_device_transmit(spi, &t);

    if (ret == ESP_OK) {
        // rx_buf[0] é lixo (foi lido enquanto o Opcode estava sendo enviado)
        uint8_t status = rx_buf[1]; // O byte de Status devolvido pelo FPGA
        
        if (status == 0x01) { 
            // O FPGA avisou que a prateleira da FIFO tem um Nonce!
            // Agora remontamos os 4 bytes em um número de 32 bits
            uint32_t nonce = (rx_buf[2] << 24) | (rx_buf[3] << 16) | (rx_buf[4] << 8) | rx_buf[5];
            
            // Log verde de aviso (Você pode usar ESP_LOGI, mas ESP_LOGW destaca na cor amarela!)
            ESP_LOGW(TAG, "$$$ SUCESSO! NONCE ENCONTRADO NO FPGA: 0x%08X $$$", (unsigned int)nonce);
        }
    }
}

// =========================================================================
// CÓDIGO PRINCIPAL
// =========================================================================
void app_main(void)
{
    esp_err_t ret;
    spi_device_handle_t spi;

    ESP_LOGI(TAG, "Inicializando Barramento SPI VSPI (Modo Mestre)...");

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 100 // Buffer generoso
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_SPEED_HZ,
        .mode = 0,                  // SPI Mode 0
        .spics_io_num = PIN_NUM_CS, 
        .queue_size = 1,            
    };

    // Inicializa o barramento
    ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "SPI Pronto a %d Hz! Iniciando Loop de Mineração...", SPI_SPEED_HZ);

    uint32_t poll_counter = 0;
    uint8_t  current_job_id = 0;

    // Loop Infinito do Músculo
    while (1) {
        
        // A cada 1000 ciclos de polling (como cada polling tem um delay de 10ms, isso dá 10 segundos)
        // Nós enviamos uma nova missão para o FPGA.
        if (poll_counter % 1000 == 0) {
            current_job_id++;
            send_new_job(spi, current_job_id);
        }

        // Vai até o FPGA e pergunta: "Tem algum Nonce na FIFO?"
        poll_nonce(spi);

        poll_counter++;

        // Atraso de 10 milissegundos para não sobrecarregar o barramento e o ESP32
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}