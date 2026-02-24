#include "sim.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <time.h>
#include <unistd.h>

#if defined(_SEM_SEMUN_UNDEFINED)
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};
#endif

int sem_set_value(int semid, int semnum, int value)
{
    union semun arg;

    arg.val = value;
    if (semctl(semid, semnum, SETVAL, arg) == -1) {
        perror("semctl SETVAL");
        return -1;
    }
    return 0;
}

int sem_op_retry(int semid, unsigned short semnum, short delta)
{
    struct sembuf op;

    op.sem_num = semnum;
    op.sem_op = delta;
    op.sem_flg = 0;

    for (;;) {
        if (semop(semid, &op, 1) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        perror("semop");
        return -1;
    }
}

long monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        return 0;
    }

    return (long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

long sim_seconds_to_real_ns(const SimConfig *cfg, int sim_seconds)
{
    long ns;

    ns = ((long)sim_seconds * (long)cfg->n_nano_secs) / 60LL;
    if (ns <= 0) {
        ns = 1;
    }
    return ns;
}

int random_percent(unsigned int *seed)
{
    return (int)(rand_r(seed) % 100U);
}

int random_between(int min_value, int max_value, unsigned int *seed)
{
    unsigned int span;

    if (min_value >= max_value) {
        return min_value;
    }

    span = (unsigned int)(max_value - min_value + 1);
    return min_value + (int)(rand_r(seed) % span);
}

long random_duration_ns(const SimConfig *cfg, int avg_seconds, int percent_span, unsigned int *seed)
{
    int delta;
    int min_seconds;
    int max_seconds;
    int picked;

    delta = (avg_seconds * percent_span) / 100;
    min_seconds = avg_seconds - delta;
    if (min_seconds < 1) {
        min_seconds = 1;
    }
    max_seconds = avg_seconds + delta;
    if (max_seconds < min_seconds) {
        max_seconds = min_seconds;
    }

    picked = random_between(min_seconds, max_seconds, seed);
    return sim_seconds_to_real_ns(cfg, picked);
}

void sleep_ns(long ns)
{
    struct timespec req;
    struct timespec rem;

    if (ns <= 0) {
        return;
    }

    req.tv_sec = (time_t)(ns / 1000000000LL);
    req.tv_nsec = (long)(ns % 1000000000LL);

    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            break;
        }
        req = rem;
    }
}

int station_to_seat_sem(int station)
{
    if (station == STATION_PRIMI) {
        return SEM_SEAT_PRIMI;
    }
    if (station == STATION_SECONDI) {
        return SEM_SEAT_SECONDI;
    }
    if (station == STATION_COFFEE) {
        return SEM_SEAT_COFFEE;
    }
    return SEM_SEAT_CASSA;
}

const char *station_name(int station)
{
    if (station == STATION_PRIMI) {
        return "primi";
    }
    if (station == STATION_SECONDI) {
        return "secondi";
    }
    if (station == STATION_COFFEE) {
        return "coffee";
    }
    if (station == STATION_CASSA) {
        return "cassa";
    }
    return "unknown";
}
