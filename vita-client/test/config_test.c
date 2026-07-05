/* Host-side test for config.c. Build & run:

     gcc -Iinclude test/config_test.c src/config.c -o config_test && ./config_test
*/

#include "config.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *label)
{
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", label);
    if (!cond)
        failures++;
}

static const char *write_tmp(const char *content)
{
    /* relative path: msvcrt's tmpnam points at the drive root */
    static char path[32];
    static int counter = 0;
    snprintf(path, sizeof(path), "config_test_%d.tmp", counter++);
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
    return path;
}

int main(void)
{
    dawncord_config cfg;

    config_defaults(&cfg, "10.0.0.1", 9100, "");
    check(strcmp(cfg.host, "10.0.0.1") == 0 && cfg.port == 9100 &&
          cfg.pair_code[0] == '\0', "defaults applied");

    check(config_load_file(&cfg, "does/not/exist.txt") == -1,
          "missing file reported");
    check(strcmp(cfg.host, "10.0.0.1") == 0, "missing file leaves defaults");

    const char *p = write_tmp(
        "# DawnCord config\r\n"
        "host = 192.168.1.42   # companion PC\r\n"
        "port=9101\n"
        "code=  segreto\n"
        "\n"
        "garbage line without equals\n"
        "port=99999\n"          /* out of range: ignored */
        "unknownkey=x\n");
    check(config_load_file(&cfg, p) == 0, "file loads");
    check(strcmp(cfg.host, "192.168.1.42") == 0,
          "host trimmed of spaces, CR and comment");
    check(cfg.port == 9101, "port parsed");
    check(strcmp(cfg.pair_code, "segreto") == 0, "code trimmed");
    remove(p);

    /* keys present but empty keep previous values */
    p = write_tmp("host=\nport=\n");
    config_load_file(&cfg, p);
    check(strcmp(cfg.host, "192.168.1.42") == 0 && cfg.port == 9101,
          "empty values ignored");
    remove(p);

    /* oversized host is truncated, not overflowed */
    {
        char big[256] = "host=";
        for (int i = 5; i < 200; i++)
            big[i] = 'a';
        big[200] = '\0';
        strcat(big, "\n");
        p = write_tmp(big);
        config_load_file(&cfg, p);
        check(strlen(cfg.host) == CFG_HOST_LEN - 1, "long host truncated safely");
        remove(p);
    }

    printf("\n%s\n", failures ? "FAILED" : "All config tests passed.");
    return failures ? 1 : 0;
}
