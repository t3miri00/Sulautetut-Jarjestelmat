#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <inttypes.h>

// ------------------- BUTTON CONFIG -------------------
#define BUTTON_0 DT_ALIAS(sw0)
#define BUTTON_1 DT_ALIAS(sw1)

static const struct gpio_dt_spec button_0 = GPIO_DT_SPEC_GET_OR(BUTTON_0, gpios, {0});
static struct gpio_callback button_0_data;

static const struct gpio_dt_spec button_1 = GPIO_DT_SPEC_GET_OR(BUTTON_1, gpios, {1});
static struct gpio_callback button_1_data;

// ------------------- LED CONFIG -------------------
static const struct gpio_dt_spec red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
// static const struct gpio_dt_spec blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

// ------------------- THREAD CONFIG -------------------
#define STACKSIZE 500
#define PRIORITY 5
void red_led_task(void *, void *, void*);
void green_led_task(void *, void *, void*);
void yellow_led_task(void *, void *, void*);
K_THREAD_DEFINE(red_thread, STACKSIZE, red_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(green_thread, STACKSIZE, green_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(yellow_thread, STACKSIZE, yellow_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);

// ------------------- GLOBAL STATE -------------------
int led_state = 1;     // 1=punainen, 2=keltainen, 3=vihreä, 4=pause
int prev_state = 1;    // talletetaan edellinen tila pausea varten

// ------------------- BUTTON HANDLERS -------------------
void button_0_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button 0 pressed\n");

    if (led_state == 4) {
        // Resume
        led_state = prev_state;
        printk("Resume from pause, back to state %d\n", led_state);
    } else {
        // Pause
        prev_state = led_state;
        led_state = 4;
        printk("Pause activated\n");
    }
}

void button_1_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button 1 pressed\n");
    
}

// ------------------- INIT BUTTONS -------------------
int init_button() {
    int ret;

    if (!gpio_is_ready_dt(&button_0)) {
        printk("Error: button 0 is not ready\n");
        return -1;
    }
    ret = gpio_pin_configure_dt(&button_0, GPIO_INPUT);
    if (ret != 0) return -1;
    ret = gpio_pin_interrupt_configure_dt(&button_0, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0) return -1;
    gpio_init_callback(&button_0_data, button_0_handler, BIT(button_0.pin));
    gpio_add_callback(button_0.port, &button_0_data);
    printk("Set up button 0 ok\n");

    if (!gpio_is_ready_dt(&button_1)) {
        printk("Error: button 1 is not ready\n");
        return -1;
    }
    ret = gpio_pin_configure_dt(&button_1, GPIO_INPUT);
    if (ret != 0) return -1;
    ret = gpio_pin_interrupt_configure_dt(&button_1, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0) return -1;
    gpio_init_callback(&button_1_data, button_1_handler, BIT(button_1.pin));
    gpio_add_callback(button_1.port, &button_1_data);
    printk("Set up button 1 ok\n");

    return 0;
}

// ------------------- INIT LEDS -------------------
int init_led() {
    int pun = gpio_pin_configure_dt(&red, GPIO_OUTPUT_ACTIVE);
    int vih = gpio_pin_configure_dt(&green, GPIO_OUTPUT_ACTIVE);

    if (pun < 0 || vih < 0) {
        printk("Error: Led configure failed\n");
        return -1;
    }

    gpio_pin_set_dt(&red, 0);
    gpio_pin_set_dt(&green, 0);

    printk("Leds initialized ok\n");
    return 0;
}

// ------------------- TASKS -------------------
void red_led_task(void *, void *, void*) {
    printk("Red led thread started\n");
    while (true) {
        if (led_state == 1) {
            gpio_pin_set_dt(&red, 1);
            printk("Red on\n");
            k_sleep(K_SECONDS(1));
            gpio_pin_set_dt(&red, 0);
            printk("Red off\n");
            k_sleep(K_SECONDS(1));

            if (led_state != 4) { // ei vaihdeta tilaa pausella
                led_state = 2;
            }
        }
        k_msleep(100);
    }
}

void yellow_led_task(void *, void *, void*) {
    printk("Yellow led thread started\n");
    while (true) {
        if (led_state == 2) {
            gpio_pin_set_dt(&red, 1);
            gpio_pin_set_dt(&green, 1);
            printk("Yellow on\n");
            k_sleep(K_SECONDS(1));
            gpio_pin_set_dt(&red, 0);
            gpio_pin_set_dt(&green, 0);
            printk("Yellow off\n");
            k_sleep(K_SECONDS(1));

            if (led_state != 4) {
                led_state = 3;
            }
        }
        k_msleep(100);
    }
}

void green_led_task(void *, void *, void*) {
    printk("Green led thread started\n");
    while (true) {
        if (led_state == 3) {
            gpio_pin_set_dt(&green, 1);
            printk("Green on\n");
            k_sleep(K_SECONDS(1));
            gpio_pin_set_dt(&green, 0);
            printk("Green off\n");
            k_sleep(K_SECONDS(1));

            if (led_state != 4) {
                led_state = 1;
            }
        }
        k_msleep(100);
    }
}

// ------------------- MAIN -------------------
int main(void)
{
    init_led();

    int ret = init_button();
    if (ret < 0) {
        return 0;
    }

    while (1) {
        k_msleep(10);
    }
    return 0;
}