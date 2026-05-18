#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

// VELOCIDADE CORRIGIDA PARA INTEGRIDADE DE SINAL NOS JUMPERS!
#define SPI_SPEED_HZ (2500 * 1000) 

static const char *TAG = "GENESIS_TEST";
spi_device_handle_t fpga_spi;

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

static void fpga_miner_task(void *arg) {
    ESP_LOGI(TAG, "Iniciando Teste do Bloco Genesis (SPI a 2.5 MHz)...");

    // =========================================================================
    // 1. GERANDO O MIDSTATE EXATO DO BLOCO GENESIS
    // =========================================================================
    uint8_t genesis_64[64] = {
        0x01, 0x00, 0x00, 0x00, // Version
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Prev Hash
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x3B, 0xA3, 0xED, 0xFD, 0x7A, 0x7B, 0x12, 0xB2, 0x7A, 0xC7, 0x2C, 0x3E, 0x67, 0x76, 0x8F, 0x61, // Merkle (pt 1)
        0x7F, 0xC8, 0x1B, 0xC3, 0x88, 0x8A, 0x51, 0x32, 0x3A, 0x9F, 0xB8, 0xAA
    };

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, genesis_64, 64);
    
    uint8_t gen_midstate[32];
    for(int i=0; i<8; i++) {
        uint32_t s = ctx.state[i];
        gen_midstate[i*4]   = (s >> 24) & 0xFF;
        gen_midstate[i*4+1] = (s >> 16) & 0xFF;
        gen_midstate[i*4+2] = (s >> 8)  & 0xFF;
        gen_midstate[i*4+3] = (s)       & 0xFF;
    }
    mbedtls_sha256_free(&ctx);

    // =========================================================================
    // 2. OS 12 BYTES RESTANTES DO CABEÇALHO DO GENESIS
    // =========================================================================
    uint8_t gen_data[12] = {
        0x4B, 0x1E, 0x5E, 0x4A, // Fim da Merkle Root
        0x29, 0xAB, 0x5F, 0x49, // Timestamp (1231006505)
        0xFF, 0xFF, 0x00, 0x1D  // Bits de Dificuldade
    };

    bool genesis_sent = false;

    while (1) {
        // Envia o trabalho do Genesis repetidamente a cada 1 segundo só para garantir
        if (!genesis_sent) {
            uint8_t tx_buf[47] = {0};
            tx_buf[0] = 0x01; // OPCODE: WRITE JOB
            tx_buf[1] = 0x01; // Job ID: 1

            memcpy(&tx_buf[2], gen_midstate, 32);
            memcpy(&tx_buf[34], gen_data, 12);

            tx_buf[46] = calculate_spi_crc5(tx_buf, 46 * 8);

            spi_transaction_t t;
            memset(&t, 0, sizeof(t));
            t.length = 47 * 8; 
            t.tx_buffer = tx_buf;
            spi_device_transmit(fpga_spi, &t);
            
            ESP_LOGI(TAG, "=> Bloco Genesis Injetado no FPGA! [CRC: 0x%02X]", tx_buf[46]);
            // genesis_sent = true; // Remova o comentário se quiser enviar apenas 1 vez. Deixando rodar pra forçar testes.
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // POLLING DO NONCE
        uint8_t poll_cmd[7] = {0x02, 0, 0, 0, 0, 0, 0}; 
        uint8_t poll_rx[7]  = {0};

        spi_transaction_t t_poll;
        memset(&t_poll, 0, sizeof(t_poll));
        t_poll.length = 7 * 8; 
        t_poll.tx_buffer = poll_cmd;
        t_poll.rx_buffer = poll_rx;
        spi_device_transmit(fpga_spi, &t_poll);

        if (poll_rx[1] == 0x01) { 
            uint32_t nonce = (poll_rx[2] << 24) | (poll_rx[3] << 16) | (poll_rx[4] << 8) | poll_rx[5];
            uint8_t fpga_crc = poll_rx[6];
            uint8_t calc_crc = calculate_spi_crc5(&poll_rx[1], 5 * 8);

            if (fpga_crc == calc_crc) {
                // O Nonce Dourado do Genesis é: 0x1DAC2B7C
                ESP_LOGW(TAG, "$$$ SUCESSO! NONCE ENCONTRADO: 0x%08X $$$", (unsigned int)nonce);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

void app_main(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO, .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK, .max_transfer_sz = 100 
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_SPEED_HZ, .mode = 0, .spics_io_num = PIN_NUM_CS, .queue_size = 1,            
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &fpga_spi));

    xTaskCreatePinnedToCore(fpga_miner_task, "fpga_miner", 4096, NULL, 4, NULL, 1);
}