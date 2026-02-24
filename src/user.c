#include "sim.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_requested = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

static int setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction SIGINT");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("sigaction SIGTERM");
        return -1;
    }

    return 0;
}

static int recv_response(int resp_fd, ResponseMsg *resp)
{
    char *ptr;
    size_t left;

    ptr = (char *)resp;
    left = sizeof(*resp);
    for (;;) {
        ssize_t read_sz = read(resp_fd, ptr, left);
        if (read_sz > 0) {
            ptr += read_sz;
            left -= (size_t)read_sz;
            if (left == 0) {
                return 0;
            }
            continue;
        }
        if (read_sz == 0) {
            return -1;
        }
        if (read_sz < 0 && errno == EINTR) {
            if (g_stop_requested) {
                return -1;
            }
            continue;
        }
        if (read_sz < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            sleep_ns(1000000);
            continue;
        }
        perror("read response");
        return -1;
    }
}

static int station_request(SharedState *shared, int semid, int day, int user_id, int station,
                           int preferred_index, int count_primi, int count_secondi, int count_coffee)
{
    RequestMsg req;
    ResponseMsg resp;
    int req_fd;
    int resp_fd;

    memset(&req, 0, sizeof(req));
    req.kind = MSG_KIND_REQUEST;
    req.day = day;
    req.user_id = user_id;
    req.station = station;
    req.preferred_index = preferred_index;
    req.count_primi = count_primi;
    req.count_secondi = count_secondi;
    req.count_coffee = count_coffee;
    req.enqueue_ns = monotonic_ns();
    req_fd = shared->req_pipe_write_fd[station];
    resp_fd = shared->resp_pipe_read_fd[user_id];

    if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
        return -1;
    }

    if (shared->terminate != 0 || shared->day_open == 0) {
        (void)sem_op_retry(semid, SEM_MUTEX, 1);
        return 0;
    }

    for (;;) {
        ssize_t written = write(req_fd, &req, sizeof(req));
        if (written == (ssize_t)sizeof(req)) {
            shared->queue_len[station]++;
            (void)sem_op_retry(semid, SEM_MUTEX, 1);
            break;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        (void)sem_op_retry(semid, SEM_MUTEX, 1);
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (written > 0 && written < (ssize_t)sizeof(req)) {
            return -1;
        }
        perror("write request");
        return -1;
    }

    for (;;) {
        if (recv_response(resp_fd, &resp) != 0) {
            return -1;
        }
        if (resp.day == day && resp.station == station) {
            return resp.ok;
        }
    }
}

int main(int argc, char **argv)
{
    int user_id;
    int shmid;
    int semid;
    SharedState *shared;
    unsigned int seed;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <user_id> <shmid> <semid>\n", argv[0]);
        return 1;
    }

    user_id = atoi(argv[1]);
    shmid = atoi(argv[2]);
    semid = atoi(argv[3]);

    if (setup_signals() != 0) {
        return 1;
    }

    shared = (SharedState *)shmat(shmid, NULL, 0);
    if (shared == (void *)-1) {
        perror("shmat user");
        return 1;
    }

    seed = (unsigned int)(getpid() ^ monotonic_ns());

    if (sem_op_retry(semid, SEM_INIT_READY, 1) != 0) {
        (void)shmdt(shared);
        return 1;
    }

    while (!g_stop_requested) {
        int day;
        int attend;
        int preferred;
        int got_primo;
        int got_secondo;
        int got_coffee;
        int paid;

        if (sem_op_retry(semid, SEM_START_USERS, -1) != 0) {
            break;
        }

        if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
            break;
        }

        if (shared->terminate != 0) {
            (void)sem_op_retry(semid, SEM_MUTEX, 1);
            break;
        }

        day = shared->current_day;
        attend = (random_percent(&seed) < shared->cfg.user_daily_presence_pct);

        if (attend) {
            shared->users_attempted_day++;
        }

        (void)sem_op_retry(semid, SEM_MUTEX, 1);

        if (!attend) {
            (void)sem_op_retry(semid, SEM_END_USERS, 1);
            continue;
        }

        preferred = random_between(0, shared->menu.primi_count - 1, &seed);
        got_primo = station_request(shared, semid, day, user_id, STATION_PRIMI, preferred, 0, 0, 0);
        if (got_primo < 0) {
            break;
        }

        preferred = random_between(0, shared->menu.secondi_count - 1, &seed);
        got_secondo = station_request(shared, semid, day, user_id, STATION_SECONDI, preferred, 0, 0, 0);
        if (got_secondo < 0) {
            break;
        }

        got_coffee = 0;
        if ((got_primo || got_secondo) && random_percent(&seed) < shared->cfg.user_coffee_pct) {
            preferred = random_between(0, shared->menu.coffee_count - 1, &seed);
            got_coffee = station_request(shared, semid, day, user_id, STATION_COFFEE, preferred, 0, 0, 0);
            if (got_coffee < 0) {
                break;
            }
        }

        if (!got_primo && !got_secondo) {
            if (sem_op_retry(semid, SEM_MUTEX, -1) == 0) {
                shared->users_unserved_day++;
                shared->users_unserved_total++;
                (void)sem_op_retry(semid, SEM_MUTEX, 1);
            }
            (void)sem_op_retry(semid, SEM_END_USERS, 1);
            continue;
        }

        paid = station_request(shared, semid, day, user_id, STATION_CASSA, -1, got_primo, got_secondo, got_coffee);
        if (paid < 0) {
            break;
        }

        if (!paid) {
            if (sem_op_retry(semid, SEM_MUTEX, -1) == 0) {
                shared->users_unserved_day++;
                shared->users_unserved_total++;
                (void)sem_op_retry(semid, SEM_MUTEX, 1);
            }
            (void)sem_op_retry(semid, SEM_END_USERS, 1);
            continue;
        }

        if (sem_op_retry(semid, SEM_TABLE_SEATS, -1) != 0) {
            break;
        }

        sleep_ns(sim_seconds_to_real_ns(&shared->cfg,
                                        shared->cfg.eat_seconds_per_dish * (got_primo + got_secondo + got_coffee)));

        (void)sem_op_retry(semid, SEM_TABLE_SEATS, 1);

        if (sem_op_retry(semid, SEM_MUTEX, -1) == 0) {
            shared->users_served_day++;
            shared->users_served_total++;
            (void)sem_op_retry(semid, SEM_MUTEX, 1);
        }

        (void)sem_op_retry(semid, SEM_END_USERS, 1);
    }

    (void)shmdt(shared);
    return 0;
}
