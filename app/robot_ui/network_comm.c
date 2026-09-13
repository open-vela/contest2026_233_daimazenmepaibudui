/**
 * network_comm.c - 网络通信模块实现
 * WiFi 连接、MQTT 通信、云端交互
 */

#include <nuttx/config.h>
#include "network_comm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>

/* NuttX 网络头文件 */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h> /* gethostbyname / struct hostent（DNS 解析） */
#include <unistd.h>

/* 板级外设状态（board/contest_board/src/sf32lb52_status.h）：
 * 这里只负责把 MQTT 连接状态喂进去，供 hw_test status / UI 统一查询。 */
#include "sf32lb52_status.h"

/* cJSON 用于 JSON 解析 */
#include <netutils/cJSON.h>

/* ai_agent TLS 客户端（HTTPS 推送） */
#include "infra/vela_tls.h"

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

/* 可用 broker 列表。公共 broker 会限流甚至直接把连接关掉（实测 broker.emqx.io
 * 在被高频重连后会连上就 RESET、不给 CONNACK），所以连不上就自动换下一台。
 */
static const char *g_mqtt_broker_list[] = {
    "broker.emqx.io",
    "test.mosquitto.org",
    "broker.hivemq.com",
};
#define MQTT_NBROKERS ((int)(sizeof(g_mqtt_broker_list) / sizeof(g_mqtt_broker_list[0])))

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
    board_status_set_mqtt(true);   /* 喂给统一状态查询 */
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
    board_status_set_mqtt(false);  /* 喂给统一状态查询 */
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

    /* 这一行是特意加出来给人看的：原来这里完全静默，串口上看不出它到底跑没跑、
     * 上报成功没有。ret < 0 最常见的原因就是 MQTT 没连上
     * （板子 DNS 解析失败，通常是 USB 没重新枚举）。 */
    printf("[ALARM] report_alarm type=%s topic=%s ret=%d\n",
           alarm_type, topic, ret);

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

/* -------------------------------------------------------------------------
 * 推送密钥
 *
 * ⚠️ 这里有**两个空串**是故意的，而且必须一直是空串：本仓库是公开仓库，
 *    把 Bark 的 device_key / PushPlus 的 token 写在源码里等于公开"给这台
 *    设备的手机推消息"的能力（历史上确实这么干过，已清理）。
 *
 *    上传/提交公开仓库前不要填 key；key 只放在设备本地、不进仓库的
 *        /etc/assets/push_key.txt
 *    （一行纯文本，自动去掉首尾空白；本机工作区里对应
 *     board/contest_board/src/etc/assets/push_key.txt，该文件已被
 *     .git/info/exclude 忽略，只用于本机演示，不要 git add）。
 *    运行时优先读这个文件，读不到才退回下面的编译期默认值；
 *    push_init(service, NULL) 走的就是这条路径。
 *    注意：两个服务共用同一个文件，同一时刻只能放一个 key（演示用的是 Bark）。
 * ---------------------------------------------------------------------- */
#define PUSH_KEY_FILE             "/etc/assets/push_key.txt"
#define PUSH_KEY_DEFAULT          ""
#define PUSH_KEY_DEFAULT_PUSHPLUS ""

/* HTTP 请求超时（秒）。TLS 握手 + 一个来回，10s 内足够。 */
#define PUSH_HTTP_TIMEOUT_SEC 10

/* 读密钥：优先 /etc/assets/push_key.txt（本机演示用，不进仓库），
 * 没有这个文件时退回编译期默认值——公开仓库里那两项都是空串，
 * 所以此时返回 false，push 会明确报"没有 key"而不是拿一个假 key 去发。 */
static bool push_load_key(push_service_t service, char *out, size_t outlen)
{
    FILE *fp;
    size_t n;

    if (out == NULL || outlen == 0) {
        return false;
    }

    fp = fopen(PUSH_KEY_FILE, "r");
    if (fp != NULL) {
        if (fgets(out, (int)outlen, fp) != NULL) {
            n = strlen(out);
            while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                             out[n - 1] == ' '  || out[n - 1] == '\t')) {
                out[--n] = '\0';
            }
            fclose(fp);
            if (n > 0) {
                printf("[PUSH] key loaded from %s (%d chars)\n",
                       PUSH_KEY_FILE, (int)n);
                return true;
            }
        } else {
            fclose(fp);
        }
    }

    strncpy(out, (service == PUSH_SERVICE_PUSHPLUS)
                     ? PUSH_KEY_DEFAULT_PUSHPLUS : PUSH_KEY_DEFAULT,
            outlen - 1);
    out[outlen - 1] = '\0';
    return out[0] != '\0';
}

/* -------------------------------------------------------------------------
 * TLS over webclient
 *
 * webclient 自身不做 TLS，但允许应用通过 ctx.tls_ops 注入实现
 * （见 apps/netutils/webclient/webclient.c 里 scheme==https && tls_ops!=NULL
 *  的分支）。这里用 mbedtls 在一个普通 fd 上做握手，webclient 就能对
 * https:// URL 正常发请求并回填 ctx.http_status。
 * 结构/流程与 apps/packages/demos/mimo/mimo_provider.c 的 mimo_tls_* 一致。
 * ---------------------------------------------------------------------- */
#ifdef CONFIG_CRYPTO_MBEDTLS

struct push_tls_ctx_s
{
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config       conf;
};

struct push_tls_conn_s
{
    mbedtls_ssl_context ssl;
    int                 fd;
};

/* mbedtls BIO：直接用 fd 收发，绕过 mbedtls_net_connect 的 getaddrinfo 差异 */
static int push_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t ret = send(fd, buf, len, 0);

    if (ret < 0) {
        return -EIO;
    }
    return (int)ret;
}

static int push_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t ret = recv(fd, buf, len, 0);

    if (ret < 0) {
        return -EIO;
    }
    if (ret == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return (int)ret;
}

static int push_tls_connect(void *ctx, const char *hostname, const char *port,
                            unsigned int timeout_sec,
                            struct webclient_tls_connection **connp)
{
    struct push_tls_ctx_s *tctx = ctx;
    struct push_tls_conn_s *conn;
    struct hostent *he;
    struct sockaddr_in server;
    int portnum;
    int ret;
    int fd;

    (void)timeout_sec;

    printf("[PUSH] TLS connecting to %s:%s\n", hostname, port);

    he = gethostbyname(hostname);
    if (he == NULL) {
        printf("[PUSH] DNS resolve failed for %s\n", hostname);
        return -EIO;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("[PUSH] socket() failed: %d\n", errno);
        return -EIO;
    }

    portnum = atoi(port);
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons((uint16_t)portnum);
    memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        printf("[PUSH] TCP connect to %s:%d failed: %d\n",
               hostname, portnum, errno);
        close(fd);
        return -EIO;
    }

    conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
        close(fd);
        return -ENOMEM;
    }

    conn->fd = fd;
    mbedtls_ssl_init(&conn->ssl);

    ret = mbedtls_ssl_setup(&conn->ssl, &tctx->conf);
    if (ret != 0) {
        printf("[PUSH] TLS setup failed: -0x%x\n", -ret);
        goto err;
    }

    ret = mbedtls_ssl_set_hostname(&conn->ssl, hostname);
    if (ret != 0) {
        printf("[PUSH] TLS set hostname failed: -0x%x\n", -ret);
        goto err;
    }

    mbedtls_ssl_set_bio(&conn->ssl, &conn->fd,
                        push_bio_send, push_bio_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf("[PUSH] TLS handshake failed: -0x%x\n", -ret);
            goto err;
        }
    }

    printf("[PUSH] TLS handshake complete\n");
    *connp = (struct webclient_tls_connection *)conn;
    return 0;

err:
    mbedtls_ssl_free(&conn->ssl);
    close(conn->fd);
    free(conn);
    return -EIO;
}

static ssize_t push_tls_send(void *ctx, struct webclient_tls_connection *base,
                             const void *buf, size_t len)
{
    struct push_tls_conn_s *conn = (struct push_tls_conn_s *)base;
    int ret;

    (void)ctx;
    ret = mbedtls_ssl_write(&conn->ssl, buf, len);
    if (ret < 0) {
        return (ret == MBEDTLS_ERR_SSL_WANT_WRITE) ? 0 : -EIO;
    }
    return ret;
}

static ssize_t push_tls_recv(void *ctx, struct webclient_tls_connection *base,
                             void *buf, size_t len)
{
    struct push_tls_conn_s *conn = (struct push_tls_conn_s *)base;
    int ret;

    (void)ctx;
    ret = mbedtls_ssl_read(&conn->ssl, buf, len);
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
            ret == MBEDTLS_ERR_SSL_WANT_READ) {
            return 0;
        }
        return -EIO;
    }
    return ret;
}

static int push_tls_close(void *ctx, struct webclient_tls_connection *base)
{
    struct push_tls_conn_s *conn = (struct push_tls_conn_s *)base;

    (void)ctx;
    mbedtls_ssl_close_notify(&conn->ssl);
    mbedtls_ssl_free(&conn->ssl);
    close(conn->fd);
    free(conn);
    return 0;
}

static int push_tls_get_poll_info(void *ctx,
                                  struct webclient_tls_connection *base,
                                  struct webclient_poll_info *info)
{
    struct push_tls_conn_s *conn = (struct push_tls_conn_s *)base;

    (void)ctx;
    info->fd = conn->fd;
    info->flags = WEBCLIENT_POLL_INFO_WANT_READ;
    return 0;
}

static const struct webclient_tls_ops g_push_tls_ops =
{
    .connect         = push_tls_connect,
    .send            = push_tls_send,
    .recv            = push_tls_recv,
    .close           = push_tls_close,
    .get_poll_info   = push_tls_get_poll_info,
    .init_connection = NULL,
};

static struct push_tls_ctx_s g_push_tls_ctx;
static bool g_push_tls_ready = false;

static int push_tls_init(void)
{
    int ret;

    if (g_push_tls_ready) {
        return 0;
    }

    mbedtls_entropy_init(&g_push_tls_ctx.entropy);
    mbedtls_ctr_drbg_init(&g_push_tls_ctx.ctr_drbg);
    mbedtls_ssl_config_init(&g_push_tls_ctx.conf);

    ret = mbedtls_ctr_drbg_seed(&g_push_tls_ctx.ctr_drbg,
                                mbedtls_entropy_func,
                                &g_push_tls_ctx.entropy,
                                (const unsigned char *)"push", 4);
    if (ret != 0) {
        printf("[PUSH] TLS drbg seed failed: -0x%x\n", -ret);
        return -1;
    }

    ret = mbedtls_ssl_config_defaults(&g_push_tls_ctx.conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("[PUSH] TLS config defaults failed: -0x%x\n", -ret);
        return -1;
    }

    printf("push_init: service=%d, key configured\n", service);
    return 0;
}
#endif /* CONFIG_CRYPTO_MBEDTLS */

/* 发送 HTTPS POST 请求（复用 ai_agent TLS 客户端） */
static int http_post(const char *url, const char *body)
{
    if (url == NULL || body == NULL) {
        return -EINVAL;
    }

    char host[128] = {0};
    char path[256] = {0};
    int port = 443;

    /* 解析 URL: https://host[:port]/path */
    if (sscanf(url, "https://%127[^/]:%d/%255s", host, &port, path) < 2) {
        if (sscanf(url, "https://%127[^/]/%255s", host, path) != 2) {
            printf("push: invalid URL: %s\n", url);
            return -1;
        }
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    /* 路径需要前导 / */
    char full_path[260];
    snprintf(full_path, sizeof(full_path), "/%s", path);

    printf("push: POST https://%s:%d%s\n", host, port, full_path);

    /* 调用 vela_tls HTTPS 客户端 */
    char resp[2048] = {0};
    int status = vela_https_post_json(host, port_str, full_path,
                                      NULL, body, resp, sizeof(resp));
    if (status < 0) {
        printf("push: TLS error %d for %s\n", status, host);
        return status;
    }
#else
    printf("[PUSH] http_post: no TLS support (CONFIG_CRYPTO_MBEDTLS off)\n");
    return -1;
#endif

    work_buf = malloc(HTTP_BUFFER_SIZE);
    if (work_buf == NULL) {
        printf("[PUSH] http_post: no memory\n");
        return -1;
    }

    memset(&resp, 0, sizeof(resp));
    headers[0] = "Content-Type: application/json";

    webclient_set_defaults(&ctx);
    ctx.protocol_version  = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
    ctx.method            = "POST";
    ctx.url               = url;
    ctx.buffer            = work_buf;
    ctx.buflen            = HTTP_BUFFER_SIZE;
    ctx.headers           = headers;
    ctx.nheaders          = 1;
    ctx.sink_callback     = push_sink_callback;
    ctx.sink_callback_arg = &resp;
    ctx.timeout_sec       = PUSH_HTTP_TIMEOUT_SEC;

#ifdef CONFIG_CRYPTO_MBEDTLS
    ctx.tls_ops = &g_push_tls_ops;
    ctx.tls_ctx = &g_push_tls_ctx;
#endif

    webclient_set_static_body(&ctx, body, strlen(body));

    ret = webclient_perform(&ctx);
    free(work_buf);

    /* 检查 HTTP 状态码 */
    if (status < 200 || status >= 300) {
        printf("push: HTTP %d from %s\n", status, host);
        return -EIO;
    }

    /* 解析业务返回码（Bark/PushPlus 都在 JSON 里返回 code 字段） */
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (code && cJSON_IsNumber(code) && (int)code->valuedouble != 200) {
            const char *msg = cJSON_GetStringValue(
                cJSON_GetObjectItem(root, "message"));
            printf("push: business error %d: %s\n",
                   (int)code->valuedouble, msg ? msg : "unknown");
            cJSON_Delete(root);
            return -EIO;
        }
        cJSON_Delete(root);
    }

    printf("push: OK (HTTP %d)\n", status);
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
            /* Bark 官方 POST 接口：POST https://api.day.app/push
             * JSON: {"device_key":..., "title":..., "body":...}
             *
             * 原来这里拼的是 GET 形式的 "%s/%s/ZhiAi"（BARK_API_URL/key/ZhiAi），
             * 有两个问题：
             *   1) title/body 根本没进 URL（只写死一个 "ZhiAi"），推送内容全丢；
             *   2) 真按 GET 形式拼的话，title/body 里有空格、方括号、中文，
             *      不做 URL 编码就是非法 URL（Bark 要求 percent-encoding）。
             * 改成 POST-JSON 之后这两个问题一起没了：内容走 body，不需要编码。 */
            {
                cJSON *bk = cJSON_Parse(json_body);
                if (bk == NULL) {
                    free(json_body);
                    return -1;
                }
                cJSON_AddStringToObject(bk, "device_key",
                                        push_config.push_key);
                free(json_body);
                json_body = cJSON_PrintUnformatted(bk);
                cJSON_Delete(bk);
                if (json_body == NULL) {
                    return -1;
                }
                snprintf(url, sizeof(url), "%s/push", BARK_API_URL);
            }
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

    /* 异步投递：交给推送任务去发，这里立刻返回，UI 不被 TLS 往返阻塞。
     * 真正的成功判据（HTTP 200/201）和状态码日志在 push_task() 里。 */
    pthread_mutex_lock(&g_push_job_lock);
    strncpy(g_push_job.url, url, sizeof(g_push_job.url) - 1);
    g_push_job.url[sizeof(g_push_job.url) - 1] = '\0';
    strncpy(g_push_job.body, json_body, sizeof(g_push_job.body) - 1);
    g_push_job.body[sizeof(g_push_job.body) - 1] = '\0';
    g_push_job.pending = true;
    pthread_mutex_unlock(&g_push_job_lock);

    sem_post(&g_push_job_sem);
    free(json_body);

    printf("[PUSH] push_send_notification queued: title=%s group=%s\n",
           title, group ? group : "-");
    return 0;
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
        printf("MQTT DNS: %s -> %s\n", host, inet_ntoa(server_addr.sin_addr));
    } else {
        server_addr.sin_addr.s_addr = inet_addr(host);
        printf("MQTT DNS: %s 解析失败, 当 IP 用\n", host);
    }

    /* 连接服务器。
     *
     * 必须用「非阻塞 connect + select 超时」，不能直接用阻塞式 connect：
     * 对方如果不回 SYN（被限流、网络抖动、IP 过期），内核要等 SYN 重传耗尽
     * 才返回，在 NuttX 的配置下要 1~3 分钟。而 network_task 是单线程轮询，
     * 这一个 connect 就会把整个任务卡住——心跳不发、重连不做、串口一行日志
     * 都没有，看起来像"死机"。5 秒返回不了就当这次失败，下一轮重试。
     */
    if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl");
        close(sockfd);
        return -1;
    }

    int cret = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (cret < 0 && errno != EINPROGRESS) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    if (cret < 0) {
        fd_set wset;
        struct timeval ctimeo;
        int cerr = 0;
        socklen_t elen = sizeof(cerr);

        FD_ZERO(&wset);
        FD_SET(sockfd, &wset);
        ctimeo.tv_sec  = 5;
        ctimeo.tv_usec = 0;

        if (select(sockfd + 1, NULL, &wset, NULL, &ctimeo) <= 0 ||
            getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &cerr, &elen) < 0 ||
            cerr != 0) {
            printf("connect timeout/error: %s:%u\n", host, (unsigned)port);
            close(sockfd);
            return -1;
        }
    }

    /* 保持非阻塞。
     *
     * network_task 是单线程轮询，任何一次阻塞都可能把整个循环卡死：
     * 心跳不发、重连不做、串口一行日志都没有，看起来像"死机"（实测过）。
     * 原来这里靠 SO_RCVTIMEO 让 recv 5 秒超时返回，但实测不可靠 ——
     * 循环会永久停在 recv 里。
     *
     * 非阻塞之后：没数据时 recv 立刻返回 -1，mqtt_parse_packet() 直接返回，
     * 循环继续跑，心跳照发、重连照做。
     */
    if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl");
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
        0x06,  // 连接标志：下面按实际内容再补（见注释）
        0x00, 0x3C,  // 保持连接时间 60 秒
    };

    /* 连接标志必须和 payload 里真实带了什么字段一致。
     *
     * 原来硬编码 0xC2（用户名+密码+遗嘱+清理会话），但下面只在 username /
     * password 非空时才往 payload 里追加对应字段 —— 两个都为空时，标志声称
     * "有用户名密码"、payload 里却没有，broker 判定报文非法，直接把连接关掉。
     * 表现就是「TCP 连得上，但永远收不到 CONNACK」。
     *
     * bit1=清理会话(0x02) bit2=遗嘱(0x04) bit7=用户名(0x80) bit6=密码(0x40)
     */
    if (mqtt_config.username[0]) {
        variable_header[7] |= 0x80;
    }
    if (mqtt_config.password[0]) {
        variable_header[7] |= 0x40;
    }

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

    /* MQTT 重连节流 + broker 降级。每次尝试都要先做 DNS 解析再 connect：
     * - 每 100ms 重来一次会把 CPU 和网络打满；
     * - 一直 5 秒一次地锤公共 broker 会被限流（broker.emqx.io 就把我们
     *   限了：连着 20 多分钟后它开始直接关连接、不给 CONNACK）。
     * 所以前几次快一点（开机后尽快连上），失败多了就放慢到 30 秒；
     * 连了 5 次还不行就换下一台 broker。
     */
    int mqtt_retry_tick = 0;
    int mqtt_fails = 0;
    int broker_idx = 0;
    int broker_switches = 0;

    while (1) {
        /* 检查 WiFi 状态 */
        if (!wifi_config.connected) {
            /* 尝试重连 */
            // TODO: 实现重连逻辑
        }

        /* 检查 MQTT 状态 */
        if (wifi_config.connected && !mqtt_config.connected) {
            if (--mqtt_retry_tick <= 0) {
                /* 只有第一轮（还没换过 broker）才用 5 秒的快节奏，好在开机后
                 * 尽快连上；一旦开始换 broker 说明网络/公共实例有问题，一律 30 秒，
                 * 否则会变成"每台试 5 次、15 次一轮"地一直锤公共服务器。
                 */
                mqtt_retry_tick =
                    (broker_switches == 0 && mqtt_fails < 5) ? 50 : 300;

                /* 尝试连接 MQTT */
                if (mqtt_connect(mqtt_config.broker, mqtt_config.port,
                                 mqtt_config.client_id, mqtt_config.username,
                                 mqtt_config.password) < 0) {
                    mqtt_fails++;

                    if (mqtt_fails >= 5 && MQTT_NBROKERS > 1) {
                        /* 这台连不上（公共实例限流很常见），换下一台 */
                        broker_idx = (broker_idx + 1) % MQTT_NBROKERS;
                        broker_switches++;
                        strncpy(mqtt_config.broker, g_mqtt_broker_list[broker_idx],
                                sizeof(mqtt_config.broker) - 1);
                        mqtt_config.broker[sizeof(mqtt_config.broker) - 1] = '\0';
                        printf("MQTT broker 切换到 %s\n", mqtt_config.broker);
                        mqtt_fails = 0;
                    }
                } else {
                    mqtt_fails = 0;
                }
            }
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
