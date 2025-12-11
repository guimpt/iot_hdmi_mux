#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "mdns.h"

#define WIFI_SSID_MAXLEN 32
#define WIFI_PASS_MAXLEN 64

#define TS3_EN     GPIO_NUM_8
#define TS3_SEL1   GPIO_NUM_2
#define TS3_SEL2   GPIO_NUM_3

static const char *TAG = "IOT_HDMI_MUX";
static char wifi_ssid[WIFI_SSID_MAXLEN] = {0};
static char wifi_pass[WIFI_PASS_MAXLEN] = {0};

// ===== HDMI Switch =====
typedef enum {
    TS3_STATE_DISABLED,
    TS3_STATE_ALL_A,
    TS3_STATE_ALL_B
} ts3_state_t;

static ts3_state_t current_state = TS3_STATE_DISABLED;
#define MAX_WIFI_NETWORKS 10
static wifi_ap_record_t top_networks[MAX_WIFI_NETWORKS];
static int top_networks_count = 0;

void scan_wifi_networks(void) {
    // 1. Initialize Wi-Fi
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&cfg);
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();

// 2. Small delay to allow driver to start
vTaskDelay(pdMS_TO_TICKS(100));

// 3. Scan
wifi_scan_config_t scan_config = {0};
scan_config.show_hidden = true;
esp_wifi_scan_start(&scan_config, true);

uint16_t ap_count = 20;
wifi_ap_record_t ap_info[20];
esp_wifi_scan_get_ap_records(&ap_count, ap_info);

// 4. Filter out empty SSIDs
int valid_count = 0;
for (int i = 0; i < ap_count; i++) {
    if (strlen((char*)ap_info[i].ssid) > 0) {
        top_networks[valid_count++] = ap_info[i];
        if (valid_count >= MAX_WIFI_NETWORKS) break;
    }
}
top_networks_count = valid_count;

// 5. Switch to AP mode for config portal
esp_wifi_stop();
esp_wifi_set_mode(WIFI_MODE_AP);
esp_wifi_start();
}

void ts3dv642_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TS3_EN) | (1ULL << TS3_SEL1) | (1ULL << TS3_SEL2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(TS3_EN, 0);
}

void ts3dv642_set_state(ts3_state_t state) {
    current_state = state;
    switch (state) {
        case TS3_STATE_DISABLED:
            gpio_set_level(TS3_EN, 0);
            break;
        case TS3_STATE_ALL_A:
            gpio_set_level(TS3_EN, 1);
            gpio_set_level(TS3_SEL1, 1);
            gpio_set_level(TS3_SEL2, 0);
            break;
        case TS3_STATE_ALL_B:
            gpio_set_level(TS3_EN, 1);
            gpio_set_level(TS3_SEL1, 1);
            gpio_set_level(TS3_SEL2, 1);
            break;
    }
    ESP_LOGI(TAG, "Switch set to %d", state);
}

// ===== HTTP Handlers =====
static esp_err_t root_get_handler(httpd_req_t *req) {
    // Convert current_state to string
    char state_str[8];
    switch(current_state) {
        case TS3_STATE_ALL_A: state_str[0]='A'; state_str[1]='\0'; break;
        case TS3_STATE_ALL_B: state_str[0]='B'; state_str[1]='\0'; break;
        case TS3_STATE_DISABLED:
        default: strcpy(state_str, "OFF"); break;
    }

    // Build HTML
    char html[2048];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "  <meta charset=\"UTF-8\">"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "  <title>HDMI MUX Control</title>"
        "  <style>"
        "    body {"
        "      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;"
        "      display: flex;"
        "      flex-direction: column;"
        "      align-items: center;"
        "      justify-content: center;"
        "      height: 100vh;"
        "      margin: 0;"
        "      background: linear-gradient(135deg, #74ABE2, #5563DE);"
        "      color: #fff;"
        "    }"
        "    h1 { margin-bottom: 20px; text-shadow: 1px 1px 2px rgba(0,0,0,0.3); }"
        "    .status { margin-bottom: 30px; font-size: 1.5rem; padding: 10px 20px; "
        "      background: rgba(255,255,255,0.2); border-radius: 10px; text-align: center; }"
        "    .btn {"
        "      background: #fff; color: #5563DE; border: none; padding: 15px 30px; "
        "      margin: 10px; font-size: 1.2rem; border-radius: 8px; cursor: pointer; "
        "      transition: 0.3s; min-width: 160px;"
        "    }"
        "    .btn:hover { background: #f0f0f0; transform: translateY(-3px); "
        "      box-shadow: 0 4px 6px rgba(0,0,0,0.2); }"
        "    form { display: flex; flex-direction: column; align-items: center; }"
        "  </style>"
        "</head>"
        "<body>"
        "  <h1>HDMI MUX Control</h1>"
        "  <div class=\"status\">Current State: <strong>%s</strong></div>"
        "  <form action=\"/set\">"
        "    <button class=\"btn\" name=\"state\" value=\"A\">Switch to A</button>"
        "    <button class=\"btn\" name=\"state\" value=\"B\">Switch to B</button>"
        "    <button class=\"btn\" name=\"state\" value=\"OFF\">Disable</button>"
        "  </form>"
        "</body>"
        "</html>",
        state_str
    );

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t set_handler(httpd_req_t *req) {
    char buf[16];
    int len = httpd_req_get_url_query_len(req) + 1;
    if (len > 1) {
        char *qry = malloc(len);
        if (httpd_req_get_url_query_str(req, qry, len) == ESP_OK) {
            if (httpd_query_key_value(qry, "state", buf, sizeof(buf)) == ESP_OK) {
                if (strcmp(buf, "A") == 0) ts3dv642_set_state(TS3_STATE_ALL_A);
                else if (strcmp(buf, "B") == 0) ts3dv642_set_state(TS3_STATE_ALL_B);
                else ts3dv642_set_state(TS3_STATE_DISABLED);
            }
        }
        free(qry);
    }
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_config_get_handler(httpd_req_t *req) {
    // Send HTML in chunks
    httpd_resp_send_chunk(req,
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "  <meta charset=\"UTF-8\">"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "  <title>Wi-Fi Config</title>"
        "  <style>"
        "    body { font-family:'Segoe UI',sans-serif; display:flex; flex-direction:column;"
        "           align-items:center; justify-content:center; height:100vh; margin:0;"
        "           background:linear-gradient(135deg,#74ABE2,#5563DE); color:#fff; }"
        "    h2 { margin-bottom:20px; }"
        "    select, input { font-size:1rem; padding:10px; margin:10px; border-radius:5px; "
        "                    width:250px; box-sizing:border-box; }"
        "    button { font-size:1rem; padding:10px 20px; border-radius:8px;"
        "             background:#fff; color:#5563DE; border:none; cursor:pointer; }"
        "    button:hover { background:#f0f0f0; }"
        "    form { display:flex; flex-direction:column; align-items:center; }"
        "  </style>"
        "</head>"
        "<body>"
        "<h2>Select Wi-Fi Network</h2>"
        "<form action=\"/save\" method=\"get\">"
        "<label for=\"ssid\">SSID:</label>"
        "<select name=\"ssid\" id=\"ssid\">", -1);

    char option[128];
    for (int i = 0; i < top_networks_count; i++) {
        int len = snprintf(option, sizeof(option),
                           "<option value=\"%s\">%s (%d dBm)</option>",
                           top_networks[i].ssid, top_networks[i].ssid, top_networks[i].rssi);
        httpd_resp_send_chunk(req, option, len);
    }

    // Finish HTML
    httpd_resp_send_chunk(req,
        "</select>"
        "<label for=\"pass\">Password:</label>"
        "<input type=\"password\" id=\"pass\" name=\"pass\" placeholder=\"Wi-Fi Password\">"
        "<button type=\"submit\">Save & Connect</button>"
        "</form>"
        "</body></html>", -1);

    httpd_resp_send_chunk(req, NULL, 0); // end of chunks
    return ESP_OK;
}


static esp_err_t wifi_save_handler(httpd_req_t *req) {
    char ssid[WIFI_SSID_MAXLEN] = {0};
    char pass[WIFI_PASS_MAXLEN] = {0};

    int len = httpd_req_get_url_query_len(req) + 1;
    if (len > 1) {
        char *qry = malloc(len);
        if (httpd_req_get_url_query_str(req, qry, len) == ESP_OK) {
            httpd_query_key_value(qry, "ssid", ssid, sizeof(ssid));
            httpd_query_key_value(qry, "pass", pass, sizeof(pass));
        }
        free(qry);
    }

    // Save to NVS
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "wifi_ssid", ssid);
        nvs_set_str(nvs, "wifi_pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    httpd_resp_sendstr(req, "Saved! Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static httpd_handle_t start_webserver(bool config_mode) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        if (config_mode) {
            httpd_uri_t root = { .uri="/", .method=HTTP_GET, .handler=wifi_config_get_handler };
            httpd_uri_t save = { .uri="/save", .method=HTTP_GET, .handler=wifi_save_handler };
            httpd_register_uri_handler(server, &root);
            httpd_register_uri_handler(server, &save);
        } else {
            httpd_uri_t root = { .uri="/", .method=HTTP_GET, .handler=root_get_handler };
            httpd_uri_t set  = { .uri="/set", .method=HTTP_GET, .handler=set_handler };
            httpd_register_uri_handler(server, &root);
            httpd_register_uri_handler(server, &set);
        }
    }
    return server;
}

// ===== mDNS =====
static void start_mdns(void) {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("hdmi-mux"));   // hostname -> hdmi-mux.local
    ESP_ERROR_CHECK(mdns_instance_name_set("HDMI MUX Web Interface"));

    // Advertise HTTP
    mdns_service_add("HDMI MUX WebServer", "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS started: http://hdmi-mux.local/");
}

// ===== Wi-Fi =====
static void wifi_connect_sta(const char *ssid, const char *pass) {
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void wifi_start_ap(void) {
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32_Config",
            .ssid_len = strlen("ESP32_Config"),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 2
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Started AP mode, SSID: ESP32_Config");
}

// ===== Main App =====
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ts3dv642_init();

    // Check for stored Wi-Fi credentials
    bool have_wifi_creds = false;
    size_t len;
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        len = sizeof(wifi_ssid);
        if (nvs_get_str(nvs, "wifi_ssid", wifi_ssid, &len) == ESP_OK) {
            len = sizeof(wifi_pass);
            if (nvs_get_str(nvs, "wifi_pass", wifi_pass, &len) == ESP_OK) {
                have_wifi_creds = true;
            }
        }
        nvs_close(nvs);
    }

    if (have_wifi_creds) {
        ESP_LOGI(TAG, "Connecting to saved WiFi: %s", wifi_ssid);
        wifi_connect_sta(wifi_ssid, wifi_pass);
        start_webserver(false);
        start_mdns();   // STA mode mdns
    } else {
        scan_wifi_networks();
        wifi_start_ap();
        start_webserver(true);
        start_mdns();   // AP mode mdns
    }
}
