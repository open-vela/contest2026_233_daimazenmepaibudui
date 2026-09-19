/**
 * network_comm.h - 网络通信模块头文件
 * WiFi 连接、MQTT 通信、云端交互
 */

#ifndef NETWORK_COMM_H
#define NETWORK_COMM_H

#include <stdint.h>
#include <stdbool.h>

/* ==================== WiFi 配置 ==================== */
typedef struct {
    char ssid[64];          // WiFi 名称
    char password[64];      // WiFi 密码
    bool connected;         // 连接状态
    int rssi;               // 信号强度
} wifi_config_t;

/* ==================== MQTT 配置 ==================== */
typedef struct {
    char broker[128];       // MQTT 服务器地址
    uint16_t port;          // 端口号
    char client_id[64];     // 客户端 ID
    char username[64];      // 用户名
    char password[64];      // 密码
    bool connected;         // 连接状态
} mqtt_config_t;

/* ==================== 设备状态 ==================== */
typedef struct {
    float temperature;      // 温度
    float humidity;         // 湿度
    int battery_level;      // 电量
    char status[32];        // 状态
    bool alarm_active;      // 报警状态
} device_status_t;

/* ==================== 消息类型 ==================== */
typedef enum {
    MSG_TYPE_STATUS,        // 状态上报
    MSG_TYPE_ALARM,         // 报警消息
    MSG_TYPE_REMINDER,      // 提醒消息
    MSG_TYPE_COMMAND,       // 控制命令
    MSG_TYPE_HEARTBEAT,     // 心跳
    MSG_TYPE_MAX
} msg_type_t;

/* ==================== 回调函数类型 ==================== */

/**
 * MQTT 收包回调（全局只有一个槽，network_set_mqtt_callback() 注册）。
 *
 * 所有订阅到的主题都从这里出来，回调拿到的是"主题 + 原始 JSON 串"，
 * 由上层按 topic 里的话题名分发。mqtt_connect() 每轮连上后会自动订阅：
 *   - zhi_ai/<client_id>/command       QoS1，控制命令
 *   - zhi_ai/<client_id>/device_state  QoS0，子设备（智能灯）执行结果，
 *     形如 {"type":"device_state","device_id":...,"state":"on"/"off",
 *           "success":true,"message":...,"timestamp":...}
 * 订阅可用 mqtt_subscribe() 追加，topic 直接给完整话题名。
 */
typedef void (*mqtt_msg_callback_t)(const char *topic, const char *payload);
typedef void (*wifi_status_callback_t)(bool connected);
typedef void (*alarm_callback_t)(const char *alarm_type, const char *details);
typedef void (*ai_command_callback_t)(const char *action, const char *param);

/* ==================== 初始化函数 ==================== */
int network_comm_init(void);
void network_comm_deinit(void);

/* ==================== WiFi 函数 ==================== */
int wifi_connect(const char *ssid, const char *password);
int wifi_disconnect(void);
bool wifi_is_connected(void);
int wifi_get_rssi(void);

/* ==================== MQTT 函数 ==================== */
int mqtt_connect(const char *broker, uint16_t port,
                const char *client_id, const char *username, const char *password);
int mqtt_disconnect(void);
bool mqtt_is_connected(void);
int mqtt_subscribe(const char *topic, int qos);
int mqtt_unsubscribe(const char *topic);
int mqtt_publish(const char *topic, const char *payload, int qos, bool retain);

/* ==================== MQTT 排队发布（跨任务组唯一安全的发布方式） ==================== */

/**
 * 把一条消息拷进队列，交给 network_task 用**它自己那条** MQTT socket 发出去。
 *
 * 为什么要有它（真机日志实证，别改回直发）：NuttX 的 fd 属于 task group。
 * mqtt_socket 是 network_task 建出来的，只存在于它那个组的 fd 表里；换一个组
 * （hello_app 的语音线程、LVGL 线程……）拿同一个数字去 send()，要么 EBADF，
 * 要么发到该组里恰好占了这个编号的别的文件上。所以：
 *
 *   MQTT connected
 *   MQTT publish failed: -1, marked disconnected for reconnect   <- hello_app 那条
 *   [语音] user_said MQTT 回传失败(-107)（界面已由直调刷过，手机端看不到）: 帮我打开灯。
 *   [AI_NET ERR] MQTT not connected, cannot send device command
 *   ...
 *   MQTT connecting: broker.emqx.io:1883 -> MQTT connected
 *   Publish to zhi_ai/zhi_ai_001/heartbeat: {...}                <- network_task 自己发，成功
 *
 * 判据很干净：心跳（network_task 自己发）永远成功，凡是 hello_app 线程发起的
 * （voice_state / user_said / device_cmd）全部失败 —— 而此时网络本身是好的
 * （同一时刻 HTTPS / ASR 都成功，free 还有 5.4 MB 空闲内存）。
 *
 * 本函数**只做"拷进队列 + 唤醒 network_task"**，一个 socket 操作都不做，
 * 所以从任何任务组、任何优先级、任何上下文（非中断）调用都安全。
 * 入参超长会截断并打日志（不静默丢）；队列满返回负值并打节流日志。
 *
 * @param  topic   完整主题名，不能为空
 * @param  payload 载荷（UTF-8 文本）
 * @param  qos     0/1/2
 * @param  retain  是否保留消息
 * @return 0 已入队；-EINVAL topic 为空；-ENOSPC 队列满（8 槽）
 */
int mqtt_publish_queued(const char *topic, const char *payload, int qos, bool retain);

/* MQTT 连续连接失败次数（network_task 内部的计数）。
 * 原来是 network_task 里的局部变量，外面一个字节都看不到，于是"连了几次没连上、
 * 换没换过 broker"这种最要命的现场只能靠串口猜 —— 而串口线现在是丢的。
 * 提升成静态量 + 这个只读口，心跳和诊断报文才报得出来。 */
int mqtt_get_fails(void);

/* ==================== 数据上报函数 ==================== */
int report_device_status(const device_status_t *status);
int report_alarm(const char *alarm_type, const char *details);

/**
 * 和 report_alarm() 完全一样：同一 topic、同一 payload 结构、同一 QoS、
 * 同一套本地回调 + 手机推送，**只有发布方式不同**（排队交给 network_task 发）。
 * 供 hello_app 的线程调用 —— 它不在 network_task 的 task group 里，
 * 直发必然失败（详见 mqtt_publish_queued() 的说明）。
 * robot_ui 自己的线程（比如 network_task 收包回调里按下报警）继续用
 * report_alarm()，那条路不绕队列。
 */
int report_alarm_queued(const char *alarm_type, const char *details);
int report_heartbeat(void);

/* ==================== 诊断（串口丢了之后唯一的观测通道） ==================== */

/**
 * robot_ui 本地状态快照：心跳和 diag 报文里那几个"只有 main.c 看得见"的量。
 * 字段约定：1 = 是，0 = 否，-1 = 取不到（没注册提供者）。
 */
typedef struct {
    int ai_init;    /* robot_ui 的 AI 模块初始化完成（main.c 的 g_ai_initialized） */
    int recording;  /* robot_ui 自己正在录音 */
    int playing;    /* robot_ui 自己正在放音 */
    int panel;      /* 镜像面板（语音对话面板）是否开着 */
} local_state_t;

/**
 * 本地状态提供者：main.c 启动时注册，network_comm 在发心跳/诊断时报一次。
 *
 * ⚠️ 这个回调跑在 **network_task（MQTT 收包）线程**里，必须是"读几个全局量就
 *    返回"的便宜货：它一阻塞，MQTT 心跳、重连、收包就一起停摆 —— 那正是
 *    "连唯一的观测通道也没了"。不许碰 LVGL、不许等设备、不许做网络请求。
 */
typedef void (*local_state_provider_t)(local_state_t *out);

void network_set_local_state_provider(local_state_provider_t provider);

/**
 * 把当前诊断快照发到 zhi_ai/<client_id>/status。
 *
 * 载荷长这样（hello 那一块是 hello_app 自己报的，原样内嵌）：
 *   {"type":"diag","device_id":"zhi_ai_001","timestamp":...,"uptime":...,
 *    "robot":{"ai_init":1,"rec":0,"play":0,"panel":0,"tasks":27,
 *             "mqtt":true,"broker":"broker.emqx.io","mqtt_fails":0,
 *             "broker_switches":0},
 *    "hello":{"alive":true,...}}
 * hello 那一块（ai_companion_diag_snapshot 的返回值）按下面几种形态出现：
 *   "hello":{...}          正常：快照是对象，原样内嵌
 *   "hello":"..."          快照不是对象 / 被截断：当字符串贴出来（最长 300 字节，
 *                          截过就附一个 "hello_len" 说明原来的长度）
 *   "hello":"unavailable"  接口返回负值 = hello_app 没在跑（现场最缺的那条证据）
 *   "hello":"empty"        接口返回 0（答了但没内容）
 *   "hello":"too_long"     快照太长、这条报文装不下（同样附 "hello_len"）
 *
 * 发布约定：QoS0、不 retain —— 详见 network_comm.c 里的说明。
 * @return 0 已发出，<0 失败（MQTT 没连上 / 载荷装不进包缓冲）。
 */
int report_diag(void);

/* ==================== 远程控制函数 ==================== */
int send_command_response(const char *cmd_id, bool success, const char *message);

/* ==================== 回调注册函数 ==================== */
void network_set_mqtt_callback(mqtt_msg_callback_t callback);
void network_set_wifi_callback(wifi_status_callback_t callback);
void network_set_alarm_callback(alarm_callback_t callback);
void network_set_ai_command_callback(ai_command_callback_t callback);

/* ==================== 异常声音检测接口 ==================== */

/**
 * 上报异常声音检测结果
 * @param sound_type  声音类型: "scream", "fall", "knock", "help"
 * @param confidence  置信度 0-100
 * @return 0 成功, -1 失败
 */
int report_abnormal_sound(const char *sound_type, int confidence);

/**
 * 和 report_abnormal_sound() 完全一样（同 topic / 同 payload / 同 QoS），
 * 只是发布走排队口。供 hello_app 的线程调用（"没听清就报未确认"那条路）。
 * robot_ui 自己的线程继续用 report_abnormal_sound()。
 */
int report_abnormal_sound_queued(const char *sound_type, int confidence);

/* ==================== 主动关怀接口 ==================== */

/**
 * 发送定时提醒到手机端
 * @param title   提醒标题
 * @param content 提醒内容
 * @return 0 成功, -1 失败
 */
int send_proactive_reminder(const char *title, const char *content);

/**
 * 发送健康数据到云端
 * @param heart_rate  心率
 * @param blood_oxy   血氧
 * @return 0 成功, -1 失败
 */
int report_health_data(int heart_rate, int blood_oxy);

/* ==================== 设备联动接口 ==================== */

/**
 * 发送设备控制命令
 * @param device_id  目标设备 ID
 * @param command    命令内容 (JSON)
 * @return 0 成功, -1 失败
 */
int send_device_command(const char *device_id, const char *command);

/**
 * 和 send_device_command() 完全一样（同 topic、同 payload、同 QoS1），
 * 只是发布走排队口。hello_app 的灯控工具就是从这里发的
 * （ai_network_send_device_command -> 本函数）：真机日志里
 * "[AI_NET ERR] MQTT not connected, cannot send device command" 就是它，
 * 直接原因是前一条 hello_app 的 publish 把 connected 标成了 false。
 * robot_ui 自己的线程（收包回调里替用户开灯那条）继续用 send_device_command()。
 */
int send_device_command_queued(const char *device_id, const char *command);

/* ==================== 手机推送接口 ==================== */

/* 推送服务类型 */
typedef enum {
    PUSH_SERVICE_BARK,      // Bark (iOS)
    PUSH_SERVICE_PUSHPLUS,  // PushPlus (Android/iOS, 微信推送)
    PUSH_SERVICE_MAX
} push_service_t;

/* 推送配置 */
typedef struct {
    push_service_t service;     // 推送服务类型
    char push_key[128];         // 推送 key
    bool enabled;               // 推送开关
} push_config_t;

/**
 * 初始化推送服务
 * @param service  推送服务类型
 * @param key      推送 key；传 NULL/空串时自动从 /etc/assets/push_key.txt
 *                 读取，读不到再用 network_comm.c 里的编译期默认值
 * @return 0 成功, -1 失败
 */
int push_init(push_service_t service, const char *key);

/**
 * 发送推送通知（异步：仅投递到后台推送任务，不等 HTTP 结果）
 * @param title    通知标题
 * @param content  通知内容
 * @param group    分组 (可选，用于分类通知)
 * @return 0 已投递到后台任务, -1 未启用/构建请求失败
 *         真正的成功判据是后台任务拿到的 HTTP 状态码 200/201，见串口 [PUSH] 日志
 */
int push_send_notification(const char *title, const char *content, const char *group);

/**
 * 发送紧急报警推送
 * @param alarm_type  报警类型
 * @param details     详细信息
 * @return 0 成功, -1 失败
 */
int push_send_alarm(const char *alarm_type, const char *details);

/**
 * 发送健康提醒推送
 * @param title   提醒标题
 * @param content 提醒内容
 * @return 0 成功, -1 失败
 */
int push_send_health_reminder(const char *title, const char *content);

/**
 * 开关推送功能
 * @param enabled  true 开启, false 关闭
 */
void push_set_enabled(bool enabled);

/**
 * 检查推送功能是否开启
 */
bool push_is_enabled(void);

/* ==================== 网络后台任务 ==================== */
void network_task(void *arg);

#endif /* NETWORK_COMM_H */
