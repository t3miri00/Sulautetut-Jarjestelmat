#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <zephyr/timing/timing.h>

/* ---------- Config / devices ---------- */
#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

/* LED aliases */
#define LED_RED_NODE   DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
static const struct gpio_dt_spec red   = GPIO_DT_SPEC_GET(LED_RED_NODE, gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);

/* Buttons */
#define BUTTON_0 DT_ALIAS(sw0)   // Pause toggle
#define BUTTON_1 DT_ALIAS(sw1)   // Manual RED
#define BUTTON_2 DT_ALIAS(sw2)   // Manual YELLOW
#define BUTTON_3 DT_ALIAS(sw3)   // Manual GREEN
#define BUTTON_4 DT_ALIAS(sw4)   // Debug ON/OFF toggle

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

/* ---------- Helpers ---------- */
static void set_red(bool on)   { gpio_pin_set_dt(&red, on ? 1 : 0); }
static void set_green(bool on) { gpio_pin_set_dt(&green, on ? 1 : 0); }
static void set_yellow(bool on) { set_red(on); set_green(on); }

/* Timing init called once in main() */
 
/* ---------- Run-time flags ---------- */
volatile bool paused = false;         // pause state
volatile bool debug_enabled = false;  // debug ON/OFF toggled by Button4

/* ---------- FIFO / dispatcher infra ---------- */
struct fifo_item {
    void *fifo_reserved;
    char color;          // 'R','Y','G'
    uint32_t duration_ms;
};
K_FIFO_DEFINE(dispatcher_fifo);

/* Condition vars & mutexes for LED tasks */
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

/* ---------- Debug FIFO (messages only when debug_enabled) ---------- */
struct debug_msg {
    void *fifo_reserved;
    char text[128];
};
K_FIFO_DEFINE(debug_fifo);

/* Helper: push debug message into debug_fifo IF debug_enabled.
   We keep this non-blocking-ish (allocate; if allocation fails we drop the message). */
static void debug_log(const char *fmt, ...)
{
    if (!debug_enabled) {
        return; /* do nothing when debug disabled */
    }

    struct debug_msg *m = k_malloc(sizeof(*m));
    if (!m) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintk(m->text, sizeof(m->text), fmt, args);
    va_end(args);

    k_fifo_put(&debug_fifo, m);
}

/* ---------- Push color helper (prints push via debug_log) ---------- */
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
    debug_log("PUSH FIFO: %c, %u ms\n", it->color, it->duration_ms);
}

/* ---------- Button handlers ---------- */
/* Button0 toggles pause */
void button_0_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    paused = !paused;
    printk("Button0 pressed: pause status=%d\n", (int)paused);
}

/* Buttons 1-3 push manual colors */
void button_1_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    if (paused) push_color_to_fifo('R', 1000);
    else debug_log("Button1 pressed but pause active -> ignored\n");
}
void button_2_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    if (paused) push_color_to_fifo('Y', 1000);
    else debug_log("Button2 pressed but pause active -> ignored\n");
}
void button_3_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    if (paused) push_color_to_fifo('G', 1000);
    else debug_log("Button3 pressed but pause active -> ignored\n");
}

/* Button4 toggles debug mode ON/OFF*/
void button_4_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    debug_enabled = !debug_enabled;
    if (debug_enabled) {
        printk("DEBUG MODE: ON\n");
    } else {
        printk("DEBUG MODE: OFF\n");
        struct debug_msg *m;
        while ((m = k_fifo_get(&debug_fifo, K_NO_WAIT)) != NULL) {
            k_free(m);
        }
    }
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
        int ret = gpio_pin_configure_dt(buttons[i], GPIO_INPUT);
        if (ret) {
            printk("Failed to configure button %d pin\n", i);
            return -1;
        }
        ret = gpio_pin_interrupt_configure_dt(buttons[i], GPIO_INT_EDGE_TO_ACTIVE);
        if (ret) {
            printk("Failed to configure interrupt for button %d\n", i);
            return -1;
        }
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

    debug_log("UART task started\n");

    while (1) {
        unsigned char c;
        if (uart_poll_in(uart_dev, &c) == 0) {
            if (idx == 0) {
            }

            if (c == '\r' || c == '\n') {
                if (idx > 0) {
                    buf[idx] = '\0';
                    timing_t ustart = timing_counter_get();

                    char color = toupper((unsigned char)buf[0]);
                    uint32_t dur = 1000;
                    char *comma = strchr(buf, ',');
                    if (comma) {
                        dur = (uint32_t)strtoul(comma + 1, NULL, 10);
                    }
                    if (color == 'R' || color == 'Y' || color == 'G') {
                        push_color_to_fifo(color, dur);
                    } else {
                        debug_log("UART: unknown color '%c' ignored\n", color);
                    }

                    timing_t uend = timing_counter_get();
                    uint64_t ucyc = timing_cycles_get(&ustart, &uend);
                    uint64_t usec = timing_cycles_to_ns(ucyc) / 1000;
                    debug_log("UART sequence handling time: %llu us\n", usec);
                }
                idx = 0;
            } else {
                if (idx < (int)sizeof(buf) - 1) {
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
    debug_log("Dispatcher task started\n");

    while (1) {
        struct fifo_item *it = k_fifo_get(&dispatcher_fifo, K_FOREVER);
        if (!it) continue;

        timing_t seq_start = timing_counter_get();

        char c = it->color;
        uint32_t dur = it->duration_ms;
        debug_log("Dispatcher got: %c, %u ms\n", c, dur);

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

        /* measure end */
        timing_t seq_end = timing_counter_get();
        uint64_t seq_cyc = timing_cycles_get(&seq_start, &seq_end);
        uint64_t seq_usec = timing_cycles_to_ns(seq_cyc) / 1000;
        debug_log("Full sequence runtime: %llu us\n", seq_usec);

        k_free(it);
    }
}

/* ---------- LED tasks ---------- */
void red_task(void *p1, void *p2, void *p3)
{
    while (1) {
        k_mutex_lock(&red_mutex, K_FOREVER);
        while (!red_pending) {
            k_condvar_wait(&red_cond, &red_mutex, K_FOREVER);
        }
        red_pending = false;
        uint32_t dur = red_duration;
        k_mutex_unlock(&red_mutex);

        timing_t start = timing_counter_get();

        set_red(true);
        k_msleep(dur);
        set_red(false);

        timing_t end = timing_counter_get();
        uint64_t cyc = timing_cycles_get(&start, &end);
        uint64_t usec = timing_cycles_to_ns(cyc) / 1000;
        debug_log("RED task runtime: %llu us\n", usec);

        k_sem_give(&release_sem);
    }
}

void yellow_task(void *p1, void *p2, void *p3)
{
    while (1) {
        k_mutex_lock(&yellow_mutex, K_FOREVER);
        while (!yellow_pending) {
            k_condvar_wait(&yellow_cond, &yellow_mutex, K_FOREVER);
        }
        yellow_pending = false;
        uint32_t dur = yellow_duration;
        k_mutex_unlock(&yellow_mutex);

        timing_t start = timing_counter_get();

        set_yellow(true);
        k_msleep(dur);
        set_yellow(false);

        timing_t end = timing_counter_get();
        uint64_t cyc = timing_cycles_get(&start, &end);
        uint64_t usec = timing_cycles_to_ns(cyc) / 1000;
        debug_log("YELLOW task runtime: %llu us\n", usec);

        k_sem_give(&release_sem);
    }
}

void green_task(void *p1, void *p2, void *p3)
{
    while (1) {
        k_mutex_lock(&green_mutex, K_FOREVER);
        while (!green_pending) {
            k_condvar_wait(&green_cond, &green_mutex, K_FOREVER);
        }
        green_pending = false;
        uint32_t dur = green_duration;
        k_mutex_unlock(&green_mutex);

        timing_t start = timing_counter_get();

        set_green(true);
        k_msleep(dur);
        set_green(false);

        timing_t end = timing_counter_get();
        uint64_t cyc = timing_cycles_get(&start, &end);
        uint64_t usec = timing_cycles_to_ns(cyc) / 1000;
        debug_log("GREEN task runtime: %llu us\n", usec);

        k_sem_give(&release_sem);
    }
}

/* ---------- Debug task: prints queued debug messages (only when debug_enabled messages are queued) ---------- */
void debug_task(void *p1, void *p2, void *p3)
{
    printk("Debug task started (prints only when DEBUG MODE ON and messages queued)\n");
    while (1) {
        struct debug_msg *m = k_fifo_get(&debug_fifo, K_FOREVER);
        if (m) {
            printk("%s", m->text);
            k_free(m);
        }
    }
}

/* ---------- Threads ---------- */
K_THREAD_DEFINE(uart_tid, STACKSIZE, uart_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(dispatcher_tid, STACKSIZE, dispatcher_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(red_tid, STACKSIZE, red_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(yellow_tid, STACKSIZE, yellow_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(green_tid, STACKSIZE, green_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(debug_tid, STACKSIZE, debug_task, NULL, NULL, NULL, PRIORITY, 0, 0);

/* ---------- Main ---------- */
int main(void)
{
    /* initialize timing once */
    timing_init();
    timing_start();

    printk("Traffic light system starting\n");

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

    printk("System online. Use serial commands like: R,2000\\r Y,1000\\r G,1500\\r\n");
    printk("Toggle debug output with BUTTON4 (DEBUG MODE ON/OFF)\n");

    while (1) {
        k_sleep(K_SECONDS(60));
    }
    return 0;
}

/* Koodiin seuraavat toiminnot ja ominaisuudeet lisätty: 
+1p suoritus: Ajoitukset liikennevaloihin 
+1p suoritus: Debug-taski, joka menee päälle ja pois Button 4:stä
+1p suoritus: Lisäajoitustietoja */
