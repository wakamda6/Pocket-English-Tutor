#include "tutor_setup.h"
#include "tutor_setup_dns.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mbedtls/platform_util.h"

static const char page_head[] =
    "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>英语外教配网</title><style>"
    "body{margin:0;background:#12222c;color:#f4ebdc;font:16px system-ui,sans-serif}"
    "main{max-width:26rem;margin:auto;padding:2rem 1.35rem}h1{font-size:1.6rem}"
    "p{color:#bfccc8;line-height:1.5}label{display:block;margin-top:1.1rem}"
    "input,select{box-sizing:border-box;width:100%;margin-top:.35rem;padding:.7rem;color:#f4ebdc;"
    "background:#20343f;border:1px solid #657c80;border-radius:.45rem;font:inherit}"
    "button,a{color:#11232c;background:#6ed6b5;border:0;border-radius:.45rem;"
    "padding:.7rem 1rem;font:inherit;display:inline-block;margin-top:1.15rem}"
    ".secondary{background:transparent;color:#bfccc8;border:1px solid #657c80}"
    "</style>";

static const char scan_script[] =
    "<script>"
    "const picker=document.getElementById('networks'), nameField=document.getElementById('ssid'),"
    "scanButton=document.getElementById('scan'), info=document.getElementById('scan-status');"
    "picker.addEventListener('change',()=>{if(picker.value)nameField.value=picker.value});"
    "async function scan(){scanButton.disabled=true;info.textContent='正在扫描附近的无线网络…';"
    "try{const response=await fetch('/networks',{cache:'no-store'});"
    "if(!response.ok)throw Error();const networks=await response.json();"
    "picker.replaceChildren(new Option('请选择无线网络',''));"
    "for(const network of networks)picker.add(new Option(network,network));"
    "picker.value=nameField.value;"
    "info.textContent=networks.length?'':'未找到无线网络，可手动输入或重新扫描。';"
    "}catch{info.textContent='扫描失败，可手动输入或重新扫描。'}"
    "finally{scanButton.disabled=false}}"
    "scanButton.addEventListener('click',scan);scan();</script>";

static httpd_handle_t server;
static esp_netif_t *ap_interface;
static QueueHandle_t requests;
static SemaphoreHandle_t dns_exited;
static atomic_bool dns_stop;
static atomic_int portal_status;
static char ssid[17];
static char csrf_token[17];

static bool arrived_on_ap(httpd_req_t *req)
{
    struct sockaddr_storage local = {0};
    socklen_t length = sizeof(local);
    int socket_fd = httpd_req_to_sockfd(req);
    if (getsockname(socket_fd, (struct sockaddr *)&local, &length) != 0) return false;

    in_addr_t ap = inet_addr(TUTOR_SETUP_IP);
    bool on_ap = false;
    if (local.ss_family == AF_INET) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)&local;
        on_ap = address->sin_addr.s_addr == ap;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)&local;
        on_ap = IN6_IS_ADDR_V4MAPPED(&address->sin6_addr) &&
                memcmp(address->sin6_addr.s6_addr + 12, &ap, sizeof(ap)) == 0;
    }
#endif
    return on_ap;
}

static bool is_local_request(httpd_req_t *req)
{
    char host[24];
    return arrived_on_ap(req) &&
           httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
           (strcmp(host, TUTOR_SETUP_IP) == 0 || strcmp(host, TUTOR_SETUP_IP ":80") == 0);
}

static esp_err_t send_setup_text(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t redirect_to_setup(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    if (!arrived_on_ap(req)) {
        return send_setup_text(req, "403 Forbidden", "请连接设备配网热点");
    }
    httpd_resp_set_hdr(req, "Location", "http://" TUTOR_SETUP_IP "/");
    return send_setup_text(req, "302 Found", "请打开设备配网页面");
}

static void set_page_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
}

static esp_err_t show_setup(httpd_req_t *req)
{
    if (!is_local_request(req)) {
        return redirect_to_setup(req, HTTPD_404_NOT_FOUND);
    }
    set_page_headers(req);
    tutor_setup_status_t state = atomic_load(&portal_status);
    if (httpd_resp_sendstr_chunk(req, page_head) != ESP_OK) return ESP_FAIL;
    if (state == TUTOR_SETUP_CHECKING &&
        httpd_resp_sendstr_chunk(req, "<meta http-equiv='refresh' content='1'>") != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, "<main><h1>英语外教配网</h1>") != ESP_OK) {
        return ESP_FAIL;
    }
    const char *message;
    switch (state) {
    case TUTOR_SETUP_CHECKING:
        message = "正在连接无线网络。请保持连接设备热点。";
        break;
    case TUTOR_SETUP_WIFI_FAILED:
        message = "连接失败。请检查无线网络名称和密码，确认路由器开启了 2.4 GHz。原设置仍然保留。";
        break;
    case TUTOR_SETUP_SAVE_FAILED:
        message = "保存失败。请重新填写后再试。";
        break;
    case TUTOR_SETUP_SAVED:
        message = "设置已保存。设备将关闭热点并重启，接下来请看设备屏幕。";
        break;
    case TUTOR_SETUP_CLEARED:
        message = "设置已清除。设备将重启并打开新的配网热点。";
        break;
    default:
        message = "选择 2.4 GHz 的无线网络，再填写密码和豆包实时语音密钥。";
        break;
    }
    if (httpd_resp_sendstr_chunk(req, "<p>") != ESP_OK ||
        httpd_resp_sendstr_chunk(req, message) != ESP_OK ||
        httpd_resp_sendstr_chunk(req, "</p>") != ESP_OK) {
        return ESP_FAIL;
    }
    if (state == TUTOR_SETUP_FORM || state == TUTOR_SETUP_WIFI_FAILED ||
        state == TUTOR_SETUP_SAVE_FAILED) {
        char token_field[72];
        snprintf(token_field, sizeof(token_field),
                 "<input type='hidden' name='token' value='%s'>", csrf_token);
        if (httpd_resp_sendstr_chunk(req, "<form method='post' action='/configure' autocomplete='off'>") != ESP_OK ||
            httpd_resp_sendstr_chunk(req, token_field) != ESP_OK ||
            httpd_resp_sendstr_chunk(req,
                "<label>附近的无线网络<select id='networks'><option value=''>请选择无线网络</option>"
                "</select></label><button class='secondary' id='scan' type='button'>重新扫描</button>"
                "<p id='scan-status' role='status'></p>"
                "<label>无线网络名称<input id='ssid' name='ssid' maxlength='32' required></label>"
                "<label>无线网络密码<input name='password' type='password' maxlength='63' "
                "placeholder='开放网络可留空'></label>"
                "<label>豆包实时语音密钥<input name='key' type='password' maxlength='128' "
                "autocapitalize='off' required></label><button>连接并保存</button></form>"
                "<form method='post' action='/configure'>") != ESP_OK ||
            httpd_resp_sendstr_chunk(req, token_field) != ESP_OK ||
            httpd_resp_sendstr_chunk(req,
                "<button class='secondary' name='clear' value='1'>清除设备配置</button></form>") != ESP_OK ||
            httpd_resp_sendstr_chunk(req, scan_script) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (httpd_resp_sendstr_chunk(req, "</main></html>") != ESP_OK) return ESP_FAIL;
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t list_networks(httpd_req_t *req)
{
    if (!is_local_request(req)) {
        return send_setup_text(req, "403 Forbidden", "请连接设备配网热点");
    }
    tutor_setup_status_t state = atomic_load(&portal_status);
    if (state != TUTOR_SETUP_FORM && state != TUTOR_SETUP_WIFI_FAILED &&
        state != TUTOR_SETUP_SAVE_FAILED) {
        return send_setup_text(req, "400 Bad Request", "无线网络正在使用中");
    }
    wifi_scan_config_t config = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 40, .max = 85 },
        .home_chan_dwell_time = 40,
    };
    if (esp_wifi_scan_start(&config, true) != ESP_OK) {
        return send_setup_text(req, "500 Internal Server Error", "扫描无线网络失败");
    }
    // The HTTP server serializes scan requests. Keep the result list off its stack.
    static wifi_ap_record_t records[16];
    uint16_t count = sizeof(records) / sizeof(records[0]);
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return send_setup_text(req, "500 Internal Server Error", "扫描无线网络失败");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (httpd_resp_sendstr_chunk(req, "[") != ESP_OK) return ESP_FAIL;
    bool first = true;
    for (uint16_t i = 0; i < count; i++) {
        if (!records[i].ssid[0]) continue;
        bool duplicate = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strcmp((const char *)records[i].ssid, (const char *)records[j].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        cJSON *value = cJSON_CreateString((const char *)records[i].ssid);
        char name_json[196];
        bool encoded = value && cJSON_PrintPreallocated(value, name_json, sizeof(name_json), false);
        cJSON_Delete(value);
        if (!encoded || (!first && httpd_resp_sendstr_chunk(req, ",") != ESP_OK) ||
            httpd_resp_sendstr_chunk(req, name_json) != ESP_OK) {
            return ESP_FAIL;
        }
        first = false;
    }
    if (httpd_resp_sendstr_chunk(req, "]") != ESP_OK) return ESP_FAIL;
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t submit_setup(httpd_req_t *req)
{
    if (!is_local_request(req)) {
        return send_setup_text(req, "403 Forbidden", "请连接设备配网热点");
    }
    char origin[25];
    if (httpd_req_get_hdr_value_len(req, "Origin") > 0 &&
        (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
         strcmp(origin, "http://" TUTOR_SETUP_IP) != 0)) {
        return send_setup_text(req, "403 Forbidden", "请从设备配网页面提交");
    }
    tutor_setup_status_t state = atomic_load(&portal_status);
    if (state == TUTOR_SETUP_CHECKING || state == TUTOR_SETUP_SAVED || state == TUTOR_SETUP_CLEARED) {
        return send_setup_text(req, "400 Bad Request", "请等待当前操作完成");
    }
    if (req->content_len == 0 || req->content_len > TUTOR_FORM_CAP) {
        return send_setup_text(req, "400 Bad Request", "表单大小不正确");
    }
    static const char form_type[] = "application/x-www-form-urlencoded";
    char content_type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncmp(content_type, form_type, sizeof(form_type) - 1) != 0 ||
        (content_type[sizeof(form_type) - 1] != 0 && content_type[sizeof(form_type) - 1] != ';')) {
        return send_setup_text(req, "400 Bad Request", "表单格式不正确");
    }
    char body[TUTOR_FORM_CAP + 1];
    size_t received = 0;
    TickType_t started = xTaskGetTickCount();
    while (received < req->content_len && xTaskGetTickCount() - started < pdMS_TO_TICKS(4000)) {
        int bytes = httpd_req_recv(req, body + received, req->content_len - received);
        if (bytes <= 0) {
            break;
        }
        received += (size_t)bytes;
    }
    body[received] = 0;
    tutor_setup_request_t request = {0};
    bool valid = received == req->content_len && !memchr(body, 0, received) &&
                 tutor_setup_parse_form(body, csrf_token, &request);
    mbedtls_platform_zeroize(body, sizeof(body));
    bool accepted = false;
    if (valid) {
        atomic_store(&portal_status, TUTOR_SETUP_CHECKING);
        accepted = xQueueSend(requests, &request, 0) == pdTRUE;
        if (!accepted) atomic_store(&portal_status, state);
    }
    mbedtls_platform_zeroize(&request, sizeof(request));
    if (!accepted) {
        return send_setup_text(req, "400 Bad Request",
                                  "配置无效，请返回并重新填写");
    }
    httpd_resp_set_hdr(req, "Location", "/");
    return send_setup_text(req, "303 See Other", "正在检查配置");
}

static void serve_dns(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint8_t packet[512];
    static const uint8_t address[] = {192, 168, 4, 1};
    while (!atomic_load(&dns_stop)) {
        struct sockaddr_in peer;
        socklen_t length = sizeof(peer);
        int bytes = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&peer, &length);
        if (bytes <= 0) continue;
        size_t reply = tutor_setup_dns_reply(packet, bytes, sizeof(packet), address);
        if (reply) sendto(fd, packet, reply, 0, (struct sockaddr *)&peer, length);
    }
    close(fd);
    xSemaphoreGive(dns_exited);
    vTaskDelete(NULL);
}

static esp_err_t start_dns(void)
{
    struct sockaddr_in local = {
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = inet_addr(TUTOR_SETUP_IP),
    };
    const struct timeval timeout = { .tv_usec = 200000 };
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return ESP_FAIL;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(fd);
        return ESP_FAIL;
    }
    dns_exited = xSemaphoreCreateBinary();
    atomic_store(&dns_stop, false);
    if (dns_exited && xTaskCreate(serve_dns, "tutor_dns", 3072, (void *)(intptr_t)fd, 4, NULL) == pdPASS) {
        return ESP_OK;
    }
    close(fd);
    if (dns_exited) vSemaphoreDelete(dns_exited);
    dns_exited = NULL;
    return ESP_ERR_NO_MEM;
}

esp_err_t tutor_setup_start(void)
{
    if (!ap_interface) {
        ap_interface = esp_netif_create_default_wifi_ap();
        if (!ap_interface) return ESP_ERR_NO_MEM;
        esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
        dns.ip.u_addr.ip4.addr = inet_addr(TUTOR_SETUP_IP);
        if (esp_netif_set_dns_info(ap_interface, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    requests = xQueueCreate(1, sizeof(tutor_setup_request_t));
    if (!requests) return ESP_ERR_NO_MEM;
    uint8_t mac[6];
    esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (result != ESP_OK) goto failed;
    snprintf(ssid, sizeof(ssid), "Passport-%02X%02X", mac[4], mac[5]);
    wifi_config_t config = {
        .ap = { .channel = 1, .max_connection = 1, .authmode = WIFI_AUTH_OPEN },
    };
    memcpy(config.ap.ssid, ssid, strlen(ssid));
    result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (result == ESP_OK) result = esp_wifi_start();
    if (result != ESP_OK) goto failed;

    uint32_t random[2];
    esp_fill_random(random, sizeof(random));
    snprintf(csrf_token, sizeof(csrf_token), "%08x%08x", (unsigned)random[0], (unsigned)random[1]);
    atomic_store(&portal_status, TUTOR_SETUP_FORM);
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_open_sockets = 3;
    http.backlog_conn = 2;
    http.recv_wait_timeout = 3;
    http.send_wait_timeout = 3;
    http.stack_size = 6144;
    http.lru_purge_enable = true;
    result = httpd_start(&server, &http);
    if (result == ESP_OK) {
        const httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = show_setup };
        const httpd_uri_t form = { .uri = "/configure", .method = HTTP_POST, .handler = submit_setup };
        const httpd_uri_t scan = { .uri = "/networks", .method = HTTP_GET, .handler = list_networks };
        result = httpd_register_uri_handler(server, &page);
        if (result == ESP_OK) result = httpd_register_uri_handler(server, &form);
        if (result == ESP_OK) result = httpd_register_uri_handler(server, &scan);
        if (result == ESP_OK) result = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_setup);
        if (result == ESP_OK) result = start_dns();
    }
    if (result == ESP_OK) return ESP_OK;
failed:
    tutor_setup_stop();
    esp_wifi_stop();
    return result;
}

void tutor_setup_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    if (dns_exited) {
        atomic_store(&dns_stop, true);
        xSemaphoreTake(dns_exited, portMAX_DELAY);
        vSemaphoreDelete(dns_exited);
        dns_exited = NULL;
    }
    if (requests) {
        tutor_setup_request_t empty = {0};
        // Overwrite the single queue slot even if its item was already consumed.
        xQueueOverwrite(requests, &empty);
        vQueueDelete(requests);
        requests = NULL;
    }
    mbedtls_platform_zeroize(csrf_token, sizeof(csrf_token));
}

bool tutor_setup_take_request(tutor_setup_request_t *request)
{
    if (!requests || xQueueReceive(requests, request, 0) != pdTRUE) {
        return false;
    }
    // The HTTP handler rejects new forms while this request is pending.
    tutor_setup_request_t empty = {0};
    xQueueOverwrite(requests, &empty);
    xQueueReset(requests);
    return true;
}

void tutor_setup_report(tutor_setup_status_t status)
{
    atomic_store(&portal_status, status);
}

const char *tutor_setup_ssid(void)
{
    return ssid;
}
