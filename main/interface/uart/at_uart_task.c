/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2017 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP32 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "soc/io_mux_reg.h"
#include "soc/gpio_periph.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "esp_at.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef CONFIG_AT_BASE_ON_UART
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "at_interface.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"
#include <inttypes.h>

#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/uart.h"
#elif CONFIG_IDF_TARGET_ESP32C3
#include "esp32c3/rom/uart.h"
#elif CONFIG_IDF_TARGET_ESP32C2
#include "esp32c2/rom/uart.h"
#endif

typedef struct {
    int32_t baudrate;
    int8_t data_bits;
    int8_t stop_bits;
    int8_t parity;
    int8_t flow_control;
} at_nvm_uart_config_struct; 

typedef struct {
    int32_t tx;
    int32_t rx;
    int32_t cts;
    int32_t rts;
} at_uart_pins_t;

static bool at_default_flag = false;
static at_uart_pins_t s_at_uart_port_pin;
static QueueHandle_t esp_at_uart_queue = NULL;
static const uint8_t esp_at_uart_parity_table[] = {UART_PARITY_DISABLE, UART_PARITY_ODD, UART_PARITY_EVEN};
extern const char *g_at_mfg_nvs_name;

#if defined(CONFIG_IDF_TARGET_ESP32)
#define CONFIG_AT_UART_PORT_TX_PIN_DEFAULT          17
#define CONFIG_AT_UART_PORT_RX_PIN_DEFAULT          16
#define CONFIG_AT_UART_PORT_CTS_PIN_DEFAULT         15
#define CONFIG_AT_UART_PORT_RTS_PIN_DEFAULT         14
#ifndef CONFIG_AT_UART_PORT
#define CONFIG_AT_UART_PORT                         UART_NUM_1
#endif
#define AT_UART_BAUD_RATE_MAX                  5000000
#define AT_UART_BAUD_RATE_MIN                       80

#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define CONFIG_AT_UART_PORT_TX_PIN_DEFAULT          7
#define CONFIG_AT_UART_PORT_RX_PIN_DEFAULT          6
#define CONFIG_AT_UART_PORT_CTS_PIN_DEFAULT         5
#define CONFIG_AT_UART_PORT_RTS_PIN_DEFAULT         4
#ifndef CONFIG_AT_UART_PORT
#define CONFIG_AT_UART_PORT                         UART_NUM_1
#endif
#define AT_UART_BAUD_RATE_MAX                  5000000
#define AT_UART_BAUD_RATE_MIN                       80

#elif defined(CONFIG_IDF_TARGET_ESP32C2)
#ifdef CONFIG_ESPTOOLPY_FLASHSIZE_2MB
#define CONFIG_AT_UART_PORT_TX_PIN_DEFAULT          7
#define CONFIG_AT_UART_PORT_RX_PIN_DEFAULT          6
#define CONFIG_AT_UART_PORT_CTS_PIN_DEFAULT         19
#define CONFIG_AT_UART_PORT_RTS_PIN_DEFAULT         20
#else
#define CONFIG_AT_UART_PORT_TX_PIN_DEFAULT          7
#define CONFIG_AT_UART_PORT_RX_PIN_DEFAULT          6
#define CONFIG_AT_UART_PORT_CTS_PIN_DEFAULT         5
#define CONFIG_AT_UART_PORT_RTS_PIN_DEFAULT         4
#endif

#ifndef CONFIG_AT_UART_PORT
#define CONFIG_AT_UART_PORT                         UART_NUM_1
#endif
#define AT_UART_BAUD_RATE_MAX                  2500000
#define AT_UART_BAUD_RATE_MIN                       80
#endif

#define AT_UART_BAUD_RATE_DEF                       115200

#define AT_UART_PATTERN_TIMEOUT_MS                  20

static uart_port_t esp_at_uart_port = CONFIG_AT_UART_PORT;

static bool at_nvm_uart_config_set (at_nvm_uart_config_struct *uart_config);
static bool at_nvm_uart_config_get (at_nvm_uart_config_struct *uart_config);


static int32_t at_port_write_data(uint8_t*data,int32_t len)
{
    uint32_t length = 0;

    length = uart_write_bytes(esp_at_uart_port,(char*)data,len);
    return length;
}

static int32_t at_port_read_data(uint8_t*buf,int32_t len)
{
    TickType_t ticks_to_wait = portTICK_PERIOD_MS;
    uint8_t *data = NULL;
    size_t size = 0;

    if (len == 0) {
        return 0;
    }

    if (buf == NULL) {
        if (len == -1) {
            if (ESP_OK != uart_get_buffered_data_len(esp_at_uart_port,&size)) {
                return -1;
            }
            len = size;
        }

        if (len == 0) {
            return 0;
        }

        data = (uint8_t *)malloc(len);
        if (data) {
            len = uart_read_bytes(esp_at_uart_port,data,len,ticks_to_wait);
            free(data);
            return len;
        } else {
            return -1;
        }
    } else {
        return uart_read_bytes(esp_at_uart_port,buf,len,ticks_to_wait);
    }
}

static int32_t at_port_get_data_length (void)
{
    size_t size = 0;
    int pattern_pos = 0;

    if (ESP_OK == uart_get_buffered_data_len(esp_at_uart_port,&size)) {
        pattern_pos = uart_pattern_get_pos(esp_at_uart_port);
        if (pattern_pos >= 0) {
            size = pattern_pos;
        }
        return size;
    } else {
        return 0;
    }
}

static bool at_port_wait_write_complete (int32_t timeout_msec)
{
    if (ESP_OK == uart_wait_tx_done(esp_at_uart_port, timeout_msec / portTICK_PERIOD_MS)) {
        return true;
    }

    return false;
}

static void uart_task(void *pvParameters)
{
    uart_event_t event;
    uint32_t data_len = 0;
    BaseType_t retry_flag = pdFALSE;
    int pattern_pos = -1;
    uint8_t *data = NULL;
    //uint64_t time = 0;
    //uint64_t _pre_time = 0;
    //int sleep_flag = 0;

    for (;;) {
        //Waiting for UART event.
        //time = esp_timer_get_time();
        //if ((time > 10000000)&&(sleep_flag == 0)){
        //    sleep_flag = 1;
        //    printf("Timeout ... deepsleep...\r\n");
        //}
        if (xQueueReceive(esp_at_uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
retry:
            switch (event.type) {
            //Event of UART receving data
            case UART_DATA:
            case UART_BUFFER_FULL:
                data_len += event.size;
                // we can put all data together to process
                retry_flag = pdFALSE;
                while (xQueueReceive(esp_at_uart_queue, (void * )&event, (TickType_t)0) == pdTRUE) {
                    if (event.type == UART_DATA) {
                        data_len += event.size;
                    } else if (event.type == UART_BUFFER_FULL) {
                        esp_at_port_recv_data_notify(data_len, portMAX_DELAY);
                        data_len = event.size;
                        break;
                    } else {
                        retry_flag = pdTRUE;
                        break;
                    }
                }
                esp_at_port_recv_data_notify (data_len, portMAX_DELAY);
                data_len = 0;

                if (retry_flag == pdTRUE) {
                    goto retry;
                }
                break;
            case UART_PATTERN_DET:
                pattern_pos = uart_pattern_pop_pos(esp_at_uart_port);
                if (pattern_pos >= 0) {
                    data = (uint8_t *)malloc(pattern_pos + 3);
                    uart_read_bytes(esp_at_uart_port,data,pattern_pos + 3,0);
                    free(data);
                    data = NULL;
                } else {
                    uart_flush_input(esp_at_uart_port);
                    xQueueReset(esp_at_uart_queue);
                }
                esp_at_transmit_terminal();
                break;
            case UART_FIFO_OVF:
                retry_flag = pdFALSE;
                while (xQueueReceive(esp_at_uart_queue, (void *)&event, (TickType_t)0) == pdTRUE) {
                    if ((event.type == UART_DATA) || (event.type == UART_BUFFER_FULL) || (event.type == UART_FIFO_OVF)) {
                        // Put all data together to process
                    } else {
                        retry_flag = pdTRUE;
                        break;
                    }
                }
                esp_at_port_recv_data_notify(at_port_get_data_length(), portMAX_DELAY);
                data_len = 0;
                if (retry_flag == pdTRUE) {
                    goto retry;
                }
                break;

            //Others
            default:
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

static void at_uart_init(void)
{
    at_nvm_uart_config_struct uart_nvm_config;
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = CONFIG_AT_UART_DEFAULT_DATABITS - 5,
        .parity = esp_at_uart_parity_table[CONFIG_AT_UART_DEFAULT_PARITY_BITS],
        .stop_bits = CONFIG_AT_UART_DEFAULT_STOPBITS,
        .flow_ctrl = CONFIG_AT_UART_DEFAULT_FLOW_CONTROL,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_intr_config_t intr_config = {
        .intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M
            | UART_RXFIFO_TOUT_INT_ENA_M
            | UART_RXFIFO_OVF_INT_ENA_M,
        .rxfifo_full_thresh = 100,
        .rx_timeout_thresh = 10,
        .txfifo_empty_intr_thresh = 10
    };

    int32_t tx_pin = CONFIG_AT_UART_PORT_TX_PIN_DEFAULT;	
    int32_t rx_pin = CONFIG_AT_UART_PORT_RX_PIN_DEFAULT;
    int32_t cts_pin = CONFIG_AT_UART_PORT_CTS_PIN_DEFAULT;
    int32_t rts_pin = CONFIG_AT_UART_PORT_RTS_PIN_DEFAULT;
    int32_t baud_rate = AT_UART_BAUD_RATE_DEF;

    // get baud rate, tx_pin, rx_pin, cts_pin, and rts_pin from flash
    at_mfg_params_storage_mode_t mode = at_get_mfg_params_storage_mode();
    if (mode == AT_PARAMS_IN_MFG_NVS) {
        nvs_handle handle;
        if (nvs_open_from_partition(g_at_mfg_nvs_name, "factory_param", NVS_READONLY, &handle) != ESP_OK) {
            printf("open factory_param namespace failed\r\n");
            return;
        }
        int8_t value = -1;
        if (nvs_get_i8(handle, "uart_port", &value) != ESP_OK) {
            nvs_close(handle);
            return;
        } else {
            esp_at_uart_port = value;
        }
        if (nvs_get_i32(handle, "uart_baudrate", &baud_rate) != ESP_OK) {
            nvs_close(handle);
            return;
        }
        if (nvs_get_i32(handle, "uart_tx_pin", &tx_pin) != ESP_OK) {
            nvs_close(handle);
            return;
        }
        if (nvs_get_i32(handle, "uart_rx_pin", &rx_pin) != ESP_OK) {
            nvs_close(handle);
            return;
        }
        if (nvs_get_i32(handle, "uart_cts_pin", &cts_pin) != ESP_OK) {
            nvs_close(handle);
            return;
        }
        if (nvs_get_i32(handle, "uart_rts_pin", &rts_pin) != ESP_OK) {
            nvs_close(handle);
            return;
        }
    } else if (mode == AT_PARAMS_IN_PARTITION) {
        char data[AT_BUFFER_ON_STACK_SIZE] = {0};
        const esp_partition_t *partition = esp_at_custom_partition_find(0x40, 0xff, "factory_param");
        if (partition) {
            if (esp_partition_read(partition, 0, data, AT_BUFFER_ON_STACK_SIZE) != ESP_OK) {
                return;
            }
            // check magic flag, should be 0xfc 0xfc
            if ((data[0] == 0xFC) && (data[1] == 0xFC)) {
                if ((data[12] != 0xFF) || (data[13] != 0xFF) || (data[14] != 0xFF) || (data[15] != 0xFF)) {
                    baud_rate = *(int32_t *)&data[12];
                }
                if ((data[16] != 0xFF) && (data[17] != 0xFF)) {
                    tx_pin = data[16];
                    rx_pin = data[17];
                }
                if (data[18] != 0xFF) {
                    cts_pin = data[18];
                } else {
                    cts_pin = -1;
                }
                if (data[19] != 0xFF) {
                    rts_pin = data[19];
                } else {
                    rts_pin = -1;
                }
            }
        }
    } else {
        return;
    }

    memset(&uart_nvm_config, 0x0, sizeof(uart_nvm_config));
    if (at_nvm_uart_config_get(&uart_nvm_config)) {
        if ((uart_nvm_config.baudrate >= AT_UART_BAUD_RATE_MIN) && (uart_nvm_config.baudrate <= AT_UART_BAUD_RATE_MAX)) {
            uart_config.baud_rate = uart_nvm_config.baudrate;
        }

        if ((uart_nvm_config.data_bits >= UART_DATA_5_BITS) && (uart_nvm_config.data_bits <= UART_DATA_8_BITS)) {
            uart_config.data_bits = uart_nvm_config.data_bits;
        }

        if ((uart_nvm_config.stop_bits >= UART_STOP_BITS_1) && (uart_nvm_config.stop_bits <= UART_STOP_BITS_2)) {
            uart_config.stop_bits = uart_nvm_config.stop_bits;
        }

        if ((uart_nvm_config.parity == UART_PARITY_DISABLE) 
            || (uart_nvm_config.parity == UART_PARITY_ODD)
            || (uart_nvm_config.parity == UART_PARITY_EVEN)) {
            uart_config.parity = uart_nvm_config.parity;
        }

        if ((uart_nvm_config.flow_control >= UART_HW_FLOWCTRL_DISABLE) && (uart_nvm_config.flow_control <= UART_HW_FLOWCTRL_CTS_RTS)) {
            uart_config.flow_ctrl = uart_nvm_config.flow_control;
        }
    } else {
        uart_config.baud_rate = baud_rate;
        uart_nvm_config.baudrate = uart_config.baud_rate;
        uart_nvm_config.data_bits = uart_config.data_bits;
        uart_nvm_config.flow_control = uart_config.flow_ctrl;
        uart_nvm_config.parity = uart_config.parity;
        uart_nvm_config.stop_bits = uart_config.stop_bits;
        at_nvm_uart_config_set(&uart_nvm_config);
    }

    // set uart parameters
    uart_param_config(esp_at_uart_port, &uart_config);
    // set uart pins (-1: default pin)
    uart_set_pin(esp_at_uart_port, tx_pin, rx_pin, rts_pin, cts_pin);
    // install uart driver
    uart_driver_install(esp_at_uart_port, 2048, 8192, 30, &esp_at_uart_queue, 0);
    uart_intr_config(esp_at_uart_port, &intr_config);

    /**
     * A workaround for uart1 outputs uninterrupted data during light sleep
     */
    PIN_SLP_INPUT_ENABLE(GPIO_PIN_MUX_REG[rx_pin]);
    gpio_sleep_set_pull_mode(rx_pin, GPIO_PULLUP_ONLY);

    // set actual uart pins
    s_at_uart_port_pin.tx = tx_pin;
    s_at_uart_port_pin.rx = rx_pin;
    s_at_uart_port_pin.cts = cts_pin;
    s_at_uart_port_pin.rts = rts_pin;

    printf("AT cmd port:uart%d tx:%d rx:%d cts:%d rts:%d baudrate:%d\r\n", esp_at_uart_port, tx_pin, rx_pin, cts_pin, rts_pin, uart_config.baud_rate);
    xTaskCreate(uart_task, "uTask", 1024, (void *)esp_at_uart_port, 1, NULL);
}

static bool at_nvm_uart_config_set (at_nvm_uart_config_struct *uart_config)
{
    nvs_handle handle;
    if (uart_config == NULL) {
        return false;
    }

    if (nvs_open("UART", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_i32(handle, "rate", uart_config->baudrate) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_set_i8(handle, "databits", uart_config->data_bits) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_set_i8(handle, "stopbits", uart_config->stop_bits) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_set_i8(handle, "parity", uart_config->parity) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_set_i8(handle, "flow_ctrl", uart_config->flow_control) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
    } else {
        return false;
    }
    nvs_close(handle);
    return true;
}

static bool at_nvm_uart_config_get (at_nvm_uart_config_struct *uart_config)
{
    nvs_handle handle;
    if (uart_config == NULL) {
        return false;
    }

    if (nvs_open("UART", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_i32(handle, "rate", &uart_config->baudrate) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_get_i8(handle, "databits", &uart_config->data_bits) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_get_i8(handle, "stopbits", &uart_config->stop_bits) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_get_i8(handle, "parity", &uart_config->parity) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
        if (nvs_get_i8(handle, "flow_ctrl", &uart_config->flow_control) != ESP_OK) {
            nvs_close(handle);
            return false;
        }
    } else {
        return false;
    }
    nvs_close(handle);

    return true;
}

static uint8_t at_setupCmdUart(uint8_t para_num)
{
    int32_t value = 0;
    int32_t cnt = 0;

    at_nvm_uart_config_struct uart_config;

    memset(&uart_config,0x0,sizeof(uart_config));
    if (para_num != 5) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (esp_at_get_para_as_digit (cnt++,&value) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if ((value < AT_UART_BAUD_RATE_MIN) || (value > AT_UART_BAUD_RATE_MAX)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    uart_config.baudrate = value;

    if (esp_at_get_para_as_digit (cnt++,&value) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if ((value < 5) || (value > 8)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    uart_config.data_bits = value - 5;

    if (esp_at_get_para_as_digit (cnt++,&value) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if ((value < 1) || (value > 3)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    uart_config.stop_bits = value;

    if (esp_at_get_para_as_digit (cnt++,&value) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if ((value >= 0) && (value <= 2)) {
        uart_config.parity = esp_at_uart_parity_table[value];
    } else {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (esp_at_get_para_as_digit (cnt++,&value) != ESP_AT_PARA_PARSE_RESULT_OK) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    if ((value < 0) || (value > 3)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }
    uart_config.flow_control = value;

    if (at_default_flag) {
        if (at_nvm_uart_config_set(&uart_config) == false) {
            return ESP_AT_RESULT_CODE_ERROR;
        }
    }
    esp_at_response_result(ESP_AT_RESULT_CODE_OK);

    uart_wait_tx_done(esp_at_uart_port,portMAX_DELAY);
    uart_set_baudrate(esp_at_uart_port,uart_config.baudrate);
    uart_set_word_length(esp_at_uart_port,uart_config.data_bits);
    uart_set_stop_bits(esp_at_uart_port,uart_config.stop_bits);
    uart_set_parity(esp_at_uart_port,uart_config.parity);
    uart_set_hw_flow_ctrl(esp_at_uart_port,uart_config.flow_control,120);
    return ESP_AT_RESULT_CODE_PROCESS_DONE;
}

static uint8_t at_setupCmdUartDef(uint8_t para_num)
{
    uint8_t ret = ESP_AT_RESULT_CODE_ERROR;
    at_default_flag = true;
    ret = at_setupCmdUart(para_num);
    at_default_flag = false;
    
    return ret;
}

static uint8_t at_queryCmdUart (uint8_t *cmd_name)
{
    uint32_t baudrate = 0;
    uart_word_length_t data_bits = UART_DATA_8_BITS;
    uart_stop_bits_t stop_bits = UART_STOP_BITS_1;
    uart_parity_t parity = UART_PARITY_DISABLE;
    uart_hw_flowcontrol_t flow_control = UART_HW_FLOWCTRL_DISABLE;

    uint8_t buffer[64];

    uart_get_baudrate(esp_at_uart_port,&baudrate);
    uart_get_word_length(esp_at_uart_port,&data_bits);
    uart_get_stop_bits(esp_at_uart_port,&stop_bits);
    uart_get_parity(esp_at_uart_port,&parity);
    uart_get_hw_flow_ctrl(esp_at_uart_port,&flow_control);

    data_bits += 5;
    if (UART_PARITY_DISABLE == parity) {
        parity = 0;
    } else if (UART_PARITY_ODD == parity) {
        parity = 1;
    } else if (UART_PARITY_EVEN == parity) {
        parity = 2;
    } else {
        parity = 0xff;
    }

    snprintf((char*)buffer,sizeof(buffer) - 1,"%s:%d,%d,%d,%d,%d\r\n",cmd_name,baudrate,data_bits,stop_bits,parity,flow_control);

    esp_at_port_write_data(buffer,strlen((char*)buffer));

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_queryCmdUartDef (uint8_t *cmd_name)
{
    at_nvm_uart_config_struct uart_nvm_config;
    uint8_t buffer[64];
    memset(&uart_nvm_config,0x0,sizeof(uart_nvm_config));
    at_nvm_uart_config_get(&uart_nvm_config);

    uart_nvm_config.data_bits += 5;
    if (UART_PARITY_DISABLE == uart_nvm_config.parity) {
        uart_nvm_config.parity = 0;
    } else if (UART_PARITY_ODD == uart_nvm_config.parity) {
        uart_nvm_config.parity = 1;
    } else if (UART_PARITY_EVEN == uart_nvm_config.parity) {
        uart_nvm_config.parity = 2;
    } else {
        uart_nvm_config.parity = 0xff;
    }
    snprintf((char*)buffer,sizeof(buffer) - 1,"%s:%d,%d,%d,%d,%d\r\n",cmd_name,uart_nvm_config.baudrate,
        uart_nvm_config.data_bits,uart_nvm_config.stop_bits,uart_nvm_config.parity,uart_nvm_config.flow_control);

    esp_at_port_write_data(buffer,strlen((char*)buffer));
    return ESP_AT_RESULT_CODE_OK;
}

static const esp_at_cmd_struct at_custom_cmd[] = {
    {"+UART", NULL, at_queryCmdUart, at_setupCmdUartDef, NULL},
    {"+UART_CUR", NULL, at_queryCmdUart, at_setupCmdUart, NULL},
    {"+UART_DEF", NULL, at_queryCmdUartDef, at_setupCmdUartDef, NULL},
};

void at_status_callback (esp_at_status_type status)
{
    switch (status) {
    case ESP_AT_STATUS_NORMAL:
        uart_disable_pattern_det_intr(esp_at_uart_port);
        break;

    case ESP_AT_STATUS_TRANSMIT: {
        /**
         * As the implement of API uart_enable_pattern_det_baud_intr() in esp-idf,
         * the last three timeout parameters is different on ESP32 and non ESP32 platform.
         *
         * That is, on ESP32 platform, it uses the APB clocks as the unit;
         * on non ESP32 platform (ESP32-C3, ..), it uses the UART baud rate clocks as the unit.
         *
         * Notes:
         * on non ESP32 platform, due to the value of input parameters have a limit of 0xFFFF (see as macro: UART_RX_GAP_TOUT_V..),
         * so the maximum uart baud rate is recommended to be less than (0xFFFF * 1000 / AT_UART_PATTERN_TIMEOUT_MS) = 3276750 ~= 3.2Mbps
         * otherwise, this uart_enable_pattern_det_baud_intr() will not work.
        */
#ifdef CONFIG_IDF_TARGET_ESP32
        int apb_clocks = (uint32_t)APB_CLK_FREQ * AT_UART_PATTERN_TIMEOUT_MS / 1000;
        uart_enable_pattern_det_baud_intr(esp_at_uart_port, '+', 3, apb_clocks, apb_clocks, apb_clocks);
#else
        uint32_t uart_baud = 0;
        uart_get_baudrate(esp_at_uart_port, &uart_baud);
        int uart_clocks = (uint32_t)uart_baud * AT_UART_PATTERN_TIMEOUT_MS / 1000;
        uart_enable_pattern_det_baud_intr(esp_at_uart_port, '+', 3, uart_clocks, uart_clocks, uart_clocks);
#endif
    }
        break;
    }
}

void at_pre_sleep_callback(at_sleep_mode_t mode)
{
#ifdef CONFIG_AT_USERWKMCU_COMMAND_SUPPORT
    at_set_mcu_state_if_sleep(mode);
#endif
}

void at_pre_deepsleep_callback (void)
{
    const int ext_wakeup_pin_0 = 4;
    printf("at_pre_deepsleep_callback\r\n");
    esp_wifi_stop();
    /* Do something before deep sleep
     * Set uart pin for power saving, in case of leakage current
    */
    if (s_at_uart_port_pin.tx >= 0) {
        gpio_set_direction(s_at_uart_port_pin.tx, GPIO_MODE_DISABLE);
    }
    if (s_at_uart_port_pin.rx >= 0) {
        gpio_set_direction(s_at_uart_port_pin.rx, GPIO_MODE_DISABLE);
    }
    if (s_at_uart_port_pin.cts >= 0) {
        gpio_set_direction(s_at_uart_port_pin.cts, GPIO_MODE_DISABLE);
    }
    if (s_at_uart_port_pin.rts >= 0) {
        gpio_set_direction(s_at_uart_port_pin.rts, GPIO_MODE_DISABLE);
    }

    esp_sleep_enable_ext0_wakeup(ext_wakeup_pin_0, 0);
    rtc_gpio_pullup_en(ext_wakeup_pin_0);
    rtc_gpio_pulldown_dis(ext_wakeup_pin_0);
    printf("deepsleep...\r\n");
    vTaskDelay(100);
        
}

void at_pre_restart_callback (void)
{
    /* Do something before restart
    */
    uart_disable_rx_intr(esp_at_uart_port);
    esp_at_port_wait_write_complete(ESP_AT_PORT_TX_WAIT_MS_MAX);
}

void at_pre_active_write_data_callback(at_write_data_fn_t fn)
{
    // Do something before active write data
#ifdef CONFIG_AT_USERWKMCU_COMMAND_SUPPORT
    at_wkmcu_if_config(fn);
#endif
}

void at_interface_init (void)
{
    esp_at_device_ops_struct esp_at_device_ops = {
        .read_data = at_port_read_data,
        .write_data = at_port_write_data,
        .get_data_length = at_port_get_data_length,
        .wait_write_complete = at_port_wait_write_complete,
    };
    
    esp_at_custom_ops_struct esp_at_custom_ops = {
        .status_callback = at_status_callback,
        .pre_sleep_callback = at_pre_sleep_callback,
        .pre_deepsleep_callback = at_pre_deepsleep_callback,
        .pre_restart_callback = at_pre_restart_callback,
        .pre_active_write_data_callback = at_pre_active_write_data_callback,
    };

    at_uart_init();

    esp_at_device_ops_regist(&esp_at_device_ops);
    esp_at_custom_ops_regist(&esp_at_custom_ops);
}


void at_custom_init(void)
{
    
    esp_at_custom_cmd_array_regist (at_custom_cmd, sizeof(at_custom_cmd)/sizeof(at_custom_cmd[0]));
    esp_at_port_active_write_data((uint8_t *)"\0\0\0\r\nready\r\n",strlen("\0\0\0\r\nready\r\n"));
}
#endif
