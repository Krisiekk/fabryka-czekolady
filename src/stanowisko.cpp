/*
 * stanowisko.cpp - Proces stanowiska produkcyjnego (typ 1 lub 2)
 * 
 * Stanowisko cyklicznie produkuje czekoladę pobierając składniki z magazynu.
 * Przepisy:
 *   - Typ 1: A + B + C -> czekolada
 *   - Typ 2: A + B + D -> czekolada
 * 
 * Stanowisko wysyła żądanie do magazynu i czeka na odpowiedź.
 * Jeśli brak składników (granted=false), czeka losowy czas i próbuje ponownie.
 * 
 * Autor: Krzysztof Pietrzak (156721)
 */

#include "../include/common.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

// --- Zmienne globalne ---
int g_msgid = -1;                    // ID kolejki komunikatów
int g_semid = -1;                    // ID zestawu semaforów
int g_shmid = -1;                    // ID pamięci dzielonej
WarehouseState *g_state = nullptr;   // wskaźnik na stan magazynu
volatile sig_atomic_t g_stop = 0;    // flaga zakończenia
int g_workerType = 1;                // typ stanowiska (1 lub 2)

// Deklaracja - używana przed definicją
void check_command();

void handle_signal(int) { g_stop = 1; }

// Tworzenie klucza IPC (identyczny jak w innych procesach)
key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_perror("ftok");
    return key;
}

/*
 * attach_ipc() - Dołączenie do istniejących zasobów IPC
 * 
 * Stanowisko nie tworzy zasobów, tylko się dołącza.
 * Kolejność: pamięć dzielona -> semafory -> kolejka.
 */
void attach_ipc() {
    key_t key = make_key();

    // Pamięć dzielona
    g_shmid = shmget(key, sizeof(WarehouseState), 0600);
    if (g_shmid == -1) die_perror("shmget");

    g_state = static_cast<WarehouseState*>(shmat(g_shmid, nullptr, 0));
    if (g_state == reinterpret_cast<void*>(-1)) die_perror("shmat");

    // Semafory
    g_semid = semget(key, SEM_COUNT, 0600);
    if (g_semid == -1) die_perror("semget");

    // Kolejka komunikatów
    g_msgid = msgget(key, 0600);
    if (g_msgid == -1) die_perror("msgget");
}

/*
 * request_and_produce() - Jeden cykl produkcji
 * 
 * Algorytm:
 *   1. Przygotuj żądanie (typ stanowiska określa które składniki)
 *   2. Wyślij żądanie do magazynu (msgsnd)
 *   3. Czekaj na odpowiedź (msgrcv z mtype=pid)
 *   4. Jeśli granted=true -> produkuj (symulacja = sleep)
 *   5. Jeśli granted=false -> czekaj i spróbuj później
 */
void request_and_produce() {
    // Przygotowanie żądania
    WorkerRequestMessage req{};
    req.mtype = static_cast<long>(MsgType::WorkerRequest);
    req.workerType = g_workerType;
    req.pid = getpid();

    // Składniki zależą od typu stanowiska
    if (g_workerType == 1) {
        // Przepis 1: A + B + C
        req.needA = 1;
        req.needB = 1;
        req.needC = 1;
        req.needD = 0;
    } else {
        // Przepis 2: A + B + D
        req.needA = 1;
        req.needB = 1;
        req.needC = 0;
        req.needD = 1;
    }

    // Wysłanie żądania do magazynu
    if (msgsnd(g_msgid, &req, sizeof(req) - sizeof(long), 0) == -1) {
        perror("msgsnd worker request");
        return;
    }

    // Czekanie na odpowiedź od magazynu
    // mtype = nasz PID, żeby odebrać odpowiedź skierowaną do nas
    WarehouseReplyMessage rep{};
    while (!g_stop) {
        ssize_t r = msgrcv(g_msgid, &rep, sizeof(rep) - sizeof(long), req.pid, IPC_NOWAIT);

        if (r >= 0) {
            std::cout << "[PRACOWNIK] got reply: pid=" << getpid()
                      << " granted=" << rep.granted << "\n";
            break;
        }

        if (errno == ENOMSG) {
            // Brak wiadomości - sprawdź komendy i czekaj
            check_command();
            usleep(100000);  // 100ms
            continue;
        }
        if (errno == EINTR) {
            // Przerwane przez sygnał
            if (g_stop) return;
            continue;
        }

        perror("msgrcv worker reply");
        return;
    }

    // Sprawdzenie czy dostaliśmy składniki
    if (!rep.granted) {
        std::cerr << "Brak surowcow dla pracownika, czekam...\n";
        usleep(1000000 + (rand() % 1000000));  // 1-2 sekundy
        return;
    }

    // Produkcja! (logowanie + symulacja czasu produkcji)
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Stanowisko %d wyprodukowano czekolade (pid=%d)",
                  g_workerType, getpid());
    log_raport(g_semid, "STANOWISKO", buf);

    std::cout << "Pracownik " << g_workerType << " Produkuje czekolade ...\n";
    int delay = (rand() % 2) + 1;  // 1-2 sekundy
    sleep(delay);
}

/*
 * check_command() - Sprawdzenie czy przyszła komenda stop
 */
void check_command() {
    Command cmd = ::check_command(g_msgid);
    if (cmd == Command::StopFabryka || cmd == Command::StopAll) {
        g_stop = 1;
    }
}

}  // namespace

/*
 * main() - Punkt wejścia stanowiska
 * 
 * Argument:
 *   argv[1] = typ stanowiska (1 lub 2) - wymagany
 * 
 * Stanowisko w pętli produkuje czekoladę, okresowo sprawdzając
 * czy nie przyszła komenda zakończenia.
 */
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Uzycie: stanowisko <1|2>\n";
        return 1;
    }

    // Walidacja typu stanowiska z strtol (wymaganie 4.1b)
    char *endptr = nullptr;
    long val = std::strtol(argv[1], &endptr, 10);
    if (endptr == argv[1] || *endptr != '\0' || (val != 1 && val != 2)) {
        std::cerr << "Błąd: typ stanowiska musi być 1 lub 2.\n";
        return 1;
    }
    g_workerType = static_cast<int>(val);

    // Inicjalizacja
    srand(static_cast<unsigned>(time(nullptr)) ^ getpid());
    setup_sigaction(handle_signal);
    attach_ipc();

    // Główna pętla produkcji
    while (!g_stop) {
        request_and_produce();
        check_command();
    }

    // Log o zakończeniu
    char endbuf[64];
    std::snprintf(endbuf, sizeof(endbuf), "Stanowisko %d konczy prace (pid=%d)", g_workerType, getpid());
    log_raport(g_semid, "STANOWISKO", endbuf);

    // Odłączenie od pamięci dzielonej
    if (g_state && shmdt(g_state) == -1) perror("shmdt");

    return 0;
}