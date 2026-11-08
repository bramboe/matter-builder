#include "log_buffer.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include <lib/support/logging/CHIPLogging.h>
#include <lib/support/logging/Constants.h>

namespace log_buffer {

namespace {

constexpr size_t SIZE = 16384;    // a quiet device's whole boot and the minutes after
constexpr size_t LINE_LEN = 192;  // longer lines are cut here (the serial port has them whole)

char ring[SIZE];
uint32_t written = 0;  // bytes ever written; ring[written % SIZE] is the next one
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t serial = nullptr;  // where the log went before (the console)
uint32_t boot_id = 0;  // differs per boot: a reader sees the device restarted

// `length` as vsnprintf counted it: a line cut at the buffer still ends in a newline
void keep(char* line, int length, size_t size) {
    if (length <= 0) {
        return;
    }
    size_t n = static_cast<size_t>(length) < size ? static_cast<size_t>(length) : size - 1;
    if (static_cast<size_t>(length) >= size) {
        line[n - 1] = '\n';
    }
    portENTER_CRITICAL_SAFE(&lock);
    for (size_t i = 0; i < n; ++i) {
        ring[(written + i) % SIZE] = line[i];
    }
    written += n;
    portEXIT_CRITICAL_SAFE(&lock);
}

int copy_line(const char* format, va_list args) {
    va_list again;
    va_copy(again, args);
    char line[LINE_LEN];
    const int length = std::vsnprintf(line, sizeof(line), format, again);
    va_end(again);
    // The Bluetooth stack logs every write (bed keepalives every 25 s): on
    // the console only, the bridge logs what happens to the beds itself
    const char* tag = length > 4 && line[0] == 'I' && line[1] == ' ' ? std::strchr(line, ')') : nullptr;
    if (tag == nullptr || std::strncmp(tag, ") NimBLE: ", 10) != 0) {
        keep(line, length, sizeof(line));
    }
    return serial != nullptr ? serial(format, args) : 0;
}

// The Matter SDK prints its prefix and newline with printf around the ESP
// log call, so the log alone gets bodies without either. Its own redirect
// gives whole lines: the console gets them as before, the buffer all but
// the message-by-message chatter, which would push everything else out.
void matter_line(const char* module, uint8_t category, const char* msg, va_list args) {
    using namespace chip::Logging;
    const char letter = category == kLogCategory_Error ? 'E' : category == kLogCategory_Detail ? 'D' : 'I';
    char line[LINE_LEN];
    int head = std::snprintf(line, sizeof(line), "%c (%lu) chip[%s]: ", letter,
                             static_cast<unsigned long>(esp_log_timestamp()), module);
    if (head < 0 || static_cast<size_t>(head) >= sizeof(line) - 2) {
        return;
    }
    int body = std::vsnprintf(line + head, sizeof(line) - head - 1, msg, args);
    if (body < 0) {
        return;
    }
    size_t end = static_cast<size_t>(head + body) < sizeof(line) - 2 ? head + body : sizeof(line) - 2;
    line[end] = '\n';
    line[end + 1] = '\0';
    std::fputs(line, stdout);
    const bool chatter = category == kLogCategory_Detail ||
                         (category != kLogCategory_Error &&
                          (!std::strcmp(module, "EM") || !std::strcmp(module, "IM") || !std::strcmp(module, "DMG")));
    if (!chatter) {
        keep(line, static_cast<int>(end + 1), sizeof(line));
    }
}

}  // namespace

void start() {
    if (serial == nullptr) {
        boot_id = esp_random() & 0x7fffffff;
        serial = esp_log_set_vprintf(copy_line);
        chip::Logging::SetLogRedirectCallback(matter_line);
    }
}

std::string read(uint32_t from, size_t max) {
    if (max > SIZE) {
        max = SIZE;
    }
    std::string text(max, '\0');  // allocated before the lock: no allocation inside it
    portENTER_CRITICAL_SAFE(&lock);
    const uint32_t end = written;
    const uint32_t oldest = end > SIZE ? end - SIZE : 0;
    // Gone already, or ahead of the log (the device restarted): from the oldest
    const bool lost = from < oldest || from > end;
    const uint32_t start = lost ? oldest : from;
    const size_t length = end - start < max ? end - start : max;
    for (size_t i = 0; i < length; ++i) {
        text[i] = ring[(start + i) % SIZE];
    }
    portEXIT_CRITICAL_SAFE(&lock);
    text.resize(length);
    char head[48];
    std::snprintf(head, sizeof(head), "log %lu %d %lu\n", static_cast<unsigned long>(start + length), lost ? 1 : 0,
                  static_cast<unsigned long>(boot_id));
    return head + text;
}

}  // namespace log_buffer
