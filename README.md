# Simulatore Mensa (versione minima)

Implementazione multi-processo della mensa con:
- `manager` (responsabile mensa)
- `operator` (operatori stazioni + cassa)
- `user` (utenti)

## Requisiti coperti
- processi separati avviati con `fork` + `exec`
- memoria condivisa (stato/statistiche)
- semafori System V (mutua esclusione, barriere, postazioni, tavoli)
- pipe per comunicazione tra processi (richieste/risposte)
- nessuna attesa attiva stretta (sync bloccante o sleep breve)
- compilazione con `make` e flag richiesti (`-Wvla -Wextra -Werror`, `_GNU_SOURCE`)
- statistiche giornaliere e finali
- terminazione per `timeout` e `overload`

## Build
```bash
make
```

## Esecuzione
```bash
./manager config/timeout.conf menu.txt
./manager config/overload.conf menu.txt
```

## File principali
- `src/manager.c`
- `src/operator.c`
- `src/user.c`
- `src/config.c`
- `src/sim_utils.c`
- `include/sim.h`

## Configurazioni consegnate
- `config/timeout.conf` -> termina per durata simulazione (`SIM_DURATION`)
- `config/overload.conf` -> termina per soglia utenti in attesa (`OVERLOAD_THRESHOLD`)
