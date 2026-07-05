#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include "protocol.h"
#include "network.h"
#include "state.h"
#include "ui.h"
#include "ime.h"
#include "config.h"
#include "imgcache.h"
#include "b64.h"
#include "voice.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time fallbacks, overridable at runtime via CONFIG_PATH
   (ux0:data/dawncord/config.txt). DAWNCORD_PAIR_CODE must match the
   companion's env var if one is set. */
#ifndef COMPANION_HOST
#define COMPANION_HOST "192.168.1.100"
#endif
#ifndef COMPANION_PORT
#define COMPANION_PORT 9100
#endif
#ifndef DAWNCORD_PAIR_CODE
#define DAWNCORD_PAIR_CODE ""
#endif

#define MESSAGE_FETCH_LIMIT 40

static dawncord_config cfg;
static dawncord_connection conn;
static app_state st;
static dawncord_view view = VIEW_GUILD_LIST;
static int running = 1;

/* Serialize with cJSON so user text is JSON-escaped correctly. */
static void send_json(dawncord_msg_type type, cJSON *obj)
{
    char *body = cJSON_PrintUnformatted(obj);
    if (body) {
        net_send_message(&conn, type, body);
        cJSON_free(body);
    }
    cJSON_Delete(obj);
}

static void request_channels(const char *guild_id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "guild_id", guild_id);
    send_json(MSG_REQUEST_CHANNELS, o);
}

static void request_messages(const char *channel_id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "channel_id", channel_id);
    cJSON_AddNumberToObject(o, "limit", MESSAGE_FETCH_LIMIT);
    send_json(MSG_REQUEST_MESSAGES, o);
}

static void set_active_channel(const char *channel_id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "channel_id", channel_id);
    send_json(MSG_SET_CHANNEL, o);
}

static void request_members(const char *channel_id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "channel_id", channel_id);
    send_json(MSG_REQUEST_MEMBERS, o);
}

/* Voice (listen-only). Joining opens the local audio path immediately so
   the companion's first UDP frames aren't lost; a VOICE_STATE reply of
   active=false tears it back down. */
static void leave_voice(void)
{
    if (st.voice_id[0] == '\0')
        return;
    voice_stop();
    st.voice_id[0] = '\0';
    st.voice_name[0] = '\0';
    net_send_message(&conn, MSG_LEAVE_VOICE, "{}");
    state_set_status(&st, "Left voice");
}

static void toggle_voice(const st_named *c)
{
    if (strcmp(st.voice_id, c->id) == 0) {
        leave_voice();
        return;
    }
    if (st.voice_id[0])
        voice_stop();   /* switch channels: drop the old audio path */
    snprintf(st.voice_id, ST_ID_LEN, "%s", c->id);
    snprintf(st.voice_name, ST_NAME_LEN, "%s", c->name);
    if (voice_start() < 0) {
        st.voice_id[0] = '\0';
        state_set_status(&st, "Audio unavailable");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "channel_id", c->id);
    send_json(MSG_JOIN_VOICE, o);
    state_set_status(&st, "Joining voice...");
}

/* Scroll-back: ask for the chunk of history right before the oldest
   loaded message. The companion echoes "before" so the reply prepends
   instead of replacing the window. */
static void request_older_messages(void)
{
    if (st.history_pending || st.history_done ||
        st.message_count == 0 || st.messages[0].id[0] == '\0')
        return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "channel_id", st.channel_id);
    cJSON_AddNumberToObject(o, "limit", ST_HISTORY_CHUNK);
    cJSON_AddStringToObject(o, "before", st.messages[0].id);
    send_json(MSG_REQUEST_MESSAGES, o);
    st.history_pending = 1;
}

static void send_chat_message(const char *text)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "channel_id", st.channel_id);
    cJSON_AddStringToObject(o, "content", text);
    send_json(MSG_SEND_MESSAGE, o);
    state_set_status(&st, "Sending...");
}

/* ---- server -> state ---- */

static void process_server_message(dawncord_message *msg)
{
    switch (msg->type) {
    case MSG_GUILD_LIST:
        if (state_parse_guilds(&st, msg->payload) == 0)
            state_set_status(&st, "%d servers", st.guild_count);
        break;

    case MSG_CHANNEL_LIST:
        if (state_parse_channels(&st, msg->payload) == 0)
            state_set_status(&st, "");
        break;

    case MSG_MESSAGE_LIST:
        if (state_parse_messages(&st, msg->payload) == 0)
            state_set_status(&st, "");
        break;

    case MSG_MESSAGE_NEW:
        state_push_message(&st, msg->payload);
        break;

    case MSG_MEMBER_LIST:
        state_parse_members(&st, msg->payload);
        break;

    case MSG_TYPING:
        state_parse_typing(&st, msg->payload);
        break;

    case MSG_VOICE_STATE: {
        cJSON *root = cJSON_Parse(msg->payload);
        int act = root && cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(root, "active"));
        if (act) {
            state_set_status(&st, "In voice: %s", st.voice_name);
        } else {
            /* Companion couldn't join: tear the local audio path down. */
            voice_stop();
            st.voice_id[0] = '\0';
            st.voice_name[0] = '\0';
            state_set_status(&st, "Voice unavailable");
        }
        cJSON_Delete(root);
        break;
    }

    case MSG_MESSAGE_SENT_ACK: {
        cJSON *root = cJSON_Parse(msg->payload);
        int ok = root && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success"));
        cJSON_Delete(root);
        if (ok) {
            state_set_status(&st, "Sent");
            /* Own messages aren't pushed back (companion skips self): re-fetch.
               Skip if the user already backed out of the channel. */
            if (view == VIEW_WORKSPACE && st.channel_id[0] != '\0')
                request_messages(st.channel_id);
        } else {
            state_set_status(&st, "Send failed");
        }
        break;
    }

    case MSG_IMAGE_DATA: {
        cJSON *root = cJSON_Parse(msg->payload);
        const cJSON *key = root ? cJSON_GetObjectItemCaseSensitive(root, "key") : NULL;
        const cJSON *data = root ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
        if (cJSON_IsString(key)) {
            if (cJSON_IsString(data) && data->valuestring[0]) {
                size_t cap = strlen(data->valuestring) * 3 / 4 + 4;
                uint8_t *jpeg = malloc(cap);
                if (jpeg) {
                    int n = b64_decode(data->valuestring, jpeg, cap);
                    img_store(key->valuestring, n > 0 ? jpeg : NULL,
                              n > 0 ? (size_t)n : 0);
                    free(jpeg);
                }
            } else {
                /* Companion has no image for this key: settle as failed
                   so it isn't re-requested every frame. */
                img_store(key->valuestring, NULL, 0);
            }
        }
        cJSON_Delete(root);
        break;
    }

    case MSG_ERROR: {
        cJSON *root = cJSON_Parse(msg->payload);
        const cJSON *err = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
        state_set_status(&st, "Error: %s",
                         cJSON_IsString(err) ? err->valuestring : "unknown");
        cJSON_Delete(root);
        break;
    }

    default:
        break;
    }
}

/* ---- input ---- */

/* Held directions auto-repeat: first move immediately, then every
   5 frames after an 18-frame delay. `held` is any truthy condition
   (D-pad bit or analog stick past its deadzone). */
static int repeat_gate(int held, int *frames)
{
    if (!held) {
        *frames = 0;
        return 0;
    }
    (*frames)++;
    return *frames == 1 || (*frames > 18 && *frames % 5 == 0);
}

static int is_category(const char *id)
{
    return strncmp(id, "cat:", 4) == 0;
}

static void move_selection(int delta)
{
    if (view == VIEW_GUILD_LIST) {
        if (st.guild_count > 0) {
            st.guild_sel += delta;
            if (st.guild_sel < 0) st.guild_sel = 0;
            if (st.guild_sel >= st.guild_count) st.guild_sel = st.guild_count - 1;
        }
        return;
    }

    /* Workspace: UP/DOWN drives whichever pane holds the focus. */
    if (st.focus == FOCUS_CHANNELS && st.channel_count > 0) {
        /* Skip category header rows in the direction of travel. */
        int i = st.channel_sel + delta;
        while (i >= 0 && i < st.channel_count && is_category(st.channels[i].id))
            i += delta;
        if (i >= 0 && i < st.channel_count)
            st.channel_sel = i;
    } else if (st.focus == FOCUS_CHAT && st.message_count > 0) {
        /* UP (delta -1) digs into history, DOWN comes back to the newest. */
        st.chat_scroll -= delta;
        if (st.chat_scroll < 0) st.chat_scroll = 0;
        if (st.chat_scroll > st.message_count - 1)
            st.chat_scroll = st.message_count - 1;
        /* Nearing the top of what's loaded: fetch the previous chunk. */
        if (delta < 0 && st.chat_scroll >= st.message_count - 8)
            request_older_messages();
    } else if (st.focus == FOCUS_MEMBERS && st.member_count > 0) {
        st.member_scroll += delta;
        if (st.member_scroll < 0) st.member_scroll = 0;
        if (st.member_scroll > st.member_count - 1)
            st.member_scroll = st.member_count - 1;
    }
}

static void move_focus(int delta)
{
    if (view != VIEW_WORKSPACE)
        return;
    st.focus += delta;
    if (st.focus < FOCUS_CHANNELS) st.focus = FOCUS_CHANNELS;
    if (st.focus > FOCUS_MEMBERS)  st.focus = FOCUS_MEMBERS;
}

static void open_channel(const st_named *c)
{
    snprintf(st.channel_id, ST_ID_LEN, "%s", c->id);
    snprintf(st.channel_name, ST_NAME_LEN, "%s", c->name);
    st.message_count = 0;
    st.member_count = 0;
    st.chat_scroll = 0;
    st.member_scroll = 0;
    st.history_pending = 0;
    st.history_done = 0;
    st.typing_ttl = 0;
    st.expanded_image[0] = '\0';
    state_set_status(&st, "Loading messages...");
    set_active_channel(st.channel_id);   /* enables live push */
    request_messages(st.channel_id);
    request_members(st.channel_id);
    st.focus = FOCUS_CHAT;
}

static void confirm_selection(void)
{
    if (view == VIEW_GUILD_LIST && st.guild_count > 0) {
        const st_named *g = &st.guilds[st.guild_sel];
        snprintf(st.guild_id, ST_ID_LEN, "%s", g->id);
        snprintf(st.guild_name, ST_NAME_LEN, "%s", g->name);
        st.channel_count = 0;
        st.channel_sel = 0;
        st.channel_id[0] = '\0';
        st.channel_name[0] = '\0';
        st.message_count = 0;
        st.member_count = 0;
        st.focus = FOCUS_CHANNELS;
        state_set_status(&st, "Loading channels...");
        request_channels(st.guild_id);
        view = VIEW_WORKSPACE;
    } else if (view == VIEW_WORKSPACE && st.focus == FOCUS_CHANNELS &&
               st.channel_count > 0) {
        const st_named *c = &st.channels[st.channel_sel];
        if (is_category(c->id))
            return;              /* headers aren't channels */
        if (c->is_voice)
            toggle_voice(c);     /* voice channels: listen, don't open chat */
        else
            open_channel(c);
    }
}

static void go_back(void)
{
    if (view != VIEW_WORKSPACE)
        return;
    if (st.channel_id[0] != '\0')
        set_active_channel("0");             /* stop pushes for this channel */
    st.channel_id[0] = '\0';
    st.channel_name[0] = '\0';
    st.message_count = 0;
    st.member_count = 0;
    st.typing_ttl = 0;
    st.expanded_image[0] = '\0';
    state_set_status(&st, "");
    view = VIEW_GUILD_LIST;
}

/* Front-touch taps: open an attachment thumbnail full-size, tap again to
   close. Touch coordinates come in panel space (1920x1088), screen is
   half that. */
static void handle_touch(void)
{
    static int was_touching = 0;
    SceTouchData td;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0)
        return;
    int touching = td.reportNum > 0;
    if (touching && !was_touching) {
        int tx = td.report[0].x / 2;
        int ty = td.report[0].y / 2;
        if (st.expanded_image[0]) {
            st.expanded_image[0] = '\0';
        } else if (view == VIEW_WORKSPACE) {
            char url[ST_URL_LEN];
            if (ui_hit_image(tx, ty, url, sizeof(url)))
                snprintf(st.expanded_image, sizeof(st.expanded_image),
                         "%s", url);
        }
    }
    was_touching = touching;
}

static void handle_input(SceCtrlData *pad, SceCtrlData *prev)
{
    unsigned int pressed = pad->buttons & ~prev->buttons;
    static int up_frames = 0, down_frames = 0;

    /* The attachment viewer owns the input while it's open. */
    if (st.expanded_image[0]) {
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS))
            st.expanded_image[0] = '\0';
        if (pressed & SCE_CTRL_SELECT)
            running = 0;
        return;
    }

    /* D-pad or either analog stick, same auto-repeat cadence. */
    int up_held   = (pad->buttons & SCE_CTRL_UP)   || pad->ly < 60  || pad->ry < 60;
    int down_held = (pad->buttons & SCE_CTRL_DOWN) || pad->ly > 196 || pad->ry > 196;

    if (repeat_gate(up_held, &up_frames))
        move_selection(-1);
    if (repeat_gate(down_held, &down_frames))
        move_selection(1);

    if (pressed & SCE_CTRL_CROSS)
        confirm_selection();
    if (pressed & SCE_CTRL_CIRCLE)
        go_back();

    /* D-pad LEFT/RIGHT or the shoulder triggers hop between the workspace
       panes (channels | chat | members). */
    if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER))
        move_focus(-1);
    if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER))
        move_focus(1);

    if ((pressed & SCE_CTRL_TRIANGLE) && view == VIEW_WORKSPACE &&
        st.channel_id[0] != '\0') {
        state_set_status(&st, "Refreshing...");
        request_messages(st.channel_id);
        request_members(st.channel_id);
    }

    if ((pressed & SCE_CTRL_START) && view == VIEW_WORKSPACE &&
        st.channel_id[0] != '\0') {
        char title[ST_NAME_LEN + 16];
        snprintf(title, sizeof(title), "Message #%s", st.channel_name);
        if (ime_start(title, "") < 0)
            state_set_status(&st, "Keyboard unavailable");
    }

    if (pressed & SCE_CTRL_SELECT)
        running = 0;
}

/* ---- entry point ---- */

static int fatal(const char *text)
{
    ui_draw_status(text);
    sceKernelDelayThread(5 * 1000 * 1000);
    ui_term();
    sceKernelExitProcess(0);
    return 1;
}

/* Open the native keyboard and pump frames until the user confirms or
   cancels. Returns 0 with the text in out, -1 on cancel/failure. */
static int setup_prompt(const char *title, const char *initial,
                        char *out, int out_size)
{
    if (ime_start(title, initial) < 0)
        return -1;
    for (;;) {
        ui_draw_status(title);
        ime_status s = ime_update(out, out_size);
        if (s == IME_DONE)
            return 0;
        if (s == IME_CANCELED || s == IME_NONE)
            return -1;
    }
}

/* No config file yet: find the companion by LAN broadcast, or fall back to
   asking for the PC's IP with the keyboard. Saves the result so the next
   boot skips all of this. */
static void first_boot_setup(void)
{
    ui_draw_status("First boot - looking for the companion on your network...");

    char found_host[CFG_HOST_LEN];
    int found_port = 0, needs_code = 0;

    if (net_discover(found_host, sizeof(found_host),
                     &found_port, &needs_code) == 0) {
        snprintf(cfg.host, sizeof(cfg.host), "%s", found_host);
        if (found_port > 0)
            cfg.port = found_port;
        if (needs_code && cfg.pair_code[0] == '\0') {
            char code[CFG_CODE_LEN];
            if (setup_prompt("Pairing code (shown on the companion PC)", "",
                             code, sizeof(code)) == 0 && code[0] != '\0')
                snprintf(cfg.pair_code, sizeof(cfg.pair_code), "%s", code);
        }
    } else {
        char buf[CFG_HOST_LEN];
        if (setup_prompt("Companion PC IP (e.g. 192.168.1.42)", cfg.host,
                         buf, sizeof(buf)) == 0 && buf[0] != '\0')
            snprintf(cfg.host, sizeof(cfg.host), "%s", buf);

        char code[CFG_CODE_LEN];
        if (setup_prompt("Pairing code (leave empty if none)", cfg.pair_code,
                         code, sizeof(code)) == 0)
            snprintf(cfg.pair_code, sizeof(cfg.pair_code), "%s", code);
    }

    config_save_file(&cfg, CONFIG_PATH);
}

int main(void)
{
    ui_init();
    state_init(&st);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                             SCE_TOUCH_SAMPLING_STATE_START);

    config_defaults(&cfg, COMPANION_HOST, COMPANION_PORT, DAWNCORD_PAIR_CODE);
    if (config_load_file(&cfg, CONFIG_PATH) < 0)
        first_boot_setup();

    {
        char banner[96];
        snprintf(banner, sizeof(banner), "DawnCord - Connecting to %s:%d...",
                 cfg.host, cfg.port);
        ui_draw_status(banner);
    }

    if (net_connect(&conn, cfg.host, cfg.port) < 0) {
        /* The PC's IP may have changed (DHCP): try to rediscover it once. */
        ui_draw_status("Connection failed - searching the network for the companion...");
        char found_host[CFG_HOST_LEN];
        int found_port = 0, needs_code = 0;
        if (net_discover(found_host, sizeof(found_host),
                         &found_port, &needs_code) == 0) {
            snprintf(cfg.host, sizeof(cfg.host), "%s", found_host);
            if (found_port > 0)
                cfg.port = found_port;
            config_save_file(&cfg, CONFIG_PATH);
        }
        if (net_connect(&conn, cfg.host, cfg.port) < 0)
            return fatal("Connection failed. Is the companion running?\n"
                         "Config: ux0:data/dawncord/config.txt "
                         "(delete it to re-run setup)");
    }

    /* Handshake: the server requires HANDSHAKE as the FIRST frame. */
    {
        cJSON *hs = cJSON_CreateObject();
        if (cfg.pair_code[0] != '\0')
            cJSON_AddStringToObject(hs, "code", cfg.pair_code);
        char *body = cJSON_PrintUnformatted(hs);
        net_send_message(&conn, MSG_HANDSHAKE, body ? body : "{}");
        cJSON_free(body);
        cJSON_Delete(hs);
    }

    dawncord_message ack;
    if (net_recv_blocking(&conn, &ack) != 0 || ack.type != MSG_HANDSHAKE_ACK) {
        net_free_message(&ack);
        net_disconnect(&conn);
        return fatal("Handshake refused. Check pairing code.");
    }

    /* Companion protocol version: an old long-running process on the PC
       answers with obsolete semantics (scroll-back, typing, profile all
       missing or wrong). Warn instead of degrading confusingly. */
    int companion_version = 1;
    {
        cJSON *root = cJSON_Parse(ack.payload);
        const cJSON *v = root ?
            cJSON_GetObjectItemCaseSensitive(root, "version") : NULL;
        if (cJSON_IsNumber(v))
            companion_version = (int)v->valuedouble;
        cJSON_Delete(root);
    }
    net_free_message(&ack);

    /* Start the background receiver AFTER the handshake, then request guilds.
       The outdated-companion warning is a sticky flag, not a status line:
       a status gets overwritten by the next event and nobody sees it. */
    st.companion_old = companion_version < 2;
    net_start_receiver(&conn);
    state_set_status(&st, "Loading servers...");
    net_send_message(&conn, MSG_REQUEST_GUILDS, "{}");

    SceCtrlData pad, prev;
    memset(&prev, 0, sizeof(prev));

    int was_connected = 1;

    while (running) {
        sceCtrlPeekBufferPositive(0, &pad, 1);

        /* The IME overlay owns the buttons while it's up. */
        if (!ime_active()) {
            handle_input(&pad, &prev);
            handle_touch();
        }
        prev = pad;

        char typed[IME_MAX_INPUT * 3 + 1];
        ime_status is = ime_update(typed, sizeof(typed));
        if (is == IME_DONE && typed[0] != '\0')
            send_chat_message(typed);

        /* Drain every message queued by the receiver thread this frame. */
        dawncord_message msg;
        while (net_poll_message(&conn, &msg)) {
            process_server_message(&msg);
            net_free_message(&msg);
        }

        if (was_connected && !conn.rx_running) {
            was_connected = 0;
            state_set_status(&st, "Disconnected from companion");
        }

        if (st.typing_ttl > 0)
            st.typing_ttl--;

        ui_render(&st, view);
        img_gc();   /* frees evicted textures once the GPU is done with them */

        /* Fetch whatever the renderer just discovered it's missing
           (avatars, icons, attachment thumbnails). Cache keys are either a
           bare URL (avatar, 64px, drawn round) or "url#size" for
           attachments; the companion gets the real URL plus the size. */
        char img_key[IMG_KEY_LEN];
        for (int k = 0; k < 3 && img_next_request(img_key, sizeof(img_key)); k++) {
            char url[IMG_KEY_LEN];
            int size = 64;
            snprintf(url, sizeof(url), "%s", img_key);
            char *hash = strrchr(url, '#');
            if (hash && hash[1]) {
                size = atoi(hash + 1);
                *hash = '\0';
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "url", url);
            cJSON_AddStringToObject(o, "key", img_key);
            cJSON_AddNumberToObject(o, "size", size);
            send_json(MSG_REQUEST_IMAGE, o);
        }
    }

    voice_stop();
    net_disconnect(&conn);
    ui_term();
    sceKernelExitProcess(0);
    return 0;
}
