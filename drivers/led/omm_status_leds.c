/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct gpio_dt_spec d1_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec d2_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static bool d2_blinking;
static bool d2_led_on;
static struct k_work_delayable d2_blink_work;

static void set_led(const struct gpio_dt_spec *led, bool on) {
    int ret = gpio_pin_set_dt(led, on ? 1 : 0);

    if (ret != 0) {
        LOG_ERR("Failed to set LED state: %d", ret);
    }
}

static bool ble_pairing_active(void) {
#if defined(CONFIG_ZMK_BLE)
    return zmk_ble_active_profile_is_open() && !zmk_ble_active_profile_is_connected();
#else
    return false;
#endif
}

static void d2_blink_work_handler(struct k_work *work) {
    if (!d2_blinking) {
        return;
    }

    d2_led_on = !d2_led_on;
    set_led(&d2_led, d2_led_on);
    k_work_reschedule(&d2_blink_work, K_MSEC(CONFIG_OMM_STATUS_LEDS_PAIRING_BLINK_MS));
}

static void update_battery_led(uint8_t state_of_charge) {
    set_led(&d1_led, state_of_charge <= CONFIG_OMM_STATUS_LEDS_LOW_BATTERY_THRESHOLD);
}

static void update_pairing_led(void) {
    bool should_blink = ble_pairing_active();

    if (should_blink == d2_blinking) {
        return;
    }

    d2_blinking = should_blink;

    if (d2_blinking) {
        d2_led_on = false;
        k_work_reschedule(&d2_blink_work, K_NO_WAIT);
    } else {
        k_work_cancel_delayable(&d2_blink_work);
        d2_led_on = false;
        set_led(&d2_led, false);
    }
}

static int omm_status_leds_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *battery_ev = as_zmk_battery_state_changed(eh);

    if (battery_ev != NULL) {
        update_battery_led(battery_ev->state_of_charge);
        return ZMK_EV_EVENT_BUBBLE;
    }

#if defined(CONFIG_ZMK_BLE)
    const struct zmk_ble_active_profile_changed *ble_ev = as_zmk_ble_active_profile_changed(eh);

    if (ble_ev != NULL) {
        update_pairing_led();
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(omm_status_leds, omm_status_leds_listener);
ZMK_SUBSCRIPTION(omm_status_leds, zmk_battery_state_changed);
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(omm_status_leds, zmk_ble_active_profile_changed);
#endif

static int omm_status_leds_init(void) {
    int ret;

    if (!device_is_ready(d1_led.port)) {
        LOG_ERR("D1 LED GPIO port is not ready");
        return -ENODEV;
    }

    if (!device_is_ready(d2_led.port)) {
        LOG_ERR("D2 LED GPIO port is not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&d1_led, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to configure D1 LED: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&d2_led, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to configure D2 LED: %d", ret);
        return ret;
    }

    k_work_init_delayable(&d2_blink_work, d2_blink_work_handler);

    update_battery_led(zmk_battery_state_of_charge());
    update_pairing_led();

    LOG_INF("OMM status LEDs enabled");
    return 0;
}

SYS_INIT(omm_status_leds_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
