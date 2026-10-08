#include "captive_dns.h"

#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";
static bool s_started;

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "socket/bind failed");
        if (sock >= 0) {
            close(sock);
        }
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fromlen);
        if (n < 12) {
            continue;
        }
        // Only standard queries with exactly one question
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) {
            continue;
        }
        // Walk the QNAME
        int q = 12;
        while (q < n && buf[q] != 0) {
            q += buf[q] + 1;
        }
        q += 5;   // zero byte + QTYPE + QCLASS
        if (q > n) {
            continue;
        }
        uint16_t qtype = (buf[q - 4] << 8) | buf[q - 3];

        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap) {
            esp_netif_get_ip_info(ap, &ip);
        }

        buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
        buf[3] = 0x80;                     // RA, no error
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;   // answer count
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = q;
        if (qtype == 1) {
            const uint8_t answer[] = {
                0xC0, 0x0C,             // pointer to QNAME
                0x00, 0x01, 0x00, 0x01, // type A, class IN
                0x00, 0x00, 0x00, 0x3C, // TTL 60
                0x00, 0x04,
            };
            memcpy(&buf[len], answer, sizeof(answer));
            len += sizeof(answer);
            memcpy(&buf[len], &ip.ip.addr, 4);
            len += 4;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fromlen);
    }
}

esp_err_t captive_dns_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_started = true;
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 5, NULL);
    return ESP_OK;
}
