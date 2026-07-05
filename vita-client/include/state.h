#ifndef DAWNCORD_STATE_H
#define DAWNCORD_STATE_H

/* Parsed application state. All snowflake IDs are kept as strings: they are
   64-bit values that don't survive a round-trip through cJSON's double
   representation, so the companion sends them as strings and we never
   convert them to numbers. */

#define ST_MAX_GUILDS   64
#define ST_MAX_CHANNELS 128
#define ST_MAX_MESSAGES 200  /* newest window + scroll-back chunks */
#define ST_HISTORY_CHUNK 40  /* messages per scroll-back request */
#define ST_MAX_MEMBERS  40
#define ST_ID_LEN       24
#define ST_NAME_LEN     64
#define ST_AUTHOR_LEN   40
#define ST_TIME_LEN     6
#define ST_CONTENT_LEN  512
#define ST_URL_LEN      336  /* CDN urls (attachments carry long signature
                                query params); matches IMG_KEY_LEN */
#define ST_STATUS_LEN   128

typedef struct {
    char id[ST_ID_LEN];
    char name[ST_NAME_LEN];
    char icon[ST_URL_LEN];       /* "" when the entry has no icon */
    int is_voice;                /* channel entries only: a voice channel */
} st_named;

#define ST_MAX_EMBEDS       2
#define ST_EMBED_TITLE_LEN  96
#define ST_EMBED_DESC_LEN   224

typedef struct {
    char title[ST_EMBED_TITLE_LEN];
    char desc[ST_EMBED_DESC_LEN];
    unsigned int color;          /* 0xRRGGBB from Discord, 0 = default */
} st_embed;

typedef struct {
    char id[ST_ID_LEN];          /* snowflake; anchors scroll-back paging */
    char author[ST_AUTHOR_LEN];
    char avatar[ST_URL_LEN];     /* "" when unknown */
    char time[ST_TIME_LEN];      /* "HH:MM" from the ISO timestamp */
    char content[ST_CONTENT_LEN];
    char image[ST_URL_LEN];      /* first image attachment/embed, "" if none */
    int video;                   /* image is a video preview: draw a play mark */
    st_embed embeds[ST_MAX_EMBEDS];
    int embed_count;
    int attachments;
} st_message;

/* Presence buckets for the member rail. Unknown statuses (big guilds where
   the companion's presence cache is cold) land in OFFLINE. */
typedef enum {
    ST_STATUS_OFFLINE = 0,
    ST_STATUS_ONLINE,
    ST_STATUS_IDLE,
    ST_STATUS_DND,
} st_presence;

typedef struct {
    char name[ST_AUTHOR_LEN];
    st_presence status;
} st_member;

typedef struct {
    st_named guilds[ST_MAX_GUILDS];
    int guild_count;
    int guild_sel;

    st_named channels[ST_MAX_CHANNELS];
    int channel_count;
    int channel_sel;

    st_message messages[ST_MAX_MESSAGES];
    int message_count;
    int chat_scroll;   /* messages skipped from the bottom: 0 = newest */

    /* Scroll-back state: a chunk request for older history is in flight /
       there is nothing older to load (or the window is at capacity). */
    int history_pending;
    int history_done;

    /* The companion answered with pre-v2 protocol semantics (old process
       or old exe): warn persistently instead of degrading in silence. */
    int companion_old;

    st_member members[ST_MAX_MEMBERS];
    int member_count;
    int member_scroll; /* first visible row of the member rail */

    /* Which workspace pane has input focus: 0 channels, 1 chat, 2 members.
       Lives here so the renderer can highlight the focused pane. */
    int focus;

    /* Logged-in user, from the GUILD_LIST payload: fills the profile box
       at the bottom of the channel rail. */
    char self_name[ST_AUTHOR_LEN];
    char self_avatar[ST_URL_LEN];

    /* "<name> is typing..." above the input bar; ttl counts down in
       frames and the label disappears at zero. */
    char typing_name[ST_AUTHOR_LEN];
    int typing_ttl;

    /* Attachment viewer overlay: URL of the image being shown full-size,
       "" when closed. */
    char expanded_image[ST_URL_LEN];

    /* Voice: the channel currently being listened to, "" if none. */
    char voice_id[ST_ID_LEN];
    char voice_name[ST_NAME_LEN];

    /* Currently open guild/channel (set on selection, matched against
       incoming payloads so stale replies for a previous channel are
       ignored). */
    char guild_id[ST_ID_LEN];
    char guild_name[ST_NAME_LEN];
    char channel_id[ST_ID_LEN];
    char channel_name[ST_NAME_LEN];

    char status[ST_STATUS_LEN];
} app_state;

void state_init(app_state *st);
void state_set_status(app_state *st, const char *fmt, ...);

/* Each parser returns 0 on success, -1 on malformed/irrelevant payload. */
int state_parse_guilds(app_state *st, const char *json);
int state_parse_channels(app_state *st, const char *json);
int state_parse_messages(app_state *st, const char *json);
int state_parse_members(app_state *st, const char *json);

/* TYPING push: 0 if for the open channel (label set), -1 otherwise. */
int state_parse_typing(app_state *st, const char *json);

/* MESSAGE_NEW push. Returns 1 if it was for the open channel (appended),
   0 if it was for another channel (ignored), -1 on parse error. */
int state_push_message(app_state *st, const char *json);

#endif
