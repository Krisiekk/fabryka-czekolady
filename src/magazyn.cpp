// Proces magazynu - tworzy zasoby IPC i zarządza nimi
// Inicjalizuje pamięć dzieloną, semafory i mutexy
// Kończy pracę po sygnale SIGTERM lub SIGUSR1

#include "../include/common.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <cerrno>
#include <sys/prctl.h>  // prctl(PR_SET_PDEATHSIG)

namespace {

// Zmienne globalne
int g_semid = -1;                        // ID semaforów
int g_shmid = -1;                        // ID pamięci dzielonej
WarehouseHeader *g_header = nullptr;     // nagłówek magazynu
volatile sig_atomic_t g_stop = 0;        // flaga zakoczenia
volatile sig_atomic_t g_save_on_exit = 0; // flaga zapisu przy wyjściu
std::string g_stateFile = "magazyn_state.txt";

// Obsługi sygnałów
void handle_sigterm(int) { g_stop = 1; }
void handle_sigusr1(int) { g_stop = 1; g_save_on_exit = 1; }

// Tworzy klucz IPC
key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_perror("ftok");
    return key;
}

// Inicjalizuje zasoby IPC
void init_ipc(int targetChocolates) {
    key_t key = make_key();
    size_t shmSize = calc_shm_size(targetChocolates);

    // Pamięć dzielona - IPC_EXCL 
    g_shmid = shmget(key, shmSize, IPC_CREAT | IPC_EXCL | 0600);
    bool fresh = true;

    if (g_shmid == -1) {
        if (errno != EEXIST) die_perror("shmget");
        // Segment już istnieje - dołącz do niego
        fresh = false;
        g_shmid = shmget(key, 0, 0600);
        if (g_shmid == -1) die_perror("shmget attach");
    }

    // Mapowanie pamięci dzielonej
    g_header = static_cast<WarehouseHeader*>(shmat(g_shmid, nullptr, 0));
    if (g_header == reinterpret_cast<void*>(-1)) die_perror("shmat");
    
    // Semafory - zawsze dołącz, nawet jeśli segment jest stary
    g_semid = semget(key, SEM_COUNT, IPC_CREAT | 0600);  
    if (g_semid == -1) die_perror("semget");
    
    if (fresh) {
        // Wyzerowanie CAŁEJ pamięci dzielonej (nagłówek + dane)
        std::memset(g_header, 0, shmSize);

        // Inicjalizacja nagłówka magazynu
        init_warehouse_header(g_header, targetChocolates);

        semun arg{};

        // MUTEX = 1
        arg.val = 1;
        if (semctl(g_semid, SEM_MUTEX, SETVAL, arg) == -1) die_perror("semctl mutex");

        // RAPORT = 1
        arg.val = 1;
        if (semctl(g_semid, SEM_RAPORT, SETVAL, arg) == -1) die_perror("semctl SEM_RAPORT");

        // EMPTY_X = pojemność
        arg.val = g_header->capacityA;
        if (semctl(g_semid, SEM_EMPTY_A, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_A");
        arg.val = g_header->capacityB;
        if (semctl(g_semid, SEM_EMPTY_B, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_B");
        arg.val = g_header->capacityC;
        if (semctl(g_semid, SEM_EMPTY_C, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_C");
        arg.val = g_header->capacityD;
        if (semctl(g_semid, SEM_EMPTY_D, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_D");

        // FULL_X = 0 (magazyn pusty)
        arg.val = 0;
        if (semctl(g_semid, SEM_FULL_A, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_A");
        if (semctl(g_semid, SEM_FULL_B, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_B");
        if (semctl(g_semid, SEM_FULL_C, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_C");
        if (semctl(g_semid, SEM_FULL_D, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_D");

        // IN_X = 0 i OUT_X = 0 (offsety startowe)
        arg.val = 0;
        if (semctl(g_semid, SEM_IN_A, SETVAL, arg) == -1) die_perror("semctl SEM_IN_A");
        if (semctl(g_semid, SEM_IN_B, SETVAL, arg) == -1) die_perror("semctl SEM_IN_B");
        if (semctl(g_semid, SEM_IN_C, SETVAL, arg) == -1) die_perror("semctl SEM_IN_C");
        if (semctl(g_semid, SEM_IN_D, SETVAL, arg) == -1) die_perror("semctl SEM_IN_D");
        if (semctl(g_semid, SEM_OUT_A, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_A");
        if (semctl(g_semid, SEM_OUT_B, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_B");
        if (semctl(g_semid, SEM_OUT_C, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_C");
        if (semctl(g_semid, SEM_OUT_D, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_D");

        // WAREHOUSE_ON = 1 (magazyn otwarty)
        arg.val = 1;
        if (semctl(g_semid, SEM_WAREHOUSE_ON, SETVAL, arg) == -1) die_perror("semctl SEM_WAREHOUSE_ON");

    }
}

// Wczytuje stan magazynu z pliku
void load_state_from_file() {
    int fd = open(g_stateFile.c_str(), O_RDONLY);
    if (fd == -1) return;

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) return;
    buf[n] = '\0';

    int target, a, b, c, d;
    if (sscanf(buf, "%d %d %d %d %d", &target, &a, &b, &c, &d) != 5) return;
    
    // Chroni przed uszkodzonym/złośliwym plikiem stanu
    auto clamp = [](int val, int minv, int maxv) {
        return (val < minv) ? minv : (val > maxv) ? maxv : val;
    };
    a = clamp(a, 0, g_header->capacityA);
    b = clamp(b, 0, g_header->capacityB);
    c = clamp(c, 0, g_header->capacityC);
    d = clamp(d, 0, g_header->capacityD);

    // Aktualizacja semaforów (ring buffer model z OFFSETAMI BAJTOWYMI)
    semun arg{};

    // FULL_X = count (tyle jest zajętych/dostępnych)
    arg.val = a;
    if (semctl(g_semid, SEM_FULL_A, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_A");
    arg.val = b;
    if (semctl(g_semid, SEM_FULL_B, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_B");
    arg.val = c;
    if (semctl(g_semid, SEM_FULL_C, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_C");
    arg.val = d;
    if (semctl(g_semid, SEM_FULL_D, SETVAL, arg) == -1) die_perror("semctl SEM_FULL_D");

    // EMPTY_X = capacity - count (tyle jest wolnych miejsc)
    arg.val = g_header->capacityA - a;
    if (semctl(g_semid, SEM_EMPTY_A, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_A");
    arg.val = g_header->capacityB - b;
    if (semctl(g_semid, SEM_EMPTY_B, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_B");
    arg.val = g_header->capacityC - c;
    if (semctl(g_semid, SEM_EMPTY_C, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_C");
    arg.val = g_header->capacityD - d;
    if (semctl(g_semid, SEM_EMPTY_D, SETVAL, arg) == -1) die_perror("semctl SEM_EMPTY_D");

    // IN_X = count * itemSize (OFFSET BAJTOWY - następny zapis)
    // Po wczytaniu dane są ciągłe od początku, więc IN = count * size
    arg.val = a * kSizeA;
    if (semctl(g_semid, SEM_IN_A, SETVAL, arg) == -1) die_perror("semctl SEM_IN_A");
    arg.val = b * kSizeB;
    if (semctl(g_semid, SEM_IN_B, SETVAL, arg) == -1) die_perror("semctl SEM_IN_B");
    arg.val = c * kSizeC;
    if (semctl(g_semid, SEM_IN_C, SETVAL, arg) == -1) die_perror("semctl SEM_IN_C");
    arg.val = d * kSizeD;
    if (semctl(g_semid, SEM_IN_D, SETVAL, arg) == -1) die_perror("semctl SEM_IN_D");

    // OUT_X = 0 (OFFSET BAJTOWY - odczyt od początku)
    arg.val = 0;
    if (semctl(g_semid, SEM_OUT_A, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_A");
    if (semctl(g_semid, SEM_OUT_B, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_B");
    if (semctl(g_semid, SEM_OUT_C, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_C");
    if (semctl(g_semid, SEM_OUT_D, SETVAL, arg) == -1) die_perror("semctl SEM_OUT_D");
    
    // Wyczyść CAŁE segmenty przed wypełnieniem (usunięcie starych danych)
    std::memset(segment_A(g_header), 0, g_header->capacityA * kSizeA);
    std::memset(segment_B(g_header), 0, g_header->capacityB * kSizeB);
    std::memset(segment_C(g_header), 0, g_header->capacityC * kSizeC);
    std::memset(segment_D(g_header), 0, g_header->capacityD * kSizeD);
    
    // Wypełnij segmenty symbolicznymi danymi (od początku, ciągle)
    // Dzięki temu pamięć jest spójna z semaforami po restarcie
    std::memset(segment_A(g_header), 'A', a * kSizeA);
    std::memset(segment_B(g_header), 'B', b * kSizeB);
    std::memset(segment_C(g_header), 'C', c * kSizeC);
    std::memset(segment_D(g_header), 'D', d * kSizeD);
    
    // Log dla testów - potwierdza wczytanie stanu
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "Wczytano stan z pliku (A=%d, B=%d, C=%d, D=%d)", a, b, c, d);
    log_raport(g_semid, "MAGAZYN", logbuf);
}

// Zapisuje stan magazynu do pliku
void save_state_to_file() {
    // Odczytaj stan z semaforów (atomowe operacje, nie potrzeba mutexu) bo tylko odczytuje dane 
    int a = semctl(g_semid, SEM_FULL_A, GETVAL);
    int b = semctl(g_semid, SEM_FULL_B, GETVAL);
    int c = semctl(g_semid, SEM_FULL_C, GETVAL);
    int d = semctl(g_semid, SEM_FULL_D, GETVAL);

    int fd = open(g_stateFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd != -1) {
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "%d %d %d %d %d\n",
                           g_header->targetChocolates, a, b, c, d);
        if (len > 0) {
            write(fd, buf, len);
        }
        close(fd);
    }
}

// Odłącza się od pamięci dzielonej i USUWA zasoby IPC
// Magazyn jest odpowiedzialny za sprzątanie - nawet jeśli dyrektor zginął
void cleanup_ipc() {
    // Odłącz pamięć
    if (g_header && shmdt(g_header) == -1) perror("shmdt");
    
    // Usuń zasoby IPC (magazyn jest właścicielem)
    if (g_shmid != -1) {
        if (shmctl(g_shmid, IPC_RMID, nullptr) == -1) perror("shmctl IPC_RMID");
    }
    if (g_semid != -1) {
        if (semctl(g_semid, 0, IPC_RMID) == -1) perror("semctl IPC_RMID");
    }
}

// Wypisuje aktualny stan magazynu
void print_state() {
    int a = semctl(g_semid, SEM_FULL_A, GETVAL);
    int b = semctl(g_semid, SEM_FULL_B, GETVAL);
    int c = semctl(g_semid, SEM_FULL_C, GETVAL);
    int d = semctl(g_semid, SEM_FULL_D, GETVAL);
    
    std::cout << "[MAGAZYN] Stan: A=" << a
              << "/" << g_header->capacityA
              << " B=" << b
              << "/" << g_header->capacityB
              << " C=" << c
              << "/" << g_header->capacityC
              << " D=" << d
              << "/" << g_header->capacityD << "\n";
}

// Główna pętla magazynu - wypisuje stan co 2 sekundy
// Kończy gdy SEM_WAREHOUSE_ON=0 lub otrzyma sygnał
void loop() {
    while (!g_stop) {
        // Sprawdź semafor-bramkę (tak jak inne procesy)
        int warehouseOn = semctl(g_semid, SEM_WAREHOUSE_ON, GETVAL);
        if (warehouseOn == 0) {
            std::cout << "[MAGAZYN] SEM_WAREHOUSE_ON=0, kończę pracę\n";
            break;
        }
        
        print_state();
        
        // Czekamy 2 sekundy lub do sygnału
        sleep(2);
    }
}

}  // namespace

// Główna funkcja magazynu
int main(int argc, char **argv) {
    int targetChocolates = kDefaultChocolates;
    if (argc > 1) {
        char *endptr = nullptr;
        long val = std::strtol(argv[1], &endptr, 10);
        if (endptr == argv[1] || *endptr != '\0') {
            std::cerr << "Błąd: '" << argv[1] << "' nie jest poprawną liczbą.\n";
            return 1;
        }
        if (val <= 0 || val > 10000) {
            std::cerr << "Błąd: liczba czekolad musi być w zakresie 1-10000.\n";
            return 1;
        }
        targetChocolates = static_cast<int>(val);
    }

    // Konfiguracja sygnałów
    struct sigaction sa_term{}, sa_usr1{};
    sa_term.sa_handler = handle_sigterm;
    sa_term.sa_flags = 0;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, nullptr);
    
    sa_usr1.sa_handler = handle_sigusr1;
    sa_usr1.sa_flags = 0;
    sigemptyset(&sa_usr1.sa_mask);
    sigaction(SIGUSR1, &sa_usr1, nullptr);

    // Jeśli dyrektor zginie (np. SIGKILL), dostaniemy SIGTERM
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    // Inicjalizacja
    ensure_ipc_key();
    init_ipc(targetChocolates);

    // Log startu
    size_t shmSize = calc_shm_size(targetChocolates);
    char startbuf[128];
    std::snprintf(startbuf, sizeof(startbuf),
                  "Start magazynu (target=%d czekolad, pamięć=%zu bajtów, A=%d B=%d C=%d D=%d max)",
                  targetChocolates, shmSize,
                  g_header->capacityA, g_header->capacityB,
                  g_header->capacityC, g_header->capacityD);
    log_raport(g_semid, "MAGAZYN", startbuf);
    std::cout << "[MAGAZYN] " << startbuf << "\n";

    // Odtworzenie stanu z poprzedniego uruchomienia
    if (access(g_stateFile.c_str(), F_OK) == 0) {
        std::cout << "[MAGAZYN] Wczytuje stan z pliku...\n";
        load_state_from_file();

        int a = semctl(g_semid, SEM_FULL_A, GETVAL);
        int b = semctl(g_semid, SEM_FULL_B, GETVAL);
        int c = semctl(g_semid, SEM_FULL_C, GETVAL);
        int d = semctl(g_semid, SEM_FULL_D, GETVAL);
        
        char loadbuf[128];
        std::snprintf(loadbuf, sizeof(loadbuf),
                      "Odtworzono stan: A=%d B=%d C=%d D=%d", a, b, c, d);
        log_raport(g_semid, "MAGAZYN", loadbuf);
    }

    // Główna pętla
    loop();

    // Log zamknięcia
    log_raport(g_semid, "MAGAZYN", "Magazyn zamknięty");

    // Zapis stanu TYLKO jeśli otrzymaliśmy SIGUSR1 (polecenie 4)
    if (g_save_on_exit) {
        int a = semctl(g_semid, SEM_FULL_A, GETVAL);
        int b = semctl(g_semid, SEM_FULL_B, GETVAL);
        int c = semctl(g_semid, SEM_FULL_C, GETVAL);
        int d = semctl(g_semid, SEM_FULL_D, GETVAL);
        
        char savebuf[128];
        std::snprintf(savebuf, sizeof(savebuf),
                      "Zapisuje stan: A=%d B=%d C=%d D=%d", a, b, c, d);
        log_raport(g_semid, "MAGAZYN", savebuf);
        std::cout << "[MAGAZYN] " << savebuf << "\n";

        save_state_to_file();
    } else {
        log_raport(g_semid, "MAGAZYN", "Zakończenie bez zapisu stanu");
    }
    
    // Odłącz pamięć (ale NIE usuwaj IPC - to robi dyrektor)
    if (g_header) {
        shmdt(g_header);
        g_header = nullptr;
    }

    std::cout << "[MAGAZYN] Zakończono.\n";
    return 0;
}
