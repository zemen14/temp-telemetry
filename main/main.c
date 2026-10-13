#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "driver/i2c.h"

#define WIFI_SSID "username"
#define WIFI_PASSWORD "password"
#define POST_URL "http://10.0.0.216:1234/temperature"
#define I2C_SCL_PIN GPIO_NUM_8
#define I2C_SDA_PIN GPIO_NUM_7
#define SHTC3_ADDR 0x70
#define SHTC3_CMD_WAKE 0x3517
#define SHTC3_CMD_MEAS 0x7CA2
#define SHTC3_CMD_SLEEP 0xB098

#define WIFI_CONNECTED_BIT BIT0
#define TAG "temp_telemetry"

static EventGroupHandle_t s_wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t base,
int32_t id, void *data)
{
if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
esp_wifi_connect();
else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
esp_wifi_connect();
else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}
}

static void wifi_init_sta(void)
{
s_wifi_event_group = xEventGroupCreate();
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
esp_netif_create_default_wifi_sta();
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
ESP_ERROR_CHECK(esp_wifi_init(&cfg));
esp_event_handler_instance_t a, b;
ESP_ERROR_CHECK(esp_event_handler_instance_register(
WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &a));
ESP_ERROR_CHECK(esp_event_handler_instance_register(
IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &b));
wifi_config_t wifi_cfg = {
.sta = { .ssid = WIFI_SSID, .password = WIFI_PASSWORD },
};
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
ESP_ERROR_CHECK(esp_wifi_start());
xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
pdFALSE, pdTRUE, portMAX_DELAY);
}

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_shtc3 = NULL;

static uint8_t shtc3_crc(const uint8_t *data, size_t len)
{
uint8_t crc = 0xFF;
for (size_t i = 0; i < len; i++) {
crc ^= data[i];
for (int bit = 0; bit < 8; bit++)
crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
}
return crc;
}

static void shtc3_write_cmd(uint16_t cmd)
{
uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
i2c_master_transmit(s_shtc3, buf, sizeof(buf), 100);
}

static void shtc3_init(void)
{
i2c_master_bus_config_t bus_config = {
.i2c_port = I2C_NUM_0,
.scl_io_num = I2C_SCL_PIN,
.sda_io_num = I2C_SDA_PIN,
.clk_source = I2C_CLK_SRC_DEFAULT,
.glitch_ignore_cnt = 7,
.flags.enable_internal_pullup = true,
};
ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_i2c_bus));
i2c_device_config_t dev_config = {
.dev_addr_length = I2C_ADDR_BIT_LEN_7,
.device_address = SHTC3_ADDR,
.scl_speed_hz = 400000,
};
ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_shtc3));
}

static bool shtc3_read(float *temp_c, float *humidity)
{
shtc3_write_cmd(SHTC3_CMD_WAKE);
vTaskDelay(pdMS_TO_TICKS(1));
shtc3_write_cmd(SHTC3_CMD_MEAS);
vTaskDelay(pdMS_TO_TICKS(15));
uint8_t raw[6];
esp_err_t err = i2c_master_receive(s_shtc3, raw, sizeof(raw), 100);
shtc3_write_cmd(SHTC3_CMD_SLEEP);
if (err != ESP_OK) return false;
if (shtc3_crc(raw, 2) != raw[2] || shtc3_crc(raw + 3, 2) != raw[5]) return false;
uint16_t raw_hum = (raw[0] << 8) | raw[1];
uint16_t raw_temp = (raw[3] << 8) | raw[4];
*humidity = 100.0f * ((float)raw_hum / 65536.0f);
*temp_c = -45.0f + 175.0f * ((float)raw_temp / 65536.0f);
return true;
}

static void post_temperature(float temp)
{
char body[64];
snprintf(body, sizeof(body), "sensor_temp=%.2f", temp);

esp_http_client_config_t cfg = {
.url = POST_URL,
.method = HTTP_METHOD_POST,
};
esp_http_client_handle_t client = esp_http_client_init(&cfg);
esp_http_client_set_post_field(client, body, strlen(body));
esp_http_client_set_header(client, "Content-Type",
"application/x-www-form-urlencoded");
esp_err_t err = esp_http_client_perform(client);
if (err == ESP_OK)
printf("[ESP32] Posted sensor temp: %.2f C\n", temp);
else
ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
esp_http_client_cleanup(client);
}

void app_main(void)
{
ESP_ERROR_CHECK(nvs_flash_init());
shtc3_init();
wifi_init_sta();
while (1) {
float temp, hum;
if (shtc3_read(&temp, &hum)) {
printf("[ESP32] Sensor temp: %.2f C, humidity: %.1f %%\n", temp, hum);
post_temperature(temp);
} else {
ESP_LOGE(TAG, "SHTC3 read failed, skipping this cycle");
}
vTaskDelay(pdMS_TO_TICKS(10000));
}
}
