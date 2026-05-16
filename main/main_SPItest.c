#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

// Definição dos pinos físicos baseada na sua montagem
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

// Começamos o teste em 1 MHz. 
// Para testar o limite, mude aqui para (5 * 1000 * 1000), (10 * 1000 * 1000), etc.
#define SPI_SPEED_HZ (5 * 1000 * 1000) 

static const char *TAG = "FPGA_PINGPONG";

void app_main(void)
{
    esp_err_t ret;
    spi_device_handle_t spi;

    ESP_LOGI(TAG, "Inicializando barramento VSPI...");

    // 1. Configuração dos Pinos do Barramento SPI
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1, // Não usado
        .quadhd_io_num = -1, // Não usado
        .max_transfer_sz = 32 // Tamanho máximo de transferência em bytes
    };

    // 2. Configuração do Dispositivo (O nosso FPGA)
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_SPEED_HZ,
        .mode = 0,                  // SPI Mode 0 (CPOL=0, CPHA=0) - Casa com o nosso Verilog
        .spics_io_num = PIN_NUM_CS, // O pino de Chip Select
        .queue_size = 1,            // Quantas transações podemos enfileirar por vez
    };

    // Inicializa o barramento SPI
    // Usamos SPI3_HOST (VSPI) e ativamos o DMA automático para máxima performance
    ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    // Conecta o FPGA ao barramento
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "SPI Inicializado. Frequência: %d Hz", SPI_SPEED_HZ);

    // Variáveis para o nosso "Super Ping-Pong"
    uint8_t send_data = 0x10; // Começa enviando 0x10
    uint8_t recv_data = 0x00;
    uint8_t expected_from_last = 0xA5; // No 1º ciclo, o Verilog manda o tx_data inicial (0xA5)

    while (1) {
        // Prepara a transação SPI
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));       // Zera a estrutura para evitar lixo de memória
        t.length = 8;                   // Tamanho em bits (1 byte = 8 bits)
        t.tx_buffer = &send_data;       // Ponteiro para o dado a enviar
        t.rx_buffer = &recv_data;       // Ponteiro para guardar a resposta

        // Executa a transação (O CS desce, o clock bate, os dados trocam, o CS sobe)
        ret = spi_device_transmit(spi, &t);
        ESP_ERROR_CHECK(ret);

        // Verifica se recebemos o que era esperado
        if (recv_data == expected_from_last) {
            ESP_LOGI(TAG, "SUCESSO | Enviei: 0x%02X | Recebi: 0x%02X", send_data, recv_data);
        } else {
            ESP_LOGE(TAG, "FALHA   | Enviei: 0x%02X | Recebi: 0x%02X (Esperava: 0x%02X)", send_data, recv_data, expected_from_last);
        }

        // Prepara a expectativa para o PRÓXIMO loop (o FPGA sempre devolve o inverso do último dado)
        expected_from_last = (uint8_t)(~send_data);

        // Incrementa o dado que vamos enviar para ver a mudança rodando
        send_data++;

        // Atraso de 500ms para você conseguir ler o terminal confortavelmente
        // Quando for testar velocidade extrema, você pode tirar esse delay!
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }
}