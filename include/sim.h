#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>

#define STATION_COUNT 4
#define COURSE_TYPES 3

#define MAX_WORKERS 256
#define MAX_USERS 1024
#define MAX_MENU_ITEMS 8
#define MAX_ITEM_NAME 32

enum Station {
    STATION_PRIMI = 0,
    STATION_SECONDI = 1,
    STATION_COFFEE = 2,
    STATION_CASSA = 3,
};

enum CourseType {
    COURSE_PRIMI = 0,
    COURSE_SECONDI = 1,
    COURSE_COFFEE = 2,
};

enum SemIndex {
    SEM_MUTEX = 0,
    SEM_INIT_READY,
    SEM_START_WORKERS,
    SEM_START_USERS,
    SEM_END_WORKERS,
    SEM_END_USERS,
    SEM_SEAT_PRIMI,
    SEM_SEAT_SECONDI,
    SEM_SEAT_COFFEE,
    SEM_SEAT_CASSA,
    SEM_TABLE_SEATS,
    SEM_COUNT
};

enum MsgKind {
    MSG_KIND_REQUEST = 1,
    MSG_KIND_STOP = 2,
};

enum TerminationCause {
    TERM_NONE = 0,
    TERM_TIMEOUT = 1,
    TERM_OVERLOAD = 2,
    TERM_SIGNAL = 3,
};

typedef struct {
    int nof_workers;
    int nof_users;
    int nof_wk_seats[STATION_COUNT];
    int nof_table_seats;

    int sim_duration_days;
    long n_nano_secs;
    int day_minutes;

    int avg_srvc_secs[STATION_COUNT];
    int avg_refill_primi;
    int avg_refill_secondi;
    int max_porzioni_primi;
    int max_porzioni_secondi;

    int nof_pause;
    int pause_min_minutes;
    int pause_max_minutes;

    int price_primi_cents;
    int price_secondi_cents;
    int price_coffee_cents;

    int overload_threshold;

    int user_daily_presence_pct;
    int user_coffee_pct;
    int eat_seconds_per_dish;
} SimConfig;

typedef struct {
    int primi_count;
    int secondi_count;
    int coffee_count;
    char primi[MAX_MENU_ITEMS][MAX_ITEM_NAME];
    char secondi[MAX_MENU_ITEMS][MAX_ITEM_NAME];
    char coffee[MAX_MENU_ITEMS][MAX_ITEM_NAME];
} MenuData;

typedef struct {
    SimConfig cfg;
    MenuData menu;

    int current_day;
    int day_open;
    int terminate;
    int termination_cause;

    int assignments[MAX_WORKERS];
    int worker_pauses_today[MAX_WORKERS];
    int worker_pauses_total[MAX_WORKERS];
    int worker_ever_active[MAX_WORKERS];

    int active_ops[STATION_COUNT];
    int queue_len[STATION_COUNT];
    int req_pipe_read_fd[STATION_COUNT];
    int req_pipe_write_fd[STATION_COUNT];
    int resp_pipe_read_fd[MAX_USERS];
    int resp_pipe_write_fd[MAX_USERS];

    int portions_primi[MAX_MENU_ITEMS];
    int portions_secondi[MAX_MENU_ITEMS];

    int users_attempted_day;
    int users_served_day;
    int users_unserved_day;

    int users_served_total;
    int users_unserved_total;

    int distributed_day[COURSE_TYPES];
    int distributed_total[COURSE_TYPES];

    int leftovers_day[COURSE_TYPES];
    int leftovers_total[COURSE_TYPES];

    int active_workers_day;
    int unique_active_workers_total;

    int pauses_day_total;
    int pauses_total;

    long wait_ns_day[STATION_COUNT];
    int wait_samples_day[STATION_COUNT];

    long wait_ns_total[STATION_COUNT];
    int wait_samples_total[STATION_COUNT];

    long revenue_cents_day;
    long revenue_cents_total;
} SharedState;

typedef struct {
    long mtype;
    int kind;
    int day;
    int user_id;
    int station;
    int preferred_index;
    int count_primi;
    int count_secondi;
    int count_coffee;
    long enqueue_ns;
} RequestMsg;

typedef struct {
    long mtype;
    int day;
    int station;
    int ok;
    int served_index;
    long wait_ns;
    int bill_cents;
} ResponseMsg;

int config_set_defaults(SimConfig *cfg);
int config_load_file(const char *path, SimConfig *cfg);
int config_validate(const SimConfig *cfg, char *errbuf, size_t errbuf_sz);
int menu_load_file(const char *path, MenuData *menu);

int sem_set_value(int semid, int semnum, int value);
int sem_op_retry(int semid, unsigned short semnum, short delta);

long monotonic_ns(void);
long sim_seconds_to_real_ns(const SimConfig *cfg, int sim_seconds);
long random_duration_ns(const SimConfig *cfg, int avg_seconds, int percent_span, unsigned int *seed);
int random_percent(unsigned int *seed);
int random_between(int min_value, int max_value, unsigned int *seed);
void sleep_ns(long ns);

int station_to_seat_sem(int station);
const char *station_name(int station);

#endif
