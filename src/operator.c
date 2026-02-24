#include "sim.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
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

static int pick_available(const int *portions, int count, int preferred)
{
    int i;

    if (preferred >= 0 && preferred < count && portions[preferred] > 0) {
        return preferred;
    }

    for (i = 0; i < count; i++) {
        if (portions[i] > 0) {
            return i;
        }
    }

    return -1;
}

static int response_send(int fd, const ResponseMsg *resp)
{
    const char *ptr;
    size_t left;

    ptr = (const char *)resp;
    left = sizeof(*resp);
    for (;;) {
        ssize_t written = write(fd, ptr, left);
        if (written > 0) {
            ptr += written;
            left -= (size_t)written;
            if (left == 0) {
                return 0;
            }
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            sleep_ns(1000000);
            continue;
        }
        if (written < 0 && (errno == EPIPE || errno == EBADF)) {
            return 0;
        }
        perror("write response");
        return -1;
    }
}

static int request_recv(int req_fd, RequestMsg *req)
{
    for (;;) {
        ssize_t read_sz = read(req_fd, req, sizeof(*req));
        if (read_sz == (ssize_t)sizeof(*req)) {
            return 0;
        }
        if (read_sz == 0) {
            return 1;
        }
        if (read_sz < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 1;
        }
        if (read_sz < 0 && errno == EINTR) {
            if (g_stop_requested) {
                return -1;
            }
            continue;
        }
        if (read_sz > 0 && read_sz < (ssize_t)sizeof(*req)) {
            return 1;
        }
        perror("read request");
        return -1;
    }
}

static bool day_is_open(int semid, SharedState *shared)
{
    bool open;

    if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
        return false;
    }
    open = (shared->day_open != 0 && shared->terminate == 0);
    (void)sem_op_retry(semid, SEM_MUTEX, 1);
    return open;
}

static void maybe_take_pause(int semid, SharedState *shared, int worker_id, int station, int seat_sem, bool *has_seat,
                             unsigned int *seed)
{
    int pause_minutes;

    if (random_percent(seed) >= 20) {
        return;
    }

    if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
        return;
    }

    if (shared->terminate != 0 || shared->day_open == 0 || shared->cfg.nof_pause <= 0 ||
        shared->worker_pauses_today[worker_id] >= shared->cfg.nof_pause || shared->active_ops[station] <= 1) {
        (void)sem_op_retry(semid, SEM_MUTEX, 1);
        return;
    }

    shared->active_ops[station]--;
    shared->worker_pauses_today[worker_id]++;
    shared->worker_pauses_total[worker_id]++;
    shared->pauses_day_total++;
    shared->pauses_total++;

    (void)sem_op_retry(semid, SEM_MUTEX, 1);

    if (*has_seat) {
        (void)sem_op_retry(semid, (unsigned short)seat_sem, 1);
        *has_seat = false;
    }

    pause_minutes = random_between(shared->cfg.pause_min_minutes, shared->cfg.pause_max_minutes, seed);
    sleep_ns((long)pause_minutes * shared->cfg.n_nano_secs);

    if (!day_is_open(semid, shared) || g_stop_requested) {
        return;
    }

    if (sem_op_retry(semid, (unsigned short)seat_sem, -1) != 0) {
        return;
    }
    if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
        (void)sem_op_retry(semid, (unsigned short)seat_sem, 1);
        return;
    }

    if (shared->terminate != 0 || shared->day_open == 0) {
        (void)sem_op_retry(semid, SEM_MUTEX, 1);
        (void)sem_op_retry(semid, (unsigned short)seat_sem, 1);
        *has_seat = false;
        return;
    }

    shared->active_ops[station]++;
    *has_seat = true;
    (void)sem_op_retry(semid, SEM_MUTEX, 1);
}

int main(int argc, char **argv)
{
    int worker_id;
    int shmid;
    int semid;
    SharedState *shared;
    unsigned int seed;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <worker_id> <shmid> <semid>\n", argv[0]);
        return 1;
    }

    worker_id = atoi(argv[1]);
    shmid = atoi(argv[2]);
    semid = atoi(argv[3]);

    if (setup_signals() != 0) {
        return 1;
    }

    shared = (SharedState *)shmat(shmid, NULL, 0);
    if (shared == (void *)-1) {
        perror("shmat operator");
        return 1;
    }

    seed = (unsigned int)(getpid() ^ monotonic_ns());

    if (sem_op_retry(semid, SEM_INIT_READY, 1) != 0) {
        (void)shmdt(shared);
        return 1;
    }

    while (!g_stop_requested) {
        int day;
        int station;
        int seat_sem;
        int req_fd;
        bool counted_day;
        bool has_seat;

        if (sem_op_retry(semid, SEM_START_WORKERS, -1) != 0) {
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
        station = shared->assignments[worker_id];
        seat_sem = station_to_seat_sem(station);
        req_fd = shared->req_pipe_read_fd[station];
        counted_day = false;
        has_seat = false;

        (void)sem_op_retry(semid, SEM_MUTEX, 1);

        for (;;) {
            struct sembuf acquire;

            acquire.sem_num = (unsigned short)seat_sem;
            acquire.sem_op = -1;
            acquire.sem_flg = IPC_NOWAIT;

            if (semop(semid, &acquire, 1) == 0) {
                has_seat = true;
                break;
            }
            if (errno == EINTR) {
                if (g_stop_requested) {
                    break;
                }
                continue;
            }
            if (errno == EAGAIN) {
                if (!day_is_open(semid, shared)) {
                    break;
                }
                sleep_ns(shared->cfg.n_nano_secs);
                continue;
            }
            if (errno == EIDRM || errno == EINVAL) {
                g_stop_requested = 1;
                break;
            }
            perror("semop seat acquire");
            g_stop_requested = 1;
            break;
        }

        if (!has_seat) {
            (void)sem_op_retry(semid, SEM_END_WORKERS, 1);
            continue;
        }

        if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
            break;
        }

        shared->active_ops[station]++;
        if (!shared->worker_ever_active[worker_id]) {
            shared->worker_ever_active[worker_id] = 1;
            shared->unique_active_workers_total++;
        }
        if (!counted_day) {
            counted_day = true;
            shared->active_workers_day++;
        }

        (void)sem_op_retry(semid, SEM_MUTEX, 1);

        while (!g_stop_requested) {
            RequestMsg req;
            ResponseMsg resp;
            long service_ns;
            long now_ns;
            long wait_ns;
            int percent_span;
            int recv_rc;

            if (!has_seat) {
                if (!day_is_open(semid, shared)) {
                    break;
                }
                sleep_ns(shared->cfg.n_nano_secs);
                continue;
            }

            if (!day_is_open(semid, shared)) {
                break;
            }

            recv_rc = request_recv(req_fd, &req);
            if (recv_rc < 0) {
                g_stop_requested = 1;
                break;
            }
            if (recv_rc > 0) {
                sleep_ns(shared->cfg.n_nano_secs / 2);
                continue;
            }

            if (req.kind != MSG_KIND_REQUEST || req.day != day) {
                continue;
            }

            memset(&resp, 0, sizeof(resp));
            resp.day = day;
            resp.station = station;
            resp.ok = 0;
            resp.served_index = -1;
            resp.bill_cents = 0;

            now_ns = monotonic_ns();
            wait_ns = now_ns - req.enqueue_ns;
            if (wait_ns < 0) {
                wait_ns = 0;
            }
            resp.wait_ns = wait_ns;

            if (station == STATION_PRIMI || station == STATION_SECONDI) {
                percent_span = 50;
            } else if (station == STATION_COFFEE) {
                percent_span = 80;
            } else {
                percent_span = 20;
            }

            service_ns = random_duration_ns(&shared->cfg, shared->cfg.avg_srvc_secs[station], percent_span, &seed);
            sleep_ns(service_ns);

            if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
                g_stop_requested = 1;
                break;
            }

            if (shared->queue_len[station] > 0) {
                shared->queue_len[station]--;
            }
            shared->wait_ns_day[station] += wait_ns;
            shared->wait_ns_total[station] += wait_ns;
            shared->wait_samples_day[station]++;
            shared->wait_samples_total[station]++;

            if (station == STATION_PRIMI) {
                int idx = pick_available(shared->portions_primi, shared->menu.primi_count, req.preferred_index);
                if (idx >= 0) {
                    shared->portions_primi[idx]--;
                    shared->distributed_day[COURSE_PRIMI]++;
                    shared->distributed_total[COURSE_PRIMI]++;
                    resp.ok = 1;
                    resp.served_index = idx;
                }
            } else if (station == STATION_SECONDI) {
                int idx = pick_available(shared->portions_secondi, shared->menu.secondi_count, req.preferred_index);
                if (idx >= 0) {
                    shared->portions_secondi[idx]--;
                    shared->distributed_day[COURSE_SECONDI]++;
                    shared->distributed_total[COURSE_SECONDI]++;
                    resp.ok = 1;
                    resp.served_index = idx;
                }
            } else if (station == STATION_COFFEE) {
                shared->distributed_day[COURSE_COFFEE]++;
                shared->distributed_total[COURSE_COFFEE]++;
                resp.ok = 1;
                resp.served_index = req.preferred_index;
            } else {
                int bill = req.count_primi * shared->cfg.price_primi_cents + req.count_secondi * shared->cfg.price_secondi_cents +
                           req.count_coffee * shared->cfg.price_coffee_cents;
                shared->revenue_cents_day += bill;
                shared->revenue_cents_total += bill;
                resp.ok = 1;
                resp.bill_cents = bill;
            }

            (void)sem_op_retry(semid, SEM_MUTEX, 1);

            if (req.user_id >= 0 && req.user_id < shared->cfg.nof_users &&
                response_send(shared->resp_pipe_write_fd[req.user_id], &resp) != 0) {
                g_stop_requested = 1;
                break;
            }

            if (station != STATION_CASSA) {
                maybe_take_pause(semid, shared, worker_id, station, seat_sem, &has_seat, &seed);
            }
        }

        if (has_seat) {
            if (sem_op_retry(semid, SEM_MUTEX, -1) == 0) {
                if (shared->active_ops[station] > 0) {
                    shared->active_ops[station]--;
                }
                (void)sem_op_retry(semid, SEM_MUTEX, 1);
            }
            (void)sem_op_retry(semid, (unsigned short)seat_sem, 1);
        }

        (void)sem_op_retry(semid, SEM_END_WORKERS, 1);
    }

    (void)shmdt(shared);
    return 0;
}
