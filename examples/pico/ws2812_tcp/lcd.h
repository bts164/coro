#pragma once

#include <hardware/i2c.h>
#include <hardware/gpio.h>
#include <pico/stdlib.h>
#include <cstdint>
#include <string_view>

// HD44780 16x2 LCD driver via PCF8574 I2C expander (4-bit mode).
//
// PCF8574 standard backpack pin mapping:
//   P0=RS  P1=RW  P2=EN  P3=BL  P4=D4  P5=D5  P6=D6  P7=D7
//
// Usage:
//   Lcd lcd(i2c0, 0x27, /*sda=*/4, /*scl=*/5);
//   lcd.init();           // blocking ~100ms; call before entering the coro runtime
//   lcd.set_cursor(0, 0);
//   lcd.write("Hello!");
class Lcd {
public:
    Lcd(i2c_inst_t* i2c, uint8_t addr, uint sda, uint scl)
        : m_i2c(i2c), m_addr(addr) {
        i2c_init(i2c, 100'000);
        gpio_set_function(sda, GPIO_FUNC_I2C);
        gpio_set_function(scl, GPIO_FUNC_I2C);
        gpio_pull_up(sda);
        gpio_pull_up(scl);
    }

    // HD44780 power-on initialization: 4-bit mode, 2-line, cursor off.
    // Blocks ~100ms via sleep_ms/sleep_us; call before the async runtime starts.
    void init() {
        sleep_ms(50);
        // Reset sequence: three 0x3 nibbles put the controller into a known state
        // regardless of whether it powered on in 4-bit or 8-bit mode.
        write_nibble(0x03, false); sleep_ms(5);
        write_nibble(0x03, false); sleep_us(150);
        write_nibble(0x03, false); sleep_us(150);
        write_nibble(0x02, false); sleep_us(150);  // switch to 4-bit mode
        send_cmd(0x28);  // 4-bit, 2-line, 5x8 dots
        send_cmd(0x0C);  // display on, cursor off, blink off
        send_cmd(0x06);  // entry: auto-increment, no display shift
        send_cmd(0x01);  // clear display
        sleep_ms(2);
    }

    void clear() {
        send_cmd(0x01);
        sleep_ms(2);
    }

    // Row 0 or 1; col 0–15.
    void set_cursor(uint8_t row, uint8_t col) {
        static constexpr uint8_t offsets[] = {0x00, 0x40};
        send_cmd(uint8_t(0x80 | (offsets[row & 1] + col)));
    }

    void write(std::string_view text) {
        for (char c : text) send_data(uint8_t(c));
    }

    void backlight(bool on) {
        m_bl = on ? k_BL : 0;
        uint8_t b = m_bl;
        i2c_write_blocking_until(m_i2c, m_addr, &b, 1, false, make_timeout_time_ms(10));
    }

private:
    static constexpr uint8_t k_RS = 0x01;
    static constexpr uint8_t k_EN = 0x04;
    static constexpr uint8_t k_BL = 0x08;

    i2c_inst_t* m_i2c;
    uint8_t     m_addr;
    uint8_t     m_bl = k_BL;  // backlight on by default

    void write_nibble(uint8_t nibble, bool rs) {
        uint8_t d = uint8_t((nibble << 4) | m_bl | (rs ? k_RS : 0));
        uint8_t hi = uint8_t(d | k_EN);
        i2c_write_blocking_until(m_i2c, m_addr, &hi, 1, false, make_timeout_time_ms(10));
        sleep_us(1);
        i2c_write_blocking_until(m_i2c, m_addr, &d, 1, false, make_timeout_time_ms(10));
        sleep_us(50);
    }

    void send_cmd(uint8_t cmd) {
        write_nibble(cmd >> 4, false);
        write_nibble(cmd & 0x0F, false);
    }

    void send_data(uint8_t data) {
        write_nibble(data >> 4, true);
        write_nibble(data & 0x0F, true);
    }
};
