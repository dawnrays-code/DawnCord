#ifndef DAWNCORD_CONFIG_H
#define DAWNCORD_CONFIG_H

/* Runtime configuration from ux0:data/dawncord/config.txt, so a prebuilt
   VPK (e.g. the CI artifact) works without recompiling. Plain key=value
   lines, '#' starts a comment:

       host=192.168.1.42
       port=9100
       code=1234

   Missing file or missing keys fall back to the compile-time defaults.
   The file is normally written by the first-boot setup, not by hand. */

#define CFG_HOST_LEN 64
#define CFG_CODE_LEN 64

#define CONFIG_DIR  "ux0:data/dawncord"
#define CONFIG_PATH "ux0:data/dawncord/config.txt"

typedef struct {
    char host[CFG_HOST_LEN];
    int port;
    char pair_code[CFG_CODE_LEN];
} dawncord_config;

void config_defaults(dawncord_config *cfg, const char *host, int port,
                     const char *pair_code);

/* Overlay values from a config file onto cfg.
   Returns 0 if the file was read, -1 if it couldn't be opened. */
int config_load_file(dawncord_config *cfg, const char *path);

/* Write cfg to path (creating CONFIG_DIR first when building for the Vita).
   Returns 0 on success, -1 if the file couldn't be written. */
int config_save_file(const dawncord_config *cfg, const char *path);

#endif
