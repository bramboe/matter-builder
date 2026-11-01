// A relay that switches on for a number of seconds, then off by itself.
// An example of a component: setup, poll and output, and report_output()
// when it switches off on its own.

#include "driver/gpio.h"
#include "esp_timer.h"
#include "mb_driver.h"

namespace {

int64_t off_at[8] = {};  // per connection: when the pulse ends (0: off)

void write(const io::Slot& s, bool on) {
    gpio_set_level(static_cast<gpio_num_t>(s.pin), on != ((s.flags & io::INVERTED) != 0));
}

bool setup(int, const io::Slot& s) {
    const auto pin = static_cast<gpio_num_t>(s.pin);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    write(s, false);
    return true;
}

void output(int slot, const io::Slot& s, bool on, uint8_t) {
    write(s, on);
    // 0 seconds means one second: a pulse always ends
    const int64_t seconds = io::number_of(s) > 0 ? io::number_of(s) : 1;
    off_at[slot] = on ? esp_timer_get_time() + seconds * 1000000 : 0;
}

void poll(int slot, const io::Slot& s, int64_t now_us) {
    if (off_at[slot] != 0 && now_us >= off_at[slot]) {
        off_at[slot] = 0;
        write(s, false);
        io::report_output(slot, false);
    }
}

}  // namespace

void mb_register_pulse_relay() {
    io::Driver d = {};
    d.type = "pulse_relay";
    d.title = "Pulse relay";
    d.matter = io::Matter::on_off;
    d.pins = 1;
    d.number_label = "Seconds on";
    d.setup = setup;
    d.poll = poll;
    d.output = output;
    io::register_driver(d);
}
