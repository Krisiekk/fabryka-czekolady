// Proces dostawcy - dostarcza składniki do magazynu
// Czyta z semaforów i pisze dane do pamięci dzielonej
// Kończy pracę po sygnale SIGTERM

#include "../include/common.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/prctl.h>  // prctl(PR_SET_PDEATHSIG)
#include <thread>

namespace {

// Zmienne globalne
int g_semid = -1;                     // ID semaforów
int g_shmid = -1;                     // ID pamięci dzielonej
WarehouseHeader *g_header = nullptr;  // nagłówek magazynu
volatile sig_atomic_t g_stop = 0;     // flaga do końca
char g_type = 'A';                    // typ składnika A/B/C/D
int g_msqid = -1;                      // kolejka komunikatów
std::thread g_mq_thread;               // wątek odbierający powiadomienia
volatile sig_atomic_t g_msg_state = -1; // ostatni stan otrzymany z dyrektora (0/1)

void handle_signal(int) { g_stop = 1; }

// Zwraca rozmiar składnika w bajtach
static int size_of(char t) {
    switch (t) {
        case 'A': return kSizeA;
        case 'B': return kSizeB;
        case 'C': return kSizeC;
        case 'D': return kSizeD;
        default:  return 1;
    }
}

// Zwraca semafor EMPTY dla danego typu
static int sem_empty_for(char t) {
    switch (t) {
        case 'A': return SEM_EMPTY_A;
        case 'B': return SEM_EMPTY_B;
        case 'C': return SEM_EMPTY_C;
        case 'D': return SEM_EMPTY_D;
        default:  return SEM_EMPTY_A;
    }
}

// Zwraca semafor FULL dla danego typu
static int sem_full_for(char t) {
    switch (t) {
        case 'A': return SEM_FULL_A;
        case 'B': return SEM_FULL_B;
        case 'C': return SEM_FULL_C;
        case 'D': return SEM_FULL_D;
        default:  return SEM_FULL_A;
    }
}

// Zwraca semafor IN dla danego typu
static int sem_in_for(char t) {
    switch (t) {
        case 'A': return SEM_IN_A;
        case 'B': return SEM_IN_B;
        case 'C': return SEM_IN_C;
        case 'D': return SEM_IN_D;
        default:  return SEM_IN_A;
    }
}

// Zwraca segment i pojemność dla typu
static char* get_segment(char t, int& capacity) {
    switch (t) {
        case 'A':
            capacity = g_header->capacityA;
            return segment_A(g_header);
        case 'B':
            capacity = g_header->capacityB;
            return segment_B(g_header);
        case 'C':
            capacity = g_header->capacityC;
            return segment_C(g_header);
        case 'D':
            capacity = g_header->capacityD;
            return segment_D(g_header);
        default:
            capacity = g_header->capacityA;
            return segment_A(g_header);
    }
}

// Tworzy klucz IPC
key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_perror("ftok");
    return key;
}

// Łączy się do zasobów IPC (które stworzył magazyn)
void attach_ipc() {
    key_t key = make_key();

    // Najpierw musimy poznać rozmiar pamięci - dołączamy bez podawania rozmiaru
    // (shmget z rozmiarem 0 dołącza do istniejącego segmentu)
    g_shmid = shmget(key, 0, 0600);
    if (g_shmid == -1) die_perror("shmget");

    // Mapowanie pamięci
    g_header = static_cast<WarehouseHeader*>(shmat(g_shmid, nullptr, 0));
    if (g_header == reinterpret_cast<void*>(-1)) die_perror("shmat");

    // Semafory
    g_semid = semget(key, SEM_COUNT, 0600);
    if (g_semid == -1) die_perror("semget");
}

// Dostarcza jeden składnik - czeka na P(EMPTY), pisze dane, robi V(FULL)
bool deliver_one() {
    // Sprawdź czy magazyn otwarty - jeśli nie, wypisz info i czekaj
    int warehouseOn = semctl(g_semid, SEM_WAREHOUSE_ON, GETVAL);
    if (warehouseOn == 0) {
        std::cout << "[DOSTAWCA " << g_type << "] Magazyn zamknięty - czekam na wznowienie pracy...\n";
    }
    
    // Czekaj aż magazyn będzie otwarty (atomowa bramka - bezpieczne przy SIGSTOP)
    if (pass_gate_intr(g_semid, SEM_WAREHOUSE_ON) == -1) {
        return false;  // EINTR = sygnał
    }

    int itemSize = size_of(g_type);
    int semEmpty = sem_empty_for(g_type);
    int semFull = sem_full_for(g_type);
    int semIn = sem_in_for(g_type);
    
    // Czekaj na miejsce w magazynie
    if (sem_P_intr(g_semid, semEmpty, 1) == -1) {
        if (errno == EINTR) return false;
        perror("sem_P EMPTY");
        return false;
    }
    
    // Pobierz segment i jego rozmiar
    int capacity;
    char *segment = get_segment(g_type, capacity);
    int segmentSize = capacity * itemSize;
    
    // Wchodzimy do sekcji krytycznej
    P_mutex(g_semid);
    
    // Pobierz offset zapisu
    int inOffset = semctl(g_semid, semIn, GETVAL);
    if (inOffset == -1) {
        perror("semctl GETVAL IN");
        V_mutex(g_semid);
        sem_V_retry(g_semid, semEmpty, 1);
        return false;
    }
    
    // Oblicz następny offset (ring buffer)
    int newInOffset = (inOffset + itemSize) % segmentSize;
    
    // Zapisz dane
    char *dest = segment + inOffset;
    std::memset(dest, static_cast<int>(g_type), itemSize);
    
    // Ustaw nowy offset
    union semun arg;
    arg.val = newInOffset;
    if (semctl(g_semid, semIn, SETVAL, arg) == -1) {
        perror("semctl SETVAL IN");
        std::memset(dest, 0, itemSize);
        V_mutex(g_semid);
        sem_V_retry(g_semid, semEmpty, 1);
        return false;
    }
    
    V_mutex(g_semid);
    
    // Sygnalizuj że są dostępne dane
    if (sem_V_retry(g_semid, semFull, 1) == -1) {
        perror("sem_V FULL");
        return false;
    }
    
    // Zaloguj dostawę z aktualnym stanem semaforów
    int elemNum = inOffset / itemSize;
    int fullVal = semctl(g_semid, semFull, GETVAL);
    int emptyVal = semctl(g_semid, semEmpty, GETVAL);
    
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Dostarczono 1 x %c (IN=%d/%d, FULL=%d, EMPTY=%d)",
                  g_type, elemNum, capacity, fullVal, emptyVal);
    log_raport(g_semid, "DOSTAWCA", buf);
    
    std::cout << "[DOSTAWCA " << g_type << "] +1 (IN=" << elemNum 
              << "/" << capacity << " FULL=" << fullVal 
              << " EMPTY=" << emptyVal << ")\n";
    
    return true;
}

}  // namespace

// Główna funkcja dostawcy
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Uzycie: dostawca <A|B|C|D>\n";
        return 1;
    }

    // Sprawdzenie typu
    g_type = argv[1][0];
    if (g_type != 'A' && g_type != 'B' && g_type != 'C' && g_type != 'D') {
        std::cerr << "Błąd: typ dostawcy musi być A, B, C lub D.\n";
        return 1;
    }

    // Inicjalizacja
    setup_sigaction(handle_signal);
    
    // Jeśli dyrektor zginie (np. SIGKILL), dostaniemy SIGTERM
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    
    attach_ipc();
    srand(static_cast<unsigned>(time(nullptr)) ^ getpid());

    // Dołącz do kolejki komunikatów utworzonej przez dyrektora
    g_msqid = msgget(make_key(), 0);
    if (g_msqid == -1) {
        // jeśli brak kolejki, kontynuujemy bez powiadomień
        perror("msgget");
    } else {
        // Uruchom listener w tle
        g_mq_thread = std::thread([](){
            while (!g_stop) {
                int state = -1;
                if (msq_recv_pid_intr(g_msqid, getpid(), &state) == -1) {
                    if (errno == EINTR) {
                        if (g_stop) break;
                        continue;
                    }
                    perror("msgrcv");
                    break;
                }
                g_msg_state = state;
                std::cout << "[DOSTAWCA " << g_type << "] Otrzymano powiadomienie: state=" << state << "\n";
            }
        });
    }

    std::cout << "[DOSTAWCA " << g_type << "] Start (pid=" << getpid() 
              << ", rozmiar=" << size_of(g_type) << "B)\n";

    // Główna pętla
    while (!g_stop) {
        if (!deliver_one()) {
            if (g_stop) break;
            continue;
        }
        if (!g_stop) {
            int delay = (rand() % 2) + 1;
            sleep(delay);
        }
    }

    // Koniec
    char endbuf[64];
    std::snprintf(endbuf, sizeof(endbuf), "Dostawca %c kończy pracę", g_type);
    log_raport(g_semid, "DOSTAWCA", endbuf);
    std::cout << "[DOSTAWCA " << g_type << "] Zakończono.\n";

    // Zatrzymaj listener kolejki (jeśli działa)
    if (g_mq_thread.joinable()) {
        // Wywołanie msgrcv jest przerwane sygnałem SIGTERM przez dyrektora,
        // więc wątek zakończy pętlę i się dołączy
        g_mq_thread.join();
    }

    // Odłącz się
    if (g_header && shmdt(g_header) == -1) perror("shmdt");

    return 0;
}
