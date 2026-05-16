#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"

#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#define SPI_SPEED_HZ (5 * 1000 * 1000) 

static const char *TAG = "MASTER_CRC";

// Nossa função matemática do LFSR do FPGA em C
uint8_t calculate_crc5(uint8_t *data, int num_bits) {
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

void send_new_job(spi_device_handle_t spi, uint8_t job_id) {
    // 47 Bytes agora! (1 CMD + 1 ID + 32 Mid + 12 Data + 1 CRC)
    uint8_t tx_buf[47] = {0};

    tx_buf[0] = 0x01;
    tx_buf[1] = job_id;
    for (int i = 2; i < 46; i++) tx_buf[i] = (uint8_t)(esp_random() % 256); // Dados loucos!

    // Calcula o CRC dos primeiros 46 bytes (368 bits)
    tx_buf[46] = calculate_crc5(tx_buf, 46 * 8);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 47 * 8; // 376 bits
    t.tx_buffer = tx_buf;

    spi_device_transmit(spi, &t);
    ESP_LOGI(TAG, "=> Enviado Job %d com CRC [0x%02X]", job_id, tx_buf[46]);
}

void poll_nonce(spi_device_handle_t spi) {
    // 7 Bytes agora! (1 CMD + 5 Dummy para payload + 1 Dummy para puxar o CRC do FPGA)
    uint8_t tx_buf[7] = {0x02, 0, 0, 0, 0, 0, 0}; 
    uint8_t rx_buf[7] = {0};

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 7 * 8; // 56 bits
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;

    spi_device_transmit(spi, &t);

    uint8_t status = rx_buf[1];
    if (status == 0x01) { 
        uint32_t nonce = (rx_buf[2] << 24) | (rx_buf[3] << 16) | (rx_buf[4] << 8) | rx_buf[5];
        uint8_t fpga_crc = rx_buf[6];

        // O FPGA calculou o CRC em cima do Status + Nonce (rx_buf 1 a 5)
        uint8_t calc_crc = calculate_crc5(&rx_buf[1], 5 * 8);

        if (fpga_crc == calc_crc) {
            ESP_LOGW(TAG, "[TX CRC OK] Nonce Valido Recebido: 0x%08X", (unsigned int)nonce);
        } else {
            ESP_LOGE(TAG, "[TX CRC ERROR] Nonce Corrompido! Recebi CRC 0x%02X mas esperava 0x%02X", fpga_crc, calc_crc);
        }
    }
}

void app_main(void)
{
    spi_device_handle_t spi;
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO, .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK, .max_transfer_sz = 100 
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_SPEED_HZ, .mode = 0, .spics_io_num = PIN_NUM_CS, .queue_size = 1,            
    };

    spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI3_HOST, &devcfg, &spi);

    uint32_t poll_counter = 0;
    uint8_t  current_job_id = 0;

    // Envia o primeiro Job imediatamente
    send_new_job(spi, ++current_job_id);

    while (1) {
        // Envia um job a cada ~10 segundos (cada loop = ~10ms)
        if (poll_counter > 0 && poll_counter % 1000 == 0) {
            current_job_id++;
            send_new_job(spi, current_job_id);
            
            // Quer testar o "Panico do LED"? Descomente a linha abaixo para injetar erro de proposito:
            // send_new_job(spi, current_job_id); /* e lá dentro da função, some +1 no tx_buf[46] */
        }

        poll_nonce(spi);
        poll_counter++;
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}