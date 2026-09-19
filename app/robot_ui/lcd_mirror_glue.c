/**
 * lcd_mirror_glue.c - 屏幕镜像的 LVGL 侧（细节见 lcd_mirror_glue.h 文件头）
 *
 * 一句话：把 LVGL 每次 flush 的那块脏矩形拷进板级模块的影子帧缓冲，
 * 其余（socket、协议、任务、nsh）全在 board/contest_board/src/lcd_mirror.c。
 *
 * 这里**不碰 flush_cb 本身**（它在 openvela 的 lv_nuttx 里），只用 LVGL 9.1
 * 前后各发一次的那两个事件 —— 和 ui_perf.c 同一套路，抗上游改动。
 *
 * 对刷屏路径的额外开销 = 每块脏区一次 memcpy，几十微秒量级（真机 flush
 * 本身是毫秒级）。不做任何等待、不碰 socket。
 */

#include "lcd_mirror_glue.h"

#include <nuttx/config.h>
#include <stdio.h>

#include "lcd_mirror.h"

/* 开机头几秒强制整屏重绘几次：界面是在 lv_nuttx_init() 之后才一个个建起来的，
 * 我们挂钩子时屏幕上还是空的。500 ms 一次、前 3 秒，足够把所有控件覆盖到。 */
#define LCD_MIRROR_SEED_FIRES   6
#define LCD_MIRROR_SEED_PERIOD  500

static lv_display_t *g_mirror_disp;
static lv_timer_t   *g_mirror_timer;
static lv_indev_t   *g_virtual_indev;
static int           g_seed_left;

/****************************************************************************
 * Name: mirror_on_flush_start
 *
 * Description:
 *   LVGL 把一块脏区交给 flush_cb **之前**。
 *   事件参数是 lv_area_t*（已经加过显示偏移，就是驱动真正要写的那块），
 *   像素在 `lv_display_get_buf_active()` 里 —— 此刻 buf_act 还是刚渲染完的
 *   那一块，双缓冲的交换要等 flush_cb 返回之后才做（lv_refr.c）。
 *
 ****************************************************************************/

static void mirror_on_flush_start(lv_event_t *e)
{
    const lv_area_t *area = (const lv_area_t *)lv_event_get_param(e);
    lv_draw_buf_t   *buf;
    int32_t          w;
    int32_t          h;

    if (area == NULL)
    {
        return;
    }

    buf = lv_display_get_buf_active(g_mirror_disp);
    if (buf == NULL || buf->data == NULL)
    {
        return;
    }

    w = lv_area_get_width(area);
    h = lv_area_get_height(area);

    if (w <= 0 || h <= 0 || area->x1 < 0 || area->y1 < 0)
    {
        return;
    }

    /* 防身：draw buffer 的行距必须装得下这一行的像素，而且整块得在 data_size
     * 之内。对不上说明布局跟我们的假设不同（例如以后开了旋转），宁可漏这一帧，
     * 也不能读越界。 */
    if (buf->header.stride < (uint32_t)w * 2)
    {
        return;
    }

    if ((uint32_t)h * buf->header.stride > buf->data_size)
    {
        return;
    }

    lcd_mirror_capture((uint16_t)area->x1, (uint16_t)area->y1,
                       (uint16_t)w, (uint16_t)h,
                       (const uint8_t *)buf->data, buf->header.stride);
}

/****************************************************************************
 * Name: mirror_timer_cb
 *
 * Description:
 *   在 **LVGL 线程里**跑：把"请整屏重绘一次"的请求变成一次 invalidate。
 *   开机头几下无条件做（把镜像的影子缓冲填满），之后就只响应请求。
 *   请求来自镜像任务（新客户端连上、重新 start 等）。
 *
 ****************************************************************************/

static void mirror_timer_cb(lv_timer_t *t)
{
    bool want = false;

    (void)t;

    if (g_seed_left > 0)
    {
        g_seed_left--;
        want = true;
    }

    if (lcd_mirror_take_redraw_request())
    {
        want = true;
    }

    if (want)
    {
        lv_obj_t *scr = lv_screen_active();

        /* lv_nuttx_init() 之后一般已经有默认屏了，但 timer 第一下可能落在
         * 界面还没建起来的时候，NULL 会踩断言，这里挡一下。 */
        if (scr != NULL)
        {
            lv_obj_invalidate(scr);
        }
    }
}

/****************************************************************************
 * Name: mirror_indev_read_cb
 *
 * Description:
 *   虚拟输入设备的读取回调 —— **在 LVGL 线程里被 lv_indev 的 read timer
 *   按周期调用**（本函数把周期提到 10ms，和真触摸那个一致）。
 *
 *   拿的是"当前状态"（板级 lcd_mirror_get_touch），不是"取走一个事件"：
 *   LVGL 每个周期都要问"现在按着没有、在哪"，所以这个读是幂等的。
 *   板级那边会把"按下"锁存一次（快速点一下不丢），并且镜像停/断开时
 *   一律强制抬起 —— 所以这里**永远不可能填出"按着不放"**。
 *
 *   镜像没在跑、或者还没收到过任何触摸消息时，一律报"抬起"：
 *   没有这一条，界面会卡在最后一次按下状态。
 *
 ****************************************************************************/

static void mirror_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0;
    uint16_t y = 0;
    bool     pressed = false;

    (void)indev;

    if (lcd_mirror_get_touch(&x, &y, &pressed))
    {
        /* 面板坐标直接就是 LVGL 坐标：本板没有旋转、没有显示偏移
         * （disp->offset_x/y 都是 0，见 lv_nuttx_lcd.c）。 */
        data->point.x = (int32_t)x;
        data->point.y = (int32_t)y;
        data->state   = pressed ? LV_INDEV_STATE_PRESSED
                                : LV_INDEV_STATE_RELEASED;
    }
    else
    {
        data->point.x = 0;
        data->point.y = 0;
        data->state   = LV_INDEV_STATE_RELEASED;
    }
}

void lcd_mirror_attach_lvgl_display(lv_display_t *disp)
{
    int ret;

    if (disp == NULL)
    {
        printf("[lcdmirror] 没挂上：disp 是 NULL\n");
        return;
    }

    g_mirror_disp = disp;
    g_seed_left   = LCD_MIRROR_SEED_FIRES;

    lv_display_add_event_cb(disp, mirror_on_flush_start, LV_EVENT_FLUSH_START,
                            NULL);

    /* 开机自动起（LCD_MIRROR_AUTOSTART=0 时这里只是返回 OK，什么都不做，
     * 改成 `hw_test lcdmirror start` 手动开）。 */
    ret = lcd_mirror_autostart();
    if (ret < 0)
    {
        printf("[lcdmirror] 启动失败: %d（稍后可以 `hw_test lcdmirror start`）\n",
               ret);
    }

    if (g_mirror_timer == NULL)
    {
        g_mirror_timer = lv_timer_create(mirror_timer_cb,
                                         LCD_MIRROR_SEED_PERIOD, NULL);
    }

    /* ===== 虚拟输入设备：PC 鼠标当触摸 =====
     * 本机触摸 IC 已经不应答（/dev/input0 都没了），且**不能再依赖它**：
     * 这里自己造一个 pointer 设备，数据源是镜像反向通道。
     * 参数按 vendored LVGL 9.x 的实际签名写（lv_indev.h:90/149/156/179）：
     *   lv_indev_t *lv_indev_create(void);
     *   void lv_indev_set_type(lv_indev_t *, lv_indev_type_t);
     *   void lv_indev_set_read_cb(lv_indev_t *, lv_indev_read_cb_t);
     *       read_cb = void (*)(lv_indev_t *indev, lv_indev_data_t *data)
     *   data->point.x / data->point.y / data->state 就是要在 read_cb 里填的。 */
    if (g_virtual_indev == NULL)
    {
        g_virtual_indev = lv_indev_create();
        if (g_virtual_indev == NULL)
        {
            printf("[lcdmirror] 虚拟输入设备建不出来，鼠标不能操作界面\n");
        }
        else
        {
            lv_indev_set_type(g_virtual_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(g_virtual_indev, mirror_indev_read_cb);
            lv_indev_set_display(g_virtual_indev, disp);

            /* 和真触摸用同一个读取周期（robot_ui/main.c 把真触摸那个也提到
             * 10ms），拖动手感才对得上。 */
            if (lv_indev_get_read_timer(g_virtual_indev) != NULL)
            {
                lv_timer_set_period(lv_indev_get_read_timer(g_virtual_indev), 10);
            }
        }
    }

    printf("[lcdmirror] 已挂上 %dx%d RGB565 flush 钩子 + 鼠标虚拟触摸，"
           "目标 %s:%d\n",
           LCD_MIRROR_PANEL_W, LCD_MIRROR_PANEL_H,
           lcd_mirror_target_ip(), (int)lcd_mirror_target_port());
    printf("[lcdmirror] PC 端：py -3.10 D:/apply/claw/_flash/lcd_mirror.py"
           "（在画布上按住鼠标 = 点屏幕）\n");
}
