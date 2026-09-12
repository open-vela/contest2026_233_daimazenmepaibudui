/****************************************************************************
 * 网络配置模板 - 请根据实际环境修改
 *
 * 本文件定义智爱陪伴应用的网络配置参数
 * 在部署到实际硬件时，请修改以下配置项
 ****************************************************************************/

#ifndef __AI_NETWORK_CONFIG_H
#define __AI_NETWORK_CONFIG_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ==================== WiFi 配置 ==================== */

/* WiFi 名称 (SSID) - 请修改为实际的 WiFi 名称 */
#define AI_DEFAULT_WIFI_SSID      "YourWiFiSSID"

/* WiFi 密码 - 请修改为实际的 WiFi 密码 */
#define AI_DEFAULT_WIFI_PASSWORD  "YourWiFiPassword"

/* ==================== MQTT 配置 ==================== */

/* MQTT 服务器地址 */
#define AI_DEFAULT_MQTT_BROKER    "broker.emqx.io"

/* MQTT 端口号 */
#define AI_DEFAULT_MQTT_PORT      1883

/* MQTT 客户端 ID (需要唯一) */
#define AI_DEFAULT_MQTT_CLIENT_ID "zhi_ai_001"

/* MQTT 用户名 (可选) */
#define AI_DEFAULT_MQTT_USERNAME  ""

/* MQTT 密码 (可选) */
#define AI_DEFAULT_MQTT_PASSWORD  ""

/* ==================== 推送配置 ==================== */

/* PushPlus 推送 Key (微信推送, 从 https://www.pushplus.plus 获取) */
#define AI_DEFAULT_PUSH_KEY       ""

/* Bark 推送 Key (iOS 推送, 从 Bark App 获取) */
#define AI_DEFAULT_BARK_KEY       ""

/* ==================== 设备配置 ==================== */

/* 设备 ID (用于 MQTT 客户端 ID 和设备标识) */
#define AI_DEFAULT_DEVICE_ID      "SF32LB52_001"

/* 设备名称 (用于显示) */
#define AI_DEFAULT_DEVICE_NAME    "智爱陪伴-001"

/* ==================== 服务器配置 ==================== */

/* LLM API 服务器 (用于 AI 对话) */
#define AI_DEFAULT_LLM_SERVER     "http://api.example.com"

/* LLM API Key */
#define AI_DEFAULT_LLM_API_KEY    ""

/* ==================== 功能开关 ==================== */

/* 使能 WiFi 自动重连 */
#define AI_ENABLE_AUTO_RECONNECT  1

/* 使能心跳包 */
#define AI_ENABLE_HEARTBEAT       1

/* 心跳间隔 (毫秒) */
#define AI_HEARTBEAT_INTERVAL     30000

/* 使能推送通知 */
#define AI_ENABLE_PUSH            0

/* ==================== 调试配置 ==================== */

/* 使能网络调试日志 */
#define AI_DEBUG_NETWORK          1

/* 使能 MQTT 调试日志 */
#define AI_DEBUG_MQTT             1

#endif /* __AI_NETWORK_CONFIG_H */
