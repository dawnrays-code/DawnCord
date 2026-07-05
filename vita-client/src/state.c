#include "state.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "cJSON.h"

/* snprintf truncation can cut a multi-byte UTF-8 sequence in half, which the
   PGF renderer would draw as garbage. Walk back over any incomplete tail. */
static void trim_utf8_tail(char *s)
{
    size_t len = strlen(s);
    while (len > 0) {
        unsigned char c = (unsigned char)s[len - 1];
        if (c < 0x80)
            return;               /* ASCII tail: fine */
        if (c >= 0xC0) {          /* lead byte: check its sequence is whole */
            int need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
            size_t have = strlen(s) - (len - 1);
            if (have < (size_t)need)
                s[len - 1] = '\0';
            return;
        }
        len--;                    /* continuation byte: keep walking back */
    }
}

/* Bounded copy done by hand: msvcrt's snprintf (used when this file is
   built for the host tests) doesn't NUL-terminate on truncation. */
static void copy_str(char *dst, size_t size, const char *src)
{
    if (!src)
        src = "";
    size_t n = strlen(src);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    trim_utf8_tail(dst);
}

static void copy_json_str(char *dst, size_t size, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    copy_str(dst, size, cJSON_IsString(item) ? item->valuestring : NULL);
}

/* IDs should always arrive as strings, but accept numbers as a fallback
   (small IDs like the DM pseudo-guild 0 are exact in a double anyway). */
static void copy_json_id(char *dst, size_t size, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        copy_str(dst, size, item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        snprintf(dst, size, "%.0f", item->valuedouble);
        dst[size - 1] = '\0';
    } else {
        copy_str(dst, size, "0");
    }
}

void state_init(app_state *st)
{
    memset(st, 0, sizeof(*st));
}

void state_set_status(app_state *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->status, sizeof(st->status), fmt, ap);
    va_end(ap);
    st->status[sizeof(st->status) - 1] = '\0';
    trim_utf8_tail(st->status);
}

static int parse_named_list(const char *json, const char *list_key,
                            st_named *out, int max, int *count)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, list_key);
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return -1;
    }

    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (n >= max)
            break;
        copy_json_id(out[n].id, ST_ID_LEN, item, "id");
        copy_json_str(out[n].name, ST_NAME_LEN, item, "name");
        copy_json_str(out[n].icon, ST_URL_LEN, item, "icon");
        char type[12];
        copy_json_str(type, sizeof(type), item, "type");
        out[n].is_voice = (strcmp(type, "voice") == 0);
        n++;
    }
    *count = n;
    cJSON_Delete(root);
    return 0;
}

int state_parse_guilds(app_state *st, const char *json)
{
    if (parse_named_list(json, "guilds", st->guilds, ST_MAX_GUILDS,
                         &st->guild_count) < 0)
        return -1;
    if (st->guild_sel >= st->guild_count)
        st->guild_sel = 0;

    /* Optional "me": who the companion is logged in as (profile box). */
    cJSON *root = cJSON_Parse(json);
    if (root) {
        const cJSON *me = cJSON_GetObjectItemCaseSensitive(root, "me");
        if (cJSON_IsObject(me)) {
            copy_json_str(st->self_name, ST_AUTHOR_LEN, me, "name");
            copy_json_str(st->self_avatar, ST_URL_LEN, me, "avatar");
        }
        cJSON_Delete(root);
    }
    return 0;
}

int state_parse_typing(app_state *st, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    char chan[ST_ID_LEN];
    copy_json_id(chan, sizeof(chan), root, "channel_id");
    if (strcmp(chan, st->channel_id) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    copy_json_str(st->typing_name, ST_AUTHOR_LEN, root, "name");
    st->typing_ttl = 8 * 60;   /* ~8 seconds at 60fps, like Discord */
    cJSON_Delete(root);
    return 0;
}

int state_parse_channels(app_state *st, const char *json)
{
    if (parse_named_list(json, "channels", st->channels, ST_MAX_CHANNELS,
                         &st->channel_count) < 0)
        return -1;
    if (st->channel_sel >= st->channel_count)
        st->channel_sel = 0;
    /* Category rows (id "cat:...") are headers, not selectable: if the
       initial selection lands on one, advance to the first real channel. */
    while (st->channel_sel < st->channel_count &&
           strncmp(st->channels[st->channel_sel].id, "cat:", 4) == 0)
        st->channel_sel++;
    if (st->channel_sel >= st->channel_count)
        st->channel_sel = 0;
    return 0;
}

static void fill_message(st_message *m, const cJSON *item)
{
    copy_json_id(m->id, ST_ID_LEN, item, "id");
    copy_json_str(m->author, ST_AUTHOR_LEN, item, "author");
    copy_json_str(m->avatar, ST_URL_LEN, item, "avatar");
    copy_json_str(m->content, ST_CONTENT_LEN, item, "content");
    copy_json_str(m->image, ST_URL_LEN, item, "image");
    m->video = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "video"));

    m->embed_count = 0;
    const cJSON *embeds = cJSON_GetObjectItemCaseSensitive(item, "embeds");
    if (cJSON_IsArray(embeds)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, embeds) {
            if (m->embed_count >= ST_MAX_EMBEDS)
                break;
            st_embed *out = &m->embeds[m->embed_count];
            copy_json_str(out->title, ST_EMBED_TITLE_LEN, e, "title");
            copy_json_str(out->desc, ST_EMBED_DESC_LEN, e, "description");
            const cJSON *col = cJSON_GetObjectItemCaseSensitive(e, "color");
            out->color = cJSON_IsNumber(col) ? (unsigned int)col->valuedouble : 0;
            if (out->title[0] || out->desc[0])
                m->embed_count++;
        }
    }

    /* "2026-07-04T15:04:05.123+00:00" -> "15:04" */
    char ts[32];
    copy_json_str(ts, sizeof(ts), item, "timestamp");
    if (strlen(ts) >= 16)
        snprintf(m->time, ST_TIME_LEN, "%.5s", ts + 11);
    else
        m->time[0] = '\0';

    const cJSON *att = cJSON_GetObjectItemCaseSensitive(item, "attachments");
    m->attachments = cJSON_IsArray(att) ? cJSON_GetArraySize(att) : 0;
    /* A lone image attachment is drawn as a thumbnail: the "[file: x]"
       tag would only repeat what the eye already sees. */
    if (m->attachments == 1 && m->image[0] != '\0')
        return;
    if (m->attachments > 0) {
        char fname[64];
        copy_json_str(fname, sizeof(fname), cJSON_GetArrayItem((cJSON *)att, 0),
                      "filename");
        char tag[96];
        if (m->attachments > 1)
            snprintf(tag, sizeof(tag), "[file: %s +%d]", fname, m->attachments - 1);
        else
            snprintf(tag, sizeof(tag), "[file: %s]", fname);
        tag[sizeof(tag) - 1] = '\0';

        if (m->content[0] == '\0') {
            copy_str(m->content, ST_CONTENT_LEN, tag);
        } else {
            size_t len = strlen(m->content);
            if (len + 2 < ST_CONTENT_LEN) {
                m->content[len] = '\n';
                copy_str(m->content + len + 1, ST_CONTENT_LEN - len - 1, tag);
            }
        }
    }
}

/* Older-history chunk (the companion echoes "before"): slide the loaded
   window up by prepending. The chunk arrives oldest-first and ends right
   before the current top, so order stays contiguous. chat_scroll counts
   from the bottom and is untouched: the view doesn't move. */
static int prepend_chunk(app_state *st, const cJSON *list)
{
    int total = cJSON_GetArraySize(list);
    int room = ST_MAX_MESSAGES - st->message_count;
    int n = total < room ? total : room;

    st->history_pending = 0;
    if (n == 0) {
        /* Nothing older, or window at capacity: stop asking. */
        st->history_done = 1;
        return 0;
    }

    memmove(&st->messages[n], &st->messages[0],
            sizeof(st_message) * st->message_count);
    /* Keep the newest part of the chunk (contiguous with the old top). */
    int skip = total - n, i = 0, out = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (i++ < skip)
            continue;
        fill_message(&st->messages[out++], item);
    }
    st->message_count += n;
    if (total < ST_HISTORY_CHUNK)
        st->history_done = 1;  /* short chunk: channel history exhausted */
    return 0;
}

int state_parse_messages(app_state *st, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    /* Replies are routed by type, so a slow MESSAGE_LIST for a channel we
       already left could arrive now: only accept the open channel's. */
    char chan[ST_ID_LEN];
    copy_json_id(chan, sizeof(chan), root, "channel_id");
    if (strcmp(chan, st->channel_id) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return -1;
    }

    if (cJSON_HasObjectItem(root, "before")) {
        int r = prepend_chunk(st, list);
        cJSON_Delete(root);
        return r;
    }

    /* Oldest-first from the companion; keep only the newest window. */
    int total = cJSON_GetArraySize(list);

    /* An outdated companion ignores "before" and answers a scroll-back
       request with the newest window again, unlabelled. Swallowing that
       as a fresh list would snap the view to the bottom (the chat seems
       to loop). If a chunk is pending and this "fresh" list ends on the
       newest message we already have, it's that echo: drop it. */
    if (st->history_pending && total > 0 && st->message_count > 0) {
        char last_id[ST_ID_LEN];
        copy_json_id(last_id, sizeof(last_id),
                     cJSON_GetArrayItem((cJSON *)list, total - 1), "id");
        if (strcmp(last_id, st->messages[st->message_count - 1].id) == 0) {
            st->history_pending = 0;
            st->history_done = 1;
            st->companion_old = 1;   /* caught red-handed */
            cJSON_Delete(root);
            return -1;
        }
    }

    int skip = total > ST_MAX_MESSAGES ? total - ST_MAX_MESSAGES : 0;

    int n = 0, i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (i++ < skip)
            continue;
        fill_message(&st->messages[n++], item);
    }
    st->message_count = n;
    st->chat_scroll = 0;   /* fresh list: snap back to the newest messages */
    st->history_pending = 0;
    st->history_done = 0;
    cJSON_Delete(root);
    return 0;
}

static st_presence parse_presence(const cJSON *item)
{
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "status");
    if (cJSON_IsString(s)) {
        if (strcmp(s->valuestring, "online") == 0) return ST_STATUS_ONLINE;
        if (strcmp(s->valuestring, "idle") == 0)   return ST_STATUS_IDLE;
        if (strcmp(s->valuestring, "dnd") == 0)    return ST_STATUS_DND;
    }
    return ST_STATUS_OFFLINE;
}

int state_parse_members(app_state *st, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    /* Same stale-reply guard as messages: only the open channel's list. */
    char chan[ST_ID_LEN];
    copy_json_id(chan, sizeof(chan), root, "channel_id");
    if (strcmp(chan, st->channel_id) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "members");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return -1;
    }

    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (n >= ST_MAX_MEMBERS)
            break;
        copy_json_str(st->members[n].name, ST_AUTHOR_LEN, item, "name");
        st->members[n].status = parse_presence(item);
        n++;
    }
    st->member_count = n;
    st->member_scroll = 0;
    cJSON_Delete(root);
    return 0;
}

int state_push_message(app_state *st, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    char chan[ST_ID_LEN];
    copy_json_id(chan, sizeof(chan), root, "channel_id");
    if (strcmp(chan, st->channel_id) != 0) {
        cJSON_Delete(root);
        return 0;
    }

    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!cJSON_IsObject(msg)) {
        cJSON_Delete(root);
        return -1;
    }

    if (st->message_count == ST_MAX_MESSAGES) {
        memmove(&st->messages[0], &st->messages[1],
                sizeof(st_message) * (ST_MAX_MESSAGES - 1));
        st->message_count--;
    }
    fill_message(&st->messages[st->message_count++], msg);
    /* If the user is reading history, don't yank the view: the new message
       lands at the bottom and the offset grows by one to compensate. */
    if (st->chat_scroll > 0 && st->chat_scroll < st->message_count - 1)
        st->chat_scroll++;
    cJSON_Delete(root);
    return 1;
}
