#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* UART device */
#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

/* LED aliases – säädä boardin mukaan */
#define LED_RED_NODE   DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
static const struct gpio_dt_spec red   = GPIO_DT_SPEC_GET(LED_RED_NODE, gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);

/* Buttons */
#define BUTTON_0 DT_ALIAS(sw0)   // Pause
#define BUTTON_1 DT_ALIAS(sw1)   // Manual RED
#define BUTTON_2 DT_ALIAS(sw2)   // Manual YELLOW
#define BUTTON_3 DT_ALIAS(sw3)   // Manual GREEN
#define BUTTON_4 DT_ALIAS(sw4)   // not used

static const struct gpio_dt_spec button_0 = GPIO_DT_SPEC_GET_OR(BUTTON_0, gpios, {0});
static const struct gpio_dt_spec button_1 = GPIO_DT_SPEC_GET_OR(BUTTON_1, gpios, {1});
static const struct gpio_dt_spec button_2 = GPIO_DT_SPEC_GET_OR(BUTTON_2, gpios, {2});
static const struct gpio_dt_spec button_3 = GPIO_DT_SPEC_GET_OR(BUTTON_3, gpios, {3});
static const struct gpio_dt_spec button_4 = GPIO_DT_SPEC_GET_OR(BUTTON_4, gpios, {4});

static struct gpio_callback button_0_cb_data;
static struct gpio_callback button_1_cb_data;
static struct gpio_callback button_2_cb_data;
static struct gpio_callback button_3_cb_data;
static struct gpio_callback button_4_cb_data;

/* Helpers */
static void set_red(bool on)   { gpio_pin_set_dt(&red, on ? 1 : 0); }
static void set_green(bool on) { gpio_pin_set_dt(&green, on ? 1 : 0); }
static void set_yellow(bool on) { set_red(on); set_green(on); }

/* Pausetoggle */
volatile bool paused = false;

/* ---------- FIFO / dispatcher infra ---------- */
struct fifo_item {
    void *fifo_reserved;
    char color;          // 'R','Y','G'
    uint32_t duration_ms;
};
K_FIFO_DEFINE(dispatcher_fifo);

/* Condition vars & mutexes */
K_MUTEX_DEFINE(red_mutex);
K_CONDVAR_DEFINE(red_cond);
static bool red_pending = false;
static uint32_t red_duration = 1000;

K_MUTEX_DEFINE(yellow_mutex);
K_CONDVAR_DEFINE(yellow_cond);
static bool yellow_pending = false;
static uint32_t yellow_duration = 1000;

K_MUTEX_DEFINE(green_mutex);
K_CONDVAR_DEFINE(green_cond);
static bool green_pending = false;
static uint32_t green_duration = 1000;

/* Dispatcher waits release */
K_SEM_DEFINE(release_sem, 0, 1);

/* ---------- Helper: push to FIFO ---------- */
static void push_color_to_fifo(char c, uint32_t duration_ms)
{
    struct fifo_item *it = k_malloc(sizeof(*it));
    if (!it) {
        printk("push_color_to_fifo: malloc failed\n");
        return;
    }
    it->color = (char)toupper((unsigned char)c);
    it->duration_ms = duration_ms;
    k_fifo_put(&dispatcher_fifo, it);
    printk("PUSH FIFO: %c, %u ms\n", it->color, it->duration_ms);
}

/* ---------- Button handlers ---------- */
void button_0_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    paused = !paused;
    printk("Button0 pressed: pause status=%d\n", (int)paused);
}

void button_1_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    if (paused) push_color_to_fifo('R', 1000);
    else printk("Button1 ignored (paused)\n");
}
void button_2_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    if (paused) push_color_to_fifo('Y', 1000);
    else printk("Button2 ignored (paused)\n");
}
void button_3_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    if (paused) push_color_to_fifo('G', 1000);
    else printk("Button3 ignored (paused)\n");
}
void button_4_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button4 pressed (unused)\n");
}

/* ---------- Buttons init ---------- */
static int init_buttons_and_callbacks(void)
{
    const struct gpio_dt_spec *buttons[] = { &button_0, &button_1, &button_2, &button_3, &button_4 };
    struct gpio_callback *cbs[] = { &button_0_cb_data, &button_1_cb_data, &button_2_cb_data, &button_3_cb_data, &button_4_cb_data };
    void (*handlers[])(const struct device *, struct gpio_callback *, uint32_t) = {
        button_0_handler, button_1_handler, button_2_handler, button_3_handler, button_4_handler
    };

    for (int i = 0; i < 5; i++) {
        if (!device_is_ready(buttons[i]->port)) {
            printk("Button %d port not ready\n", i);
            return -1;
        }
        gpio_pin_configure_dt(buttons[i], GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(buttons[i], GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(cbs[i], handlers[i], BIT(buttons[i]->pin));
        gpio_add_callback(buttons[i]->port, cbs[i]);
        printk("Button %d set ok\n", i);
    }
    return 0;
}

/* ---------- UART task ---------- */
#define STACKSIZE 1024
#define PRIORITY 5

void uart_task(void *p1, void *p2, void *p3)
{
    char buf[32];
    int idx = 0;
    printk("UART task started\n");

    while (1) {
        unsigned char c;
        if (uart_poll_in(uart_dev, &c) == 0) {
            if (c == '\r' || c == '\n') {
                buf[idx] = '\0';
                if (idx > 0) {
                    char color = toupper((unsigned char)buf[0]);
                    uint32_t dur = 1000;
                    char *comma = strchr(buf, ',');
                    if (comma) {
                        dur = strtoul(comma + 1, NULL, 10);
                    }
                    if (color == 'R' || color == 'Y' || color == 'G') {
                        push_color_to_fifo(color, dur);
                    }
                }
                idx = 0;
            } else {
                if (idx < sizeof(buf) - 1) {
                    buf[idx++] = (char)c;
                }
            }
        }
        k_msleep(10);
    }
}

/* ---------- Dispatcher ---------- */
void dispatcher_task(void *p1, void *p2, void *p3)
{
    printk("Dispatcher task started\n");
    while (1) {
        struct fifo_item *it = k_fifo_get(&dispatcher_fifo, K_FOREVER);
        if (!it) continue;
        char c = it->color;
        uint32_t dur = it->duration_ms;
        printk("Dispatcher got: %c, %u ms\n", c, dur);

        if (c == 'R') {
            k_mutex_lock(&red_mutex, K_FOREVER);
            red_pending = true;
            red_duration = dur;
            k_condvar_signal(&red_cond);
            k_mutex_unlock(&red_mutex);
        } else if (c == 'Y') {
            k_mutex_lock(&yellow_mutex, K_FOREVER);
            yellow_pending = true;
            yellow_duration = dur;
            k_condvar_signal(&yellow_cond);
            k_mutex_unlock(&yellow_mutex);
        } else if (c == 'G') {
            k_mutex_lock(&green_mutex, K_FOREVER);
            green_pending = true;
            green_duration = dur;
            k_condvar_signal(&green_cond);
            k_mutex_unlock(&green_mutex);
        }

        k_sem_take(&release_sem, K_FOREVER);
        printk("Dispatcher: release received\n");

        k_free(it);
    }
}

/* ---------- LED tasks ---------- */
void red_task(void *p1, void *p2, void *p3)
{
    while (1) {
        k_mutex_lock(&red_mutex, K_FOREVER);
        while (!red_pending)
            k_condvar_wait(&red_cond, &red_mutex, K_FOREVER);
        red_pending = false;
        uint32_t dur = red_duration;
        k_mutex_unlock(&red_mutex);

        set_red(true);
        printk("RED ON (%u ms)\n", dur);
        k_msleep(dur);
        set_red(false);
        printk("RED OFF\n");

        k_sem_give(&release_sem);
    }
}

void yellow_task(void *p1, void *p2, void *p3)
{
    while (1) {
        k_mutex_lock(&yellow_mutex, K_FOREVER);
        while (!yellow_pending)
            k_condvar_wait(&yellow_cond, &yellow_mutex, K_FOREVER);
        yellow_pending = false;
        uint32_t dur = yellow_duration;
        k_mutex_unlock(&yellow_mutex);

        set_yellow(true);
        printk("YELLOW ON (%u ms)\n", dur);
        k_msleep(dur);
        set_yellow(false);
        printk("YELLOW OFF\n");

        k_sem_give(&release_sem);
    }
}

void green_task(void *p1, void *p2, void *p3)
{
    while (1) {
        k_mutex_lock(&green_mutex, K_FOREVER);
        while (!green_pending)
            k_condvar_wait(&green_cond, &green_mutex, K_FOREVER);
        green_pending = false;
        uint32_t dur = green_duration;
        k_mutex_unlock(&green_mutex);

        set_green(true);
        printk("GREEN ON (%u ms)\n", dur);
        k_msleep(dur);
        set_green(false);
        printk("GREEN OFF\n");

        k_sem_give(&release_sem);
    }
}

/* ---------- Threads ---------- */
K_THREAD_DEFINE(uart_tid, STACKSIZE, uart_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(dispatcher_tid, STACKSIZE, dispatcher_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(red_tid, STACKSIZE, red_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(yellow_tid, STACKSIZE, yellow_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(green_tid, STACKSIZE, green_task, NULL, NULL, NULL, PRIORITY, 0, 0);

/* ---------- Main ---------- */
int main(void)
{
    printk("Traffic light started\n");

    if (!device_is_ready(uart_dev)) {
        printk("UART not ready\n");
        return -1;
    }
    if (!device_is_ready(red.port) || !device_is_ready(green.port)) {
        printk("LEDs not ready\n");
        return -1;
    }
    gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);

    if (init_buttons_and_callbacks() != 0) {
        printk("Button init failed\n");
        return -1;
    }

    printk("System online.\n To use - Send SerialTerminal-commands: R,2000\\Y,500\\G,1500\\r,y,g\n");
    while (1) {
        k_sleep(K_SECONDS(60));
    }
    return 0;
}

/* Koodiin seuraavat toiminnot ja ominaisuudeet lisätty:
+1p suoritus: Sekvenssin vastaanotto sarjaportista 
+1p suoritus: Refaktorointi 
+1p suoritus: Sekvenssin ajastus
Bonus: Ledien valot korjattu näyttämään halutut värit, vihreä, keltainen, punainen.*/