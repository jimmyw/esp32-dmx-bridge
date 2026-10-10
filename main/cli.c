#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "audio.h"
#include "beat_led.h"
#include "config.h"
#include "console.h"
#include "dmx_buffer.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "names.h"
#include "scenes.h"
#include "script.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_mgr.h"

static void restart_soon(void)
{
    printf("Restarting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static const char *src_name(uint8_t t)
{
    return t == SRC_ARTNET ? "Art-Net" : t == SRC_SACN ? "sACN" : "none";
}

// Commit a modified copy of the config; prints errors. Returns true on success.
static bool commit(const bridge_config_t *c, bool *reboot)
{
    const char *err = config_commit(c, reboot);
    if (err) {
        printf("Error: %s\n", err);
        return false;
    }
    return true;
}

/* ---- status ---- */

static int cmd_status(int argc, char **argv)
{
    dmx_stats_t st;
    dmx_buffer_get_stats(&st);
    esp_netif_ip_info_t ip;

    printf("Firmware   : %s (%s)\n", esp_app_get_description()->version,
           esp_ota_get_running_partition()->label);
    printf("Name       : %s  (%s.local)\n", g_config.name, g_config.hostname);
    printf("Uptime     : %llu s, free heap %lu\n",
           (unsigned long long)(esp_timer_get_time() / 1000000), (unsigned long)esp_get_free_heap_size());
    if (wifi_mgr_sta_connected() && wifi_mgr_get_sta_ip_info(&ip) == ESP_OK) {
        printf("Wi-Fi      : connected to \"%s\", %d dBm, IP " IPSTR "\n",
               g_config.wifi_ssid, wifi_mgr_rssi(), IP2STR(&ip.ip));
    } else if (g_config.wifi_ssid[0]) {
        printf("Wi-Fi      : not connected (trying \"%s\")\n", g_config.wifi_ssid);
    } else {
        printf("Wi-Fi      : no network configured\n");
    }
    printf("Setup AP   : %s\n", wifi_mgr_ap_active() ? wifi_mgr_ap_ssid() : "off");
    if (st.signal) {
        esp_ip4_addr_t a = { .addr = st.active_ip };
        printf("DMX input  : %s from " IPSTR " (priority %u, %u source%s)\n", src_name(st.active_type),
               IP2STR(&a), st.active_priority, st.num_sources, st.num_sources == 1 ? "" : "s");
    } else {
        printf("DMX input  : no signal\n");
    }
    printf("Packets    : Art-Net %lu, sACN %lu, DMX frames sent %lu\n",
           (unsigned long)st.artnet_packets, (unsigned long)st.sacn_packets, (unsigned long)st.frames_sent);
    return 0;
}

/* ---- config ---- */

static int cmd_config(int argc, char **argv)
{
    const bridge_config_t *c = &g_config;
    printf("wifi_ssid       = %s\n", c->wifi_ssid);
    printf("wifi_pass       = %s\n", c->wifi_pass[0] ? "********" : "(none)");
    printf("hostname        = %s\n", c->hostname);
    printf("name            = %s\n", c->name);
    printf("protocol        = %s\n", c->protocol == PROTO_BOTH ? "both" :
                                     c->protocol == PROTO_ARTNET ? "artnet" : "sacn");
    printf("artnet_universe = %u  (net %u, sub %u, uni %u)\n", c->artnet_port_addr,
           c->artnet_port_addr >> 8, (c->artnet_port_addr >> 4) & 15, c->artnet_port_addr & 15);
    printf("sacn_universe   = %u\n", c->sacn_universe);
    printf("tx_pin          = %d\n", c->tx_pin);
    printf("de_pin          = %d\n", c->de_pin);
    printf("led_pin         = %d\n", c->led_pin);
    printf("uart            = %u\n", c->uart_num);
    printf("refresh_hz      = %u\n", c->refresh_hz);
    printf("on_loss         = %s\n", c->on_loss == LOSS_BLACKOUT ? "blackout" : "hold");
    printf("loss_timeout_ms = %u\n", c->loss_timeout_ms);
    printf("mic_sck/ws/sd   = %d / %d / %d\n", c->mic_sck, c->mic_ws, c->mic_sd);
    printf("mic_gate        = %u  (-%u dBFS)\n", c->mic_gate, c->mic_gate);
    printf("beat_sens       = %u  (x%.1f)\n", c->beat_sens, c->beat_sens / 10.0);
    printf("beat_offset_ms  = %d  (beats %s)\n", c->beat_offset_ms, c->beat_offset_ms >= 0 ? "earlier" : "later");
    return 0;
}

/* ---- rgbled [r g b [ms]] | pin <gpio> ---- */

static int cmd_rgbled(int argc, char **argv)
{
    esp_err_t err;
    if (argc == 3 && strcmp(argv[1], "pin") == 0) {
        err = beat_led_set_pin(atoi(argv[2]));
    } else if (argc == 4 || argc == 5) {
        err = beat_led_test(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), argc == 5 ? atoi(argv[4]) : 5000);
    } else if (argc == 1) {
        printf("beat LED on GPIO %d\n", beat_led_pin());
        return 0;
    } else {
        printf("Usage: rgbled [<r> <g> <b> [ms]] | pin <gpio>\n");
        return 1;
    }
    printf("%s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* ---- audio [demo on|off] ---- */

static int cmd_audio(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "demo") == 0) {
        audio_set_demo(strcmp(argv[2], "on") == 0);
    } else if (argc != 1) {
        printf("Usage: audio [demo on|off]\n");
        return 1;
    }
    audio_state_t a;
    audio_get(&a);
    printf("mic %s%s, input %.1f dBFS%s\n", a.mic ? "on" : "not configured", a.demo ? " (demo)" : "",
           a.db, a.signal ? "" : " (below gate)");
    printf("level %.2f  bass %.2f  mid %.2f  high %.2f\n", a.level, a.bass, a.mid, a.high);
    printf("beats %lu  bpm %.1f\n", (unsigned long)a.beats, a.bpm);
    return 0;
}

/* ---- set <key> <value> ---- */

static struct {
    struct arg_str *key;
    struct arg_str *value;
    struct arg_end *end;
} set_args;

static int cmd_set(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&set_args) != 0) {
        arg_print_errors(stderr, set_args.end, argv[0]);
        printf("Keys:");
        for (int i = 0; CONFIG_KEYS[i]; i++) {
            printf(" %s", CONFIG_KEYS[i]);
        }
        printf("\n");
        return 1;
    }
    const char *key = set_args.key->sval[0];
    if (strcmp(key, "wifi_ssid") == 0 || strcmp(key, "wifi_pass") == 0) {
        printf("Use: wifi <ssid> [password]\n");
        return 1;
    }
    bridge_config_t c = g_config;
    const char *err = config_set_field(&c, key, set_args.value->sval[0]);
    if (err) {
        printf("Error: %s\n", err);
        return 1;
    }
    bool reboot = false;
    if (!commit(&c, &reboot)) {
        return 1;
    }
    printf("OK%s\n", reboot ? " - takes effect after 'reboot'" : "");
    return 0;
}

/* ---- wifi <ssid> [password] | wifi --forget ---- */

static struct {
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_lit *forget;
    struct arg_end *end;
} wifi_args;

static int cmd_wifi(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&wifi_args) != 0) {
        arg_print_errors(stderr, wifi_args.end, argv[0]);
        return 1;
    }
    bridge_config_t c = g_config;
    if (wifi_args.forget->count) {
        c.wifi_ssid[0] = '\0';
        c.wifi_pass[0] = '\0';
        if (!commit(&c, NULL)) {
            return 1;
        }
        printf("Wi-Fi credentials erased; the bridge will start its setup AP.\n");
        restart_soon();
        return 0;
    }
    if (wifi_args.ssid->count == 0) {
        printf("Usage: wifi <ssid> [password]   (quote names with spaces: wifi \"My Net\" secret)\n"
               "       wifi --forget\n");
        return 1;
    }
    const char *err = config_set_field(&c, "wifi_ssid", wifi_args.ssid->sval[0]);
    if (!err) {
        // Always set the password (empty = open network), even when the SSID is unchanged.
        err = config_set_field(&c, "wifi_pass", wifi_args.pass->count ? wifi_args.pass->sval[0] : "");
    }
    if (err) {
        printf("Error: %s\n", err);
        return 1;
    }
    if (!commit(&c, NULL)) {
        return 1;
    }
    printf("Saved Wi-Fi \"%s\" (%s).\n", c.wifi_ssid, c.wifi_pass[0] ? "with password" : "open network");
    restart_soon();
    return 0;
}

/* ---- scan ---- */

static int cmd_scan(int argc, char **argv)
{
    uint16_t n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        return 1;
    }
    printf("Scanning...\n");
    esp_err_t err = wifi_mgr_scan(recs, &n);
    if (err != ESP_OK) {
        printf("Scan failed: %s\n", esp_err_to_name(err));
        free(recs);
        return 1;
    }
    printf("  RSSI  CH  SEC   SSID\n");
    for (int i = 0; i < n; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        printf("  %4d  %2d  %-4s  %s\n", recs[i].rssi, recs[i].primary,
               recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "yes", (const char *)recs[i].ssid);
    }
    free(recs);
    return 0;
}

/* ---- dmx [count]: show output channels ---- */

static struct {
    struct arg_int *count;
    struct arg_end *end;
} dmx_args;

static int cmd_dmx(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&dmx_args) != 0) {
        arg_print_errors(stderr, dmx_args.end, argv[0]);
        return 1;
    }
    int n = dmx_args.count->count ? dmx_args.count->ival[0] : 32;
    if (n < 1 || n > DMX_SLOTS) {
        n = DMX_SLOTS;
    }
    static uint8_t v[DMX_SLOTS];
    dmx_buffer_peek(v, n);
    for (int i = 0; i < n; i++) {
        if (i % 16 == 0) {
            printf("%s%3d:", i ? "\n" : "", i + 1);
        }
        printf(" %3u", v[i]);
    }
    printf("\n");
    return 0;
}

/* ---- name <ch> [text] / names ---- */

static int cmd_name(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: name <channel 1-512> [text]   (no text = remove; quote text with spaces)\n");
        return 1;
    }
    char *end;
    long ch = strtol(argv[1], &end, 10);
    if (*end || ch < 1 || ch > DMX_SLOTS) {
        printf("Error: channel must be 1-512\n");
        return 1;
    }
    // Allow unquoted multi-word names: join the remaining arguments.
    char text[NAME_MAX_LEN * 2 + 1] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) strlcat(text, " ", sizeof(text));
        strlcat(text, argv[i], sizeof(text));
    }
    names_set(ch - 1, text);
    names_save_soon();
    console_names_changed();
    printf("Channel %ld: %s\n", ch, names_get(ch - 1)[0] ? names_get(ch - 1) : "(no name)");
    return 0;
}

static int cmd_names(int argc, char **argv)
{
    int n = 0;
    for (int i = 0; i < DMX_SLOTS; i++) {
        if (names_get(i)[0] || names_hidden(i)) {
            printf("%4d  %s%s\n", i + 1, names_get(i), names_hidden(i) ? "  (hidden)" : "");
            n++;
        }
    }
    if (!n) {
        printf("No channel names set. Use: name <channel> <text>\n");
    }
    return 0;
}

/* ---- hide <ch> / unhide <ch|all> ---- */

static int cmd_hide(int argc, char **argv)
{
    bool hide = strcmp(argv[0], "hide") == 0;
    if (argc != 2) {
        printf("Usage: %s\n", hide ? "hide <channel 1-512>" : "unhide <channel 1-512|all>");
        return 1;
    }
    if (!hide && strcmp(argv[1], "all") == 0) {
        names_show_all();
    } else {
        char *end;
        long ch = strtol(argv[1], &end, 10);
        if (*end || ch < 1 || ch > DMX_SLOTS) {
            printf("Error: channel must be 1-512\n");
            return 1;
        }
        names_set_hidden(ch - 1, hide);
    }
    names_save_soon();
    console_names_changed();
    printf("OK\n");
    return 0;
}

/* ---- script list|run|stop|log|param ---- */

static int cmd_script(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (strcmp(sub, "list") == 0) {
        cJSON *st = script_status_json();
        const cJSON *run = cJSON_GetObjectItemCaseSensitive(st, "running");
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(st, "scripts")) {
            const char *n = cJSON_GetObjectItemCaseSensitive(it, "name")->valuestring;
            printf("%c %-24s %6d bytes\n", cJSON_IsString(run) && strcmp(run->valuestring, n) == 0 ? '*' : ' ',
                   n, cJSON_GetObjectItemCaseSensitive(it, "size")->valueint);
        }
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(st, "params")) {
            printf("  param %-15s %g\n", cJSON_GetObjectItemCaseSensitive(it, "name")->valuestring,
                   cJSON_GetObjectItemCaseSensitive(it, "value")->valuedouble);
        }
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(st, "failed");
        if (f) {
            printf("failed: %s: %s\n", cJSON_GetObjectItemCaseSensitive(f, "name")->valuestring,
                   cJSON_GetObjectItemCaseSensitive(f, "error")->valuestring);
        }
        cJSON_Delete(st);
        return 0;
    }
    if (strcmp(sub, "run") == 0 && argc == 3) {
        const char *err = script_run(argv[2]);
        printf("%s\n", err ? err : "OK");
        return err ? 1 : 0;
    }
    if (strcmp(sub, "stop") == 0) {
        script_stop();
        printf("OK\n");
        return 0;
    }
    if (strcmp(sub, "log") == 0) {
        char *text = NULL;
        script_log_read(0, &text);
        printf("%s", text ? text : "");
        free(text);
        return 0;
    }
    if (strcmp(sub, "param") == 0 && argc == 4) {
        const char *err = script_set_param(argv[2], strtod(argv[3], NULL));
        printf("%s\n", err ? err : "OK");
        return err ? 1 : 0;
    }
    printf("Usage: script [list] | run <name> | stop | log | param <name> <value>\n");
    return 1;
}

/* ---- scene list|save|recall|rename|delete ---- */

static int cmd_scene(int argc, char **argv)
{
    const char *usage =
        "Usage: scene list\n"
        "       scene save <1-64> [name]        capture the console faders\n"
        "       scene recall <1-64> [fade sec]  e.g. scene recall 3 2.5\n"
        "       scene rename <1-64> <name>\n"
        "       scene delete <1-64>\n";
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        int n = 0;
        for (int i = 0; i < SCENE_COUNT; i++) {
            if (scenes_used(i)) {
                printf("%3d %c %s\n", i + 1, scenes_active() == i ? '*' : ' ', scenes_name(i));
                n++;
            }
        }
        if (!n) {
            printf("No scenes stored.\n%s", argc < 2 ? usage : "");
        }
        return 0;
    }
    if (argc < 3) {
        printf("%s", usage);
        return 1;
    }
    char *end;
    long id = strtol(argv[2], &end, 10);
    if (*end || id < 1 || id > SCENE_COUNT) {
        printf("Error: scene number must be 1-%d\n", SCENE_COUNT);
        return 1;
    }
    char text[NAME_MAX_LEN * 2 + 1] = "";
    for (int i = 3; i < argc; i++) {
        if (i > 3) strlcat(text, " ", sizeof(text));
        strlcat(text, argv[i], sizeof(text));
    }
    esp_err_t err;
    const char *op = argv[1];
    if (strcmp(op, "save") == 0) {
        err = scenes_save(id - 1, text);
    } else if (strcmp(op, "recall") == 0) {
        float fade = argc > 3 ? strtof(argv[3], NULL) : 0;
        err = scenes_recall(id - 1, fade > 0 ? (uint32_t)(fade * 1000) : 0);
    } else if (strcmp(op, "rename") == 0) {
        err = scenes_rename(id - 1, text);
    } else if (strcmp(op, "delete") == 0) {
        err = scenes_delete(id - 1);
    } else {
        printf("%s", usage);
        return 1;
    }
    if (err != ESP_OK) {
        printf("Error: %s\n", err == ESP_ERR_NOT_FOUND ? "scene is empty" : esp_err_to_name(err));
        return 1;
    }
    printf("OK: %s scene %ld%s%s\n", op, id, scenes_used(id - 1) ? " - " : "", scenes_name(id - 1));
    return 0;
}

/* ---- misc ---- */

static int cmd_reboot(int argc, char **argv)
{
    restart_soon();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("This erases all settings including Wi-Fi. Type: factory_reset yes\n");
        return 1;
    }
    config_factory_reset();
    restart_soon();
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: log <none|error|warn|info|debug> [tag]\n");
        return 1;
    }
    static const char *names[] = { "none", "error", "warn", "info", "debug", "verbose" };
    for (int i = 0; i < 6; i++) {
        if (strcmp(argv[1], names[i]) == 0) {
            esp_log_level_set(argc > 2 ? argv[2] : "*", (esp_log_level_t)i);
            return 0;
        }
    }
    printf("Unknown level\n");
    return 1;
}

esp_err_t cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "dmx> ";
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 6144;

    esp_console_register_help_command();

    set_args.key = arg_str1(NULL, NULL, "<key>", "setting name (see 'config')");
    set_args.value = arg_str1(NULL, NULL, "<value>", "new value");
    set_args.end = arg_end(2);
    wifi_args.ssid = arg_str0(NULL, NULL, "<ssid>", "network name");
    wifi_args.pass = arg_str0(NULL, NULL, "<password>", "password (omit for an open network)");
    wifi_args.forget = arg_lit0("f", "forget", "erase Wi-Fi credentials and start the setup AP");
    wifi_args.end = arg_end(3);
    dmx_args.count = arg_int0(NULL, NULL, "<n>", "number of channels (default 32)");
    dmx_args.end = arg_end(1);

    const esp_console_cmd_t cmds[] = {
        { .command = "status", .help = "Show Wi-Fi, IP and DMX input status", .func = cmd_status },
        { .command = "config", .help = "Show all settings", .func = cmd_config },
        { .command = "set", .help = "Change a setting, e.g. 'set sacn_universe 2', 'set protocol artnet'",
          .func = cmd_set, .argtable = &set_args },
        { .command = "wifi", .help = "Set Wi-Fi credentials and restart: wifi <ssid> [password]",
          .func = cmd_wifi, .argtable = &wifi_args },
        { .command = "scan", .help = "Scan for Wi-Fi networks", .func = cmd_scan },
        { .command = "dmx", .help = "Show current DMX output values", .func = cmd_dmx, .argtable = &dmx_args },
        { .command = "name", .help = "Name a channel: name <1-512> [text] (no text = remove)", .func = cmd_name },
        { .command = "names", .help = "List channel names and hidden channels", .func = cmd_names },
        { .command = "hide", .help = "Hide a channel on the web console: hide <1-512>", .func = cmd_hide },
        { .command = "unhide", .help = "Show a hidden channel again: unhide <1-512|all>", .func = cmd_hide },
        { .command = "scene", .help = "Console scenes: scene list|save|recall|rename|delete", .func = cmd_scene },
        { .command = "script", .help = "Effect scripts: script [list] | run <name> | stop | log | param <name> <v>",
          .func = cmd_script },
        { .command = "rgbled", .help = "Beat LED test: rgbled <r> <g> <b> [ms] (default 5 s), rgbled pin <gpio>",
          .func = cmd_rgbled },
        { .command = "audio", .help = "Microphone analysis: audio [demo on|off]", .func = cmd_audio },
        { .command = "log", .help = "Set log level: log <none|error|warn|info|debug> [tag]", .func = cmd_log },
        { .command = "reboot", .help = "Restart the bridge", .func = cmd_reboot },
        { .command = "factory_reset", .help = "Erase all settings: factory_reset yes", .func = cmd_factory_reset },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        return err;
    }
    return esp_console_start_repl(repl);
}
