#include "hal_data.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define TICK_MS          (10U)       /* 主循环节拍 */
#define RX_RING_SIZE     (1024U)     /* 接收环形缓冲长度 */
#define RX_LINE_MAX      (512U)      /* 单行最大长度（含字符串结束符） */

fsp_err_t err = FSP_SUCCESS;

unsigned char usb_uart_send_buff[100];
volatile bool usb_uart_send_complete_flag = false;

/* 接收环形缓冲: 中断写入 rx_wr_index, 主循环读取 rx_rd_index */
static volatile char rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_wr_index = 0;
static volatile uint16_t rx_rd_index = 0;
static volatile bool rx_ring_overflowed = false;

static char rx_line[RX_LINE_MAX];
static uint16_t rx_line_len = 0;
static bool rx_line_overflow = false;
static bool rx_ring_overflow_reported = false;

void usb_uart_cb (uart_callback_args_t * p_args)
{
    switch (p_args->event)
    {
        case UART_EVENT_TX_COMPLETE:
        {
            usb_uart_send_complete_flag = true;
            break;
        }
        case UART_EVENT_RX_CHAR:
        {
            uint16_t next = (uint16_t)((rx_wr_index + 1U) % RX_RING_SIZE);
            if (next != rx_rd_index)            /* 缓冲满则丢弃 */
            {
                rx_ring[rx_wr_index] = (char)p_args->data;
                rx_wr_index = next;
            }
            else
            {
                rx_ring_overflowed = true;
            }
            break;
        }
        default:
        {
            break;
        }
    }
}


unsigned char wifi_uart_send_buff[100];
volatile bool wifi_uart_send_complete_flag = false;

/* WIFI 接收环形缓冲: 中断写入 wifi_rx_wr_index, 主循环读取 wifi_rx_rd_index */
static volatile char wifi_rx_ring[RX_RING_SIZE];
static volatile uint16_t wifi_rx_wr_index = 0;
static volatile uint16_t wifi_rx_rd_index = 0;
static volatile bool wifi_rx_ring_overflowed = false;

static char wifi_rx_line[RX_LINE_MAX];
static uint16_t wifi_rx_line_len = 0;
static bool wifi_rx_ring_overflow_reported = false;


/* DA16200 WIFI 口回调: 发送完成标志 + 收到字符入环形缓冲 */
void wifi_uart_cb (uart_callback_args_t * p_args)
{
    switch (p_args->event)
    {
        case UART_EVENT_TX_COMPLETE:
        {
            wifi_uart_send_complete_flag = true;
            break;
        }
        case UART_EVENT_RX_CHAR:
        {
            uint16_t next = (uint16_t)((wifi_rx_wr_index + 1U) % RX_RING_SIZE);
            if (next != wifi_rx_rd_index)            /* 缓冲满则丢弃 */
            {
                wifi_rx_ring[wifi_rx_wr_index] = (char)p_args->data;
                wifi_rx_wr_index = next;
            }
            else
            {
                wifi_rx_ring_overflowed = true;
            }
            break;
        }
        default:
        {
            break;
        }
    }
}

#ifdef __GNUC__                                 //串口重定向
    #define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
    #define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif
PUTCHAR_PROTOTYPE
{
        err = R_SCI_UART_Write(&g_usb_uart_ctrl, (uint8_t *)&ch, 1);
        if(FSP_SUCCESS != err) __BKPT();
        while(usb_uart_send_complete_flag == false){}
        usb_uart_send_complete_flag = false;
        return ch;
}

int _write(int fd,char *pBuffer,int size)
{
    for(int i=0;i<size;i++)
    {
        __io_putchar(*pBuffer++);
    }
    return size;
}

int _read(int fd, char *pBuffer, int size)
{
    (void)fd;
    (void)pBuffer;
    (void)size;
    return 0;
}

int _close(int fd)
{
    (void)fd;
    return -1;
}

int _lseek(int fd, int offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    return 0;
}

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd)
{
    (void)fd;
    return 1;
}

/* 通过 WIFI 串口发送一行数据, 没有以 \r\n 结尾则自动补上 */
static void wifi_line_send (const char * p_buf, uint16_t len)
{
    static char tx_buf[RX_LINE_MAX + 2];
    uint16_t tx_len = 0U;

    if (len >= RX_LINE_MAX)
    {
        len = RX_LINE_MAX - 1U;
    }

    memcpy(tx_buf, p_buf, len);
    tx_len = len;

    if ((0U == tx_len) || (('\r' != tx_buf[tx_len - 1U]) && ('\n' != tx_buf[tx_len - 1U])))
    {
        tx_buf[tx_len++] = '\r';            /* 没有换行则自动补 \r\n */
        tx_buf[tx_len++] = '\n';
    }

    err = R_SCI_UART_Write(&g_wifi_uart_ctrl, (uint8_t *)tx_buf, tx_len);
    if (FSP_SUCCESS == err)
    {
        while (wifi_uart_send_complete_flag == false){}
        wifi_uart_send_complete_flag = false;
    }
    else
    {
        printf("[ERR] wifi write, err=%d\r\n", (int)err);
    }
}

/* USB 输入的一行: 回显到 USB 并转发给 WIFI 串口 */
static void rx_line_flush (void)
{
    if (rx_line_len > 0U)
    {
        rx_line[rx_line_len] = '\0';
        printf("[TX] %s\r\n", rx_line);     /* 输入命令回显到 USB */
        wifi_line_send(rx_line, rx_line_len);   /* 转发给 WIFI 模块 */
        rx_line_len = 0U;
    }
}

/* 取出 USB 串口收到的数据, 收到换行符作为一行结束并转发给 WIFI 串口 */
static void usb_uart_rx_process (void)
{
    while (rx_rd_index != rx_wr_index)
    {
        char c = rx_ring[rx_rd_index];
        rx_rd_index = (uint16_t)((rx_rd_index + 1U) % RX_RING_SIZE);

        if ((c == '\r') || (c == '\n'))
        {
            if (rx_line_overflow)
            {
                rx_line_len = 0U;
                rx_line_overflow = false;
                printf("[ERR] USB command too long, discarded\r\n");
            }
            else
            {
                rx_line_flush();
            }
        }
        else if (!rx_line_overflow && (rx_line_len < (RX_LINE_MAX - 1U)))
        {
            rx_line[rx_line_len++] = c;
        }
        else if (!rx_line_overflow)
        {
            rx_line_len = 0U;
            rx_line_overflow = true;
        }
    }
}
static void wifi_rx_line_flush (void)
{
    if (wifi_rx_line_len > 0U)
    {
        wifi_rx_line[wifi_rx_line_len] = '\0';
        printf("[WIFI] %s\r\n", wifi_rx_line);
        wifi_rx_line_len = 0U;
    }
}


/* 取出串口收到的数据, 收到换行符作为一行结束并打印 */
static void wifi_uart_rx_process (void)
{
    while (wifi_rx_rd_index != wifi_rx_wr_index)
    {
        char c = wifi_rx_ring[wifi_rx_rd_index];
        wifi_rx_rd_index = (uint16_t)((wifi_rx_rd_index + 1U) % RX_RING_SIZE);

        if ((c == '\r') || (c == '\n'))     /* 换行符作为结束标志 */
        {
            wifi_rx_line_flush();                /* 输出整行(\r\n 只输出一次) */
        }
        else if (wifi_rx_line_len < (RX_LINE_MAX - 1U))
        {
            wifi_rx_line[wifi_rx_line_len++] = c;
        }
        else                                /* 缓冲满, 先输出再存, 防止溢出 */
        {
            wifi_rx_line_flush();
            wifi_rx_line[wifi_rx_line_len++] = c;
        }
    }
}




void hal_entry(void)
{
    /* Open the uart instances with initial configuration. */
    err = R_SCI_UART_Open(&g_usb_uart_ctrl, &g_usb_uart_cfg);
    assert(FSP_SUCCESS == err);

    err = R_SCI_UART_Open(&g_wifi_uart_ctrl, &g_wifi_uart_cfg);
    assert(FSP_SUCCESS == err);

    printf("\r\nRA E2STUDIO da16200_at_bridge\r\n");
    printf("USB input -> WIFI uart (auto CRLF), WIFI response back to USB\r\n");

    while (1)
    {
        R_BSP_SoftwareDelay(TICK_MS, BSP_DELAY_UNITS_MILLISECONDS);

        if (rx_ring_overflowed && !rx_ring_overflow_reported)
        {
            rx_ring_overflow_reported = true;
            printf("[ERR] USB RX ring overflow, input bytes dropped\r\n");
        }
        if (wifi_rx_ring_overflowed && !wifi_rx_ring_overflow_reported)
        {
            wifi_rx_ring_overflow_reported = true;
            printf("[ERR] WIFI RX ring overflow, response bytes dropped\r\n");
        }

        usb_uart_rx_process();               /* USB 输入转发给 WIFI 串口 */
        wifi_uart_rx_process();              /* WIFI 返回打印到 USB */
    }
#if BSP_TZ_SECURE_BUILD
    /* Enter non-secure code */
    R_BSP_NonSecureEnter();
#endif
}
