/*
 * dostawca.cpp - Proces dostawcy składników (A, B, C lub D)
 * 
 * Każdy dostawca odpowiada za jeden typ składnika i cyklicznie
 * dostarcza go do magazynu. Ilość dostawy zależy od wolnego miejsca
 * i aktualnego zapasu danego składnika.
 * 
 * Rozmiary składników w jednostkach magazynowych:
 *   A = 1, B = 1, C = 2, D = 3
 * 
 * Dostawca reaguje na komendy:
 *   - StopDostawcaX (X=A/B/C/D) - zatrzymaj konkretnego dostawcę
 *   - StopAll - zatrzymaj wszystkich
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
volatile sig_atomic_t g_stop = 0;    // flaga zakończenia (sig_atomic_t dla bezpieczeństwa)
char g_type = 'A';                   // typ składnika (A/B/C/D)

void handle_signal(int) { g_stop = 1; }

// Ile jednostek magazynowych zajmuje jeden składnik danego typu
static int units_per_item(char t) {
    switch (t) {
        case 'A': return 1;
        case 'B': return 1;
        case 'C': return 2;  // składnik C jest większy
        case 'D': return 3;  // składnik D jest największy
        default:  return 1;
    }
}

// Mapowanie typu składnika na indeks semafora
static int sem_index_for(char t) {
    switch (t) {
        case 'A': return SEM_A;
        case 'B': return SEM_B;
        case 'C': return SEM_C;
        case 'D': return SEM_D;
        default:  return SEM_A;
    }
}

/*
 * target_units() - Docelowa ilość jednostek danego składnika
 * 
 * Dostawca stara się utrzymać taki miks w magazynie:
 *   A: 2/9, B: 2/9, C: 2/9, D: 3/9 pojemności
 * 
 * To zapewnia że stanowiska zawsze mają z czego produkować.
 */
static int target_units(char t, int capUnits) {
    switch (t) {
        case 'A': return (2 * capUnits) / 9;
        case 'B': return (2 * capUnits) / 9;
        case 'C': return (2 * capUnits) / 9;
        case 'D': return (3 * capUnits) / 9;
        default:  return capUnits / 4;
    }
}

// Tworzenie klucza IPC (ten sam co w magazynie)
key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_perror("ftok");
    return key;
}

/*
 * attach_ipc() - Dołączenie do istniejących zasobów IPC
 * 
 * Dostawca nie tworzy zasobów (robi to magazyn), tylko się dołącza.
 * Bez IPC_CREAT - jeśli zasoby nie istnieją, kończymy z błędem.
 */
void attach_ipc() {
    key_t key = make_key();

    // Pamięć dzielona (bez IPC_CREAT = dołącz do istniejącej)
    g_shmid = shmget(key, sizeof(WarehouseState), 0600);
    if (g_shmid == -1) die_perror("shmget");

    // Mapowanie pamięci
    g_state = static_cast<WarehouseState*>(shmat(g_shmid, nullptr, 0));
    if (g_state == reinterpret_cast<void*>(-1)) die_perror("shmat");

    // Semafory
    g_semid = semget(key, SEM_COUNT, 0600);
    if (g_semid == -1) die_perror("semget");

    // Kolejka komunikatów
    g_msgid = msgget(key, 0600);
    if (g_msgid == -1) die_perror("msgget");
}

// Deklaracja - używana w deliver_once()
void check_command();

/*
 * deliver_once() - Jedna dostawa składnika do magazynu
 * 
 * Algorytm:
 *   1. Sprawdź ile miejsca wolnego w magazynie
 *   2. Oblicz ile składnika już jest i ile brakuje do targetu
 *   3. Rezerwuj miejsce (P na SEM_CAPACITY)
 *   4. Aktualizuj stan w pamięci dzielonej
 *   5. Zwiększ semafor składnika (V) - stanowiska mogą teraz pobierać
 * 
 * Między krokami sprawdzamy czy nie przyszła komenda stop.
 */
void deliver_once(int amount) {
    int uPer = units_per_item(g_type);
    int units = amount * uPer;

    // Odczyt stanu magazynu (z mutexem)
    P_mutex(g_semid);
    int cap = g_state->capacity;
    int a = g_state->a, b = g_state->b, c = g_state->c, d = g_state->d;
    int usedUnits = a + b + 2*c + 3*d;
    int freeUnits = cap - usedUnits;
    if (freeUnits < 0) freeUnits = 0;

    // Ile jednostek naszego składnika jest obecnie
    int myUnitsNow = 0;
    if (g_type == 'A') myUnitsNow = a * 1;
    if (g_type == 'B') myUnitsNow = b * 1;
    if (g_type == 'C') myUnitsNow = c * 2;
    if (g_type == 'D') myUnitsNow = d * 3;

    int myTarget = target_units(g_type, cap);
    V_mutex(g_semid);

    // Jeśli już mamy wystarczająco dużo tego składnika, odpuszczamy
    // (żeby nie zapychać magazynu jednym typem)
    if (freeUnits < 10 && myUnitsNow > myTarget) {
        std::cout << "[DOSTAWCA " << g_type << "] miks OK? mam " << myUnitsNow
                  << "u > target " << myTarget << "u, czekam\n";
        // Krótkie czekanie ze sprawdzaniem czy nie przyszła komenda stop
        for (int i = 0; i < 3 && !g_stop; i++) {
            usleep(100000);
            check_command();
        }
        return;
    }

    // Rezerwacja miejsca w magazynie (nieblokujące)
    // IPC_NOWAIT - jeśli nie ma miejsca, nie czekamy
    sembuf op{static_cast<unsigned short>(SEM_CAPACITY), static_cast<short>(-units), IPC_NOWAIT};
    if (semop(g_semid, &op, 1) == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::cout << "[DOSTAWCA " << g_type << "] magazyn pełny, czekam\n";
            for (int i = 0; i < 3 && !g_stop; i++) {
                usleep(100000);
                check_command();
            }
            return;
        }
        perror("semop capacity");
        return;
    }

    // Aktualizacja stanu w pamięci dzielonej
    P_mutex(g_semid);
    if (g_type == 'A') g_state->a += amount;
    if (g_type == 'B') g_state->b += amount;
    if (g_type == 'C') g_state->c += amount;
    if (g_type == 'D') g_state->d += amount;
    V_mutex(g_semid);

    // Zwiększenie semafora składnika - stanowiska mogą teraz pobierać
    V(g_semid, sem_index_for(g_type), amount);

    // Log do pliku raportu
    char buf[128];
    P_mutex(g_semid);
    std::snprintf(buf, sizeof(buf), "Dostarczono %d x %c (stan: A=%d B=%d C=%d D=%d)",
                  amount, g_type, g_state->a, g_state->b, g_state->c, g_state->d);
    V_mutex(g_semid);
    log_raport(g_semid, "DOSTAWCA", buf);

    std::cout << "[DOSTAWCA " << g_type << "] +" << amount << "\n";

    // Raport do magazynu (przez kolejkę komunikatów)
    SupplierReportMessage rep{};
    rep.mtype = static_cast<long>(MsgType::SupplierReport);
    std::snprintf(rep.text, sizeof(rep.text), "Dostawca %c + %d", g_type, amount);
    if (msgsnd(g_msgid, &rep, sizeof(rep) - sizeof(long), IPC_NOWAIT) == -1 && errno != EAGAIN) {
        perror("msgsnd report");
    }
}

/*
 * check_command() - Sprawdzenie czy przyszła komenda stop
 * 
 * Używa globalnej funkcji ::check_command() z common.h, która
 * sprawdza kolejkę po PID procesu (mtype = getpid()).
 */
void check_command() {
    Command cmd = ::check_command(g_msgid);
    if (cmd == Command::StopDostawcy || cmd == Command::StopAll) {
        g_stop = 1;
    }
}

}  // namespace

/*
 * main() - Punkt wejścia dostawcy
 * 
 * Argumenty:
 *   argv[1] = typ składnika (A/B/C/D) - wymagany
 *   argv[2] = ilość na dostawę (opcjonalnie, domyślnie 1)
 * 
 * Dostawca w pętli:
 *   1. Sprawdza komendy
 *   2. Wykonuje dostawę
 *   3. Losowa przerwa 1-3 sekundy (z okresowym sprawdzaniem komend)
 */
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Uzycie: dostawca <A|B|C|D> [amount]\n";
        return 1;
    }

    // Walidacja typu składnika
    g_type = argv[1][0];
    if (g_type != 'A' && g_type != 'B' && g_type != 'C' && g_type != 'D') {
        std::cerr << "Błąd: typ dostawcy musi być A, B, C lub D.\n";
        return 1;
    }

    // Walidacja ilości z strtol 
    int amount = 1;
    if (argc >= 3) {
        char *endptr = nullptr;
        long val = std::strtol(argv[2], &endptr, 10);
        if (endptr == argv[2] || *endptr != '\0' || val <= 0 || val > 100) {
            std::cerr << "Błąd: amount musi być liczbą 1-100.\n";
            return 1;
        }
        amount = static_cast<int>(val);
    }

    // Konfiguracja sygnałów i dołączenie do IPC
    setup_sigaction(handle_signal);
    attach_ipc();

    // Seed dla losowych przerw (różny dla każdego procesu dzięki XOR z PID)
    srand(static_cast<unsigned>(time(nullptr)) ^ getpid());

    // Główna pętla dostawcy
    while (!g_stop) {
        check_command();
        if (g_stop) break;

        deliver_once(amount);

        check_command();
        if (g_stop) break;

        // Losowa przerwa 1-3 sekundy (z okresowym sprawdzaniem komend)
        int delay = (rand() % 3) + 1;
        for (int i = 0; i < delay * 10 && !g_stop; i++) {
            usleep(100000);  // 100ms
            check_command();
        }
    }

    // Log o zakończeniu pracy
    char endbuf[64];
    std::snprintf(endbuf, sizeof(endbuf), "Dostawca %c konczy prace (pid=%d)", g_type, getpid());
    log_raport(g_semid, "DOSTAWCA", endbuf);

    // Odłączenie od pamięci dzielonej (nie usuwamy zasobów!)
    if (g_state && shmdt(g_state) == -1) perror("shmdt");

    return 0;
}