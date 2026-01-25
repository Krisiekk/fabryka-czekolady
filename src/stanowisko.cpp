// Proces stanowiska produkcyjnego (1 lub 2)
// Pobiera surowce z magazynu i produkuje czekoladę
// Typ 1: A+B+C, Typ 2: A+B+D

#include "../include/common.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/prctl.h>  // prctl(PR_SET_PDEATHSIG)

namespace {

// Zmienne globalne
int g_semid = -1;                     // ID semaforów
int g_shmid = -1;                     // ID pamięci dzielonej
WarehouseHeader *g_header = nullptr;  // nagłówek magazynu
volatile sig_atomic_t g_stop = 0;     // flaga do koniec pracy
int g_workerType = 1;                 // typ stanowiska (1 lub 2)
int g_produced = 0;                   // ile czekolad wyprodukowano

// Obsługuje sygnał - ustawia flagę aby wyjść z pętli
void handle_signal(int) { g_stop = 1; }

// Pobiera klucz IPC
key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_perror("ftok");
    return key;
}

// Łączy się do istniejących zasobów IPC (magazyn je wcześniej stworzył)
void attach_ipc() {
    key_t key = make_key();

    // Dołącz do pamięci dzielonej
    g_shmid = shmget(key, 0, 0600);
    if (g_shmid == -1) die_perror("shmget");

    g_header = static_cast<WarehouseHeader*>(shmat(g_shmid, nullptr, 0));
    if (g_header == reinterpret_cast<void*>(-1)) die_perror("shmat");

    // Dołącz do semaforów
    g_semid = semget(key, SEM_COUNT, 0600);
    if (g_semid == -1) die_perror("semget");
}

// Zwraca informacje o segmencie (gdzie są dane, jaki rozmiar, które semafory)
void get_segment_info(char type, char*& segment, int& itemSize, int& capacity, 
                      int& semEmpty, int& semOut) {
    switch (type) {
        case 'A':
            segment = segment_A(g_header);
            itemSize = kSizeA;
            capacity = g_header->capacityA;
            semEmpty = SEM_EMPTY_A;
            semOut = SEM_OUT_A;
            break;
        case 'B':
            segment = segment_B(g_header);
            itemSize = kSizeB;
            capacity = g_header->capacityB;
            semEmpty = SEM_EMPTY_B;
            semOut = SEM_OUT_B;
            break;
        case 'C':
            segment = segment_C(g_header);
            itemSize = kSizeC;
            capacity = g_header->capacityC;
            semEmpty = SEM_EMPTY_C;
            semOut = SEM_OUT_C;
            break;
        case 'D':
            segment = segment_D(g_header);
            itemSize = kSizeD;
            capacity = g_header->capacityD;
            semEmpty = SEM_EMPTY_D;
            semOut = SEM_OUT_D;
            break;
        default:
            segment = segment_A(g_header);
            itemSize = kSizeA;
            capacity = g_header->capacityA;
            semEmpty = SEM_EMPTY_A;
            semOut = SEM_OUT_A;
    }
}

// Mapuje typ składnika na odpowiedni semafor FULL
static int sem_full_for(char type) {
    switch (type) {
        case 'A': return SEM_FULL_A;
        case 'B': return SEM_FULL_B;
        case 'C': return SEM_FULL_C;
        case 'D': return SEM_FULL_D;
        default:  return SEM_FULL_A;
    }
}

// Pobiera jeden składnik z magazynu (ring buffer - wyciąga dane z segmentu)
// Czeka na P(FULL), potem czyta dane i zwolnia miejsce z V(EMPTY)
bool consume_one(char type) {
    char *segment;
    int itemSize, capacity, semEmpty, semOut;
    get_segment_info(type, segment, itemSize, capacity, semEmpty, semOut);
    int segmentSize = capacity * itemSize;
    int semFull = sem_full_for(type);
    
    // Wejdź do sekcji krytycznej (żeby OUT nie zmienił się w środku)
    P_mutex(g_semid);
    
    // Przeczytaj gdzie jest dane do odczytania
    int outOffset = semctl(g_semid, semOut, GETVAL);
    if (outOffset == -1) {
        perror("semctl GETVAL OUT");
        V_mutex(g_semid);
        return false;
    }
    
    // Oblicz następny offset (ring buffer - wracamy na początek)
    int newOutOffset = (outOffset + itemSize) % segmentSize;
    
    // Przesuń wskaźnik OUT na następne dane
    union semun arg;
    arg.val = newOutOffset;
    if (semctl(g_semid, semOut, SETVAL, arg) == -1) {
        perror("semctl SETVAL OUT");
        V_mutex(g_semid);
        return false;
    }
    
    // Wyczyść miejsce gdzie były dane
    char *slot = segment + outOffset;
    std::memset(slot, 0, itemSize);
    
    V_mutex(g_semid);
    // Koniec sekcji krytycznej
    
    // Powiadomimy dostawcę że teraz jest miejsce na nowe dane
    if (sem_V_retry(g_semid, semEmpty, 1) == -1) {
        perror("sem_V EMPTY");
    }
    
    return true;
}

// Produkuje jedną porcję czekolady - pobiera A, B i C (dla typu 1) lub D (dla typu 2)
// Czeka na każdy składnik (P na FULL) i pobiera go (consume_one)
bool produce_one() {
    // Sprawdź czy magazyn otwarty - jeśli nie, wypisz info i czekaj
    int warehouseOn = semctl(g_semid, SEM_WAREHOUSE_ON, GETVAL);
    if (warehouseOn == 0) {
        std::cout << "[STANOWISKO " << g_workerType << "] Magazyn zamknięty - czekam na wznowienie pracy...\n";
    }
    
    // Czekaj aż magazyn będzie otwarty (atomowa bramka - bezpieczne przy SIGSTOP)
    if (pass_gate_intr(g_semid, SEM_WAREHOUSE_ON) == -1) {
        return false;  // EINTR = sygnał
    }

    char typeC_or_D = (g_workerType == 1) ? 'C' : 'D';
    int semFullC_or_D = (g_workerType == 1) ? SEM_FULL_C : SEM_FULL_D;
    
    // Czekaj na składnik A
    std::cout << "[STANOWISKO " << g_workerType << "] Czekam na A...\n";
    if (sem_P_intr(g_semid, SEM_FULL_A, 1) == -1) {
        return false;
    }
    if (!consume_one('A')) {
        return false;
    }
    
    // Czekaj na składnik B
    std::cout << "[STANOWISKO " << g_workerType << "] Czekam na B...\n";
    if (sem_P_intr(g_semid, SEM_FULL_B, 1) == -1) {
        return false;
    }
    if (!consume_one('B')) {
        return false;
    }
    
    // Czekaj na C lub D (zależy od typu stanowiska)
    std::cout << "[STANOWISKO " << g_workerType << "] Czekam na " << typeC_or_D << "...\n";
    if (sem_P_intr(g_semid, semFullC_or_D, 1) == -1) {
        return false;
    }
    if (!consume_one(typeC_or_D)) {
        return false;
    }
    
    // Mamy wszystko! Produkujemy czekoladę
    g_produced++;
    
    char buf[128];
    std::snprintf(buf, sizeof(buf), 
                  "Stanowisko %d wyprodukowano czekoladę #%d (A+B+%c)",
                  g_workerType, g_produced, typeC_or_D);
    log_raport(g_semid, "STANOWISKO", buf);
    
    std::cout << "[STANOWISKO " << g_workerType << "] Produkuję czekoladę #" 
              << g_produced << "...\n";
    
    // Symulacja czasu produkcji
    sleep(1);
    
    return true;
}

}  // namespace

// Główna funkcja - uruchamia pracownika na stanowisku
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Uzycie: stanowisko <1|2>\n";
        return 1;
    }

    // Sprawdzenie czy podano prawidłowy typ stanowiska (1 lub 2)
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
    
    // Jeśli dyrektor zginie (np. SIGKILL), dostaniemy SIGTERM
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    
    attach_ipc();

    const char *recipe = (g_workerType == 1) ? "A+B+C" : "A+B+D";
    std::cout << "[STANOWISKO " << g_workerType << "] Start (pid=" << getpid() 
              << ", przepis=" << recipe << ")\n";

    // Główna pętla - produkuj czekoladę aż do sygnału SIGTERM
    while (!g_stop) {
        if (!produce_one()) {
            // Błąd lub przerwanie sygnałem
            if (g_stop) break;
            sleep(1);
        }
    }

    // Wypisz podsumowanie
    char endbuf[128];
    std::snprintf(endbuf, sizeof(endbuf), 
                  "Stanowisko %d kończy pracę (wyprodukowano %d czekolad)",
                  g_workerType, g_produced);
    log_raport(g_semid, "STANOWISKO", endbuf);
    std::cout << "[STANOWISKO " << g_workerType << "] Zakończono. "
              << "Wyprodukowano: " << g_produced << " czekolad.\n";

    // Odłącz się od pamięci dzielonej
    if (g_header && shmdt(g_header) == -1) perror("shmdt");

    return 0;
}
