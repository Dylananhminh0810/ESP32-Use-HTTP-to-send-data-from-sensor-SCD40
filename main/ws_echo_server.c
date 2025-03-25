/* WebSocket Echo Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include <math.h>

#include <esp_http_server.h>
#include "scd4x.h"

#define UART_STACK_SIZE             (4096)
#define TEMPERATURE_OFFSET          (4.0)
#define SENSOR_ALTITUDE             (0)
#define SLEEP_DELAY 2000
#define INIT_DELAY 1000

/* A simple example that demonstrates using websocket echo server
 */
static const char *TAG = "ws_echo_server";
static const char *SENSORS_TAG = "sensors";
char data_response[100];

float temperature = 0.0;
float humidity = 0.0;
float co2_level = 0.0;

char scale = SCALE_CELCIUS;
/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 */
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

/*
 * async send function, which we put into the httpd work queue
 */
static void ws_async_send(void *arg)
{
    static const char * data = "Async data";
    struct async_resp_arg *resp_arg = arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = strlen(data);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg);
}

static esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req)
{
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    if (resp_arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);
    esp_err_t ret = httpd_queue_work(handle, ws_async_send, resp_arg);
    if (ret != ESP_OK) {
        free(resp_arg);
    }
    return ret;
}

/*
 * This handler echos back the received ws data
 * and triggers an async send if certain message received
 */
static esp_err_t echo_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    static char data[100];
    int status = 1;
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }
    ESP_LOGI(TAG, "2 Packet type: %d", ws_pkt.type);
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT &&
        strcmp((char*)ws_pkt.payload,"Trigger async") == 0) {
        free(buf);
        return trigger_async_send(req->handle, req);
    }

    // sprintf(data, "{\"co2\": %f, \"temperature\": %f, \"humidity\": %f}", ((float)rand()/(float)(RAND_MAX)) * 1600 + 400, ((float)rand()/(float)(RAND_MAX)) * 99 + 1, ((float)rand()/(float)(RAND_MAX)) * 99 + 1);
    scd4x_sensors_values_t sensors_values = {
        .co2 = 0x00,
        .temperature = 0x00,
        .humidity = 0x00
    };
    if (scd4x_read_measurement(&sensors_values) != ESP_OK) {
        status=0;
        ESP_LOGE(SENSORS_TAG, "Sensors read measurement error!");
        }
    else
    {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    //
    co2_level = sensors_values.co2;
    temperature = sensors_values.temperature;
    humidity = sensors_values.humidity;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);

    sprintf(data, "{\"status\": %d, \"co2\": %f, \"temperature\": %f, \"humidity\": %f}", status, co2_level, temperature, humidity);
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = strlen(data);
    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
    }
    free(buf);
    return ret;
}

void sensor_begin(void){
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &i2c_config));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, i2c_config.mode,
                    I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0));

    esp_log_level_set(SENSORS_TAG, ESP_LOG_INFO);
    scd4x_stop_periodic_measurement();

    vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
    ESP_LOGI(SENSORS_TAG, "Sensor serial number 0x%012llX", scd4x_get_serial_number());

    vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
    float temperature_offset = scd4x_get_temperature_offset();

    vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
    uint16_t sensor_altitude = scd4x_get_sensor_altitude();

    if(temperature_offset != SCD41_READ_ERROR && sensor_altitude != SCD41_READ_ERROR) {

        if(temperature_offset != TEMPERATURE_OFFSET) {
            ESP_LOGW(SENSORS_TAG, "Temperature offset calibration from %.1f °%c to %.1f °%c",
                     temperature_offset, scale, TEMPERATURE_OFFSET, scale);

            vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
            ESP_ERROR_CHECK_WITHOUT_ABORT(scd4x_set_temperature_offset(TEMPERATURE_OFFSET));

            vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
            ESP_ERROR_CHECK_WITHOUT_ABORT(scd4x_persist_settings());

            vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
            temperature_offset = scd4x_get_temperature_offset();
        }

        if(sensor_altitude != SENSOR_ALTITUDE) {
            ESP_LOGW(SENSORS_TAG, "Sensor altitude calibration from %d m to %d m",
                     sensor_altitude, SENSOR_ALTITUDE);

            vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
            ESP_ERROR_CHECK_WITHOUT_ABORT(scd4x_set_sensor_altitude(SENSOR_ALTITUDE));

            vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
            ESP_ERROR_CHECK_WITHOUT_ABORT(scd4x_persist_settings());

            vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
            sensor_altitude = scd4x_get_sensor_altitude();
        }
        ESP_LOGI(SENSORS_TAG, "Temperature offset %.1f °%c - Sensor altitude %d %s",
                 temperature_offset, scale, sensor_altitude, scale == SCALE_CELCIUS ? "m" : "ft");
    } else {
        ESP_LOGE(SENSORS_TAG, "Sensor offset/altitude read error!");
    }
    vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
    scd4x_start_periodic_measurement();
    vTaskDelay(INIT_DELAY / portTICK_PERIOD_MS);
}

static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = echo_handler,
        .user_ctx   = NULL,
        .is_websocket = true
};


static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Registering the ws handler
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &ws);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static esp_err_t stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    return httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}


void app_main(void)
{
    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_WIFI
#ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET
    sensor_begin();
    /* Start the server for the first time */
    server = start_webserver();
}
