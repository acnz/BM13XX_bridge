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

// Pode testar a 5 MHz sem medo!
#define SPI_SPEED_HZ (5 * 1000 * 1000) 

static const char *TAG = "SPI_CRC_TEST";

// Função em C que imita perfeitamente o hardware LFSR do FPGA
uint8_t calculate_crc5(uint8_t *data, int num_bits) {
    uint8_t crc = 0x1F;
    for (int i = 0; i < num_bits; i++) {
        uint8_t byte = data[i / 8];
        uint8_t bit = (byte >> (7 - (i % 8))) & 1; // Lê bit a bit (MSB first)
        
        uint8_t inv = bit ^ ((crc >> 4) & 1);
        crc = (crc << 1) & 0x1F; // Desloca mantendo 5 bits
        if (inv) {
            crc ^= 0x05; // Aplica o polinômio 00101
        }
    }
    return crc;
}

void app_main(void)
{
    spi_device_handle_t spi;
    
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

    spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI3_HOST, &devcfg, &spi);

    ESP_LOGI(TAG, "Iniciando teste de Payload e CRC a %d Hz", SPI_SPEED_HZ);

    uint8_t payload[46]; // 45 bytes de dados + 1 byte para "puxar" o CRC
    uint8_t resposta[46];

    while (1) {
        memset(payload, 0, sizeof(payload));
        memset(resposta, 0, sizeof(resposta));

        // Vamos gerar dados dinâmicos para ter certeza que o CRC muda
        payload[0] = 0x01; // Job ID
        for(int i = 1; i < 45; i++) {
            payload[i] = (uint8_t)(esp_random() % 256); // Gera lixo aleatório simulando o Midstate
        }

        // Calcula o CRC esperado usando o ESP32 (Apenas dos 45 bytes / 360 bits!)
        uint8_t expected_crc = calculate_crc5(payload, 360);

        // Envia os 46 bytes (O último é 0x00 para manter o clock rodando)
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));       
        t.length = 46 * 8;                   
        t.tx_buffer = payload;       
        t.rx_buffer = resposta;       

        spi_device_transmit(spi, &t);

        // O FPGA coloca o CRC no 46º byte da resposta!
        uint8_t fpga_crc = resposta[45];

        if (fpga_crc == expected_crc) {
            ESP_LOGI(TAG, "[SUCESSO] CRC FPGA: 0x%02X | CRC ESP32: 0x%02X", fpga_crc, expected_crc);
        } else {
            ESP_LOGE(TAG, "[ERRO] CRC FPGA: 0x%02X | CRC ESP32: 0x%02X", fpga_crc, expected_crc);
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}