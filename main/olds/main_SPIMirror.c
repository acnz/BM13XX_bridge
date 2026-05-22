#include <stdio.h>
#include <string.h>
#include <stdbool.h>
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

// Vamos testar na velocidade máxima de trabalho: 5 MHz
#define SPI_SPEED_HZ (2.5 * 1000 * 1000) 

static const char *TAG = "SPI_LOOPBACK";
spi_device_handle_t fpga_spi;

static void loopback_test_task(void *arg) {
    ESP_LOGI(TAG, "Iniciando Teste Rigoroso de Loopback SPI (47 Bytes)...");

    uint8_t tx_buf[47] = {0};
    uint8_t rx_buf[47] = {0};
    uint8_t expected_rx[47] = {0};
    uint32_t round_counter = 0;

    while (1) {
        // O que vamos enviar nesta rodada será o "esperado" na PRÓXIMA rodada
        memcpy(expected_rx, tx_buf, 47);

        // Preenche o pacote atual com dados sequenciais que mudam a cada rodada
        for(int i = 0; i < 47; i++) {
            tx_buf[i] = (uint8_t)(round_counter + i);
        }

        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = 47 * 8; // 376 bits exatos
        t.tx_buffer = tx_buf;
        t.rx_buffer = rx_buf;

        // Dispara os dados para o FPGA
        spi_device_transmit(fpga_spi, &t);

        // A rodada 0 é ignorada porque o buffer do FPGA ainda estava vazio (vai cuspir zeros)
        if (round_counter > 0) {
            bool package_is_perfect = true;
            
            // Verifica byte a byte se o FPGA nos devolveu o pacote passado corretamente
            for(int i = 0; i < 47; i++) {
                if (rx_buf[i] != expected_rx[i]) {
                    package_is_perfect = false;
                    break;
                }
            }

            if (package_is_perfect) {
                ESP_LOGI(TAG, "[SUCESSO] Mandei (antes): %02X %02X %02X... | Recebi (Eco): %02X %02X %02X...", 
                            expected_rx[0], expected_rx[1], expected_rx[2],
                            rx_buf[0], rx_buf[1], rx_buf[2]);
            } else {
                // Se der erro, printamos o pacote completo para ver onde corrompeu
                ESP_LOGE(TAG, "[FALHA FATAL] Dados corrompidos na ponte SPI!");
                printf("Mandei: ");
                for(int i=0; i<47; i++) printf("%02X ", expected_rx[i]);
                printf("\nRecebi: ");
                for(int i=0; i<47; i++) printf("%02X ", rx_buf[i]);
                printf("\n");
            }
        }

        round_counter++;
        
        // Dispara um pacote de teste a cada 1 segundo
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}

void app_main(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO, 
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK, 
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

    xTaskCreatePinnedToCore(loopback_test_task, "loopback_test", 4096, NULL, 4, NULL, 1);
}