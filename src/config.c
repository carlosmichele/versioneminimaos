#include "sim.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim_in_place(char *s)
{
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static int parse_int(const char *text, int *out)
{
    char *endptr;
    long value;

    errno = 0;
    value = strtol(text, &endptr, 10);
    if (errno != 0 || endptr == text || *endptr != '\0') {
        return -1;
    }
    if (value < -2147483647L - 1L || value > 2147483647L) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

int config_set_defaults(SimConfig *cfg)
{
    if (cfg == NULL) {
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->nof_workers = 8;
    cfg->nof_users = 40;

    cfg->nof_wk_seats[STATION_PRIMI] = 2;
    cfg->nof_wk_seats[STATION_SECONDI] = 2;
    cfg->nof_wk_seats[STATION_COFFEE] = 2;
    cfg->nof_wk_seats[STATION_CASSA] = 1;

    cfg->nof_table_seats = 20;

    cfg->sim_duration_days = 3;
    cfg->n_nano_secs = 30000000L;
    cfg->day_minutes = 120;

    cfg->avg_srvc_secs[STATION_PRIMI] = 20;
    cfg->avg_srvc_secs[STATION_SECONDI] = 25;
    cfg->avg_srvc_secs[STATION_COFFEE] = 8;
    cfg->avg_srvc_secs[STATION_CASSA] = 6;

    cfg->avg_refill_primi = 20;
    cfg->avg_refill_secondi = 20;
    cfg->max_porzioni_primi = 60;
    cfg->max_porzioni_secondi = 60;

    cfg->nof_pause = 2;
    cfg->pause_min_minutes = 3;
    cfg->pause_max_minutes = 8;

    cfg->price_primi_cents = 450;
    cfg->price_secondi_cents = 650;
    cfg->price_coffee_cents = 120;

    cfg->overload_threshold = 10;

    cfg->user_daily_presence_pct = 85;
    cfg->user_coffee_pct = 60;
    cfg->eat_seconds_per_dish = 20;

    return 0;
}

static int set_cfg_value(SimConfig *cfg, const char *key, const char *value)
{
    int parsed;

    if (parse_int(value, &parsed) != 0) {
        return -1;
    }

    if (strcmp(key, "NOF_WORKERS") == 0) {
        cfg->nof_workers = parsed;
    } else if (strcmp(key, "NOF_USERS") == 0) {
        cfg->nof_users = parsed;
    } else if (strcmp(key, "NOF_WK_SEATS_PRIMI") == 0) {
        cfg->nof_wk_seats[STATION_PRIMI] = parsed;
    } else if (strcmp(key, "NOF_WK_SEATS_SECONDI") == 0) {
        cfg->nof_wk_seats[STATION_SECONDI] = parsed;
    } else if (strcmp(key, "NOF_WK_SEATS_COFFEE") == 0) {
        cfg->nof_wk_seats[STATION_COFFEE] = parsed;
    } else if (strcmp(key, "NOF_WK_SEATS_CASSA") == 0) {
        cfg->nof_wk_seats[STATION_CASSA] = parsed;
    } else if (strcmp(key, "NOF_TABLE_SEATS") == 0) {
        cfg->nof_table_seats = parsed;
    } else if (strcmp(key, "SIM_DURATION") == 0) {
        cfg->sim_duration_days = parsed;
    } else if (strcmp(key, "N_NANO_SECS") == 0) {
        cfg->n_nano_secs = parsed;
    } else if (strcmp(key, "DAY_MINUTES") == 0) {
        cfg->day_minutes = parsed;
    } else if (strcmp(key, "AVG_SRVC_PRIMI") == 0) {
        cfg->avg_srvc_secs[STATION_PRIMI] = parsed;
    } else if (strcmp(key, "AVG_SRVC_MAIN_COURSE") == 0) {
        cfg->avg_srvc_secs[STATION_SECONDI] = parsed;
    } else if (strcmp(key, "AVG_SRVC_COFFEE") == 0) {
        cfg->avg_srvc_secs[STATION_COFFEE] = parsed;
    } else if (strcmp(key, "AVG_SRVC_CASSA") == 0) {
        cfg->avg_srvc_secs[STATION_CASSA] = parsed;
    } else if (strcmp(key, "AVG_REFILL_PRIMI") == 0) {
        cfg->avg_refill_primi = parsed;
    } else if (strcmp(key, "AVG_REFILL_SECONDI") == 0) {
        cfg->avg_refill_secondi = parsed;
    } else if (strcmp(key, "MAX_PORZIONI_PRIMI") == 0) {
        cfg->max_porzioni_primi = parsed;
    } else if (strcmp(key, "MAX_PORZIONI_SECONDI") == 0) {
        cfg->max_porzioni_secondi = parsed;
    } else if (strcmp(key, "NOF_PAUSE") == 0) {
        cfg->nof_pause = parsed;
    } else if (strcmp(key, "PAUSE_MIN_MINUTES") == 0) {
        cfg->pause_min_minutes = parsed;
    } else if (strcmp(key, "PAUSE_MAX_MINUTES") == 0) {
        cfg->pause_max_minutes = parsed;
    } else if (strcmp(key, "PRICE_PRIMI") == 0) {
        cfg->price_primi_cents = parsed;
    } else if (strcmp(key, "PRICE_SECONDI") == 0) {
        cfg->price_secondi_cents = parsed;
    } else if (strcmp(key, "PRICE_COFFEE") == 0) {
        cfg->price_coffee_cents = parsed;
    } else if (strcmp(key, "OVERLOAD_THRESHOLD") == 0) {
        cfg->overload_threshold = parsed;
    } else if (strcmp(key, "USER_DAILY_PRESENCE_PCT") == 0) {
        cfg->user_daily_presence_pct = parsed;
    } else if (strcmp(key, "USER_COFFEE_PCT") == 0) {
        cfg->user_coffee_pct = parsed;
    } else if (strcmp(key, "EAT_SECONDS_PER_DISH") == 0) {
        cfg->eat_seconds_per_dish = parsed;
    }

    return 0;
}

int config_load_file(const char *path, SimConfig *cfg)
{
    FILE *fp;
    char line[512];
    int line_no;

    if (config_set_defaults(cfg) != 0) {
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen config");
        return -1;
    }

    line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq;
        char *key;
        char *value;

        line_no++;

        key = trim_in_place(line);
        if (*key == '\0' || *key == '#') {
            continue;
        }

        eq = strchr(key, '=');
        if (eq == NULL) {
            fprintf(stderr, "Config syntax error at line %d\n", line_no);
            fclose(fp);
            return -1;
        }

        *eq = '\0';
        value = trim_in_place(eq + 1);
        key = trim_in_place(key);

        if (set_cfg_value(cfg, key, value) != 0) {
            fprintf(stderr, "Invalid value for key '%s' at line %d\n", key, line_no);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}

int config_validate(const SimConfig *cfg, char *errbuf, size_t errbuf_sz)
{
    if (cfg->nof_workers < STATION_COUNT) {
        snprintf(errbuf, errbuf_sz, "NOF_WORKERS must be >= %d", STATION_COUNT);
        return -1;
    }
    if (cfg->nof_workers > MAX_WORKERS) {
        snprintf(errbuf, errbuf_sz, "NOF_WORKERS must be <= %d", MAX_WORKERS);
        return -1;
    }
    if (cfg->nof_users <= 0 || cfg->nof_users > MAX_USERS) {
        snprintf(errbuf, errbuf_sz, "NOF_USERS must be between 1 and %d", MAX_USERS);
        return -1;
    }
    if (cfg->nof_table_seats <= 0) {
        snprintf(errbuf, errbuf_sz, "NOF_TABLE_SEATS must be > 0");
        return -1;
    }
    if (cfg->sim_duration_days <= 0) {
        snprintf(errbuf, errbuf_sz, "SIM_DURATION must be > 0");
        return -1;
    }
    if (cfg->n_nano_secs <= 0) {
        snprintf(errbuf, errbuf_sz, "N_NANO_SECS must be > 0");
        return -1;
    }
    if (cfg->day_minutes <= 0) {
        snprintf(errbuf, errbuf_sz, "DAY_MINUTES must be > 0");
        return -1;
    }
    if (cfg->avg_refill_primi <= 0 || cfg->avg_refill_secondi <= 0) {
        snprintf(errbuf, errbuf_sz, "AVG_REFILL_* must be > 0");
        return -1;
    }
    if (cfg->max_porzioni_primi < cfg->avg_refill_primi || cfg->max_porzioni_secondi < cfg->avg_refill_secondi) {
        snprintf(errbuf, errbuf_sz, "MAX_PORZIONI_* must be >= AVG_REFILL_*");
        return -1;
    }
    if (cfg->pause_min_minutes <= 0 || cfg->pause_max_minutes < cfg->pause_min_minutes) {
        snprintf(errbuf, errbuf_sz, "Invalid pause interval");
        return -1;
    }
    if (cfg->user_daily_presence_pct < 0 || cfg->user_daily_presence_pct > 100) {
        snprintf(errbuf, errbuf_sz, "USER_DAILY_PRESENCE_PCT must be 0..100");
        return -1;
    }
    if (cfg->user_coffee_pct < 0 || cfg->user_coffee_pct > 100) {
        snprintf(errbuf, errbuf_sz, "USER_COFFEE_PCT must be 0..100");
        return -1;
    }

    if (cfg->nof_wk_seats[STATION_PRIMI] <= 0 || cfg->nof_wk_seats[STATION_SECONDI] <= 0 ||
        cfg->nof_wk_seats[STATION_COFFEE] <= 0 || cfg->nof_wk_seats[STATION_CASSA] <= 0) {
        snprintf(errbuf, errbuf_sz, "All NOF_WK_SEATS_* must be > 0");
        return -1;
    }

    return 0;
}

static int parse_menu_list(char *value, char dst[MAX_MENU_ITEMS][MAX_ITEM_NAME], int *count)
{
    char *saveptr;
    char *token;
    int idx;

    idx = 0;
    token = strtok_r(value, ",", &saveptr);
    while (token != NULL) {
        char *item = trim_in_place(token);
        if (*item != '\0') {
            if (idx >= MAX_MENU_ITEMS) {
                return -1;
            }
            snprintf(dst[idx], MAX_ITEM_NAME, "%s", item);
            idx++;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    *count = idx;
    return 0;
}

int menu_load_file(const char *path, MenuData *menu)
{
    FILE *fp;
    char line[512];

    memset(menu, 0, sizeof(*menu));

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen menu");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq;
        char *key;
        char *value;

        key = trim_in_place(line);
        if (*key == '\0' || *key == '#') {
            continue;
        }

        eq = strchr(key, '=');
        if (eq == NULL) {
            continue;
        }

        *eq = '\0';
        value = trim_in_place(eq + 1);
        key = trim_in_place(key);

        if (strcasecmp(key, "PRIMI") == 0) {
            if (parse_menu_list(value, menu->primi, &menu->primi_count) != 0) {
                fclose(fp);
                return -1;
            }
        } else if (strcasecmp(key, "SECONDI") == 0) {
            if (parse_menu_list(value, menu->secondi, &menu->secondi_count) != 0) {
                fclose(fp);
                return -1;
            }
        } else if (strcasecmp(key, "COFFEE") == 0) {
            if (parse_menu_list(value, menu->coffee, &menu->coffee_count) != 0) {
                fclose(fp);
                return -1;
            }
        }
    }

    fclose(fp);

    if (menu->primi_count < 2 || menu->secondi_count < 2 || menu->coffee_count < 4) {
        fprintf(stderr, "menu.txt must contain at least 2 primi, 2 secondi and 4 coffee types\n");
        return -1;
    }

    return 0;
}
