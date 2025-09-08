#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#define LED_RED_NODE   DT_ALIAS(led0)
#define LED_YELLOW_NODE DT_ALIAS(led1)  // toteutetaan kahdella LEDillä: red+green
#define LED_GREEN_NODE DT_ALIAS(led2)

static const struct gpio_dt_spec red   = GPIO_DT_SPEC_GET(LED_RED_NODE, gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);

// ---------------------------------------------------
// Button aliases
// ---------------------------------------------------
#define BUTTON_0 DT_ALIAS(sw0)   // Pause
#define BUTTON_1 DT_ALIAS(sw1)   // Manual RED
#define BUTTON_2 DT_ALIAS(sw2)   // Manual YELLOW
#define BUTTON_3 DT_ALIAS(sw3)   // Manual GREEN
#define BUTTON_4 DT_ALIAS(sw4)   // Yellow blink mode

static const struct gpio_dt_spec button_0 = GPIO_DT_SPEC_GET(BUTTON_0, gpios);
static const struct gpio_dt_spec button_1 = GPIO_DT_SPEC_GET(BUTTON_1, gpios);
static const struct gpio_dt_spec button_2 = GPIO_DT_SPEC_GET(BUTTON_2, gpios);
static const struct gpio_dt_spec button_3 = GPIO_DT_SPEC_GET(BUTTON_3, gpios);
static const struct gpio_dt_spec button_4 = GPIO_DT_SPEC_GET(BUTTON_4, gpios);

static struct gpio_callback button_0_cb_data;
static struct gpio_callback button_1_cb_data;
static struct gpio_callback button_2_cb_data;
static struct gpio_callback button_3_cb_data;
static struct gpio_callback button_4_cb_data;

// ---------------------------------------------------
// State variables
// ---------------------------------------------------
volatile int led_state = 1;          // 1=red, 2=yellow, 3=green, 4=pause, 5=yellow blink
volatile bool paused = false;

bool red_on_manual = false;
bool yellow_on_manual = false;
bool green_on_manual = false;

bool yellow_blink_mode = false;
int saved_state_for_yellow_blink = 1;

// ---------------------------------------------------
// Helpers
// ---------------------------------------------------
void set_red(bool on)   { gpio_pin_set_dt(&red, on); }
void set_green(bool on) { gpio_pin_set_dt(&green, on); }

void set_yellow(bool on) {
    gpio_pin_set_dt(&red, on);
    gpio_pin_set_dt(&green, on);
}

// ---------------------------------------------------
// Button handlers
// ---------------------------------------------------
void button_0_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button 0 (pause) pressed\n");
    if (paused) {
        paused = false;
        led_state = 1;
        printk("Resume sequence\n");
    } else {
        paused = true;
        led_state = 4;
        set_red(0); set_green(0);
        printk("Pause sequence\n");
    }
}

void button_1_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button 1 (manual RED) pressed\n");
    if (led_state == 4) {
        red_on_manual = !red_on_manual;
        set_red(red_on_manual);
        printk("Manual red %s\n", red_on_manual ? "ON" : "OFF");
    }
}

void button_2_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button 2 (manual YELLOW) pressed\n");
    if (led_state == 4) {
        yellow_on_manual = !yellow_on_manual;
        set_yellow(yellow_on_manual);
        printk("Manual yellow %s\n", yellow_on_manual ? "ON" : "OFF");
    }
}

void button_3_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button 3 (manual GREEN) pressed\n");
    if (led_state == 4) {
        green_on_manual = !green_on_manual;
        set_green(green_on_manual);
        printk("Manual green %s\n", green_on_manual ? "ON" : "OFF");
    }
}

void button_4_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    printk("Button 4 (yellow blink mode) pressed\n");

    if (led_state == 5) {
        led_state = saved_state_for_yellow_blink;
        yellow_blink_mode = false;
        printk("Exit yellow blink mode, back to state %d\n", led_state);
    } else {
        saved_state_for_yellow_blink = led_state;
        led_state = 5;
        yellow_blink_mode = true;
        printk("Yellow blink mode activated\n");
    }
}

// ---------------------------------------------------
// Init functions
// ---------------------------------------------------
static int init_led(const struct gpio_dt_spec *led)
{
    if (!device_is_ready(led->port)) {
        printk("LED device not ready\n");
        return -1;
    }
    gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
    return 0;
}

static int init_button(const struct gpio_dt_spec *button,
                       struct gpio_callback *cb,
                       gpio_callback_handler_t handler)
{
    if (!device_is_ready(button->port)) {
        printk("Button device not ready\n");
        return -1;
    }
    gpio_pin_configure_dt(button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(button, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(cb, handler, BIT(button->pin));
    gpio_add_callback(button->port, cb);
    return 0;
}

// ---------------------------------------------------
// Main task (LED control)
// ---------------------------------------------------
void led_task(void)
{
    while (1) {
        switch (led_state) {
        case 1: // red
            set_red(1); set_green(0);
            printk("RED\n");
            k_sleep(K_SECONDS(2));
            if (!paused && led_state == 1) led_state = 2;
            break;

        case 2: // yellow
            set_yellow(1);
            printk("YELLOW\n");
            k_sleep(K_SECONDS(1));
            if (!paused && led_state == 2) led_state = 3;
            break;

        case 3: // green
            set_red(0); set_green(1);
            printk("GREEN\n");
            k_sleep(K_SECONDS(2));
            if (!paused && led_state == 3) led_state = 2;

        case 4: // paused
            k_sleep(K_MSEC(200));
            break;

        case 5: // yellow blink mode
            set_yellow(1);
            printk("YELLOW BLINK ON\n");
            k_sleep(K_SECONDS(1));
            set_yellow(0);
            printk("YELLOW BLINK OFF\n");
            k_sleep(K_SECONDS(1));
            break;

        default:
            led_state = 1;
            break;
        }
    }
}

// ---------------------------------------------------
// Main
// ---------------------------------------------------
int main(void)
{
    printk("Traffic light demo start\n");

    init_led(&red);
    init_led(&green);

    init_button(&button_0, &button_0_cb_data, button_0_handler);
    init_button(&button_1, &button_1_cb_data, button_1_handler);
    init_button(&button_2, &button_2_cb_data, button_2_handler);
    init_button(&button_3, &button_3_cb_data, button_3_handler);
    init_button(&button_4, &button_4_cb_data, button_4_handler);

    led_task();
    return 0;
}