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
#include <syslog.h> /* syslog()：把"这一轮实际连上的 broker"打进系统日志（串口） */

/* 配置读取（claw_config_get()）。和 ai_agent / time_sync.c / ambient_listen.c 用的是
 * 同一份键值存储：开机时板级代码把 /etc/assets/agent_config.json 拷到
 * /data/ai_agent/config/config.json。本文件只用它读 mqtt_broker 这一个键，
 * 见 mqtt_load_broker_config()。 */
#include "infra/config_store.h"

/* 板级外设状态（board/contest_board/src/sf32lb52_status.h）：
 * 这里只负责把 MQTT 连接状态喂进去，供 hw_test status / UI 统一查询。 */
#include "sf32lb52_status.h"

/* hello_app 的语音链路诊断接口（app/hello_app/ai_companion_diag.h）：
 *   int ai_companion_diag_snapshot(char *buf, size_t len);  返回写入长度，<0=不可用
 * 头文件路径由 app/robot_ui/CMakeLists.txt 的 INCLUDE_DIRECTORIES `../hello_app`
 * 提供（robot_ui 早就在反向调 hello_app：ai_companion_req.h / mimo_voice.h）。
 * 符号在最终链接时解析（整机是单一大镜像）。
 *
 * 为什么在这里用：串口线丢了之后，MQTT 是**唯一**的观测通道，而 hello_app 的
 * 语音链路哑掉时它自己的 MQTT 上报是死的（被 g_net_started 钉死）。心跳里的
 * 那个 ha 位、以及 {"action":"diag"} 里内嵌的 hello 块，都只能靠这个接口问出来；
 * 它返回负值这件事本身就是"hello_app 没在跑"的证据。 */
#include "ai_companion_diag.h"

/* nxsched_foreach()：诊断里报"ps 里有多少个任务/线程"。
 * 它把每个 TCB 交给回调，内部短暂进出临界区（普通任务里可以调，别在中断里调）。
 * 这是在数"hello_app 的几条线程还在不在"的粗粒度证据 —— 出过的那次事故就是
 * hello_app 组里的 pthread 全没了，而外面完全看不出来。 */
#include <nuttx/sched.h>

/* cJSON 用于 JSON 解析 */
#include <netutils/cJSON.h>

/* 带 TLS 的 HTTP 客户端：手机推送走 https 必需。
 * webclient 自己不带 TLS，但允许应用用 ctx.tls_ops 注入实现。
 * 参考 apps/packages/demos/mimo/mimo_provider.c（CONFIG_DEMOS_MIMO=y）
 * 里已验证可用的 mimo_tls_* 写法，本文件的 push_tls_* 就是照它改的。
 * 依赖 Kconfig：CONFIG_NETUTILS_WEBCLIENT + CONFIG_CRYPTO_MBEDTLS（本工程已开）。 */
#include <netutils/webclient.h>
#include <semaphore.h>
#include <pthread.h>
#ifdef CONFIG_CRYPTO_MBEDTLS
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#endif

/* ==================== 全局变量 ==================== */
static wifi_config_t wifi_config = {0};
static mqtt_config_t mqtt_config = {0};
static device_status_t device_status = {0};

/* MQTT 连接 socket */
static int mqtt_socket = -1;

/* MQTT 连续连接失败次数 + 已经换过几次 broker（降级）。
 *
 * 这两个原来是 network_task() 里的局部变量，模块外一个字节都看不到，于是
 * "连了几次没连上、换没换过台"这种现场只能靠串口日志猜 —— 串口线丢了之后
 * 就彻底是黑盒。提升成静态量（network_task 里只管加/清零），心跳和诊断报文
 * 才报得出来，外部也能用 mqtt_get_fails() 读。
 *
 * 只由 network_task 写、别的线程读：32 位对齐的 int 在这颗 Cortex-M 上是
 * 原子读写，观测用的计数不需要额外加锁。 */
static int g_mqtt_fails = 0;
static int g_mqtt_broker_switches = 0;

/* ---- MQTT 收包重组缓冲（跨 recv 的半包）----
 *
 * 一次 recv() 的字节边界可能落在任意一包中间（NuttX 会把 readahead 灌满缓冲才
 * 返回，broker 合并下发时很常见），所以没解析完的尾巴必须留到下次 recv 前面
 * 拼起来。它要活过单次 recv()，只能是静态的 —— 顺带把原来 mqtt_parse_packet()
 * 栈上那个 buffer[1024] 省掉了（network_task 只有 12KB 栈）。
 *
 * 容量关系（改数字要一起看）：缓冲固定 2048 字节，
 *   头部 g_mqtt_rx_len 字节 = 上次留下的半包，
 *   剩下的 (2048 - g_mqtt_rx_len) 字节才是这次 recv() 的接收窗口。
 * 所以"残留 + 本次 recv"恒等于 ≤2048，不会越界；也意味着任何 ≤2KB 的 MQTT 包
 * 都能被完整重组。更大的包走 g_mqtt_rx_skip 整包丢弃（见 mqtt_parse_packet）。
 *
 * 只有 network_task 读写（mqtt_parse_packet 只被它调用），不需要锁；
 * mqtt_disconnect() 里复位一次，免得上一条连接的残字节混进新连接的报文。 */
#define MQTT_RX_BUF_SIZE 2048

static uint8_t g_mqtt_rx_buf[MQTT_RX_BUF_SIZE];
static int g_mqtt_rx_len = 0;    /* 缓冲头部有效字节数：上次没解析完的半包 */
static int g_mqtt_rx_skip = 0;   /* 已在丢弃的超大包还剩多少字节要丢 */

/* PUBACK 发送失败次数。只为限频打印用（见 mqtt_parse_packet 的 QoS1 分支）。 */
static int g_mqtt_puback_fails = 0;

static void mqtt_rx_reset(void)
{
    g_mqtt_rx_len = 0;
    g_mqtt_rx_skip = 0;
}

/* robot_ui 本地状态的提供者（main.c 启动时注册，见 network_set_local_state_provider）。
 * NULL = 没注册，诊断/心跳里那几个字段一律报 -1（"取不到"），不编造。 */
static local_state_provider_t g_local_state_provider = NULL;

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

/* 配置键 mqtt_broker：值形如 "host" 或 "host:port"。
 *
 * 键名和 ai_agent 自带 MQTT 通道用的是同一个
 * （packages/ai_agent/include/agent_config.h 的 AGENT_CFG_KEY_MQTT_BROKER
 * 就是字面量 "mqtt_broker"，mqtt_channel.c 也按 host:port 解析），
 * 所以现场只要在 agent_config.json 里加一行 mqtt_broker 就能钉住 broker。
 *
 * 空 / 没有这个键 -> g_mqtt_broker_pinned 保持 false，broker 仍是
 * "broker.emqx.io + 失败 5 次降级到下一台"的老行为（报警推送那条链路
 * 就是靠这个行为跑通的，别动）。
 */
#define MQTT_CFG_KEY_BROKER "mqtt_broker"

static bool g_mqtt_broker_pinned = false;

/* 把配置里的 mqtt_broker 应用到 mqtt_config。没配就什么都不做。
 *
 * 为什么配上之后要"钉住"（不降级）：演示现场队友的智能灯挂在
 * test.mosquitto.org 上，如果板子在失败几次后自己漂到 emqx/hivemq，
 * 表面上"MQTT 已连接"、实际上永远收不到灯的状态，最难查。
 * 所以配置一旦给了 broker，就只用这一台，失败也只重试它。
 * 内置三台的默认行为完全不变（没配 = 走老路）。 */
static void mqtt_load_broker_config(void)
{
    char cfg[sizeof(mqtt_config.broker)];
    const char *colon;
    size_t host_len;
    long port;

    cfg[0] = '\0';
    if (claw_config_get(MQTT_CFG_KEY_BROKER, cfg, sizeof(cfg)) != OK ||
        cfg[0] == '\0') {
        printf("MQTT broker: 配置里没有 %s，用内置默认 %s\n",
               MQTT_CFG_KEY_BROKER, mqtt_config.broker);
        return;
    }

    /* 解析 host[:port]：最后一个 ':' 才是端口分隔（IP 里也有 ':' 就交给
     * 后面 gethostbyname 当主机名处理，这里不做 IPv6 字面量支持）。 */
    colon = strrchr(cfg, ':');
    if (colon != NULL && colon != cfg) {
        host_len = (size_t)(colon - cfg);
        port = atol(colon + 1);
    } else {
        host_len = strlen(cfg);
        port = 0;
    }

    if (host_len >= sizeof(mqtt_config.broker)) {
        host_len = sizeof(mqtt_config.broker) - 1;
    }
    memcpy(mqtt_config.broker, cfg, host_len);
    mqtt_config.broker[host_len] = '\0';

    if (port > 0 && port <= 65535) {
        mqtt_config.port = (uint16_t)port;
    }

    g_mqtt_broker_pinned = true;
    printf("MQTT broker: 配置 %s=%s -> %s:%u（固定使用，不再降级）\n",
           MQTT_CFG_KEY_BROKER, cfg, mqtt_config.broker, (unsigned)mqtt_config.port);
}

/* ==================== 内部函数声明 ==================== */
static int create_tcp_socket(const char *host, uint16_t port);
static int mqtt_send_connect(void);
static int mqtt_send_subscribe(const char *topic, int qos);
static int mqtt_send_publish(const char *topic, const char *payload, int qos, bool retain);
static int mqtt_send_puback(uint16_t packet_id);
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

    /* 配置里给了 mqtt_broker 就覆盖上面这两项（没配则行为不变） */
    mqtt_load_broker_config();

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
    /* 已经连着的时候**绝不开第二条**。
     *
     * 这个入口有两个调用者：唯一持有 socket 的 network_task（robot_ui），
     * 以及 hello_app 的 ai_network_start_shared() 兜底路径（跑在 hello_app 的
     * 任务组里）。后者在"开机时界面的 MQTT 还没连上"那一小段窗口里会走到这里
     * （它先等几秒，等不到就自己连一次），而这里是**同一个 client_id**：
     *   - broker 按 MQTT 3.1.1 会把先来的那条连接踢掉，而 CONNECT 里带的是
     *     clean session —— **先来那条连接的订阅（zhi_ai/<client_id>/command）
     *     跟着一起没**，外面发什么都进不来；
     *   - 新 socket 建在调用者的任务组里，却写进全局 mqtt_socket，于是
     *     network_task 的 recv/send 打在"只有别的组才有效"的 fd 上；
     *   - 旧 socket 的 fd 直接被覆盖，再也没人关得上。
     * 现场表现就是最难查的那种"看着连着、心跳看得见，命令/诊断永远不回"。
     * 所以这里只复用、不新建：真断了的时候 connected 会是 false，那条重连
     * 路径照旧（network_task 自己会重连并重新订阅）。 */
    if (mqtt_socket >= 0 && mqtt_config.connected) {
        printf("[MQTT] 连接已存在（%s:%u），复用不新建第二条（请求方 client_id=%s）\n",
               mqtt_config.broker, (unsigned)mqtt_config.port, client_id);
        return 0;
    }

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

    /* 串口上明确打出"这一轮实际连上的是哪台 broker"。
     * 光有上面那句 "MQTT connected" 分不清落在哪台：network_task 里的降级逻辑
     * 会在连续失败后自己换台（broker.emqx.io -> test.mosquitto.org -> hivemq），
     * 现场"板子显示已连接、却收不到设备状态"十有八九就是两台不在同一台上。 */
    syslog(LOG_INFO, "[MQTT] connected broker=%s:%u source=%s\n",
           broker, (unsigned)port,
           g_mqtt_broker_pinned ? "config:" MQTT_CFG_KEY_BROKER
                                : "built-in default/degraded");

    /* 订阅命令主题 */
    char topic[128];
    snprintf(topic, sizeof(topic), "zhi_ai/%s/command", client_id);
    mqtt_subscribe(topic, 1);

    /* 设备状态主题（智能灯那类子设备执行完命令后的回执）。
     * 队友在 PC 上模拟的灯收到 device_cmd 后会往
     * zhi_ai/<client_id>/device_state 发
     * {"type":"device_state","device_id":...,"state":"on"/"off",
     *  "success":true,"message":...,"timestamp":...}（QoS1 发布）。
     *
     * 这里**故意订 QoS0**：MQTT 的投递 QoS = min(发布 QoS, 订阅 QoS)，
     * 队友发 QoS1、我们订 QoS0，broker 就以 QoS0 投给我们——没有 Packet
     * Identifier，正好绕开 mqtt_parse_packet() 之前那段"把 QoS>0 的 2 字节
     * 包 ID 当 payload 头"的解析 bug（bug 已一并修好，但订阅端保持 QoS0
     * 更稳：设备状态是周期性/幂等的上报，丢一条无所谓，也不需要 PUBACK 往返）。
     *
     * 收上来的 payload 会走唯一回调 mqtt_msg_callback(topic, payload)
     * （main.c 注册的 on_mqtt_message_received）。 */
    snprintf(topic, sizeof(topic), "zhi_ai/%s/device_state", client_id);
    mqtt_subscribe(topic, 0);

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

    /* 半包重组缓冲跟着连接一起作废：里面残留的是上一条连接的字节，粘到新连接
     * 的报文前面会被解析成假报文（甚至假 PUBLISH -> 假 PUBACK）。 */
    mqtt_rx_reset();

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

/* 连续连接失败次数（只读）。见 g_mqtt_fails 上面的说明。 */
int mqtt_get_fails(void)
{
    return g_mqtt_fails;
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

    int ret = mqtt_send_publish(topic, payload, qos, retain);

    /* send() 返回负值 = 这条 TCP 连接已经死了（broker 断流 / 被限流 / 中途
     * 换过网络），但 connected 还是 true。network_task 只在 !connected 时才
     * 重连，于是会一直"假装连着"：报警上报、心跳全都发不出去，直到重启板子
     * （实测现象：`[ALARM] report_alarm ... ret=-1` 而 hw_test status 里
     * MQTT 却显示"已连接"）。
     *
     * ⚠️ 这里**只许改标志，不许碰 socket**：本函数是在调用者的任务里跑的
     * （报警按钮就是 LVGL 任务），而 mqtt_socket 是 network_task 的。
     * 跨任务 close() 会和 network_task 里的 recv() 抢同一个 fd（关掉后 fd
     * 号立刻被 TLS/音频之类的 open 复用），实测会把整机打复位。
     * 真正的关闭交给 network_task 自己做（见那边的 !connected && socket>=0 分支）。
     */
    if (ret < 0) {
        printf("MQTT publish failed: %d, marked disconnected for reconnect\n", ret);
        mqtt_config.connected = false;
        board_status_set_mqtt(false);   /* 喂给统一状态查询 */
    }

    return ret;
}

/* ==================== MQTT 排队发布（跨任务组唯一安全的发布方式） ==================== */
/*
 * 为什么必须排队、不能在自己的线程里直接 mqtt_send_publish()：
 *
 * NuttX 的 fd 是**按 task group 分配的**。mqtt_socket 是 network_task 建出来的，
 * 只存在于它那个组的 fd 表里。hello_app 的任务组里那个数字要么压根不存在
 * （EBADF），要么指向它自己打开过的别的文件 —— 两种都是"往错误的 fd 上 send"。
 *
 * 真机日志（一轮完整语音之后，网络本身完全正常：同一时刻 HTTPS / ASR 都成功、
 * free 还有 5.4 MB 空闲内存）：
 *
 *   MQTT connecting: broker.emqx.io:1883
 *   MQTT DNS: broker.emqx.io -> 44.232.241.40
 *   MQTT connected
 *   [MQTT] connected broker=broker.emqx.io:1883
 *   MQTT publish failed: -1, marked disconnected for reconnect   <- hello_app 那条
 *   MQTT disconnected
 *   [语音] user_said MQTT 回传失败(-107)（界面已由直调刷过，手机端看不到）: 帮我打开灯。
 *   [AI_NET ERR] MQTT not connected, cannot send device command
 *   [意图] 命中灯控: 开灯，但 device_cmd 发送失败(-107)，兜底回话「网络没连上，灯没打开」
 *   ...
 *   MQTT connecting: broker.emqx.io:1883 -> MQTT connected
 *   Publish to zhi_ai/zhi_ai_001/heartbeat: {...}                <- network_task 自己发，成功
 *
 * 判据非常干净：**心跳（network_task 自己发）永远成功，凡是 hello_app 线程
 * 发起的（voice_state / user_said / device_cmd）全部失败**。
 * 而且第一条失败还会把 connected 标成 false（那是 mqtt_publish 里正确的重连
 * 逻辑），于是 hello_app 后面几条连试都不试，直接 -ENOTCONN(-107) ——
 * 日志里那两行 -107 就是这么来的，不是"网络断了"。
 *
 * 所以这一节只做两件事：把 topic + payload **拷进队列**，再给 network_task
 * 一个信号。一个字节的 socket 操作都不做（连"现在连没连上"都不判 —— 那是
 * 真正发送那一刻的事）。真正的发送由 network_task 在它自己的循环里取出来做，
 * 用的是原来那条已经带重连 / 失败标记的路径，这里不另写第二套。
 */

#define MQTT_QUEUE_SLOTS        8      /* 槽位数。满了丢新来的：这些都是"此刻状态"，
                                        * 旧消息过了这一拍就没价值了。 */
#define MQTT_QUEUE_TOPIC_MAX    128    /* 装得下 zhi_ai/<client_id>/device_state 这类 */
#define MQTT_QUEUE_PAYLOAD_MAX  640    /* 比 ai_network.c 能拼出的最长载荷（约 576）宽一点，
                                        * 正常路径不会截断；topic + payload 也远小于
                                        * mqtt_send_publish() 的 1024 字节包缓冲。 */

typedef struct
{
    char topic[MQTT_QUEUE_TOPIC_MAX];
    char payload[MQTT_QUEUE_PAYLOAD_MAX];
    int  qos;
    bool retain;
} mqtt_queue_item_t;

static mqtt_queue_item_t g_mqtt_queue[MQTT_QUEUE_SLOTS];
static int g_mqtt_queue_head = 0;    /* 取的位置 */
static int g_mqtt_queue_tail = 0;    /* 放的位置 */
static int g_mqtt_queue_count = 0;

/* 队列锁：只保护上面那三个下标和槽里的字节，**绝不可以在持锁时做 socket 操作**。
 *
 * 用 PTHREAD_MUTEX_INITIALIZER（和本文件推送队列 g_push_job_lock 同一个写法）：
 * 静态初始化过的 pthread_mutex 在 NuttX task 和 pthread 之间都能用，
 * 而本文件那个推送队列早就被 net_task 和 LVGL 线程同时锁过了（真机跑通的）。
 * 出过事的是"忘了初始化、.bss 全 0 就当互斥量用"，那才会撞 NXSEM_IS_MUTEX 断言。 */
static pthread_mutex_t g_mqtt_queue_lock = PTHREAD_MUTEX_INITIALIZER;

/* 生产者 -> network_task 的唤醒信号。
 * 只是"有货了"的提示：network_task 的分片睡眠里 trywait 到就提前结束这一觉，
 * 所以消息最迟 10ms 被取走（不靠它做互斥，也就不怕丢信号）。 */
static sem_t g_mqtt_queue_sem;
static bool  g_mqtt_queue_sem_ready = false;

/* 取出时的落地副本。
 * 用 static 而不是 network_task 的栈：那个任务的栈只有 12 KB，而它这一路还要
 * 走 report_diag()（里面要装 hello_app 快照），能省一点是一点。
 * 只有 network_task 会取消息，所以不存在两个取者抢这份副本。 */
static mqtt_queue_item_t g_mqtt_queue_drain_item;

/* 丢弃计数 + 日志节流时刻。队列满和"MQTT 没连上整队丢"分开计，
 * 现场看日志一眼就能分清是"发得太快"还是"根本没连上"。 */
static unsigned g_mqtt_queue_drop_full = 0;
static unsigned g_mqtt_queue_drop_nolink = 0;
static time_t   g_mqtt_queue_drop_log_at = 0;

/* 必须在持 g_mqtt_queue_lock 时调：懒初始化唤醒信号。
 * 静态 sem_t 全 0 本来就是"计数 0"的合法初值，这里显式初始化一次只是
 * 让它和本文件其它信号量（g_push_job_sem）的写法一致、也免得以后有人改坏。 */
static void mqtt_queue_sem_ensure(void)
{
    if (!g_mqtt_queue_sem_ready) {
        sem_init(&g_mqtt_queue_sem, 0, 0);
        g_mqtt_queue_sem_ready = true;
    }
}

/* 队列里还有几条（只给日志用，读一个 int 就行） */
static int mqtt_queue_count_read(void)
{
    int n;

    pthread_mutex_lock(&g_mqtt_queue_lock);
    n = g_mqtt_queue_count;
    pthread_mutex_unlock(&g_mqtt_queue_lock);

    return n;
}

int mqtt_publish_queued(const char *topic, const char *payload, int qos, bool retain)
{
    const char *t = (topic != NULL) ? topic : "";
    const char *p = (payload != NULL) ? payload : "";
    size_t tlen;
    size_t plen;
    int ret = OK;

    /* 下面两个拒绝都在**拿锁之前**判定：它们既不读也不改队列里的字节，
     * 所以一律直接 return —— 此时**没持锁，也就绝不能有 unlock**
     * （这里曾经抄进来过一句多余的 unlock，对着没锁的互斥量解锁）。
     * 拿锁之后的出口只剩函数末尾那一处（见 out:）。 */
    if (t[0] == '\0') {
        printf("[MQTTQ] 拒绝入队：topic 为空\n");
        return -EINVAL;
    }

    tlen = strlen(t);
    plen = strlen(p);

    /* 超长**直接拒发**，绝不截断后照发。
     *
     * 队列里发的全是 JSON：截断必然产出**非法 JSON**，对端 `cJSON_Parse` 失败 =
     * 静默丢消息，比"没发"更难查（而且 topic 被截还会发到错误的话题上）。
     * 当前工程里最长载荷约 576 字节、余量只有 64 字节 —— 将来谁把
     * `AI_CMD_PARAM_MAX` 调大一点就会踩上，所以这里不给自己留"看起来能跑"的假象。
     * 日志做节流（这条不是热路径错误，但也不能刷屏）。 */
    if (tlen > MQTT_QUEUE_TOPIC_MAX - 1 || plen > MQTT_QUEUE_PAYLOAD_MAX - 1) {
        static unsigned long oversize_log_ms = 0;
        unsigned long now_ms = (unsigned long)(time(NULL) * 1000);

        if (now_ms - oversize_log_ms >= 1000) {
            oversize_log_ms = now_ms;
            printf("[MQTTQ] 载荷超长**拒发**（不截断，截断会产出非法 JSON）: "
                   "topic %u/%d 字节, payload %u/%d 字节\n",
                   (unsigned)strlen(t), MQTT_QUEUE_TOPIC_MAX - 1,
                   (unsigned)strlen(p), MQTT_QUEUE_PAYLOAD_MAX - 1);
        }

        return -EMSGSIZE;    /* 未持锁 */
    }

    pthread_mutex_lock(&g_mqtt_queue_lock);
    mqtt_queue_sem_ensure();

    if (g_mqtt_queue_count >= MQTT_QUEUE_SLOTS) {
        time_t now;

        g_mqtt_queue_drop_full++;

        /* 节流：一秒最多打一条。这里已经被别的任务拿它当高频入口了
         * （voice_state 每变一次就来一条），不节流会把串口刷爆。 */
        now = time(NULL);
        if (now != g_mqtt_queue_drop_log_at) {
            g_mqtt_queue_drop_log_at = now;
            printf("[MQTTQ] 队列满(%d 槽)，本条丢弃: topic=%s（累计丢 %u 条）\n",
                   MQTT_QUEUE_SLOTS, t, g_mqtt_queue_drop_full);
        }

        ret = -ENOSPC;
        goto out;
    }

    {
        mqtt_queue_item_t *slot = &g_mqtt_queue[g_mqtt_queue_tail];

        memcpy(slot->topic, t, tlen);
        slot->topic[tlen] = '\0';
        memcpy(slot->payload, p, plen);
        slot->payload[plen] = '\0';
        slot->qos = qos;
        slot->retain = retain;

        g_mqtt_queue_tail = (g_mqtt_queue_tail + 1) % MQTT_QUEUE_SLOTS;
        g_mqtt_queue_count++;
    }

out:
    pthread_mutex_unlock(&g_mqtt_queue_lock);

    if (ret == OK) {
        /* 出锁之后再唤醒：signalling 跟队列本身没有关系，也没必要占着锁。 */
        sem_post(&g_mqtt_queue_sem);
    }

    return ret;
}

/* 取一条出来（0 = 取到，-1 = 队列空）。
 * 交出去的是一份**拷贝**，槽位立刻释放 —— 所以绝不能把槽位指针传到外面去。 */
static int mqtt_queue_pop(mqtt_queue_item_t *out)
{
    int ret = -1;

    pthread_mutex_lock(&g_mqtt_queue_lock);

    if (g_mqtt_queue_count > 0) {
        *out = g_mqtt_queue[g_mqtt_queue_head];
        g_mqtt_queue_head = (g_mqtt_queue_head + 1) % MQTT_QUEUE_SLOTS;
        g_mqtt_queue_count--;
        ret = 0;
    }

    pthread_mutex_unlock(&g_mqtt_queue_lock);

    return ret;
}

/* 每拍搬一次队。**只有 network_task 调**（它是 mqtt_socket 的拥有者）。
 *
 * 取一条、立刻出锁、再发 —— 持锁期间只碰队列里的字节，绝不 send()。
 * MQTT 没连上时整队丢掉：留着它们会一直占满 8 个槽（新消息全被拒），
 * 而且重连之后会把一堆过期状态一次性冲出去（界面会闪回旧状态）。 */
static void mqtt_queue_drain(void)
{
    int sent = 0;

    while (sent < MQTT_QUEUE_SLOTS && mqtt_queue_pop(&g_mqtt_queue_drain_item) == 0) {
        if (!mqtt_config.connected) {
            g_mqtt_queue_drop_nolink++;
            if (g_mqtt_queue_drop_nolink == 1 ||
                (g_mqtt_queue_drop_nolink % 32) == 0) {
                printf("[MQTTQ] MQTT 未连接，丢弃排队消息（累计丢 %u 条）\n",
                       g_mqtt_queue_drop_nolink);
            }
            continue;
        }

        {
            int ret = mqtt_publish(g_mqtt_queue_drain_item.topic,
                                   g_mqtt_queue_drain_item.payload,
                                   g_mqtt_queue_drain_item.qos,
                                   g_mqtt_queue_drain_item.retain);

            /* 这一行就是"排队这条路通了"的验收证据：它前面紧跟着
             * mqtt_publish 自己打的 "Publish to <topic>: <payload>"。 */
            printf("[MQTTQ] 队列发出: topic=%s qos=%d ret=%d（队列剩 %d 条）\n",
                   g_mqtt_queue_drain_item.topic,
                   g_mqtt_queue_drain_item.qos, ret, mqtt_queue_count_read());
        }

        sent++;
    }
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
/* queued=false：直接发。**调用者必须是 network_task 那个 task group**
 * （只有它手里有 mqtt_socket），robot_ui 收包回调 / 心跳那条路走的就是它。
 * queued=true ：拷进队列，交给 network_task 发。给 hello_app 的线程用
 * （见 mqtt_publish_queued() 上面那段真机日志）。
 * 两种走法除发布方式外完全一致：同 topic、同 payload、同 QoS、同本地回调 + 推送。 */
static int alarm_publish(const char *alarm_type, const char *details, bool queued)
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
    int ret = queued ? mqtt_publish_queued(topic, json, 1, false)
                     : mqtt_publish(topic, json, 1, false);
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

int report_alarm(const char *alarm_type, const char *details)
{
    return alarm_publish(alarm_type, details, false);
}

int report_alarm_queued(const char *alarm_type, const char *details)
{
    return alarm_publish(alarm_type, details, true);
}

/* ==================== 诊断：把板子内部状态从 MQTT 捞出来 ==================== */
/*
 * 背景：**串口线丢了**，MQTT 是唯一的观测通道。而这次故障最难的地方是
 * "hello_app 的语音链路哑了，但从外面看不出来"——它自己的 MQTT 上报被
 * g_net_started 钉死，心跳和 light_on 都是 robot_ui 干的活，证明不了它活着。
 *
 * 所以这一节做两件事：
 *   1) 心跳（每 30 秒一次、已经被证明可靠）里加几个状态字段 —— 长期观测面；
 *   2) {"action":"diag"} 动作（main.c 那边分发）立刻回一条完整快照 —— 现场取证。
 *
 * 快照分三块，各由"知道它的人"提供：
 *   robot —— robot_ui 自己的（AI 初始化 / 录音 / 放音 / 面板），由 main.c 注册的
 *            提供者回调给；
 *   net   —— 本文件知道的（broker、连接状态、连续失败次数、uptime、任务数）；
 *   hello —— hello_app 自己的（ai_companion_diag_snapshot 原样内嵌；拿不到就是
 *            "unavailable"，而"拿不到"恰恰是最有价值的证据）。
 *
 * 纪律：取状态不许阻塞、不许引入失败 —— 拿不到就写 -1/"unavailable"，
 * 绝不能因为某个字段取不到而让心跳或诊断本身发不出去。
 */

void network_set_local_state_provider(local_state_provider_t provider)
{
    g_local_state_provider = provider;
}

/* 问一次 robot_ui 那侧的状态。没有提供者就不覆盖，四个字段留在 -1。 */
static void diag_local_state(local_state_t *out)
{
    out->ai_init   = -1;
    out->recording = -1;
    out->playing   = -1;
    out->panel     = -1;

    if (g_local_state_provider != NULL) {
        g_local_state_provider(out);
    }
}

/* hello_app 的语音诊断接口在不在？>=0 = 它在跑并交了快照，<0 = 不可用。
 * 心跳里只留一个 bit（ha），所以这里不要完整快照，给个够用的缓冲、内容丢掉。
 * 缓冲不给太小：快照按字段拼长度，万一实现是"装不下就报错"的那种，
 * 缓冲过小会把"hello_app 活着"误报成不可用，那比不报还糟。 */
static bool hello_diag_available(void)
{
    char buf[128];

    return ai_companion_diag_snapshot(buf, sizeof(buf)) >= 0;
}

/* 开机到现在的秒数。用 CLOCK_MONOTONIC 而不是 time()：后者会因为开机对时而
 * 跳变（板子没有备份电池，上电时间从 2000 年开始，对时之后直接跳），
 * uptime 要的是"这次开机跑了多久"，正好用来区分"板子重启过"和"一直没动过"。
 * 取不到就报 -1：这只是观测信息，绝不能让心跳/诊断因此失败。 */
static long diag_uptime_sec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }

    return (long)ts.tv_sec;
}

/* 数一下现在有多少个任务/线程（就是 ps 里的行数）。 */
static void diag_task_count_cb(FAR struct tcb_s *tcb, FAR void *arg)
{
    (void)tcb;
    (*(int *)arg)++;
}

static int diag_task_count(void)
{
    int n = 0;

    nxsched_foreach(diag_task_count_cb, &n);
    return n;
}

/* 内嵌 hello_app 快照的上限。整条载荷最后要塞进 mqtt_send_publish() 那个
 * 1024 字节的固定包缓冲（topic 最长 77 + 固定头），快照不能无限大。 */
#define DIAG_HELLO_MAX    512

/* 快照不能当对象内嵌、只能当字符串贴出来时的上限。
 * 为什么必须截：cJSON 会把引号/反斜杠转义成两个字节，不截的话一条 512 字节的
 * 快照最坏能顶出 1 KB，把整条报文顶出上面那个包缓冲。300 字节最坏转义成 600，
 * 加上 robot 和外壳仍在安全范围内。截掉多少由 hello_len 说明（见 diag_build_payload）。 */
#define DIAG_HELLO_STR_MAX 300

/* 整条载荷的上限：超过就退化成"不内嵌快照"的短版本。宁可用一条短回复说明
 * "快照太长"，也不要发一条被截断成非法 JSON 出去 —— 对端解析不了就全丢了。 */
#define DIAG_PAYLOAD_MAX  900

/* 组一条 diag 载荷。
 *   hello/hello_len: hello_app 的快照（hello_len < 0 = 不可用）
 *   skip_hello     : true = 不内嵌内容，只报长度（快照太长时的退路）
 * 无论哪条路，返回的都是**合法 JSON 的字符串**（堆上，调用方 free）：
 * 快照解析不了（被截断 / 不是合法对象）时按字符串贴出来，cJSON 负责转义。 */
static char *diag_build_payload(const char *hello, int hello_len, bool skip_hello)
{
    local_state_t st;
    cJSON *root;
    cJSON *robot;
    cJSON *item;
    char *json;

    diag_local_state(&st);

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "type", "diag");
    cJSON_AddStringToObject(root, "device_id", mqtt_config.client_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "uptime", (double)diag_uptime_sec());

    robot = cJSON_CreateObject();
    if (robot == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddNumberToObject(robot, "ai_init", st.ai_init);
    cJSON_AddNumberToObject(robot, "rec", st.recording);
    cJSON_AddNumberToObject(robot, "play", st.playing);
    cJSON_AddNumberToObject(robot, "panel", st.panel);
    cJSON_AddNumberToObject(robot, "tasks", diag_task_count());
    cJSON_AddBoolToObject(robot, "mqtt", mqtt_config.connected);
    cJSON_AddStringToObject(robot, "broker", mqtt_config.broker);
    cJSON_AddNumberToObject(robot, "mqtt_fails", g_mqtt_fails);
    cJSON_AddNumberToObject(robot, "broker_switches", g_mqtt_broker_switches);
    cJSON_AddItemToObject(root, "robot", robot);

    if (skip_hello && hello_len > 0) {
        /* 上一次组装出来太长（见 report_diag）：这一次一个字节内容都不内嵌，
         * 只说快照有多长。这一路是保底 —— 保证对端至少能收到 robot 那一块。 */
        cJSON_AddStringToObject(root, "hello", "too_long");
        cJSON_AddNumberToObject(root, "hello_len", hello_len);
    } else if (hello_len < 0) {
        /* <0 = hello_app 没在跑（接口取不到），这是最该报出来的一条证据，原样说 */
        cJSON_AddStringToObject(root, "hello", "unavailable");
    } else if (hello_len == 0) {
        /* 接口答了但什么都没写。**不能**当成 unavailable：那是对 hello_app
         * 状态的另一种断言（"它没在跑"），而这里并不知道这件事 —— 观测报文宁可
         * 说"没内容"，也不要编一个结论出来。 */
        cJSON_AddStringToObject(root, "hello", "empty");
    } else {
        item = cJSON_Parse(hello);
        if (item != NULL && cJSON_IsObject(item)) {
            cJSON_AddItemToObject(root, "hello", item);   /* 正常：对象原样内嵌 */
        } else {
            char cut[DIAG_HELLO_STR_MAX + 1];
            int  n = (hello_len > DIAG_HELLO_STR_MAX) ? DIAG_HELLO_STR_MAX
                                                      : hello_len;

            /* 解析不成对象（被截断的片段 / 接口报的不是对象）：原样贴出来，
             * 现场才有东西可看。但要按 DIAG_HELLO_STR_MAX 截一刀，
             * 理由见那个宏的说明。截掉多少由 hello_len 说明。 */
            if (item != NULL) {
                cJSON_Delete(item);
            }

            memcpy(cut, hello, n);
            cut[n] = '\0';
            cJSON_AddStringToObject(root, "hello", cut);

            if (n < hello_len) {
                cJSON_AddNumberToObject(root, "hello_len", hello_len);
            }
        }
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ==================== 上报心跳 ==================== */
int report_heartbeat(void)
{
    char topic[128];
    local_state_t st;

    snprintf(topic, sizeof(topic), "zhi_ai/%s/heartbeat", mqtt_config.client_id);

    /* 把所有要报的状态先读齐（读不到的字段自己会留 -1），再建 JSON：
     * 中途不碰任何可能阻塞的东西，见本节头上的纪律。 */
    diag_local_state(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "device_id", mqtt_config.client_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

    /* 心跳是长期观测面（30 秒一条、且已经证明可靠），所以状态字段挂在这里。
     * 全部是当场读的廉价量，一个都不去等设备/等网络：
     *   ha           hello_app 的语音诊断接口还在不在（= 它还在跑）。1/0。
     *   rec/play/panel  robot_ui 自己：在录音 / 在放音 / 镜像面板开着。1/0/-1。
     *   mqtt_fails   连续连接失败次数（这个字段本身在 MQTT 连着的时候才有意义，
     *                它一直涨说明"连上了又被踢"，也是现场常见的那种）。
     *   uptime       开机秒数，用来区分"板子重启过"和"一直是同一次开机"。
     * 取不到就写 -1，绝不为了凑齐字段而等待。 */
    cJSON_AddNumberToObject(root, "ha", hello_diag_available() ? 1 : 0);
    cJSON_AddNumberToObject(root, "rec", st.recording);
    cJSON_AddNumberToObject(root, "play", st.playing);
    cJSON_AddNumberToObject(root, "panel", st.panel);
    cJSON_AddNumberToObject(root, "mqtt_fails", g_mqtt_fails);
    cJSON_AddNumberToObject(root, "uptime", (double)diag_uptime_sec());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return -1;
    }

    int ret = mqtt_publish(topic, json, 0, false);
    free(json);

    return ret;
}

/* ==================== 上报完整诊断快照（{"action":"diag"}） ==================== */
int report_diag(void)
{
    char topic[128];
    char hello[DIAG_HELLO_MAX];
    char *json;
    int hlen;
    int ret;

    /* 1) 先问 hello_app 要它那一份。返回负值 = 它没在跑 / 接口取不到 ——
     *    这个结论本身就照原样报出去（"hello":"unavailable"），不要吞掉。 */
    hlen = ai_companion_diag_snapshot(hello, sizeof(hello));

    /* 防守：接口承诺"返回写入长度"，但真写满时不保证有结尾的 '\0'，
     * 而下面要把它当字符串解析/输出，所以这里夹一下长度再自己收尾。
     * 夹完 hlen <= sizeof(hello) - 1，hello[hlen] 一定在数组内。 */
    if (hlen > (int)sizeof(hello) - 1) {
        hlen = (int)sizeof(hello) - 1;
    }
    if (hlen > 0) {
        hello[hlen] = '\0';
    }

    /* 2) 组装。太长（hello_app 报了个特别长的快照）就退化成不内嵌内容的短版本：
     *    mqtt_send_publish() 的包缓冲只有 1024 字节，硬发会被它自己拒掉，
     *    那样对端连"板子回话了"都看不到。 */
    json = diag_build_payload(hello, hlen, false);
    if (json == NULL) {
        printf("[Diag] 载荷组装失败（内存不足）\n");
        return -1;
    }

    if (strlen(json) > DIAG_PAYLOAD_MAX) {
        printf("[Diag] 载荷 %u 字节太长，退化成不内嵌快照的短版本\n",
               (unsigned)strlen(json));
        free(json);
        json = diag_build_payload(hello, hlen, true);
        if (json == NULL) {
            return -1;
        }
    }

    /* 3) 发到 zhi_ai/<client_id>/status。
     *    选这条 topic 而不是新开一条：它已经是本设备在用的"状态"话题
     *    （遗嘱消息也发在这里），对端只订阅 zhi_ai/# 就能同时看到心跳、遗嘱和
     *    诊断，不用额外记住一个新名字；诊断消息靠 "type":"diag" 区分。
     *    QoS0 + 不 retain：
     *      QoS0     —— 诊断是"问一次答一次"的即时快照，丢一条再问一次就是，
     *                  不必为它引入 QoS1 的重发/包 ID 那套；
     *      不 retain —— 这条 topic 上保留着的是遗嘱（设备掉线时 broker 代发的
     *                  "OFF"）和设备状态的老语义；把一份带时间戳的快照留在
     *                  broker 上，下一个订阅者连上就会看到一份过期的诊断。 */
    snprintf(topic, sizeof(topic), "zhi_ai/%s/status", mqtt_config.client_id);

    ret = mqtt_publish(topic, json, 0, false);

    /* 这一行和 mqtt_parse_packet() 里那句 "[DIAG] rx action=..." 配成一对：
     * 两条都在 = 请求进来、回执发出去了，剩下的就是主机侧的事；只有 rx = 回执
     * 没能发出去（看 ret）；两条都没有 = 请求根本没到板子（订阅没了 / 发在了
     * 别的 broker）。 */
    printf("[DIAG] tx topic=%s len=%u hello_len=%d ret=%d\n",
           topic, (unsigned)strlen(json), hlen, ret);

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

    /* 板端没有预置 CA 证书，先和 MiMo demo 一样跳过证书校验；生产环境
     * 应在这里加载 CA 链并把 authmode 改成 MBEDTLS_SSL_VERIFY_REQUIRED。 */
    mbedtls_ssl_conf_authmode(&g_push_tls_ctx.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&g_push_tls_ctx.conf, mbedtls_ctr_drbg_random,
                         &g_push_tls_ctx.ctr_drbg);

    g_push_tls_ready = true;
    return 0;
}
#endif /* CONFIG_CRYPTO_MBEDTLS */

/* webclient 的响应体累加器 */
struct push_resp_buf_s
{
    char  *data;
    size_t len;
    size_t cap;
};

static int push_sink_callback(char **buffer, int offset, int datend,
                              int *buflen, void *arg)
{
    struct push_resp_buf_s *resp = arg;
    int len = datend - offset;

    (void)buflen;
    if (len <= 0) {
        return 0;
    }

    while (resp->len + (size_t)len + 1 > resp->cap) {
        size_t newcap = resp->cap ? resp->cap * 2 : 256;
        char *newdata = realloc(resp->data, newcap);

        if (newdata == NULL) {
            return -ENOMEM;
        }
        resp->data = newdata;
        resp->cap = newcap;
    }

    memcpy(resp->data + resp->len, &((*buffer)[offset]), len);
    resp->len += (size_t)len;
    resp->data[resp->len] = '\0';
    return 0;
}

/* 发送 HTTP POST 请求（带 TLS）。
 * 返回值：>=0 为 HTTP 状态码（200/201 表示成功），<0 为连接/传输失败。 */
static int http_post(const char *url, const char *body)
{
    struct webclient_context ctx;
    struct push_resp_buf_s resp;
    const char *headers[1];
    char *work_buf;
    int ret;

    printf("[PUSH] http_post url=%s body_len=%d\n", url, (int)strlen(body));

#ifdef CONFIG_CRYPTO_MBEDTLS
    if (push_tls_init() < 0) {
        printf("[PUSH] http_post: TLS init failed\n");
        return -1;
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

    if (ret < 0) {
        printf("[PUSH] http_post: request failed ret=%d\n", ret);
        if (resp.data != NULL) {
            free(resp.data);
        }
        return -1;
    }

    /* 状态码 + 响应体前若干字节都打出来，串口上才能判断成功没成功 */
    printf("[PUSH] HTTP status=%u resp_len=%d resp=%.200s\n",
           ctx.http_status, (int)resp.len,
           resp.data ? resp.data : "(empty)");

    ret = (int)ctx.http_status;
    if (resp.data != NULL) {
        free(resp.data);
    }
    return ret;
}

/* -------------------------------------------------------------------------
 * 推送后台任务
 *
 * push_send_notification() 被 report_alarm()（UI 报警按钮）同步调用，
 * 一次 TLS 往返可能几百 ms 到数秒。这里把 url+body 放进单槽队列后立即返回，
 * 由独立任务真正发 HTTP，UI 线程不被阻塞。
 * ---------------------------------------------------------------------- */
#define PUSH_TASK_STACKSIZE 32768
#define PUSH_TASK_PRIORITY  120

struct push_job_s
{
    bool pending;
    char url[256];
    char body[HTTP_BUFFER_SIZE];
};

static struct push_job_s g_push_job;
static pthread_mutex_t g_push_job_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t g_push_job_sem;
static pthread_t g_push_task_id;
static bool g_push_task_started = false;

static void *push_task(void *arg)
{
    (void)arg;

    for (;;) {
        char url[sizeof(g_push_job.url)];
        char body[sizeof(g_push_job.body)];
        bool have = false;
        int status;

        sem_wait(&g_push_job_sem);

        pthread_mutex_lock(&g_push_job_lock);
        if (g_push_job.pending) {
            strncpy(url, g_push_job.url, sizeof(url) - 1);
            url[sizeof(url) - 1] = '\0';
            strncpy(body, g_push_job.body, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            g_push_job.pending = false;
            have = true;
        }
        pthread_mutex_unlock(&g_push_job_lock);

        if (!have) {
            continue;
        }

        status = http_post(url, body);
        if (status == 200 || status == 201) {
            printf("[PUSH] OK: HTTP %d -> %s\n", status, url);
        } else {
            printf("[PUSH] FAIL: HTTP %d -> %s\n", status, url);
        }
    }

    return NULL;
}

static void push_task_start_once(void)
{
    struct sched_param param;
    pthread_attr_t attr;

    if (g_push_task_started) {
        return;
    }

    if (sem_init(&g_push_job_sem, 0, 0) != 0) {
        printf("[PUSH] sem_init failed\n");
        return;
    }

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PUSH_TASK_STACKSIZE);

    /* 用比 UI 更低的优先级（数值更大），确保 TLS 计算不抢 UI */
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    memset(&param, 0, sizeof(param));
    param.sched_priority = PUSH_TASK_PRIORITY;
    pthread_attr_setschedparam(&attr, &param);

    if (pthread_create(&g_push_task_id, &attr, push_task, NULL) != 0) {
        printf("[PUSH] push task create failed\n");
        pthread_attr_destroy(&attr);
        return;
    }

    pthread_attr_destroy(&attr);
    g_push_task_started = true;
    printf("[PUSH] async push task started (stack=%d prio=%d)\n",
           PUSH_TASK_STACKSIZE, PUSH_TASK_PRIORITY);
}

/* 初始化推送服务。
 * key 为 NULL/空串时，按 service 从 /etc/assets/push_key.txt 或
 * PUSH_KEY_DEFAULT* 取（这样调用方就不用在源码里硬编码密钥）。 */
int push_init(push_service_t service, const char *key)
{
    char loaded[sizeof(push_config.push_key)] = {0};
    const char *use = key;

    if (use == NULL || use[0] == '\0') {
        if (!push_load_key(service, loaded, sizeof(loaded))) {
            printf("push_init: no key for service=%d\n", service);
            return -1;
        }
        use = loaded;
    }

    push_config.service = service;
    strncpy(push_config.push_key, use, sizeof(push_config.push_key) - 1);
    push_config.push_key[sizeof(push_config.push_key) - 1] = '\0';
    push_config.enabled = true;

    /* 不打印完整 key，避免密钥进串口日志 */
    printf("push_init: service=%d, key=%.4s...(%d chars)\n",
           service, use, (int)strlen(use));

    push_task_start_once();
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

/* ==================== 异常声音检测接口 ==================== */

/* queued 的语义同 alarm_publish()（见上面那段说明）：
 * false = 直接发（调用者必须在 network_task 组里），true = 排队交给 network_task。 */
static int sound_alarm_publish(const char *sound_type, int confidence, bool queued)
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
    int ret = queued ? mqtt_publish_queued(topic, json, 1, false)
                     : mqtt_publish(topic, json, 1, false);
    free(json);

    /* 触发本地报警回调 */
    if (alarm_callback) {
        alarm_callback(sound_type, "Abnormal sound detected");
    }

    return ret;
}

int report_abnormal_sound(const char *sound_type, int confidence)
{
    return sound_alarm_publish(sound_type, confidence, false);
}

int report_abnormal_sound_queued(const char *sound_type, int confidence)
{
    return sound_alarm_publish(sound_type, confidence, true);
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

/* queued 的语义同 alarm_publish()：
 * false = 直接发（robot_ui 收包回调里"替用户开灯"那条路），
 * true  = 排队交给 network_task（hello_app 的灯控工具那条路）。 */
static int device_command_publish(const char *device_id, const char *command,
                                  bool queued)
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

    int ret = queued ? mqtt_publish_queued(topic, json, 1, false)
                     : mqtt_publish(topic, json, 1, false);
    free(json);

    return ret;
}

int send_device_command(const char *device_id, const char *command)
{
    return device_command_publish(device_id, command, false);
}

int send_device_command_queued(const char *device_id, const char *command)
{
    return device_command_publish(device_id, command, true);
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

    /* 装不下就别装。
     *
     * 这一层原来没有长度检查：超过 packet[] 的载荷会在下面 memcpy 时直接写坏
     * network_task 的栈（表现是随机的整机异常，极难查）。心跳/报警那几条一直很短
     * 所以没撞上，而诊断报文里要内嵌 hello_app 的快照（长度不受我们控制），
     * 正好是最容易撞上这个上限的一条 —— 所以在这里补一道。
     *
     * 开销：1 字节固定头 + 最多 4 字节剩余长度 + 2 字节 topic 长度，其余是
     * topic 和载荷，QoS>0 再多 2 字节包 ID。宁可这次不发（返回 -1，调用方按
     * 失败处理/记日志），也绝不打坏栈。 */
    if (strlen(topic) + strlen(payload) + ((qos > 0) ? 2 : 0) + 7 > sizeof(packet)) {
        printf("MQTT publish 拒绝：太长 topic=%d payload=%d（包缓冲 %u 字节）\n",
               (int)strlen(topic), (int)strlen(payload), (unsigned)sizeof(packet));
        return -1;
    }

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

/* 发送 MQTT PUBACK（确认一条 QoS1 的入站 PUBLISH）
 *
 * 不回的后果：broker 认为客户端没收到，会按它自己的策略重投（有的公共
 * broker 几秒一次），同一条控制命令会被执行多遍——灯被闪来闪去、或者
 * 状态消息刷屏。报文格式很固定：固定头 0x40 + 剩余长度 0x02 + 2 字节包 ID。
 *
 * 安全性：只被 mqtt_parse_packet() 调用，而它只跑在 network_task 里，
 * 也就是 mqtt_socket 的属主任务。所以这里的 send() 不会和其他任务抢同一个
 * fd（跨任务 close/send 会把整机打复位，见 mqtt_publish() 里那段注释），
 * 不需要额外的锁或状态机。
 *
 * QoS2 不在这里处理：协议上要回的是 PUBREC + 等 PUBREL 再 PUBCOMP，
 * 拿 PUBACK 回给 QoS2 反而是协议错误。目前没有任何对端用 QoS2 发消息给板子。 */
static int mqtt_send_puback(uint16_t packet_id)
{
    uint8_t packet[4];

    packet[0] = 0x40;                          /* PUBACK */
    packet[1] = 0x02;                          /* 剩余长度固定 2 */
    packet[2] = (packet_id >> 8) & 0xFF;
    packet[3] = packet_id & 0xFF;

    return send(mqtt_socket, packet, sizeof(packet), 0);
}

/* 从入站载荷里抠出 "action":"xxx" 的值（没有就把 dst 置空）。
 *
 * 只为一行日志服务：这条路上跑的是 {"action":"diag"} 这种小对象，不值当再引
 * 一次 cJSON_Parse（紧接着的回调里本来就会真解析一遍）。 */
static void diag_peek_action(const char *payload, char *dst, size_t dstlen)
{
    const char *p;
    size_t n = 0;

    if (dst == NULL || dstlen == 0) {
        return;
    }

    dst[0] = '\0';

    p = strstr(payload, "\"action\"");
    if (p == NULL) {
        return;
    }

    p = strchr(p + 8, ':');
    if (p == NULL) {
        return;
    }

    p = strchr(p + 1, '"');
    if (p == NULL) {
        return;
    }

    for (p++; *p != '\0' && *p != '"' && n + 1 < dstlen; p++) {
        dst[n++] = *p;
    }

    dst[n] = '\0';
}

/* 解析 MQTT 数据包
 *
 * 一次 recv() 拿到的是**一段字节流**，里面可能挤着不止一包（broker 合并发送
 * 很常见：SUBACK 紧跟 PUBLISH、连着几条 PUBLISH 一起发）。所以这里按报文自带
 * 的剩余长度一包一包往前走，走到头为止。
 *
 * 原来只解析第一包、后面的字节随着这次 recv 一起丢掉，而且一声不响 —— 这种
 * 静默丢弃正是"收发都正常、命令就是不进来"里最难查的一环。
 *
 * 跨 recv 的半包重组：解析循环走到末尾还剩不足一包时，把残字节留在
 * g_mqtt_rx_buf 头部，下次 recv 拼在前面再解析（缓冲大小关系见那个变量的注释）。
 * 原来这里是把半包丢掉 —— 丢掉的包尾巴会在下一次 recv 里被当成新报文解析，
 * 而 '0'-'9' 的高半字节正好是 3 = PUBLISH，于是解出假主题、还可能回一个假
 * PUBACK 出去。丢弃一定有日志，绝不静默。
 */
static int mqtt_parse_packet(void)
{
    uint8_t *buffer = g_mqtt_rx_buf;
    int len;
    int pos;
    int total_len;

    /* 防御：残留长度只可能落在 [0, 2048)，越界说明状态被写坏了。真发生了也只
     * 是把这一轮的字节丢掉，不会越界读。 */
    if (g_mqtt_rx_len < 0 || g_mqtt_rx_len >= MQTT_RX_BUF_SIZE) {
        printf("[MQTT] 重组缓冲残留长度异常(%d)，复位\n", g_mqtt_rx_len);
        mqtt_rx_reset();
    }

    /* 上次的半包留在缓冲头部，本次只往**剩下的空间**里收：
     * 布局 = [g_mqtt_rx_len 字节残留][最多 2048-g_mqtt_rx_len 字节新数据] */
    len = recv(mqtt_socket, &g_mqtt_rx_buf[g_mqtt_rx_len],
               MQTT_RX_BUF_SIZE - g_mqtt_rx_len, 0);

    if (len == 0) {
        /* recv 返回 0 = 对端关了连接：broker 限流、被**同 client_id 的另一条
         * 连接顶掉**、或者网络掉了。
         *
         * 这里只置标志，一个字节的 socket 操作都不做：关连接和重连都归
         * network_task（它是 mqtt_socket 的属主，见 mqtt_publish 上面那段
         * "跨任务 close 会把整机打复位"）。
         *
         * 不置这个标志的后果是"假装连着"：订阅是跟着连接走的（CONNECT 带的
         * 是 clean session），连接一没，zhi_ai/<client_id>/command 上就没有
         * 订阅者了，而板子要等到下一次心跳 publish 失败才发现（最长 30 秒）。
         * 那段时间窗口里外面发什么都进不来 —— 现场看到的就是"心跳看得见、
         * 命令/诊断永远不回"。 */
        printf("[MQTT] 对端关闭连接（recv=0），标记未连接，交由 network_task "
               "重连（重连时会重新订阅）\n");
        mqtt_rx_reset();
        mqtt_config.connected = false;
        board_status_set_mqtt(false);
        return -1;
    }

    if (len < 0) {
        /* 非阻塞 socket 上"暂时没数据"确实是负返回值，但**不是所有负返回值都是
         * 没数据**：broker 被 RST、TCP 重传超时、RNDIS 掉线在 NuttX 上是
         * -ENOTCONN。原来一律当"没数据"直接 return，于是掉线一行日志都没有，
         * 板子还能显示"已连接"最长 30 秒（要等下一次心跳 publish 失败才发现）。
         * 只有 EAGAIN/EWOULDBLOCK 才是"没数据"。
         *
         * EAGAIN 这条路上**故意保留**半包残留：那包数据只是还没到齐，没坏。
         * （socket 收尾仍然只置标志，不在这个任务之外碰 fd。） */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }

        printf("[MQTT] recv 失败（errno=%d），标记未连接，交由 network_task 重连\n",
               errno);
        mqtt_rx_reset();
        mqtt_config.connected = false;
        board_status_set_mqtt(false);
        return -1;
    }

    total_len = g_mqtt_rx_len + len;

    /* 从这里起"残留"由本函数重新决定：只有真的又留下半包才会重新写它。 */
    g_mqtt_rx_len = 0;

    for (pos = 0; pos < total_len; ) {
        uint8_t type;
        int remaining;
        int multiplier;
        int hdr_end;
        int total;
        bool rl_ok;

        /* 上轮判定"这包太大、重组不了"时留下的待丢字节数：按字节数跳过去，
         * 别让超大包的中段被当成新报文解析（'0'-'9' 的高半字节正好是 3 =
         * PUBLISH，会解出假主题并回假 PUBACK）。 */
        if (g_mqtt_rx_skip > 0) {
            int avail = total_len - pos;

            if (avail < g_mqtt_rx_skip) {
                g_mqtt_rx_skip -= avail;
                break;
            }

            pos += g_mqtt_rx_skip;
            g_mqtt_rx_skip = 0;
            continue;
        }

        type = (buffer[pos] >> 4) & 0x0F;
        remaining = 0;
        multiplier = 1;
        rl_ok = false;

        /* 剩余长度：变长编码，最多 4 字节（再多就是非法报文） */
        hdr_end = pos + 1;
        while (hdr_end < total_len) {
            uint8_t byte = buffer[hdr_end];

            remaining += (byte & 0x7F) * multiplier;
            hdr_end++;
            if ((byte & 0x80) == 0) {
                rl_ok = true;
                break;
            }
            multiplier *= 128;
            if (multiplier > 128 * 128 * 128) {
                break;
            }
        }

        total = hdr_end + remaining;

        if (!rl_ok) {
            /* 剩余长度用了超过 4 字节还没结束 = 报文本身不合法，字节流已经没
             * 法重新对齐，只能整段丢掉（MUST NOT 死循环：pos 直接推到末尾）。 */
            printf("[MQTT] 剩余长度非法（type=%u），丢弃剩余 %d 字节\n",
                   (unsigned)type, total_len - pos);
            break;
        }

        /* total 是**绝对**下标（本包最后一个字节的下一位），total_len 是缓冲里
         * 已有的字节总数，所以直接比就行，别拿 total 和"剩余长度"比。 */
        if (total > total_len) {
            int plen = total - pos;              /* 这一包总共多少字节 */
            int partial = total_len - pos;       /* 已经到手多少字节 */

            /* 半包：本次 recv 只拿到报文的一部分。
             *
             * 能装下就留到下次 recv 拼起来再解析 —— 这是本次加固的正题：丢掉
             * 它，它的尾巴就会在下次 recv 里被当成新报文（错位解析）。
             * 只留 partial（不是 plen）：缓冲头部的这段就是下次要拼的内容。 */
            if (plen <= MQTT_RX_BUF_SIZE) {
                memmove(buffer, &buffer[pos], (size_t)partial);
                g_mqtt_rx_len = partial;
                printf("[MQTT] 半包（type=%u 共 %d 字节，已收 %d），"
                       "留待下次 recv 重组\n", (unsigned)type, plen, partial);
                break;
            }

            /* 这包比重组缓冲还大（>2KB）：等不到了，改成记下"还要丢多少字节"，
             * 把这一包剩下的整段丢掉。丢弃策略是**有界**的：只丢这一包，
             * 丢完就从下一个包边界继续，缓冲永远不增长、不会死循环。 */
            g_mqtt_rx_skip = plen - partial;
            printf("[MQTT] 报文过大（type=%u 共 %d 字节 > 重组上限 %d），"
                   "整包丢弃并跳过其后 %d 字节\n",
                   (unsigned)type, plen, MQTT_RX_BUF_SIZE, g_mqtt_rx_skip);
            break;
        }

        switch (type) {
            case 0x02:  // CONNACK
                /* broker 对 CONNECT 的回执。**回执码非 0 = 这次连接被拒了**
                 * （0x01 协议版本 / 0x02 client_id 被拒 / 0x04 用户名密码 /
                 * 0x05 没授权）。原来这里落进 default 打成"Unknown packet
                 * type"，被拒也照样显示"已连接" —— 这种板子最会骗人。
                 */
                printf("[MQTT] CONNACK session_present=%d rc=%d\n",
                       (remaining >= 1) ? buffer[hdr_end] : -1,
                       (remaining >= 2) ? buffer[hdr_end + 1] : -1);

                if (remaining < 2 || buffer[hdr_end + 1] != 0) {
                    printf("[MQTT] 连接被 broker 拒绝（rc=%d），标记未连接等待重连\n",
                           (remaining >= 2) ? buffer[hdr_end + 1] : -1);
                    mqtt_config.connected = false;
                    board_status_set_mqtt(false);
                }
                break;

            case 0x09:  // SUBACK
                /* "订阅到底建起来没有"唯一的一手证据。返回码 0x80 = 失败，
                 * 那种情况下命令主题上根本没有订阅者。 */
                {
                    int i;

                    for (i = 2; i < remaining; i++) {
                        printf("[MQTT] SUBACK pid=%d rc=0x%02X%s\n",
                               (remaining >= 2)
                                   ? ((buffer[hdr_end] << 8) | buffer[hdr_end + 1])
                                   : -1,
                               buffer[hdr_end + i],
                               (buffer[hdr_end + i] == 0x80)
                                   ? "（订阅被拒！命令收不到）" : "");
                    }
                }
                break;

            case 0x04:  // PUBACK
                /* 我们自己发出去的 QoS1 PUBLISH 的回执（心跳/告警/诊断都是
                 * QoS1）。收到就说明 broker 收下了，本地没有要维护的状态。
                 * 空分支是**故意**的：不写它就会落进 default 打出
                 * "Unknown packet type: 4" —— 每发一条 QoS1 刷一行，看起来
                 * 像出了错，实际是正常协议交互。 */
                break;

            case 0x0D:  // PINGRESP
                printf("Received PINGRESP\n");
                break;

            case 0x03:  // PUBLISH
                {
                    int cur = hdr_end;
                    int qos = (buffer[pos] >> 1) & 0x03;   /* bit0=retain，bit2:1=QoS */
                    int topic_len;
                    int copy_len;
                    int payload_len;
                    char topic[128];
                    char payload[1024];

                    /* 主题名：2 字节长度 + 内容。长度是网络上来的，先确认它
                     * 落在这一包之内再动指针。 */
                    if (cur + 2 > total) {
                        printf("[MQTT] PUBLISH 主题长度不完整，丢弃这一包\n");
                        break;
                    }

                    topic_len = (buffer[cur] << 8) | buffer[cur + 1];
                    cur += 2;

                    if (cur + topic_len > total) {
                        printf("[MQTT] PUBLISH 主题名越界（声明 %d 字节，本包只剩 %d），"
                               "丢弃这一包\n", topic_len, total - cur);
                        break;
                    }

                    copy_len = (topic_len < (int)(sizeof(topic) - 1))
                                   ? topic_len : (int)(sizeof(topic) - 1);
                    memcpy(topic, &buffer[cur], copy_len);
                    topic[copy_len] = '\0';
                    cur += topic_len;

                    /* MQTT 3.1.1 的 PUBLISH 可变头里，主题名之后**只有 QoS>0**
                     * 才跟 2 字节 Packet Identifier。
                     *
                     * 原来这里漏了这一步，把主题之后的全部字节都当载荷，于是入站
                     * QoS1 的报文（payload 头上多出 2 字节包 ID，高字节通常是 0x00）
                     * 一律解析失败——队友的智能灯/设备状态都是 QoS1 发布，命令和
                     * 状态全丢，串口上只能看到 JSON parse failed。
                     */
                    if (qos > 0 && cur + 2 <= total) {
                        uint16_t packet_id = (uint16_t)((buffer[cur] << 8) | buffer[cur + 1]);

                        cur += 2;

                        /* QoS1 必须回 PUBACK，否则 broker 会重投、命令被重复执行。
                         * QoS2 该回的是 PUBREC，这里不动（目前没有对端用 QoS2）。
                         *
                         * 返回值必须看：socket 是非阻塞的，发送缓冲满时 send()
                         * 返回 -EAGAIN，PUBACK 就这么没了 —— broker 以为我们没
                         * 收到，会重投同一条命令，于是命令被执行两遍（灯被闪两次、
                         * 继电器被点两次）。这里不重试（重试要写发送队列，是另一
                         * 件事），至少要留下痕迹；第一次和每 10 次各打一行，别刷屏。 */
                        if (qos == 1) {
                            int ack_ret = mqtt_send_puback(packet_id);

                            if (ack_ret < 0) {
                                g_mqtt_puback_fails++;
                                if (g_mqtt_puback_fails == 1 ||
                                    (g_mqtt_puback_fails % 10) == 0) {
                                    printf("[MQTT] PUBACK 发送失败（errno=%d，累计 %d 次），"
                                           "broker 可能重投 pid=%u，注意该命令会被执行两次\n",
                                           errno, g_mqtt_puback_fails,
                                           (unsigned)packet_id);
                                }
                            }
                        }
                    }

                    payload_len = total - cur;
                    if (payload_len < 0) {
                        payload_len = 0;
                    }
                    if (payload_len > (int)(sizeof(payload) - 1)) {
                        payload_len = (int)(sizeof(payload) - 1);
                    }
                    memcpy(payload, &buffer[cur], payload_len);
                    payload[payload_len] = '\0';

                    printf("Received: topic=%s qos=%d, payload=%s\n",
                           topic, qos, payload);

                    /* 命令/诊断进来时的第一条线索，和 report_diag() 的
                     * "[DIAG] tx ..." 配成一对：
                     *   只有 rx 没有 tx —— 收到了，但分发那边没认（或响应发不出去）；
                     *   连 rx 都没有   —— 请求根本没到板子（订阅没了 / 发在别的 broker）。 */
                    if (strstr(payload, "\"action\"") != NULL) {
                        char action[32];

                        diag_peek_action(payload, action, sizeof(action));
                        printf("[DIAG] rx action=%s topic=%s\n", action, topic);
                    }

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

        pos = total;
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
    int broker_idx = 0;
    int i;

    /* 失败计数和换台次数不再是本函数的局部变量：它们的定义搬到了文件头上的
     * g_mqtt_fails / g_mqtt_broker_switches（否则心跳和诊断报文读不到它们）。 */

    while (1) {
        /* 检查 WiFi 状态 */
        if (!wifi_config.connected) {
            /* 尝试重连 */
            // TODO: 实现重连逻辑
        }

        /* 上一轮 publish 失败时只把 connected 置了 false（别的任务不敢碰
         * socket，怕和这里的 recv() 抢 fd）。socket 的收尾就在本任务里做：
         * 关掉旧连接，下面那段自然会走重连。 */
        if (!mqtt_config.connected && mqtt_socket >= 0) {
            mqtt_disconnect();
        }

        /* 检查 MQTT 状态 */
        if (wifi_config.connected && !mqtt_config.connected) {
            if (--mqtt_retry_tick <= 0) {
                /* 只有第一轮（还没换过 broker）才用 5 秒的快节奏，好在开机后
                 * 尽快连上；一旦开始换 broker 说明网络/公共实例有问题，一律 30 秒，
                 * 否则会变成"每台试 5 次、15 次一轮"地一直锤公共服务器。
                 */
                mqtt_retry_tick =
                    (g_mqtt_broker_switches == 0 && g_mqtt_fails < 5) ? 50 : 300;

                /* 尝试连接 MQTT */
                if (mqtt_connect(mqtt_config.broker, mqtt_config.port,
                                 mqtt_config.client_id, mqtt_config.username,
                                 mqtt_config.password) < 0) {
                    g_mqtt_fails++;

                    /* 配置里钉了 broker（mqtt_broker）就不再降级：现场要的是
                     * "只连队友那台"，自己漂到别的 broker 反而更难查
                     * （板子显示已连接、收不到东西）。没配时行为完全不变。 */
                    if (g_mqtt_fails >= 5 && MQTT_NBROKERS > 1 && !g_mqtt_broker_pinned) {
                        /* 这台连不上（公共实例限流很常见），换下一台 */
                        broker_idx = (broker_idx + 1) % MQTT_NBROKERS;
                        g_mqtt_broker_switches++;
                        strncpy(mqtt_config.broker, g_mqtt_broker_list[broker_idx],
                                sizeof(mqtt_config.broker) - 1);
                        mqtt_config.broker[sizeof(mqtt_config.broker) - 1] = '\0';
                        printf("MQTT broker 切换到 %s\n", mqtt_config.broker);
                        g_mqtt_fails = 0;
                    }
                } else {
                    g_mqtt_fails = 0;
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

        /* 把别的任务组排好队的消息发出去。
         *
         * 这是**唯一**允许在 network_task 之外产生的 publish 的落地处：hello_app
         * 那里的 voice_state / user_said / device_cmd / 报警全部走 mqtt_publish_queued()
         * 拷进队列（它自己一个 socket 操作都不做），由这一句取出来发。
         * 放的位置和心跳同一段：都用本任务手里那条 socket，必须串行。
         * 没连上时它会在内部整队丢掉（不会留一堆过期状态等着重连后再冲出去）。 */
        mqtt_queue_drain();

        /* 睡一小会儿。
         *
         * 分片睡（10ms × 10）而不是一次 usleep(100000)，是为了让"生产者拷进队列 +
         * 唤醒本任务"这句话真的有用：队列里有货就立刻结束这一觉，消息最迟 10ms
         * 就被取走（原来最多要等满 100ms）。总时长不变，节奏和以前一样。
         *
         * 刻意**不用**阻塞式信号量等待（nxsem_tickwait 之类）：本项目有过
         * "带超时的等待没按时返回"的实锤，而这条单线程循环是 MQTT 的命根子
         * （它一停就心跳、重连、收包全停，看起来跟死机一样），不能赌。 */
        for (i = 0; i < 10; i++) {
            if (sem_trywait(&g_mqtt_queue_sem) == OK) {
                break;      /* 有货，别睡了，回上面去发 */
            }

            usleep(10000);  // 10ms
        }
    }
}
