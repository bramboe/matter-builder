#include "setup_dns.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace setup_dns {

namespace {

const char* TAG = "dns";
constexpr size_t HEADER_LEN = 12;
constexpr size_t MAX_PACKET = 512;
constexpr size_t ANSWER_LEN = 16;  // name pointer, type, class, TTL, length, IPv4

uint32_t answer_ip = 0;

// The reply to one query: the question echoed, plus an A record with the
// bridge's address when it asks for IPv4 (other types get no answer)
int reply(uint8_t* packet, int length) {
    if (length < static_cast<int>(HEADER_LEN) || (packet[2] & 0x80) != 0 || packet[4] != 0 || packet[5] != 1) {
        return 0;  // not a single-question query
    }
    size_t end = HEADER_LEN;
    while (end < static_cast<size_t>(length) && packet[end] != 0) {
        end += packet[end] + 1;
    }
    end += 1 + 4;  // the root label, type and class
    if (end > static_cast<size_t>(length) || end + ANSWER_LEN > MAX_PACKET) {
        return 0;
    }
    const bool ipv4 = packet[end - 4] == 0 && packet[end - 3] == 1;
    packet[2] = 0x84 | (packet[2] & 0x01);  // response, authoritative, keep "recursion desired"
    packet[3] = 0x00;
    packet[6] = 0;
    packet[7] = ipv4 ? 1 : 0;
    std::memset(packet + 8, 0, 4);  // no authority or additional records
    if (!ipv4) {
        return static_cast<int>(end);
    }
    const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
    std::memcpy(packet + end, answer, sizeof(answer));
    std::memcpy(packet + end + sizeof(answer), &answer_ip, 4);
    return static_cast<int>(end + ANSWER_LEN);
}

void serve(void*) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ESP_LOGE(TAG, "Cannot listen on port 53");
        vTaskDelete(nullptr);
        return;
    }
    uint8_t packet[MAX_PACKET];
    while (true) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int length = recvfrom(sock, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        const int out = length > 0 ? reply(packet, length) : 0;
        if (out > 0) {
            sendto(sock, packet, out, 0, reinterpret_cast<sockaddr*>(&from), from_len);
        }
    }
}

}  // namespace

void start(uint32_t ip) {
    answer_ip = ip;
    xTaskCreate(serve, "setup_dns", 3072, nullptr, 5, nullptr);
}

}  // namespace setup_dns
