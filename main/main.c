#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define SIM_TXD_PIN      CONFIG_UART_TXD
#define SIM_RXD_PIN      CONFIG_UART_RXD
#define SIM_UART_PORT    CONFIG_UART_PORT_NUM

#define FPGA_UART_PORT   UART_NUM_2
#define FPGA_TXD_PIN     5  
#define FPGA_RXD_PIN     23 
#define FPGA_BAUD_RATE   115200 

#define INPUT_PIN  GPIO_NUM_4
#define OUTPUT_PIN GPIO_NUM_2

static const char *TAG = "BM13XX_BRIDGE";

// ==========================================
// MODO DEBUG
// ==========================================
bool verbose = true; // Mude para false para parar de imprimir os pacotes em Hexadecimal

void print_hex_dump(const char *prefix, const uint8_t *data, int len) {
    printf("%s", prefix);
    for (int i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

static void transparent_bridge_task(void *arg) {
    uart_config_t sim_config = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(SIM_UART_PORT, &sim_config);
    uart_set_pin(SIM_UART_PORT, SIM_TXD_PIN, SIM_RXD_PIN, -1, -1);
    uart_driver_install(SIM_UART_PORT, 1024, 1024, 0, NULL, 0);

    uart_config_t fpga_config = {
        .baud_rate = FPGA_BAUD_RATE, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(FPGA_UART_PORT, &fpga_config);
    uart_set_pin(FPGA_UART_PORT, FPGA_TXD_PIN, FPGA_RXD_PIN, -1, -1);
    uart_driver_install(FPGA_UART_PORT, 1024, 1024, 0, NULL, 0);

    uint8_t buffer[256];

    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, "🔗 BRIDGE TRANSPARENTE ATIVA (MODO ANALISADOR)");
    ESP_LOGI(TAG, "===============================================");

    while(1) {
        // ROTA A: Micro-stratum ---> FPGA
        int len_a = uart_read_bytes(SIM_UART_PORT, buffer, sizeof(buffer), 10 / portTICK_PERIOD_MS);
        if (len_a > 0) {
            // Log apenas se for o início de um pacote Bitmain (55 AA)
            if (buffer[0] == 0x55 && buffer[1] == 0xAA) {
                ESP_LOGW(TAG, ">> CMD (0x%02x) -> FPGA (%d bytes)", buffer[2], len_a);
                if (verbose) {
                    print_hex_dump("   HEX >>: ", buffer, len_a);
                }
            } else if (verbose) {
                // Imprime lixo ou dados quebrados também para debugarmos
                ESP_LOGW(TAG, ">> DADO DESCONHECIDO -> FPGA (%d bytes)", len_a);
                print_hex_dump("   HEX >>: ", buffer, len_a);
            }
            uart_write_bytes(FPGA_UART_PORT, buffer, len_a);
        }

        // ROTA B: FPGA ---> Micro-stratum
        int len_b = uart_read_bytes(FPGA_UART_PORT, buffer, sizeof(buffer), 10 / portTICK_PERIOD_MS);
        if (len_b > 0) {
            
            // Verifica se é o início de uma resposta do FPGA (Boot = 55 AA | Nonce = AA 55)
            if ((buffer[0] == 0x55 && buffer[1] == 0xAA) || (buffer[0] == 0xAA && buffer[1] == 0x55)) {
                
                // ANTI-FRAGMENTAÇÃO: Segura a onda e obriga a ler no mínimo 11 bytes!
                while (len_b < 11) {
                    int extra = uart_read_bytes(FPGA_UART_PORT, buffer + len_b, sizeof(buffer) - len_b, 20 / portTICK_PERIOD_MS);
                    if (extra <= 0) break; // Sai se a UART ficar em silêncio
                    len_b += extra;
                }
                
                if (buffer[0] == 0xAA && buffer[1] == 0x55) {
                    ESP_LOGE(TAG, "<< 🔥 FPGA ACHOU NONCE -> uStratum (%d bytes)", len_b);
                } else {
                    ESP_LOGI(TAG, "<< 🤖 BOOT FPGA -> uStratum (%d bytes)", len_b);
                }
                
                if (verbose) {
                    print_hex_dump("   HEX <<: ", buffer, len_b);
                }
            } 
            else if (verbose) {
                ESP_LOGI(TAG, "<< LIXO/SERIAL FPGA -> uStratum (%d bytes)", len_b);
                print_hex_dump("   HEX <<: ", buffer, len_b);
            }

            // Manda o pacote blindado para o Micro-stratum
            uart_write_bytes(SIM_UART_PORT, buffer, len_b);
        }
    }
}

static void rstcheck(void *arg){
        // 3. Main Mirroring Loop
    while (1) {
        // Read input pin
        int level = gpio_get_level(INPUT_PIN);
        
        // Write state to output pin
        gpio_set_level(OUTPUT_PIN, level);
        
        // Delay for 10ms to prevent high CPU usage
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

void app_main(void) {
    // 1. Configure the Input Pin
    gpio_config_t input_conf = {
        .pin_bit_mask = (1ULL << INPUT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Use internal pull-up if needed
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&input_conf);

    // 2. Configure the Output Pin
    gpio_config_t output_conf = {
        .pin_bit_mask = (1ULL << OUTPUT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&output_conf);
    xTaskCreatePinnedToCore(transparent_bridge_task, "bridge_task", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(rstcheck, "rst_task", 8192, NULL, 5, NULL, 0);
}