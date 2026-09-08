/**
 * network_comm.c - 网络通信模块实现
 * WiFi 连接、MQTT 通信、云端交互
 */

#include "network_comm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* NuttX 网络头文件 */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>

/* cJSON 用于 JSON 解析 */
#include <cjson/cJSON.h>

/* ==================== 全局变量 ==================== */
static wifi_config_t wifi_config = {0};
static mqtt_config_t mqtt_config = {0};
static device_status_t device_status = {0};

/* MQTT 连接 socket */
static int mqtt_socket = -1;

/* 回调函数 */
static mqtt_msg_callback_t mqtt_callback = NULL;
static wifi_status_callback_t wifi_callback = NULL;
static alarm_callback_t alarm_callback = NULL;
static ai_command_callback_t ai_command_callback = NULL;

/* 心跳定时器 */
static uint32_t last_heartbeat_time = 0;
#define HEARTBEAT_INTERVAL 30000  // 30秒

/* ==================== 内部函数声明 ==================== */
static int create_tcp_socket(const char *host, uint16_t port);
static int mqtt_send_connect(void);
static int mqtt_send_subscribe(const char *topic, int qos);
static int mqtt_send_publish(const char *topic, const char *payload, int qos, bool retain);
static int mqtt_send_pingreq(void);
static int mqtt_send_disconnect(void);
static int mqtt_parse_packet(void);
static char* create_json_message(msg_type_t type, const void *data);

/* ==================== 编码 MQTT 剩余长度 ==================== */
static int mqtt_encode_remaining_length(uint8_t *buf, int length)
{
    int pos = 0;
    do {
        uint8_t byte = length % 128;
        length /= 128;
        if (length > 0) {
            byte |= 0x80;
        }
        buf[pos++] = byte;
    } while (length > 0);
    return pos;
}

/* ==================== 初始化网络通信 ==================== */
int network_comm_init(void)
{
    printf("network_comm init...\n");

    /* 初始化配置 */
    memset(&wifi_config, 0, sizeof(wifi_config));
    memset(&mqtt_config, 0, sizeof(mqtt_config));
    memset(&device_status, 0, sizeof(device_status));

    /* 设置默认 MQTT 服务器 */
    strncpy(mqtt_config.broker, "broker.emqx.io", sizeof(mqtt_config.broker) - 1);
    mqtt_config.port = 1883;
    strncpy(mqtt_config.client_id, "zhi_ai_001", sizeof(mqtt_config.client_id) - 1);

    printf("network_comm init done\n");
    return 0;
}

/* ==================== 反初始化 ==================== */
void network_comm_deinit(void)
{
    mqtt_disconnect();
    wifi_disconnect();
    printf("network_comm deinit\n");
}

/* ==================== WiFi 连接 ==================== */
int wifi_connect(const char *ssid, const char *password)
{
    printf("WiFi connecting: %s\n", ssid);

    /* 保存配置 */
    strncpy(wifi_config.ssid, ssid, sizeof(wifi_config.ssid) - 1);
    strncpy(wifi_config.password, password, sizeof(wifi_config.password) - 1);

    /* NuttX WiFi 连接实现 */
    #ifdef CONFIG_NETINET_WIRELESS
    #include <nuttx/wireless/wireless.h>

    /* 打开网络设备 */
    int fd = open("/dev/wlan0", O_RDWR);
    if (fd < 0) {
        printf("WiFi: open /dev/wlan0 failed: %d\n", errno);
        return -1;
    }

    /* 设置 WiFi 模式为 Station */
    struct wireless_config_s wconfig;
    memset(&wconfig, 0, sizeof(wconfig));
    strncpy(wconfig.essid, ssid, sizeof(wconfig.essid) - 1);
    wconfig.has_essid = true;
    wconfig.mode = IW_MODE_INFRA;

    int ret = ioctl(fd, SIOCSIWLCFG, &wconfig);
    if (ret < 0) {
        printf("WiFi: set mode failed: %d\n", errno);
        close(fd);
        return -1;
    }

    /* 设置密码（WPA2） */
    struct iwreq wr;
    memset(&wr, 0, sizeof(wr));
    strncpy(wr.ifr_name, "wlan0", IFNAMSIZ);

    struct iw_encode_ext *ext = malloc(sizeof(struct iw_encode_ext) + strlen(password));
    if (ext) {
        memset(ext, 0, sizeof(*ext));
        ext->alg = SIOCSIWENCODEEXT;
        ext->key_len = strlen(password);
        ext->ext_flags |= IW_ENCODE_EXT_SET_CRYPT_KEY;
        memcpy(ext->key, password, strlen(password));

        wr.u.data.pointer = ext;
        wr.u.data.length = sizeof(*ext) + strlen(password);

        ret = ioctl(fd, SIOCSIWENCODEEXT, &wr);
        free(ext);

        if (ret < 0) {
            printf("WiFi: set password failed: %d\n", errno);
            close(fd);
            return -1;
        }
    }

    /* 连接网络 */
    memset(&wconfig, 0, sizeof(wconfig));
    strncpy(wconfig.essid, ssid, sizeof(wconfig.essid) - 1);
    wconfig.has_essid = true;

    ret = ioctl(fd, SIOCSIWESSID, &wconfig);
    if (ret < 0) {
        printf("WiFi: connect failed: %d\n", errno);
        close(fd);
        return -1;
    }

    close(fd);
    #endif

    /* 模拟连接成功（如果上面的驱动不可用） */
    wifi_config.connected = true;
    wifi_config.rssi = -50;

    printf("WiFi connected: %s\n", ssid);

    /* 触发回调 */
    if (wifi_callback) {
        wifi_callback(true);
    }

    return 0;
}

/* ==================== WiFi 断开 ==================== */
int wifi_disconnect(void)
{
    /* NuttX WiFi 断开实现 */
    #ifdef CONFIG_NETINET_WIRELESS
    int fd = open("/dev/wlan0", O_RDWR);
    if (fd >= 0) {
        /* 断开连接 */
        struct wireless_config_s wconfig;
        memset(&wconfig, 0, sizeof(wconfig));
        wconfig.has_essid = true;

        ioctl(fd, SIOCSIWESSID, &wconfig);
        close(fd);
    }
    #endif

    wifi_config.connected = false;
    printf("WiFi disconnected\n");

    if (wifi_callback) {
        wifi_callback(false);
    }

    return 0;
}

/* ==================== 检查 WiFi 状态 ==================== */
bool wifi_is_connected(void)
{
    return wifi_config.connected;
}

/* ==================== 获取信号强度 ==================== */
int wifi_get_rssi(void)
{
    /* NuttX WiFi 获取信号强度 */
    #ifdef CONFIG_NETINET_WIRELESS
    int fd = open("/dev/wlan0", O_RDWR);
    if (fd >= 0) {
        struct iwreq wr;
        memset(&wr, 0, sizeof(wr));
        strncpy(wr.ifr_name, "wlan0", IFNAMSIZ);

        if (ioctl(fd, SIOCSIWRATE, &wr) == 0) {
            wifi_config.rssi = wr.u.bitrate.value;
        }
        close(fd);
    }
    #endif

    return wifi_config.rssi;
}

/* ==================== MQTT 连接 ==================== */
int mqtt_connect(const char *broker, uint16_t port,
                const char *client_id, const char *username, const char *password)
{
    printf("MQTT connecting: %s:%d\n", broker, port);

    /* 保存配置 */
    strncpy(mqtt_config.broker, broker, sizeof(mqtt_config.broker) - 1);
    mqtt_config.port = port;
    strncpy(mqtt_config.client_id, client_id, sizeof(mqtt_config.client_id) - 1);
    if (username) {
        strncpy(mqtt_config.username, username, sizeof(mqtt_config.username) - 1);
    }
    if (password) {
        strncpy(mqtt_config.password, password, sizeof(mqtt_config.password) - 1);
    }

    /* 创建 TCP 连接 */
    mqtt_socket = create_tcp_socket(broker, port);
    if (mqtt_socket < 0) {
        printf("TCP connect failed\n");
        return -1;
    }

    /* 发送 MQTT CONNECT 包 */
    if (mqtt_send_connect() < 0) {
        printf("MQTT CONNECT failed\n");
        close(mqtt_socket);
        mqtt_socket = -1;
        return -1;
    }

    mqtt_config.connected = true;
    printf("MQTT connected\n");

    /* 订阅命令主题 */
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/command", client_id);
    mqtt_subscribe(topic, 1);

    return 0;
}

/* ==================== MQTT 断开 ==================== */
int mqtt_disconnect(void)
{
    if (mqtt_socket >= 0) {
        mqtt_send_disconnect();
        close(mqtt_socket);
        mqtt_socket = -1;
    }

    mqtt_config.connected = false;
    printf("MQTT disconnected\n");
    return 0;
}

/* ==================== 检查 MQTT 状态 ==================== */
bool mqtt_is_connected(void)
{
    return mqtt_config.connected;
}

/* ==================== MQTT 订阅 ==================== */
int mqtt_subscribe(const char *topic, int qos)
{
    if (!mqtt_config.connected) {
        printf("MQTT not connected\n");
        return -1;
    }

    printf("Subscribe: %s\n", topic);
    return mqtt_send_subscribe(topic, qos);
}

/* ==================== MQTT 取消订阅 ==================== */
int mqtt_unsubscribe(const char *topic)
{
    if (!mqtt_config.connected) {
        return -1;
    }

    printf("Unsubscribe: %s\n", topic);
    // TODO: 实现 UNSUBSCRIBE
    return 0;
}

/* ==================== MQTT 发布 ==================== */
int mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!mqtt_config.connected) {
        printf("MQTT not connected\n");
        return -1;
    }

    printf("Publish to %s: %s\n", topic, payload);
    return mqtt_send_publish(topic, payload, qos, retain);
}

/* ==================== 上报设备状态 ==================== */
int report_device_status(const device_status_t *status)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/status", mqtt_config.client_id);

    char *json = create_json_message(MSG_TYPE_STATUS, status);
    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 0, true);
    free(json);

    return ret;
}

/* ==================== 上报报警 ==================== */
int report_alarm(const char *alarm_type, const char *details)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/alarm", mqtt_config.client_id);

    /* 构建报警 JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "alarm");
    cJSON_AddStringToObject(root, "alarm_type", alarm_type);
    cJSON_AddStringToObject(root, "details", details);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    /* 发布报警消息（QoS 1，确保送达） */
    int ret = mqtt_publish(topic, json, 1, false);
    free(json);

    /* 触发本地报警回调 */
    if (alarm_callback) {
        alarm_callback(alarm_type, details);
    }

    /* 发送手机推送通知 */
    push_send_alarm(alarm_type, details);

    return ret;
}

/* ==================== 上报心跳 ==================== */
int report_heartbeat(void)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/heartbeat", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "device_id", mqtt_config.client_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 0, false);
    free(json);

    return ret;
}

/* ==================== 发送命令响应 ==================== */
int send_command_response(const char *cmd_id, bool success, const char *message)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/response", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd_id", cmd_id);
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "message", message);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 1, false);
    free(json);

    return ret;
}

/* ==================== 注册回调函数 ==================== */
void network_set_mqtt_callback(mqtt_msg_callback_t callback)
{
    mqtt_callback = callback;
}

void network_set_wifi_callback(wifi_status_callback_t callback)
{
    wifi_callback = callback;
}

void network_set_alarm_callback(alarm_callback_t callback)
{
    alarm_callback = callback;
}

void network_set_ai_command_callback(ai_command_callback_t callback)
{
    ai_command_callback = callback;
}

/* ==================== 手机推送接口 ==================== */

/* 推送配置 */
static push_config_t push_config = {0};

/* Bark API 地址 */
#define BARK_API_URL "https://api.day.app"

/* PushPlus API 地址 */
#define PUSHPLUS_API_URL "https://www.pushplus.plus/send"

/* HTTP 请求缓冲区大小 */
#define HTTP_BUFFER_SIZE 2048

/* 初始化推送服务 */
int push_init(push_service_t service, const char *key)
{
    if (!key || key[0] == '\0') {
        printf("push_init: invalid key\n");
        return -1;
    }

    push_config.service = service;
    strncpy(push_config.push_key, key, sizeof(push_config.push_key) - 1);
    push_config.enabled = true;

    printf("push_init: service=%d, key=%s\n", service, key);
    return 0;
}

/* 发送 HTTP POST 请求 */
static int http_post(const char *url, const char *body)
{
    char host[128] = {0};
    char path[256] = {0};
    int port = 443;

    /* 解析 URL */
    if (sscanf(url, "https://%127[^/]/%255s", host, path) != 2) {
        if (sscanf(url, "http://%127[^/]/%255s", host, path) != 2) {
            printf("http_post: invalid URL: %s\n", url);
            return -1;
        }
        port = 80;
    }

    printf("http_post: host=%s, path=%s, port=%d\n", host, path, port);

    /* 创建 TCP 连接 */
    int sockfd = create_tcp_socket(host, port);
    if (sockfd < 0) {
        printf("http_post: TCP connect failed\n");
        return -1;
    }

    /* 构建 HTTP 请求 */
    char request[HTTP_BUFFER_SIZE];
    int body_len = strlen(body);
    snprintf(request, sizeof(request),
        "POST /%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, body_len, body);

    /* 发送请求 */
    int ret = send(sockfd, request, strlen(request), 0);
    if (ret < 0) {
        printf("http_post: send failed\n");
        close(sockfd);
        return -1;
    }

    /* 接收响应 */
    char response[1024];
    int len = recv(sockfd, response, sizeof(response) - 1, 0);
    if (len > 0) {
        response[len] = '\0';
        printf("http_post: response=%s\n", response);
    }

    close(sockfd);
    return 0;
}

/* 发送推送通知 */
int push_send_notification(const char *title, const char *content, const char *group)
{
    if (!push_config.enabled || push_config.push_key[0] == '\0') {
        printf("push_send_notification: push not enabled\n");
        return -1;
    }

    char url[256] = {0};
    char body[HTTP_BUFFER_SIZE] = {0};

    /* 构建 JSON 请求体 */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "title", title);
    cJSON_AddStringToObject(root, "body", content);
    if (group) {
        cJSON_AddStringToObject(root, "group", group);
    }
    cJSON_AddStringToObject(root, "device", "ZhiAi-Companion");

    char *json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_body) {
        return -1;
    }

    /* 根据服务类型构建 URL */
    switch (push_config.service) {
        case PUSH_SERVICE_BARK:
            snprintf(url, sizeof(url), "%s/%s/ZhiAi",
                    BARK_API_URL, push_config.push_key);
            break;

        case PUSH_SERVICE_PUSHPLUS:
            /* PushPlus 使用 POST 请求，token 放在 JSON 里 */
            {
                cJSON *pp_root = cJSON_Parse(json_body);
                cJSON_AddStringToObject(pp_root, "token", push_config.push_key);
                free(json_body);
                json_body = cJSON_PrintUnformatted(pp_root);
                cJSON_Delete(pp_root);
                snprintf(url, sizeof(url), "%s", PUSHPLUS_API_URL);
            }
            break;

        default:
            free(json_body);
            return -1;
    }

    /* 发送 HTTP 请求 */
    int ret = http_post(url, json_body);
    free(json_body);

    printf("push_send_notification: title=%s, ret=%d\n", title, ret);
    return ret;
}

/* 发送紧急报警推送 */
int push_send_alarm(const char *alarm_type, const char *details)
{
    char title[128] = {0};
    char content[256] = {0};

    snprintf(title, sizeof(title), "[ALARM] %s", alarm_type);
    snprintf(content, sizeof(content),
            "Device detected: %s\nDetails: %s\nPlease check immediately!",
            alarm_type, details);

    return push_send_notification(title, content, "alarm");
}

/* 发送健康提醒推送 */
int push_send_health_reminder(const char *title, const char *content)
{
    char push_title[128] = {0};
    snprintf(push_title, sizeof(push_title), "[Health] %s", title);

    return push_send_notification(push_title, content, "health");
}

/* 开关推送功能 */
void push_set_enabled(bool enabled)
{
    push_config.enabled = enabled;
    printf("push_set_enabled: %s\n", enabled ? "true" : "false");
}

/* 检查推送功能是否开启 */
bool push_is_enabled(void)
{
    return push_config.enabled;
}

/* ==================== AI 语音交互接口 ==================== */

int ai_send_voice_data(const uint8_t *audio_data, int len,
                       ai_reply_callback_t callback)
{
    if (!mqtt_config.connected) {
        printf("MQTT not connected, cannot send voice\n");
        return -1;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/voice", mqtt_config.client_id);

    /* 构建语音数据 JSON（实际项目中应使用二进制传输） */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "voice");
    cJSON_AddNumberToObject(root, "length", len);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    /* 简化：实际应将 audio_data 编码为 base64 */
    cJSON_AddStringToObject(root, "data", "binary_audio_data");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 1, false);
    free(json);

    /* TODO: 实际项目中需要等待云端回复并调用 callback */

    return ret;
}

int ai_send_text(const char *text, ai_reply_callback_t callback)
{
    if (!mqtt_config.connected) {
        printf("MQTT not connected, cannot send text\n");
        return -1;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/chat", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "chat");
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 1, false);
    free(json);

    /* TODO: 实际项目中需要等待云端回复并调用 callback */

    return ret;
}

/* ==================== 异常声音检测接口 ==================== */

int report_abnormal_sound(const char *sound_type, int confidence)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/sound_alarm", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "sound_alarm");
    cJSON_AddStringToObject(root, "sound_type", sound_type);
    cJSON_AddNumberToObject(root, "confidence", confidence);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    /* 高优先级发送（QoS 1） */
    int ret = mqtt_publish(topic, json, 1, false);
    free(json);

    /* 触发本地报警回调 */
    if (alarm_callback) {
        alarm_callback(sound_type, "Abnormal sound detected");
    }

    return ret;
}

/* ==================== 主动关怀接口 ==================== */

int send_proactive_reminder(const char *title, const char *content)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/reminder", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "reminder");
    cJSON_AddStringToObject(root, "title", title);
    cJSON_AddStringToObject(root, "content", content);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 1, false);
    free(json);

    return ret;
}

int report_health_data(int heart_rate, int blood_oxy)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/health", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "health");
    cJSON_AddNumberToObject(root, "heart_rate", heart_rate);
    cJSON_AddNumberToObject(root, "blood_oxygen", blood_oxy);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 0, true);
    free(json);

    return ret;
}

/* ==================== 设备联动接口 ==================== */

int send_device_command(const char *device_id, const char *command)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/device_cmd", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "device_command");
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "command", command);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 1, false);
    free(json);

    return ret;
}

/* ==================== 内部函数实现 ==================== */

/* 创建 TCP Socket */
static int create_tcp_socket(const char *host, uint16_t port)
{
    int sockfd;
    struct sockaddr_in server_addr;

    /* 创建 socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    /* 设置服务器地址 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    /* 解析主机名或 IP */
    struct hostent *he = gethostbyname(host);
    if (he) {
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    } else {
        server_addr.sin_addr.s_addr = inet_addr(host);
    }

    /* 连接服务器 */
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* 发送 MQTT CONNECT 包 */
static int mqtt_send_connect(void)
{
    uint8_t packet[512];
    int pos = 0;

    /* 固定头: CONNECT = 0x10 */
    packet[pos++] = 0x10;

    /* 可变头 */
    uint8_t variable_header[] = {
        0x00, 0x04, 'M', 'Q', 'T', 'T',  // 协议名
        0x04,  // 协议级别 (MQTT 3.1.1)
        0xC2,  // 连接标志（用户名+密码+遗嘱+清理会话）
        0x00, 0x3C,  // 保持连接时间 60 秒
    };

    /* 构建载荷 */
    uint8_t payload[256];
    int payload_pos = 0;

    /* 客户端 ID */
    int client_id_len = strlen(mqtt_config.client_id);
    payload[payload_pos++] = (client_id_len >> 8) & 0xFF;
    payload[payload_pos++] = client_id_len & 0xFF;
    memcpy(&payload[payload_pos], mqtt_config.client_id, client_id_len);
    payload_pos += client_id_len;

    /* 遗嘱主题 */
    char will_topic[128];
    snprintf(will_topic, sizeof(will_topic), "zhi_ai/%s/status", mqtt_config.client_id);
    int will_topic_len = strlen(will_topic);
    payload[payload_pos++] = (will_topic_len >> 8) & 0xFF;
    payload[payload_pos++] = will_topic_len & 0xFF;
    memcpy(&payload[payload_pos], will_topic, will_topic_len);
    payload_pos += will_topic_len;

    /* 遗嘱消息 */
    payload[payload_pos++] = 0x00;
    payload[payload_pos++] = 0x03;
    payload[payload_pos++] = 'O';
    payload[payload_pos++] = 'F';
    payload[payload_pos++] = 'F';

    /* 用户名 */
    if (mqtt_config.username[0]) {
        int username_len = strlen(mqtt_config.username);
        payload[payload_pos++] = (username_len >> 8) & 0xFF;
        payload[payload_pos++] = username_len & 0xFF;
        memcpy(&payload[payload_pos], mqtt_config.username, username_len);
        payload_pos += username_len;
    }

    /* 密码 */
    if (mqtt_config.password[0]) {
        int password_len = strlen(mqtt_config.password);
        payload[payload_pos++] = (password_len >> 8) & 0xFF;
        payload[payload_pos++] = password_len & 0xFF;
        memcpy(&payload[payload_pos], mqtt_config.password, password_len);
        payload_pos += password_len;
    }

    /* 计算剩余长度并编码 */
    int remaining = sizeof(variable_header) + payload_pos;
    pos += mqtt_encode_remaining_length(&packet[pos], remaining);

    /* 复制可变头和载荷 */
    memcpy(&packet[pos], variable_header, sizeof(variable_header));
    pos += sizeof(variable_header);
    memcpy(&packet[pos], payload, payload_pos);
    pos += payload_pos;

    /* 发送 */
    return send(mqtt_socket, packet, pos, 0);
}

/* 发送 MQTT SUBSCRIBE 包 */
static int mqtt_send_subscribe(const char *topic, int qos)
{
    uint8_t packet[256];
    int pos = 0;

    /* 固定头: SUBSCRIBE = 0x82 */
    packet[pos++] = 0x82;

    /* 剩余长度 = 2(包ID) + 2(topic长度) + topic_len + 1(QoS) */
    int topic_len = strlen(topic);
    int remaining = 2 + 2 + topic_len + 1;
    pos += mqtt_encode_remaining_length(&packet[pos], remaining);

    /* 包 ID */
    static uint16_t subscribe_id = 0;
    subscribe_id++;
    packet[pos++] = (subscribe_id >> 8) & 0xFF;
    packet[pos++] = subscribe_id & 0xFF;

    /* 主题过滤器 */
    packet[pos++] = (topic_len >> 8) & 0xFF;
    packet[pos++] = topic_len & 0xFF;
    memcpy(&packet[pos], topic, topic_len);
    pos += topic_len;

    /* QoS */
    packet[pos++] = qos;

    return send(mqtt_socket, packet, pos, 0);
}

/* 发送 MQTT PUBLISH 包 */
static int mqtt_send_publish(const char *topic, const char *payload, int qos, bool retain)
{
    uint8_t packet[1024];
    int pos = 0;

    /* 固定头 */
    uint8_t type = 0x30;  // PUBLISH
    if (qos == 1) type |= 0x02;
    if (qos == 2) type |= 0x04;
    if (retain) type |= 0x01;
    packet[pos++] = type;

    /* 剩余长度 = 2 + topic_len + payload_len [+ 2(包ID)] */
    int topic_len = strlen(topic);
    int payload_len = strlen(payload);
    int remaining = 2 + topic_len + payload_len;
    if (qos > 0) remaining += 2;
    pos += mqtt_encode_remaining_length(&packet[pos], remaining);

    /* 主题名 */
    packet[pos++] = (topic_len >> 8) & 0xFF;
    packet[pos++] = topic_len & 0xFF;
    memcpy(&packet[pos], topic, topic_len);
    pos += topic_len;

    /* 包 ID（QoS 1 或 2 时） */
    if (qos > 0) {
        static uint16_t packet_id = 0;
        packet_id++;
        packet[pos++] = (packet_id >> 8) & 0xFF;
        packet[pos++] = packet_id & 0xFF;
    }

    /* 载荷 */
    memcpy(&packet[pos], payload, payload_len);
    pos += payload_len;

    return send(mqtt_socket, packet, pos, 0);
}

/* 发送 MQTT PINGREQ */
static int mqtt_send_pingreq(void)
{
    uint8_t packet[2] = {0xC0, 0x00};
    return send(mqtt_socket, packet, 2, 0);
}

/* 发送 MQTT DISCONNECT */
static int mqtt_send_disconnect(void)
{
    uint8_t packet[2] = {0xE0, 0x00};
    return send(mqtt_socket, packet, 2, 0);
}

/* 解析 MQTT 数据包 */
static int mqtt_parse_packet(void)
{
    uint8_t buffer[1024];
    int len = recv(mqtt_socket, buffer, sizeof(buffer), 0);
    if (len <= 0) {
        return -1;
    }

    uint8_t type = (buffer[0] >> 4) & 0x0F;

    switch (type) {
        case 0x0D:  // PINGRESP
            printf("Received PINGRESP\n");
            break;

        case 0x03:  // PUBLISH
            /* 解析主题和载荷 */
            {
                int pos = 1;

                /* 跳过剩余长度编码 */
                int remaining = 0;
                int multiplier = 1;
                do {
                    remaining += (buffer[pos] & 0x7F) * multiplier;
                    multiplier *= 128;
                    pos++;
                } while (buffer[pos - 1] & 0x80);

                /* 解析主题 */
                int topic_len = (buffer[pos] << 8) | buffer[pos + 1];
                pos += 2;

                char topic[128];
                int copy_len = topic_len < (int)(sizeof(topic) - 1) ? topic_len : (int)(sizeof(topic) - 1);
                memcpy(topic, &buffer[pos], copy_len);
                topic[copy_len] = '\0';
                pos += topic_len;

                /* 解析载荷 */
                char payload[1024];
                int payload_len = len - pos;
                if (payload_len > (int)(sizeof(payload) - 1)) {
                    payload_len = (int)(sizeof(payload) - 1);
                }
                memcpy(payload, &buffer[pos], payload_len);
                payload[payload_len] = '\0';

                printf("Received: topic=%s, payload=%s\n", topic, payload);

                /* 调用回调 */
                if (mqtt_callback) {
                    mqtt_callback(topic, payload);
                }
            }
            break;

        default:
            printf("Unknown packet type: %d\n", type);
            break;
    }

    return 0;
}

/* 创建 JSON 消息 */
static char* create_json_message(msg_type_t type, const void *data)
{
    cJSON *root = cJSON_CreateObject();

    switch (type) {
        case MSG_TYPE_STATUS: {
            const device_status_t *status = (const device_status_t *)data;
            cJSON_AddStringToObject(root, "type", "status");
            cJSON_AddNumberToObject(root, "temperature", status->temperature);
            cJSON_AddNumberToObject(root, "humidity", status->humidity);
            cJSON_AddNumberToObject(root, "battery", status->battery_level);
            cJSON_AddStringToObject(root, "status", status->status);
            cJSON_AddBoolToObject(root, "alarm", status->alarm_active);
            cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
            break;
        }

        case MSG_TYPE_HEARTBEAT:
            cJSON_AddStringToObject(root, "type", "heartbeat");
            cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
            break;

        default:
            cJSON_Delete(root);
            return NULL;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json;
}

/* ==================== 网络任务（后台运行） ==================== */
void network_task(void *arg)
{
    printf("network_task started\n");

    while (1) {
        /* 检查 WiFi 状态 */
        if (!wifi_config.connected) {
            /* 尝试重连 */
            // TODO: 实现重连逻辑
        }

        /* 检查 MQTT 状态 */
        if (wifi_config.connected && !mqtt_config.connected) {
            /* 尝试连接 MQTT */
            mqtt_connect(mqtt_config.broker, mqtt_config.port,
                        mqtt_config.client_id, mqtt_config.username, mqtt_config.password);
        }

        /* 接收 MQTT 消息 */
        if (mqtt_config.connected) {
            mqtt_parse_packet();

            /* 发送心跳 */
            uint32_t now = (uint32_t)time(NULL) * 1000;
            if (now - last_heartbeat_time >= HEARTBEAT_INTERVAL) {
                report_heartbeat();
                last_heartbeat_time = now;
            }
        }

        usleep(100000);  // 100ms
    }
}
