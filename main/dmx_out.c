#include "dmx_out.h"

#include "config.h"
#include "dmx_buffer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dmx_out";

#define DMX_BAUD     250000
#define DMX_BREAK_US 176   // spec min 88 us; 176 is the common, robust choice
#define DMX_MAB_US   12    // spec min 8 us

static uart_port_t s_uart;

static void dmx_task(void *arg)
{
    // Start code (0x00) + 512 slots
    static uint8_t frame[1 + DMX_SLOTS];
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        frame[0] = 0x00;
        dmx_buffer_get_output(&frame[1]);

        // BREAK: invert the idle-high TX line so it is held low, then MARK-after-break.
        uart_set_line_inverse(s_uart, UART_SIGNAL_TXD_INV);
        esp_rom_delay_us(DMX_BREAK_US);
        uart_set_line_inverse(s_uart, UART_SIGNAL_INV_DISABLE);
        esp_rom_delay_us(DMX_MAB_US);

        uart_write_bytes(s_uart, frame, sizeof(frame));
        uart_wait_tx_done(s_uart, pdMS_TO_TICKS(50));
        dmx_buffer_count_frame();

        TickType_t period = pdMS_TO_TICKS(1000 / g_config.refresh_hz);
        if (period < 1) {
            period = 1;
        }
        // A full frame takes ~23 ms on the wire, so at high rates this simply runs back-to-back.
        if (!xTaskDelayUntil(&last_wake, period)) {
            last_wake = xTaskGetTickCount();
        }
    }
}

esp_err_t dmx_out_start(void)
{
    s_uart = (uart_port_t)g_config.uart_num;

    const uart_config_t cfg = {
        .baud_rate = DMX_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // RX buffer must exceed the HW FIFO size even though we never receive.
    ESP_ERROR_CHECK(uart_driver_install(s_uart, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(s_uart, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(s_uart, g_config.tx_pin, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    if (g_config.de_pin >= 0) {
        // Transmit-only node: keep the driver enabled permanently.
        gpio_reset_pin(g_config.de_pin);
        gpio_set_direction(g_config.de_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(g_config.de_pin, 1);
    }

    ESP_LOGI(TAG, "DMX out on UART%d, TX=GPIO%d, DE=%d, %d Hz",
             s_uart, g_config.tx_pin, g_config.de_pin, g_config.refresh_hz);

    BaseType_t ok = xTaskCreatePinnedToCore(dmx_task, "dmx_out", 4096, NULL, 15, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
