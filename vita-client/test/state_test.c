/* Host-side test for state.c (no VitaSDK needed — state.c only uses
   libc + cJSON). Build & run:

     tcc -Iinclude -Isrc/cjson -run test/state_test.c src/state.c src/cjson/cJSON.c
   or
     gcc -Iinclude -Isrc/cjson test/state_test.c src/state.c src/cjson/cJSON.c -o state_test && ./state_test
*/

#include "state.h"
#include "b64.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void check(int cond, const char *label)
{
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
    if (!cond)
        failures++;
}

int main(void)
{
    app_state st;
    state_init(&st);

    /* guilds: IDs beyond 2^53 must survive byte-for-byte */
    check(state_parse_guilds(&st,
        "{\"guilds\":[{\"id\":\"0\",\"name\":\"Direct Messages\",\"icon\":null},"
        "{\"id\":\"1181234567890123456\",\"name\":\"ZZZ Italia\",\"icon\":null}]}") == 0,
        "guild list parses");
    check(st.guild_count == 2, "guild count");
    check(strcmp(st.guilds[1].id, "1181234567890123456") == 0,
          "64-bit snowflake preserved exactly");

    /* legacy numeric id fallback (small values only) */
    state_parse_guilds(&st, "{\"guilds\":[{\"id\":0,\"name\":\"DMs\"}]}");
    check(strcmp(st.guilds[0].id, "0") == 0, "numeric id fallback");

    /* channels */
    check(state_parse_channels(&st,
        "{\"guild_id\":\"1181234567890123456\",\"channels\":"
        "[{\"id\":\"42\",\"name\":\"generale\",\"type\":\"text\"}]}") == 0,
        "channel list parses");
    check(strcmp(st.channels[0].name, "generale") == 0, "channel name");

    /* messages: only accepted for the open channel */
    snprintf(st.channel_id, ST_ID_LEN, "%s", "42");
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":[") == -1,
        "malformed json rejected");
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":["
        "{\"id\":\"1\",\"author\":\"alice\",\"author_id\":\"7\",\"content\":\"ciao\","
        "\"avatar\":\"https://cdn.discordapp.com/avatars/7/abc.png\","
        "\"timestamp\":\"2026-07-04T15:04:05.123456+00:00\",\"attachments\":[]},"
        "{\"id\":\"2\",\"author\":\"bob\",\"author_id\":\"8\",\"content\":\"\","
        "\"avatar\":null,"
        "\"timestamp\":\"\",\"attachments\":[{\"url\":\"u\",\"filename\":\"f.png\"}]}"
        "]}") == 0, "message list parses");
    check(st.message_count == 2, "message count");
    check(strcmp(st.messages[0].avatar,
                 "https://cdn.discordapp.com/avatars/7/abc.png") == 0,
          "avatar url parsed");
    check(st.messages[1].avatar[0] == '\0', "null avatar becomes empty string");
    check(strcmp(st.messages[0].time, "15:04") == 0, "HH:MM extracted");
    check(strcmp(st.messages[1].content, "[file: f.png]") == 0,
          "attachment-only message shows filename");

    check(state_parse_messages(&st,
        "{\"channel_id\":\"999\",\"messages\":[]}") == -1,
        "stale reply for another channel ignored");
    check(st.message_count == 2, "state untouched by stale reply");

    /* push routing */
    check(state_push_message(&st,
        "{\"channel_id\":\"999\",\"message\":{\"author\":\"x\",\"content\":\"y\"}}") == 0,
        "push for other channel ignored");
    check(state_push_message(&st,
        "{\"channel_id\":\"42\",\"message\":{\"author\":\"carol\",\"content\":\"nuovo\","
        "\"timestamp\":\"2026-07-04T16:00:00+00:00\",\"attachments\":[]}}") == 1,
        "push for open channel appended");
    check(st.message_count == 3 &&
          strcmp(st.messages[2].author, "carol") == 0, "pushed message content");

    /* rolling window: fill to the cap, then push once more */
    for (int i = st.message_count; i < ST_MAX_MESSAGES; i++)
        state_push_message(&st,
            "{\"channel_id\":\"42\",\"message\":{\"author\":\"fill\",\"content\":\"n\"}}");
    check(st.message_count == ST_MAX_MESSAGES, "filled to cap");
    state_push_message(&st,
        "{\"channel_id\":\"42\",\"message\":{\"author\":\"last\",\"content\":\"n\"}}");
    check(st.message_count == ST_MAX_MESSAGES &&
          strcmp(st.messages[ST_MAX_MESSAGES - 1].author, "last") == 0 &&
          strcmp(st.messages[0].author, "bob") == 0,
          "window slides: oldest dropped, newest kept");

    /* scroll-back paging: at capacity, a chunk can't fit -> done flag */
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"before\":\"999\",\"messages\":["
        "{\"id\":\"1\",\"author\":\"x\",\"content\":\"y\"}]}") == 0,
        "chunk at capacity accepted");
    check(st.message_count == ST_MAX_MESSAGES && st.history_done == 1,
        "full window: count unchanged, history marked done");

    /* fresh list resets the scroll-back flags and the window */
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":["
        "{\"id\":\"20\",\"author\":\"now1\",\"content\":\"a\"},"
        "{\"id\":\"21\",\"author\":\"now2\",\"content\":\"b\"}]}") == 0,
        "fresh list after chunks");
    check(st.message_count == 2 && st.history_done == 0 &&
          strcmp(st.messages[0].id, "20") == 0,
          "fresh list resets window, flags and keeps ids");

    /* older chunk prepends without moving the view */
    st.chat_scroll = 1;
    st.history_pending = 1;
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"before\":\"20\",\"messages\":["
        "{\"id\":\"17\",\"author\":\"old1\",\"content\":\"p\"},"
        "{\"id\":\"18\",\"author\":\"old2\",\"content\":\"q\"}]}") == 0,
        "older chunk parses");
    check(st.message_count == 4 &&
          strcmp(st.messages[0].author, "old1") == 0 &&
          strcmp(st.messages[2].author, "now1") == 0,
          "chunk prepended in order before the old top");
    check(st.chat_scroll == 1 && st.history_pending == 0,
          "view offset untouched, pending cleared");
    check(st.history_done == 1,
          "short chunk (below ST_HISTORY_CHUNK) ends the history");

    /* old-companion echo: a pending chunk request answered with the same
       newest window, unlabelled, must be dropped (not treated as fresh:
       that snapped the view to the bottom, the "chat loop" bug) */
    st.history_pending = 1;
    st.history_done = 0;
    st.chat_scroll = 3;
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":["
        "{\"id\":\"20\",\"author\":\"now1\",\"content\":\"a\"},"
        "{\"id\":\"21\",\"author\":\"now2\",\"content\":\"b\"}]}") == -1,
        "unlabelled echo of the current window dropped while pending");
    check(st.message_count == 4 && st.chat_scroll == 3 &&
          st.history_pending == 0 && st.history_done == 1,
        "echo drop: window and view untouched, history closed");
    check(st.companion_old == 1,
        "echo drop flags the companion as outdated");

    /* a genuinely fresh list (different newest id) still replaces */
    st.history_pending = 1;
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":["
        "{\"id\":\"40\",\"author\":\"fresh\",\"content\":\"c\"}]}") == 0,
        "genuine fresh list while pending accepted");
    check(st.message_count == 1 && st.chat_scroll == 0,
        "fresh list replaced the window");

    /* logged-in user ("me") for the profile box */
    check(state_parse_guilds(&st,
        "{\"guilds\":[{\"id\":\"0\",\"name\":\"DMs\"}],"
        "\"me\":{\"name\":\"dawnrays\",\"avatar\":null}}") == 0,
        "guild list with me parses");
    check(strcmp(st.self_name, "dawnrays") == 0 && st.self_avatar[0] == '\0',
        "me name stored, null avatar empty");

    /* image attachments: thumbnail url stored, redundant tag dropped */
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":["
        "{\"id\":\"30\",\"author\":\"pic\",\"content\":\"\","
        "\"image\":\"https://cdn.discordapp.com/attachments/1/2/m.jpg?ex=abc\","
        "\"attachments\":[{\"url\":\"u\",\"filename\":\"m.jpg\"}]},"
        "{\"id\":\"31\",\"author\":\"mix\",\"content\":\"guarda\","
        "\"image\":\"https://cdn.discordapp.com/attachments/1/3/n.png\","
        "\"attachments\":[{\"url\":\"u\",\"filename\":\"n.png\"},"
        "{\"url\":\"v\",\"filename\":\"doc.pdf\"}]}]}") == 0,
        "messages with images parse");
    check(strstr(st.messages[0].image, "m.jpg") != NULL &&
          st.messages[0].content[0] == '\0',
        "single image: url stored, no redundant [file] tag");
    check(strstr(st.messages[1].content, "[file:") != NULL,
        "multiple attachments keep the tag");

    /* embeds: structured boxes, empty ones skipped */
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":["
        "{\"id\":\"50\",\"author\":\"bot\",\"content\":\"\","
        "\"embeds\":[{\"title\":\"Patch 2.4\",\"description\":\"riga uno\\nriga due\","
        "\"color\":5793266},{\"title\":\"\",\"description\":\"\"}]}]}") == 0,
        "message with embeds parses");
    check(st.messages[0].embed_count == 1 &&
          strcmp(st.messages[0].embeds[0].title, "Patch 2.4") == 0 &&
          st.messages[0].embeds[0].color == 5793266,
        "embed title and color stored, empty embed skipped");

    /* video previews carry a play-mark flag */
    check(state_parse_messages(&st,
        "{\"channel_id\":\"42\",\"messages\":["
        "{\"id\":\"60\",\"author\":\"yt\",\"content\":\"\",\"video\":true,"
        "\"image\":\"https://i.ytimg.com/vi/x/hq720.jpg\"}]}") == 0,
        "video message parses");
    check(st.messages[0].video == 1 && st.messages[0].image[0] != '\0',
        "video flag and preview url stored");

    /* typing push: guard + label */
    check(state_parse_typing(&st,
        "{\"channel_id\":\"999\",\"name\":\"ghost\"}") == -1,
        "typing for another channel ignored");
    check(st.typing_ttl == 0, "ttl untouched by stale typing");
    check(state_parse_typing(&st,
        "{\"channel_id\":\"42\",\"name\":\"alice\"}") == 0,
        "typing for open channel accepted");
    check(strcmp(st.typing_name, "alice") == 0 && st.typing_ttl > 0,
        "typing label set with a timeout");

    /* member list: stale-reply guard + presence buckets */
    check(state_parse_members(&st,
        "{\"channel_id\":\"999\",\"members\":[{\"name\":\"x\",\"status\":\"online\"}]}") == -1,
        "member list for another channel ignored");
    check(st.member_count == 0, "members untouched by stale reply");
    check(state_parse_members(&st,
        "{\"channel_id\":\"42\",\"members\":["
        "{\"name\":\"alice\",\"status\":\"online\"},"
        "{\"name\":\"bob\",\"status\":\"idle\"},"
        "{\"name\":\"carol\",\"status\":\"dnd\"},"
        "{\"name\":\"dave\",\"status\":\"offline\"},"
        "{\"name\":\"eve\",\"status\":\"streaming\"}]}") == 0,
        "member list parses");
    check(st.member_count == 5 &&
          strcmp(st.members[0].name, "alice") == 0, "member names parsed");
    check(st.members[0].status == ST_STATUS_ONLINE &&
          st.members[1].status == ST_STATUS_IDLE &&
          st.members[2].status == ST_STATUS_DND &&
          st.members[3].status == ST_STATUS_OFFLINE &&
          st.members[4].status == ST_STATUS_OFFLINE,
          "presence buckets mapped, unknown falls back to offline");

    /* UTF-8 truncation safety: a name longer than ST_NAME_LEN ending in
       multi-byte chars must not keep a half sequence */
    {
        char json[512];
        char name[128];
        int i = 0;
        while (i < 62) name[i++] = 'a';   /* 62 ascii + 2-byte char straddles 63 */
        name[i] = '\0';
        strcat(name, "\xC3\xA8");         /* è */
        snprintf(json, sizeof(json),
                 "{\"guilds\":[{\"id\":\"1\",\"name\":\"%s\"}]}", name);
        state_parse_guilds(&st, json);
        size_t len = strlen(st.guilds[0].name);
        check(len == 62, "truncated mid-sequence UTF-8 byte dropped");
    }

    /* status formatting */
    state_set_status(&st, "%d servers", 5);
    check(strcmp(st.status, "5 servers") == 0, "status formatting");

    /* base64 (image payloads) */
    {
        uint8_t buf[32];
        int n = b64_decode("aGVsbG8=", buf, sizeof(buf));
        check(n == 5 && memcmp(buf, "hello", 5) == 0, "b64 basic decode");
        n = b64_decode("/9g=", buf, sizeof(buf));
        check(n == 2 && buf[0] == 0xFF && buf[1] == 0xD8, "b64 jpeg magic");
        n = b64_decode("aGVs\nbG8=", buf, sizeof(buf));
        check(n == 5 && memcmp(buf, "hello", 5) == 0, "b64 ignores whitespace");
        check(b64_decode("a!b", buf, sizeof(buf)) == -1, "b64 invalid char rejected");
        check(b64_decode("aGVsbG8=", buf, 3) == -1, "b64 output overflow rejected");
    }

    printf("\n%s\n", failures ? "FAILED" : "All state tests passed.");
    return failures ? 1 : 0;
}
