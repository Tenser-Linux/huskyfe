#include "Flashlight.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>


namespace huskyfe::flashlight {

namespace {

constexpr const char* kI2cPath   = "/dev/i2c-14";
constexpr int         kI2cAddr   = 0x63;
constexpr const char* kGpioPath  = "/dev/gpiochip8";
constexpr int         kGpioLine  = 2;

int  g_i2c_fd     = -1;
int  g_gpio_req_fd = -1;
bool g_on         = false;

bool gpio_set_value(bool v) {
    struct gpio_v2_line_values vals{};
    vals.mask = 1ULL;
    vals.bits = v ? 1ULL : 0ULL;
    if (ioctl(g_gpio_req_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
        fprintf(stderr, "flashlight: SET_VALUES: %s\n", strerror(errno));
        return false;
    }
    return true;
}

bool gpio_claim_line() {
    int chip_fd = open(kGpioPath, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
        fprintf(stderr, "flashlight: open %s: %s\n", kGpioPath, strerror(errno));
        return false;
    }
    struct gpio_v2_line_request req{};
    req.num_lines  = 1;
    req.offsets[0] = kGpioLine;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].mask = 1ULL;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0ULL;
    strncpy(req.consumer, "huskyfe-flash", sizeof(req.consumer) - 1);
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "flashlight: GPIO_V2_GET_LINE: %s\n", strerror(errno));
        close(chip_fd);
        return false;
    }
    close(chip_fd);
    g_gpio_req_fd = req.fd;
    return true;
}

bool i2c_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    if (write(g_i2c_fd, buf, 2) != 2) {
        fprintf(stderr, "flashlight: i2c write 0x%02x=0x%02x: %s\n",
                reg, val, strerror(errno));
        return false;
    }
    return true;
}

}

bool init() {
    if (ok()) return true;
    g_i2c_fd = open(kI2cPath, O_RDWR | O_CLOEXEC);
    if (g_i2c_fd < 0) {
        fprintf(stderr, "flashlight: open %s: %s\n", kI2cPath, strerror(errno));
        return false;
    }
    if (ioctl(g_i2c_fd, I2C_SLAVE, kI2cAddr) < 0) {
        fprintf(stderr, "flashlight: I2C_SLAVE 0x%02x: %s\n", kI2cAddr, strerror(errno));
        close(g_i2c_fd); g_i2c_fd = -1;
        return false;
    }
    if (!gpio_claim_line()) {
        close(g_i2c_fd); g_i2c_fd = -1;
        return false;
    }
    return true;
}

void shutdown() {
    if (g_i2c_fd >= 0 && g_on) i2c_write_reg(0x01, 0x00);
    if (g_gpio_req_fd >= 0) {
        gpio_set_value(false);
        close(g_gpio_req_fd); g_gpio_req_fd = -1;
    }
    if (g_i2c_fd >= 0) { close(g_i2c_fd); g_i2c_fd = -1; }
    g_on = false;
}

bool ok()    { return g_i2c_fd >= 0 && g_gpio_req_fd >= 0; }
bool is_on() { return g_on; }

bool set_level(int level) {
    if (!ok() || !g_on) return false;
    if (level < 0)    level = 0;
    if (level > 0x7F) level = 0x7F;
    if (!i2c_write_reg(0x05, (uint8_t)level)) return false;
    if (!i2c_write_reg(0x06, (uint8_t)level)) return false;
    return true;
}

bool set(bool on, int level) {
    if (!ok()) return false;
    if (on) {
        if (level < 0)    level = 0;
        if (level > 0x7F) level = 0x7F;


        if (!gpio_set_value(true)) return false;
        usleep(2000);
        if (!i2c_write_reg(0x05, (uint8_t)level)) return false;
        if (!i2c_write_reg(0x06, (uint8_t)level)) return false;
        if (!i2c_write_reg(0x01, 0x0B)) return false;
        g_on = true;
    } else {
        i2c_write_reg(0x01, 0x00);
        gpio_set_value(false);
        g_on = false;
    }
    return true;
}

}
