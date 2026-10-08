#include "artnet.h"

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "dmx_buffer.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi_mgr.h"

static const char *TAG = "artnet";

#define OP_POLL      0x2000
#define OP_POLLREPLY 0x2100
#define OP_DMX       0x5000
#define OP_SYNC      0x5200

static const uint8_t ARTNET_ID[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };

typedef struct __attribute__((packed)) {
    uint8_t  id[8];
    uint16_t opcode;          // little endian
    uint8_t  ip[4];
    uint16_t port;            // little endian
    uint8_t  vers_hi, vers_lo;
    uint8_t  net_switch;
    uint8_t  sub_switch;
    uint8_t  oem_hi, oem_lo;
    uint8_t  ubea;
    uint8_t  status1;
    uint8_t  esta_lo, esta_hi;
    char     short_name[18];
    char     long_name[64];
    char     node_report[64];
    uint8_t  num_ports_hi, num_ports_lo;
    uint8_t  port_types[4];
    uint8_t  good_input[4];
    uint8_t  good_output_a[4];
    uint8_t  sw_in[4];
    uint8_t  sw_out[4];
    uint8_t  acn_priority;
    uint8_t  sw_macro;
    uint8_t  sw_remote;
    uint8_t  spare[3];
    uint8_t  style;
    uint8_t  mac[6];
    uint8_t  bind_ip[4];
    uint8_t  bind_index;
    uint8_t  status2;
    uint8_t  good_output_b[4];
    uint8_t  status3;
    uint8_t  default_resp_uid[6];
    uint8_t  user_hi, user_lo;
    uint8_t  refresh_hi, refresh_lo;
    uint8_t  filler[11];
} art_poll_reply_t;

_Static_assert(sizeof(art_poll_reply_t) == 239, "ArtPollReply must be 239 bytes");

static int s_sock = -1;
static uint32_t s_poll_replies;

static void send_poll_reply(const struct sockaddr_in *to)
{
    esp_netif_ip_info_t ip;
    if (wifi_mgr_get_ip_info(&ip) != ESP_OK) {
        return;
    }
    dmx_stats_t st;
    dmx_buffer_get_stats(&st);

    art_poll_reply_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.id, ARTNET_ID, sizeof(ARTNET_ID));
    r.opcode = OP_POLLREPLY;
    memcpy(r.ip, &ip.ip.addr, 4);
    r.port = ARTNET_PORT;
    r.vers_hi = 1;
    r.vers_lo = 0;
    uint16_t pa = g_config.artnet_port_addr;
    r.net_switch = (pa >> 8) & 0x7F;
    r.sub_switch = (pa >> 4) & 0x0F;
    r.oem_hi = 0x00;
    r.oem_lo = 0xFF;                  // generic / unregistered OEM
    r.status1 = 0xE0;                 // indicators normal, port-address set via network
    strlcpy(r.short_name, g_config.hostname, sizeof(r.short_name));
    strlcpy(r.long_name, g_config.name, sizeof(r.long_name));
    snprintf(r.node_report, sizeof(r.node_report), "#0001 [%04lu] %s",
             (unsigned long)(s_poll_replies % 10000),
             st.signal ? "DMX output active" : "Idle");
    r.num_ports_lo = 1;
    r.port_types[0] = 0x80;           // can output DMX512 from Art-Net
    r.good_output_a[0] = st.signal && st.active_type == SRC_ARTNET ? 0x80 : 0x00;
    r.sw_out[0] = pa & 0x0F;
    r.style = 0x00;                   // StNode
    esp_read_mac(r.mac, ESP_MAC_WIFI_STA);
    memcpy(r.bind_ip, &ip.ip.addr, 4);
    r.bind_index = 1;
    r.status2 = 0x0E;                 // 15-bit port address, DHCP capable, DHCP active
    r.good_output_b[0] = 0xC0;        // RDM disabled, continuous output
    r.refresh_hi = 0;
    r.refresh_lo = g_config.refresh_hz;

    struct sockaddr_in dst = *to;
    dst.sin_port = htons(ARTNET_PORT);
    sendto(s_sock, &r, sizeof(r), 0, (struct sockaddr *)&dst, sizeof(dst));
    s_poll_replies++;
}

static void handle_packet(const uint8_t *p, int len, const struct sockaddr_in *from)
{
    if (len < 10 || memcmp(p, ARTNET_ID, 8) != 0) {
        return;
    }
    uint16_t op = p[8] | (p[9] << 8);

    switch (op) {
    case OP_POLL:
        send_poll_reply(from);
        break;

    case OP_DMX: {
        if (!(g_config.protocol & PROTO_ARTNET) || len < 18) {
            return;
        }
        uint16_t port_addr = (p[15] << 8) | p[14];
        if ((port_addr & 0x7FFF) != g_config.artnet_port_addr) {
            return;
        }
        uint16_t dlen = (p[16] << 8) | p[17];
        if (dlen > DMX_SLOTS) {
            dlen = DMX_SLOTS;
        }
        if (18 + dlen > len) {
            dlen = len - 18;
        }
        uint8_t id[16] = { 'A' };
        memcpy(&id[1], &from->sin_addr.s_addr, 4);
        // Art-Net has no priority; treat it as the sACN default (100).
        dmx_buffer_submit(SRC_ARTNET, id, from->sin_addr.s_addr, 100, &p[18], dlen);
        break;
    }

    case OP_SYNC:
    default:
        break;
    }
}

static void artnet_task(void *arg)
{
    static uint8_t buf[600];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            handle_packet(buf, n, &from);
        } else if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t artnet_start(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ARTNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(s_sock);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "listening on UDP %d, port-address %u", ARTNET_PORT, g_config.artnet_port_addr);
    xTaskCreatePinnedToCore(artnet_task, "artnet", 4096, NULL, 12, NULL, 0);
    return ESP_OK;
}
