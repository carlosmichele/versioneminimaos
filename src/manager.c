#include "sim.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
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

static int init_semaphores(int semid, const SimConfig *cfg)
{
    if (sem_set_value(semid, SEM_MUTEX, 1) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_INIT_READY, 0) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_START_WORKERS, 0) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_START_USERS, 0) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_END_WORKERS, 0) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_END_USERS, 0) != 0) {
        return -1;
    }

    if (sem_set_value(semid, SEM_SEAT_PRIMI, cfg->nof_wk_seats[STATION_PRIMI]) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_SEAT_SECONDI, cfg->nof_wk_seats[STATION_SECONDI]) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_SEAT_COFFEE, cfg->nof_wk_seats[STATION_COFFEE]) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_SEAT_CASSA, cfg->nof_wk_seats[STATION_CASSA]) != 0) {
        return -1;
    }
    if (sem_set_value(semid, SEM_TABLE_SEATS, cfg->nof_table_seats) != 0) {
        return -1;
    }

    return 0;
}

static int set_nonblocking_fd(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        return -1;
    }
    return 0;
}

static int setup_pipes(SharedState *shared, const SimConfig *cfg)
{
    int s;
    int u;

    for (s = 0; s < STATION_COUNT; s++) {
        shared->req_pipe_read_fd[s] = -1;
        shared->req_pipe_write_fd[s] = -1;
    }
    for (u = 0; u < MAX_USERS; u++) {
        shared->resp_pipe_read_fd[u] = -1;
        shared->resp_pipe_write_fd[u] = -1;
    }

    for (s = 0; s < STATION_COUNT; s++) {
        int fds[2];
        if (pipe(fds) != 0) {
            perror("pipe request");
            return -1;
        }
        if (set_nonblocking_fd(fds[0]) != 0 || set_nonblocking_fd(fds[1]) != 0) {
            close(fds[0]);
            close(fds[1]);
            return -1;
        }
        shared->req_pipe_read_fd[s] = fds[0];
        shared->req_pipe_write_fd[s] = fds[1];
    }

    for (u = 0; u < cfg->nof_users; u++) {
        int fds[2];
        if (pipe(fds) != 0) {
            perror("pipe response");
            return -1;
        }
        shared->resp_pipe_read_fd[u] = fds[0];
        shared->resp_pipe_write_fd[u] = fds[1];
    }

    return 0;
}

static void close_all_pipes(SharedState *shared, const SimConfig *cfg)
{
    int s;
    int u;

    for (s = 0; s < STATION_COUNT; s++) {
        if (shared->req_pipe_read_fd[s] >= 0) {
            close(shared->req_pipe_read_fd[s]);
            shared->req_pipe_read_fd[s] = -1;
        }
        if (shared->req_pipe_write_fd[s] >= 0) {
            close(shared->req_pipe_write_fd[s]);
            shared->req_pipe_write_fd[s] = -1;
        }
    }

    for (u = 0; u < cfg->nof_users; u++) {
        if (shared->resp_pipe_read_fd[u] >= 0) {
            close(shared->resp_pipe_read_fd[u]);
            shared->resp_pipe_read_fd[u] = -1;
        }
        if (shared->resp_pipe_write_fd[u] >= 0) {
            close(shared->resp_pipe_write_fd[u]);
            shared->resp_pipe_write_fd[u] = -1;
        }
    }
}

static pid_t spawn_worker(int worker_id, int shmid, int semid)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        perror("fork worker");
        return -1;
    }

    if (pid == 0) {
        char worker_buf[16];
        char shm_buf[16];
        char sem_buf[16];

        snprintf(worker_buf, sizeof(worker_buf), "%d", worker_id);
        snprintf(shm_buf, sizeof(shm_buf), "%d", shmid);
        snprintf(sem_buf, sizeof(sem_buf), "%d", semid);

        execl("./operator", "operator", worker_buf, shm_buf, sem_buf, (char *)NULL);
        perror("execl operator");
        _exit(1);
    }

    return pid;
}

static pid_t spawn_user(int user_id, int shmid, int semid)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        perror("fork user");
        return -1;
    }

    if (pid == 0) {
        char user_buf[16];
        char shm_buf[16];
        char sem_buf[16];

        snprintf(user_buf, sizeof(user_buf), "%d", user_id);
        snprintf(shm_buf, sizeof(shm_buf), "%d", shmid);
        snprintf(sem_buf, sizeof(sem_buf), "%d", semid);

        execl("./user", "user", user_buf, shm_buf, sem_buf, (char *)NULL);
        perror("execl user");
        _exit(1);
    }

    return pid;
}

static void reset_day_stats(SharedState *state)
{
    int i;

    state->users_attempted_day = 0;
    state->users_served_day = 0;
    state->users_unserved_day = 0;

    memset(state->distributed_day, 0, sizeof(state->distributed_day));
    memset(state->leftovers_day, 0, sizeof(state->leftovers_day));
    memset(state->wait_ns_day, 0, sizeof(state->wait_ns_day));
    memset(state->wait_samples_day, 0, sizeof(state->wait_samples_day));
    memset(state->active_ops, 0, sizeof(state->active_ops));
    memset(state->queue_len, 0, sizeof(state->queue_len));
    memset(state->worker_pauses_today, 0, sizeof(state->worker_pauses_today));

    state->active_workers_day = 0;
    state->pauses_day_total = 0;
    state->revenue_cents_day = 0;

    for (i = 0; i < state->menu.primi_count; i++) {
        state->portions_primi[i] = state->cfg.avg_refill_primi;
    }
    for (; i < MAX_MENU_ITEMS; i++) {
        state->portions_primi[i] = 0;
    }

    for (i = 0; i < state->menu.secondi_count; i++) {
        state->portions_secondi[i] = state->cfg.avg_refill_secondi;
    }
    for (; i < MAX_MENU_ITEMS; i++) {
        state->portions_secondi[i] = 0;
    }
}

static void refill_portions(SharedState *state)
{
    int i;

    for (i = 0; i < state->menu.primi_count; i++) {
        int updated = state->portions_primi[i] + state->cfg.avg_refill_primi;
        if (updated > state->cfg.max_porzioni_primi) {
            updated = state->cfg.max_porzioni_primi;
        }
        state->portions_primi[i] = updated;
    }

    for (i = 0; i < state->menu.secondi_count; i++) {
        int updated = state->portions_secondi[i] + state->cfg.avg_refill_secondi;
        if (updated > state->cfg.max_porzioni_secondi) {
            updated = state->cfg.max_porzioni_secondi;
        }
        state->portions_secondi[i] = updated;
    }
}

static void assign_workers(SharedState *state)
{
    int i;
    int max_station;
    int max_value;

    if (state->cfg.nof_workers < STATION_COUNT) {
        return;
    }

    state->assignments[0] = STATION_PRIMI;
    state->assignments[1] = STATION_SECONDI;
    state->assignments[2] = STATION_COFFEE;
    state->assignments[3] = STATION_CASSA;

    max_station = STATION_PRIMI;
    max_value = state->cfg.avg_srvc_secs[STATION_PRIMI];
    for (i = STATION_SECONDI; i < STATION_COUNT; i++) {
        if (state->cfg.avg_srvc_secs[i] > max_value) {
            max_value = state->cfg.avg_srvc_secs[i];
            max_station = i;
        }
    }

    for (i = STATION_COUNT; i < state->cfg.nof_workers; i++) {
        state->assignments[i] = max_station;
    }
}

static int write_response_pipe(int fd, const ResponseMsg *resp)
{
    const char *ptr;
    size_t left;

    ptr = (const char *)resp;
    left = sizeof(*resp);
    while (left > 0) {
        ssize_t written = write(fd, ptr, left);
        if (written > 0) {
            ptr += written;
            left -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            sleep_ns(1000000);
            continue;
        }
        return -1;
    }
    return 0;
}

static void drain_pending_requests(int day, SharedState *shared, int semid)
{
    int station;

    for (station = 0; station < STATION_COUNT; station++) {
        int fd = shared->req_pipe_read_fd[station];
        for (;;) {
            RequestMsg req;
            ssize_t rc;

            rc = read(fd, &req, sizeof(req));
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("read drain");
                break;
            }
            if (rc == 0) {
                break;
            }
            if ((size_t)rc != sizeof(req)) {
                continue;
            }
            if (req.kind != MSG_KIND_REQUEST || req.day != day) {
                continue;
            }

            if (req.station >= 0 && req.station < STATION_COUNT) {
                if (sem_op_retry(semid, SEM_MUTEX, -1) == 0) {
                    if (shared->queue_len[req.station] > 0) {
                        shared->queue_len[req.station]--;
                    }
                    (void)sem_op_retry(semid, SEM_MUTEX, 1);
                }
            }

            if (req.user_id >= 0 && req.user_id < shared->cfg.nof_users) {
                ResponseMsg resp;

                memset(&resp, 0, sizeof(resp));
                resp.day = req.day;
                resp.station = req.station;
                resp.ok = 0;
                resp.served_index = -1;

                if (write_response_pipe(shared->resp_pipe_write_fd[req.user_id], &resp) != 0) {
                    break;
                }
            }
        }
    }
}

static int queue_pending(const SharedState *state)
{
    int pending;
    int s;

    pending = 0;
    for (s = 0; s < STATION_COUNT; s++) {
        pending += state->queue_len[s];
    }

    return pending;
}

static double avg_wait_ms(long total_ns, int samples)
{
    if (samples <= 0) {
        return 0.0;
    }
    return (double)total_ns / (double)samples / 1000000.0;
}

static void print_money(const char *label, long cents)
{
    long abs_cents;

    abs_cents = cents;
    if (abs_cents < 0) {
        abs_cents = -abs_cents;
    }

    printf("%s: %s%ld.%02ld euro\n", label, cents < 0 ? "-" : "", (long)(abs_cents / 100),
           (long)(abs_cents % 100));
}

static void print_day_stats(const SharedState *s, int day, int pending_at_close)
{
    long total_wait_day;
    int total_samples_day;
    long total_wait_total;
    int total_samples_total;
    int distributed_day_total;
    int distributed_total_all;
    int leftovers_day_total;
    int leftovers_total_all;
    double avg_served_day;
    double avg_unserved_day;
    double avg_distributed_all_day;
    double avg_distributed_primi_day;
    double avg_distributed_secondi_day;
    double avg_distributed_coffee_day;
    double avg_leftovers_all_day;
    double avg_leftovers_primi_day;
    double avg_leftovers_secondi_day;
    double avg_leftovers_coffee_day;
    double avg_pauses_day;

    total_wait_day = s->wait_ns_day[STATION_PRIMI] + s->wait_ns_day[STATION_SECONDI] + s->wait_ns_day[STATION_COFFEE] +
                     s->wait_ns_day[STATION_CASSA];
    total_samples_day = s->wait_samples_day[STATION_PRIMI] + s->wait_samples_day[STATION_SECONDI] +
                        s->wait_samples_day[STATION_COFFEE] + s->wait_samples_day[STATION_CASSA];
    total_wait_total = s->wait_ns_total[STATION_PRIMI] + s->wait_ns_total[STATION_SECONDI] + s->wait_ns_total[STATION_COFFEE] +
                       s->wait_ns_total[STATION_CASSA];
    total_samples_total = s->wait_samples_total[STATION_PRIMI] + s->wait_samples_total[STATION_SECONDI] +
                          s->wait_samples_total[STATION_COFFEE] + s->wait_samples_total[STATION_CASSA];

    distributed_day_total = s->distributed_day[COURSE_PRIMI] + s->distributed_day[COURSE_SECONDI] + s->distributed_day[COURSE_COFFEE];
    distributed_total_all =
        s->distributed_total[COURSE_PRIMI] + s->distributed_total[COURSE_SECONDI] + s->distributed_total[COURSE_COFFEE];
    leftovers_day_total = s->leftovers_day[COURSE_PRIMI] + s->leftovers_day[COURSE_SECONDI] + s->leftovers_day[COURSE_COFFEE];
    leftovers_total_all = s->leftovers_total[COURSE_PRIMI] + s->leftovers_total[COURSE_SECONDI] + s->leftovers_total[COURSE_COFFEE];

    if (day <= 0) {
        day = 1;
    }
    avg_served_day = (double)s->users_served_total / (double)day;
    avg_unserved_day = (double)s->users_unserved_total / (double)day;
    avg_distributed_all_day = (double)distributed_total_all / (double)day;
    avg_distributed_primi_day = (double)s->distributed_total[COURSE_PRIMI] / (double)day;
    avg_distributed_secondi_day = (double)s->distributed_total[COURSE_SECONDI] / (double)day;
    avg_distributed_coffee_day = (double)s->distributed_total[COURSE_COFFEE] / (double)day;
    avg_leftovers_all_day = (double)leftovers_total_all / (double)day;
    avg_leftovers_primi_day = (double)s->leftovers_total[COURSE_PRIMI] / (double)day;
    avg_leftovers_secondi_day = (double)s->leftovers_total[COURSE_SECONDI] / (double)day;
    avg_leftovers_coffee_day = (double)s->leftovers_total[COURSE_COFFEE] / (double)day;
    avg_pauses_day = (s->active_workers_day > 0) ? ((double)s->pauses_day_total / (double)s->active_workers_day) : 0.0;

    printf("\n===== Giorno %d =====\n", day);
    printf("Utenti serviti giornata: %d\n", s->users_served_day);
    printf("Utenti non serviti giornata: %d\n", s->users_unserved_day);
    printf("Piatti distribuiti giornata: %d (primi=%d, secondi=%d, coffee=%d)\n", distributed_day_total, s->distributed_day[COURSE_PRIMI],
           s->distributed_day[COURSE_SECONDI], s->distributed_day[COURSE_COFFEE]);
    printf("Piatti avanzati giornata: %d (primi=%d, secondi=%d, coffee=%d)\n", leftovers_day_total, s->leftovers_day[COURSE_PRIMI],
           s->leftovers_day[COURSE_SECONDI], s->leftovers_day[COURSE_COFFEE]);

    printf("Utenti serviti totali simulazione: %d\n", s->users_served_total);
    printf("Utenti non serviti totali simulazione: %d\n", s->users_unserved_total);
    printf("Utenti serviti medi al giorno: %.2f\n", avg_served_day);
    printf("Utenti non serviti medi al giorno: %.2f\n", avg_unserved_day);

    printf("Piatti distribuiti totali simulazione: %d (primi=%d, secondi=%d, coffee=%d)\n", distributed_total_all,
           s->distributed_total[COURSE_PRIMI], s->distributed_total[COURSE_SECONDI], s->distributed_total[COURSE_COFFEE]);
    printf("Piatti avanzati totali simulazione: %d (primi=%d, secondi=%d, coffee=%d)\n", leftovers_total_all, s->leftovers_total[COURSE_PRIMI],
           s->leftovers_total[COURSE_SECONDI], s->leftovers_total[COURSE_COFFEE]);
    printf("Piatti distribuiti medi al giorno: totale=%.2f, primi=%.2f, secondi=%.2f, coffee=%.2f\n", avg_distributed_all_day,
           avg_distributed_primi_day, avg_distributed_secondi_day, avg_distributed_coffee_day);
    printf("Piatti avanzati medi al giorno: totale=%.2f, primi=%.2f, secondi=%.2f, coffee=%.2f\n", avg_leftovers_all_day,
           avg_leftovers_primi_day, avg_leftovers_secondi_day, avg_leftovers_coffee_day);

    printf("Attesa media simulazione (ms): totale=%.2f, primi=%.2f, secondi=%.2f, coffee=%.2f, cassa=%.2f\n",
           avg_wait_ms(total_wait_total, total_samples_total),
           avg_wait_ms(s->wait_ns_total[STATION_PRIMI], s->wait_samples_total[STATION_PRIMI]),
           avg_wait_ms(s->wait_ns_total[STATION_SECONDI], s->wait_samples_total[STATION_SECONDI]),
           avg_wait_ms(s->wait_ns_total[STATION_COFFEE], s->wait_samples_total[STATION_COFFEE]),
           avg_wait_ms(s->wait_ns_total[STATION_CASSA], s->wait_samples_total[STATION_CASSA]));

    printf("Attesa media giornata (ms): totale=%.2f, primi=%.2f, secondi=%.2f, coffee=%.2f, cassa=%.2f\n",
           avg_wait_ms(total_wait_day, total_samples_day), avg_wait_ms(s->wait_ns_day[STATION_PRIMI], s->wait_samples_day[STATION_PRIMI]),
           avg_wait_ms(s->wait_ns_day[STATION_SECONDI], s->wait_samples_day[STATION_SECONDI]),
           avg_wait_ms(s->wait_ns_day[STATION_COFFEE], s->wait_samples_day[STATION_COFFEE]),
           avg_wait_ms(s->wait_ns_day[STATION_CASSA], s->wait_samples_day[STATION_CASSA]));

    printf("Operatori attivi giornata: %d\n", s->active_workers_day);
    printf("Operatori attivi simulazione: %d\n", s->unique_active_workers_total);
    printf("Pause medie giornata per operatore attivo: %.2f\n", avg_pauses_day);
    printf("Pause totali simulazione: %d\n", s->pauses_total);
    print_money("Ricavo giornata", s->revenue_cents_day);

    printf("Utenti in attesa al termine giornata: %d\n", pending_at_close);
}

static void print_final_stats(const SharedState *s, int completed_days)
{
    long total_wait;
    int total_samples;
    int avg_rev;

    if (completed_days <= 0) {
        completed_days = 1;
    }

    total_wait = s->wait_ns_total[STATION_PRIMI] + s->wait_ns_total[STATION_SECONDI] + s->wait_ns_total[STATION_COFFEE] +
                 s->wait_ns_total[STATION_CASSA];
    total_samples = s->wait_samples_total[STATION_PRIMI] + s->wait_samples_total[STATION_SECONDI] +
                    s->wait_samples_total[STATION_COFFEE] + s->wait_samples_total[STATION_CASSA];
    avg_rev = (int)(s->revenue_cents_total / completed_days);

    printf("\n===== Fine simulazione =====\n");
    if (s->termination_cause == TERM_TIMEOUT) {
        printf("Causa terminazione: timeout (SIM_DURATION raggiunta)\n");
    } else if (s->termination_cause == TERM_OVERLOAD) {
        printf("Causa terminazione: overload (coda oltre soglia)\n");
    } else if (s->termination_cause == TERM_SIGNAL) {
        printf("Causa terminazione: segnale esterno\n");
    } else {
        printf("Causa terminazione: non specificata\n");
    }

    printf("Giorni completati: %d\n", completed_days);
    printf("Utenti serviti totali: %d\n", s->users_served_total);
    printf("Utenti non serviti totali: %d\n", s->users_unserved_total);
    printf("Piatti distribuiti totali: %d\n",
           s->distributed_total[COURSE_PRIMI] + s->distributed_total[COURSE_SECONDI] + s->distributed_total[COURSE_COFFEE]);
    printf("Piatti avanzati totali: %d\n",
           s->leftovers_total[COURSE_PRIMI] + s->leftovers_total[COURSE_SECONDI] + s->leftovers_total[COURSE_COFFEE]);
    printf("Attesa media complessiva (ms): %.2f\n", avg_wait_ms(total_wait, total_samples));
    printf("Operatori attivi simulazione: %d\n", s->unique_active_workers_total);
    printf("Pause totali simulazione: %d\n", s->pauses_total);
    print_money("Ricavo totale", s->revenue_cents_total);
    print_money("Ricavo medio per giornata", avg_rev);
}

static void cleanup_ipc(int shmid, int semid, SharedState *shared)
{
    if (shared != NULL) {
        close_all_pipes(shared, &shared->cfg);
    }
    if (shared != NULL && shmdt(shared) != 0) {
        perror("shmdt");
    }
    if (semid >= 0 && semctl(semid, 0, IPC_RMID) != 0) {
        perror("semctl IPC_RMID");
    }
    if (shmid >= 0 && shmctl(shmid, IPC_RMID, NULL) != 0) {
        perror("shmctl IPC_RMID");
    }
}

int main(int argc, char **argv)
{
    SimConfig cfg;
    MenuData menu;
    SharedState *shared;
    pid_t worker_pids[MAX_WORKERS];
    pid_t user_pids[MAX_USERS];
    int shmid;
    int semid;
    int total_children;
    int i;
    int days_completed;
    int overload_triggered;
    char errbuf[256];
    const char *cfg_path;
    const char *menu_path;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <config_file> [menu_file]\n", argv[0]);
        return 1;
    }

    cfg_path = argv[1];
    menu_path = (argc >= 3) ? argv[2] : "menu.txt";

    if (setup_signals() != 0) {
        return 1;
    }

    if (config_load_file(cfg_path, &cfg) != 0) {
        return 1;
    }
    if (config_validate(&cfg, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "Invalid config: %s\n", errbuf);
        return 1;
    }
    if (menu_load_file(menu_path, &menu) != 0) {
        return 1;
    }

    shmid = shmget(IPC_PRIVATE, sizeof(SharedState), IPC_CREAT | 0600);
    if (shmid < 0) {
        perror("shmget");
        return 1;
    }

    semid = semget(IPC_PRIVATE, SEM_COUNT, IPC_CREAT | 0600);
    if (semid < 0) {
        perror("semget");
        cleanup_ipc(shmid, -1, NULL);
        return 1;
    }

    shared = (SharedState *)shmat(shmid, NULL, 0);
    if (shared == (void *)-1) {
        perror("shmat");
        cleanup_ipc(shmid, semid, NULL);
        return 1;
    }

    memset(shared, 0, sizeof(*shared));
    shared->cfg = cfg;
    shared->menu = menu;

    if (setup_pipes(shared, &cfg) != 0) {
        cleanup_ipc(shmid, semid, shared);
        return 1;
    }

    if (init_semaphores(semid, &cfg) != 0) {
        cleanup_ipc(shmid, semid, shared);
        return 1;
    }

    for (i = 0; i < cfg.nof_workers; i++) {
        pid_t pid = spawn_worker(i, shmid, semid);
        if (pid < 0) {
            g_stop_requested = 1;
            cfg.nof_workers = i;
            break;
        }
        worker_pids[i] = pid;
    }

    for (i = 0; i < cfg.nof_users; i++) {
        pid_t pid = spawn_user(i, shmid, semid);
        if (pid < 0) {
            g_stop_requested = 1;
            cfg.nof_users = i;
            break;
        }
        user_pids[i] = pid;
    }

    total_children = cfg.nof_workers + cfg.nof_users;
    if (total_children > 0 && sem_op_retry(semid, SEM_INIT_READY, (short)-total_children) != 0) {
        g_stop_requested = 1;
    }

    days_completed = 0;
    overload_triggered = 0;

    for (i = 1; i <= cfg.sim_duration_days; i++) {
        int minute;
        int pending_at_close;
        int s;

        if (g_stop_requested) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        shared->current_day = i;
        shared->day_open = 1;
        reset_day_stats(shared);
        assign_workers(shared);

        if (sem_op_retry(semid, SEM_MUTEX, 1) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        if (sem_op_retry(semid, SEM_START_WORKERS, (short)cfg.nof_workers) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }
        if (sem_op_retry(semid, SEM_START_USERS, (short)cfg.nof_users) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        for (minute = 1; minute <= cfg.day_minutes; minute++) {
            sleep_ns(cfg.n_nano_secs);
            if (minute % 10 == 0) {
                if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
                    shared->termination_cause = TERM_SIGNAL;
                    break;
                }
                refill_portions(shared);
                if (sem_op_retry(semid, SEM_MUTEX, 1) != 0) {
                    shared->termination_cause = TERM_SIGNAL;
                    break;
                }
            }
            if (g_stop_requested) {
                shared->termination_cause = TERM_SIGNAL;
                break;
            }
        }

        if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        shared->day_open = 0;
        pending_at_close = queue_pending(shared);

        if (sem_op_retry(semid, SEM_MUTEX, 1) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        if (sem_op_retry(semid, SEM_END_WORKERS, (short)-cfg.nof_workers) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        drain_pending_requests(i, shared, semid);

        if (sem_op_retry(semid, SEM_END_USERS, (short)-cfg.nof_users) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        if (sem_op_retry(semid, SEM_MUTEX, -1) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        shared->leftovers_day[COURSE_PRIMI] = 0;
        for (s = 0; s < shared->menu.primi_count; s++) {
            shared->leftovers_day[COURSE_PRIMI] += shared->portions_primi[s];
        }

        shared->leftovers_day[COURSE_SECONDI] = 0;
        for (s = 0; s < shared->menu.secondi_count; s++) {
            shared->leftovers_day[COURSE_SECONDI] += shared->portions_secondi[s];
        }

        shared->leftovers_day[COURSE_COFFEE] = 0;

        for (s = 0; s < COURSE_TYPES; s++) {
            shared->leftovers_total[s] += shared->leftovers_day[s];
        }

        if (sem_op_retry(semid, SEM_MUTEX, 1) != 0) {
            shared->termination_cause = TERM_SIGNAL;
            break;
        }

        print_day_stats(shared, i, pending_at_close);
        days_completed = i;

        if (pending_at_close > cfg.overload_threshold) {
            shared->termination_cause = TERM_OVERLOAD;
            overload_triggered = 1;
            break;
        }
    }

    if (!g_stop_requested && !overload_triggered && shared->termination_cause == TERM_NONE) {
        shared->termination_cause = TERM_TIMEOUT;
    }

    if (g_stop_requested && shared->termination_cause == TERM_NONE) {
        shared->termination_cause = TERM_SIGNAL;
    }

    shared->terminate = 1;
    shared->day_open = 0;

    (void)sem_op_retry(semid, SEM_START_WORKERS, (short)cfg.nof_workers);
    (void)sem_op_retry(semid, SEM_START_USERS, (short)cfg.nof_users);

    for (i = 0; i < cfg.nof_workers; i++) {
        int status;
        (void)waitpid(worker_pids[i], &status, 0);
    }

    for (i = 0; i < cfg.nof_users; i++) {
        int status;
        (void)waitpid(user_pids[i], &status, 0);
    }

    print_final_stats(shared, days_completed);

    cleanup_ipc(shmid, semid, shared);
    return 0;
}
