/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct gpio_dt_spec d1_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec d2_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static int omm_led_test_init(void) {
    int ret;

    if (!device_is_ready(d1_led.port)) {
        LOG_ERR("D1 LED GPIO port is not ready");
        return -ENODEV;
    }

    if (!device_is_ready(d2_led.port)) {
        LOG_ERR("D2 LED GPIO port is not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&d1_led, GPIO_OUTPUT_ACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to turn on D1 LED: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&d2_led, GPIO_OUTPUT_ACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to turn on D2 LED: %d", ret);
        return ret;
    }

    LOG_INF("OMM LED test enabled: D1 and D2 are on");
    return 0;
}

SYS_INIT(omm_led_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
