#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/stat.h>
#endif

static void copy_bounded(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void config_defaults(dawncord_config *cfg, const char *host, int port,
                     const char *pair_code)
{
    copy_bounded(cfg->host, sizeof(cfg->host), host);
    cfg->port = port;
    copy_bounded(cfg->pair_code, sizeof(cfg->pair_code), pair_code);
}

/* Trim leading/trailing spaces, tabs and CR/LF in place; returns start. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

int config_load_file(dawncord_config *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';

        const char *key = trim(line);
        const char *val = trim(eq + 1);
        if (*val == '\0')
            continue;

        if (strcmp(key, "host") == 0) {
            copy_bounded(cfg->host, sizeof(cfg->host), val);
        } else if (strcmp(key, "port") == 0) {
            int p = atoi(val);
            if (p > 0 && p <= 65535)
                cfg->port = p;
        } else if (strcmp(key, "code") == 0) {
            copy_bounded(cfg->pair_code, sizeof(cfg->pair_code), val);
        }
    }

    fclose(f);
    return 0;
}

int config_save_file(const dawncord_config *cfg, const char *path)
{
#ifdef __vita__
    /* Ignore the result: the directory usually exists already. */
    sceIoMkdir(CONFIG_DIR, 0777);
#endif
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "# Written by DawnCord's setup. Delete this file to run it again.\n");
    fprintf(f, "host=%s\n", cfg->host);
    fprintf(f, "port=%d\n", cfg->port);
    if (cfg->pair_code[0] != '\0')
        fprintf(f, "code=%s\n", cfg->pair_code);

    fclose(f);
    return 0;
}
