#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// O pino que estamos testando
#define PIN_NUM_CS 23

static const char *TAG = "TESTE_CABO";

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando teste fisico do pino GPIO...");

    // Configura o pino 5 como saída digital simples
    gpio_reset_pin(PIN_NUM_CS);
    gpio_set_direction(PIN_NUM_CS, GPIO_MODE_OUTPUT);

    int nivel_logico = 0;

    while (1) {
        // Aplica o nível lógico (0 ou 1) no pino
        gpio_set_level(PIN_NUM_CS, nivel_logico);
        
        if (nivel_logico == 1) {
            ESP_LOGI(TAG, "GPIO em HIGH (3.3V)");
        } else {
            ESP_LOGI(TAG, "GPIO em LOW (0.0V)");
        }

        // Inverte o estado para a próxima rodada (0 vira 1, 1 vira 0)
        nivel_logico = !nivel_logico; 

        // Espera 2000 milissegundos (2 segundos)
        vTaskDelay(pdMS_TO_TICKS(2000)); 
    }
}