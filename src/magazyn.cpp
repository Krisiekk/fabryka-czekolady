/*
 * magazyn.cpp - Proces zarządzający magazynem składników
 * 
 * Magazyn jest "sercem" systemu - tworzy i inicjalizuje wszystkie zasoby IPC,
 * przechowuje stan składników w pamięci dzielonej, obsługuje żądania od
 * stanowisk produkcyjnych i zapisuje/odtwarza stan z pliku.
 * 
 * Składniki i ich rozmiar w jednostkach magazynowych:
 *   A, B = 1 jednostka
 *   C = 2 jednostki  
 *   D = 3 jednostki
 * 
 * Autor: Krzysztof Pietrzak (156721)
 */

#include "../include/common.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <cerrno>

namespace {

// --- Zmienne globalne ---
int g_msgid = -1;                    // ID kolejki komunikatów
int g_semid = -1;                    // ID zestawu semaforów
int g_shmid = -1;                    // ID pamięci dzielonej
WarehouseState *g_state = nullptr;   // wskaźnik na stan magazynu w pamięci dzielonej
volatile sig_atomic_t g_stop = 0;    // flaga zakończenia
bool g_fresh = false;                // czy semafory zostały świeżo utworzone

std::string g_stateFile = "magazyn_state.txt";  // plik do zapisu/odczytu stanu

// Unia potrzebna dla semctl() - wymóg POSIX
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
};

void handle_signal(int) { g_stop = 1; }

key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_perror("ftok");
    return key;
}

/*
 * init_ipc() - Inicjalizacja wszystkich zasobów IPC
 * 
 * Ta funkcja tworzy (lub dołącza do istniejących):
 *   - pamięć dzieloną z WarehouseState
 *   - kolejkę komunikatów do poleceń
 *   - 7 semaforów (mutex, pojemność, 4x składnik, raport)
 * 
 * Flaga IPC_EXCL przy semget() pozwala wykryć czy semafory już istnieją.
 * Jeśli tak (errno==EEXIST), to znaczy że inny proces magazynu działa
 * lub poprzedni nie posprzątał - wtedy dołączamy bez reinicjalizacji.
 * 
 * Wszystkie zasoby mają uprawnienia 0600 (tylko właściciel).
 */
void init_ipc(int capacity) {
    key_t key = make_key();

    // 1. Pamięć dzielona - przechowuje stan magazynu
    g_shmid = shmget(key, sizeof(WarehouseState), IPC_CREAT | 0600);
    if (g_shmid == -1) die_perror("shmget");

    // 2. Mapowanie pamięci dzielonej do przestrzeni adresowej procesu
    //    shmat zwraca -1 jako void* przy błędzie (dziwna konwencja...)
    g_state = static_cast<WarehouseState*>(shmat(g_shmid, nullptr, 0));
    if (g_state == reinterpret_cast<void*>(-1)) die_perror("shmat");

    // 3. Kolejka komunikatów - do odbierania poleceń od dyrektora
    g_msgid = msgget(key, IPC_CREAT | 0600);
    if (g_msgid == -1) die_perror("msgget");

    // 4. Semafory - IPC_EXCL sprawdza czy już istnieją
    g_semid = semget(key, SEM_COUNT, IPC_CREAT | IPC_EXCL | 0600);
    bool fresh = true;

    if (g_semid == -1) {
        // Semafory już istnieją - dołączamy bez reinicjalizacji
        if (errno != EEXIST) die_perror("semget");
        fresh = false;
        g_semid = semget(key, SEM_COUNT, 0600);
        if (g_semid == -1) die_perror("semget attach");
    }

    g_fresh = fresh;

    // Inicjalizacja tylko gdy tworzymy semafory od zera
    if (fresh) {
        // Zerujemy stan magazynu
        g_state->capacity = capacity;
        g_state->a = g_state->b = g_state->c = g_state->d = 0;

        semun arg{};

        // MUTEX = 1 (binarny, do ochrony sekcji krytycznych)
        arg.val = 1;
        if (semctl(g_semid, SEM_MUTEX, SETVAL, arg) == -1) die_perror("semctl mutex");

        // CAPACITY = N (ile jeszcze jednostek zmieści się w magazynie)
        arg.val = capacity;
        if (semctl(g_semid, SEM_CAPACITY, SETVAL, arg) == -1) die_perror("semctl capacity");

        // Semafory składników = 0 (magazyn pusty na starcie)
        // Stanowiska będą czekać na P() aż dostawca zrobi V()
        arg.val = 0;
        if (semctl(g_semid, SEM_A, SETVAL, arg) == -1) die_perror("semctl SEM_A");
        if (semctl(g_semid, SEM_B, SETVAL, arg) == -1) die_perror("semctl SEM_B");
        if (semctl(g_semid, SEM_C, SETVAL, arg) == -1) die_perror("semctl SEM_C");
        if (semctl(g_semid, SEM_D, SETVAL, arg) == -1) die_perror("semctl SEM_D");

        // RAPORT = 1 (mutex do synchronizacji zapisu do pliku raportu)
        arg.val = 1;
        if (semctl(g_semid, SEM_RAPORT, SETVAL, arg) == -1) die_perror("semctl SEM_RAPORT");
    }
}

/*
 * load_state_from_file() - Odczyt stanu magazynu z pliku
 * 
 * Po restarcie magazynu chcemy odtworzyć poprzedni stan składników.
 * Plik zawiera 5 liczb: pojemność, a, b, c, d.
 * 
 * Używamy open/read/close (syscalle) zamiast fopen/fscanf,
 *
 * 
 * Po wczytaniu aktualizujemy też wartości semaforów składników,
 * żeby stanowiska mogły od razu pobierać to co było w magazynie.
 */
void load_state_from_file() {
    int fd = open(g_stateFile.c_str(), O_RDONLY);
    if (fd == -1) return;  // plik nie istnieje = świeży start

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) return;
    buf[n] = '\0';

    int cap, a, b, c, d;
    if (sscanf(buf, "%d %d %d %d %d", &cap, &a, &b, &c, &d) != 5) return;

    // Aktualizacja pamięci dzielonej (z mutexem dla bezpieczeństwa)
    P_mutex(g_semid);
    g_state->capacity = cap;
    g_state->a = a;
    g_state->b = b;
    g_state->c = c;
    g_state->d = d;
    V_mutex(g_semid);

    // Aktualizacja semaforów składników
    // Dzięki temu stanowiska od razu "widzą" ile jest składników
    semun arg{};

    arg.val = 1;
    if (semctl(g_semid, SEM_MUTEX, SETVAL, arg) == -1) die_perror("semctl mutex");

    arg.val = a;
    if (semctl(g_semid, SEM_A, SETVAL, arg) == -1) die_perror("semctl SEM_A");

    arg.val = b;
    if (semctl(g_semid, SEM_B, SETVAL, arg) == -1) die_perror("semctl SEM_B");

    arg.val = c;
    if (semctl(g_semid, SEM_C, SETVAL, arg) == -1) die_perror("semctl SEM_C");

    arg.val = d;
    if (semctl(g_semid, SEM_D, SETVAL, arg) == -1) die_perror("semctl SEM_D");

    // Przeliczamy ile jednostek jest zajętych (C=2, D=3 jednostki)
    int usedUnits = a + b + 2*c + 3*d;
    int freeUnits = cap - usedUnits;
    if (freeUnits < 0) freeUnits = 0;

    arg.val = freeUnits;
    if (semctl(g_semid, SEM_CAPACITY, SETVAL, arg) == -1) die_perror("semctl capacity");
}

/*
 * save_state_to_file() - Zapis stanu magazynu do pliku
 * 
 * Zapisuje aktualny stan do pliku tekstowego, żeby można było
 * go odtworzyć po restarcie. Format: "pojemność a b c d\n"
 * 
 * Mutex chroni przed zapisem w trakcie modyfikacji stanu.
 */
void save_state_to_file() {
    P_mutex(g_semid);

   
    int fd = open(g_stateFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd != -1) {
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "%d %d %d %d %d\n",
                           g_state->capacity, g_state->a, g_state->b, g_state->c, g_state->d);
        if (len > 0) {
            if (write(fd, buf, len) == -1) perror("write state");
        }
        close(fd);
    } else {
        perror("open state");
    }

    V_mutex(g_semid);
}

/*
 * try_P() - Nieblokująca próba zmniejszenia semafora
 * 
 * Zwraca true jeśli operacja się udała, false jeśli semafor
 * miałby zejść poniżej zera (IPC_NOWAIT = nie czekaj).
 */
bool try_P(int semid, int semnum, int delta) {
    if (delta <= 0) return true;
    sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(-delta), IPC_NOWAIT};
    return semop(semid, &op, 1) == 0;
}

/*
 * cleanup_ipc() - Odłączenie od pamięci dzielonej
 * 
 * UWAGA: Nie usuwamy zasobów IPC (msgctl/semctl/shmctl z IPC_RMID),
 * bo to robi dyrektor. Magazyn tylko się odłącza.
 */
void cleanup_ipc() {
    if (g_state && shmdt(g_state) == -1) perror("shmdt");
}

/*
 * process_worker_request() - Obsługa żądania surowców od stanowiska
 * 
 * Algorytm all-or-nothing:
 *   1. Próbujemy pobrać składnik A (nieblokująco)
 *   2. Jeśli OK, próbujemy B, itd.
 *   3. Jeśli któryś brakuje - oddajemy wszystkie wcześniej pobrane
 *   4. Wysyłamy odpowiedź: granted=true/false
 * 
 * To zapobiega deadlockom - stanowisko albo dostaje wszystko,
 * albo nic nie zabiera i próbuje później.
 */
void process_worker_request(const WorkerRequestMessage &req) {
    // Logowanie do pliku raportu
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Żądanie od stanowiska %d (pid=%d): A=%d B=%d C=%d D=%d",
                  req.workerType, req.pid, req.needA, req.needB, req.needC, req.needD);
    log_raport(g_semid, "MAGAZYN", buf);

    std::cout << "[MAGAZYN] request od PID=" << req.pid
              << " A=" << req.needA << " B=" << req.needB
              << " C=" << req.needC << " D=" << req.needD << "\n";

    // --- Próba pobrania składników (all-or-nothing) ---

    // Krok 1: Składnik A
    bool gotA = try_P(g_semid, SEM_A, req.needA);
    if (!gotA) {
        WarehouseReplyMessage reply{};
        reply.mtype = req.pid;
        reply.granted = false;
        std::cout << "[MAGAZYN] brak A, odmowa dla PID=" << req.pid << "\n";
        msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0);
        return;
    }

    // Krok 2: Składnik B
    bool gotB = try_P(g_semid, SEM_B, req.needB);
    if (!gotB) {
        // Oddaj A, wysłij odmowę
        if (req.needA) V(g_semid, SEM_A, req.needA);
        WarehouseReplyMessage reply{};
        reply.mtype = req.pid;
        reply.granted = false;
        std::cout << "[MAGAZYN] brak B, odmowa dla PID=" << req.pid << "\n";
        msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0);
        return;
    }

    // Krok 3: Składnik C
    bool gotC = try_P(g_semid, SEM_C, req.needC);
    if (!gotC) {
        // Oddaj A i B
        if (req.needA) V(g_semid, SEM_A, req.needA);
        if (req.needB) V(g_semid, SEM_B, req.needB);
        WarehouseReplyMessage reply{};
        reply.mtype = req.pid;
        reply.granted = false;
        std::cout << "[MAGAZYN] brak C, odmowa dla PID=" << req.pid << "\n";
        msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0);
        return;
    }

    // Krok 4: Składnik D
    bool gotD = try_P(g_semid, SEM_D, req.needD);
    if (!gotD) {
        // Oddaj A, B i C
        if (req.needA) V(g_semid, SEM_A, req.needA);
        if (req.needB) V(g_semid, SEM_B, req.needB);
        if (req.needC) V(g_semid, SEM_C, req.needC);
        WarehouseReplyMessage reply{};
        reply.mtype = req.pid;
        reply.granted = false;
        std::cout << "[MAGAZYN] brak D, odmowa dla PID=" << req.pid << "\n";
        msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0);
        return;
    }

    // --- Sukces! Aktualizacja stanu magazynu ---
    P_mutex(g_semid);
    g_state->a -= req.needA;
    g_state->b -= req.needB;
    g_state->c -= req.needC;
    g_state->d -= req.needD;
    V_mutex(g_semid);

    // Zwolnienie miejsca w magazynie (różne składniki zajmują różną ilość)
    int freed = req.needA + req.needB + 2*req.needC + 3*req.needD;
    if (freed > 0) V(g_semid, SEM_CAPACITY, freed);

    // Odpowiedź pozytywna
    WarehouseReplyMessage reply{};
    reply.mtype = req.pid;
    reply.granted = true;

    // Logowanie wydania
    char buf2[128];
    P_mutex(g_semid);
    std::snprintf(buf2, sizeof(buf2), "Wydano surowce dla stanowiska %d (pid=%d), stan: A=%d B=%d C=%d D=%d",
                  req.workerType, req.pid, g_state->a, g_state->b, g_state->c, g_state->d);
    V_mutex(g_semid);
    log_raport(g_semid, "MAGAZYN", buf2);

    std::cout << "[MAGAZYN] reply -> PID=" << reply.mtype
              << " granted=" << reply.granted << "\n";

    if (msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0) == -1)
        perror("msgsnd reply");
}

// Obsługa raportu od dostawcy (tylko wypisanie na stdout)
void process_supplier_report(const SupplierReportMessage &rep) {
    std::cout << "[MAGAZYN] " << rep.text << std::endl;
}

/*
 * loop() - Główna pętla magazynu
 * 
 * Magazyn w nieskończoność sprawdza kolejkę komunikatów:
 *   1. Czy przyszła komenda StopMagazyn/StopAll? -> zakończ
 *   2. Czy stanowisko prosi o składniki? -> obsłuż żądanie
 *   3. Czy dostawca przysłał raport? -> wypisz na stdout
 *   4. Jeśli nic nie ma -> sleep(1) i od początku
 * 
 * IPC_NOWAIT sprawia że msgrcv nie blokuje, zwraca ENOMSG.
 */
void loop() {
    while (!g_stop) {
        // --- 1. Sprawdzenie komend od dyrektora ---
        Command cmd = check_command(g_msgid);
        if (cmd == Command::StopMagazyn || cmd == Command::StopAll) {
            const char* cmdName = (cmd == Command::StopAll) ? "StopAll" : "StopMagazyn";
            char cmdbuf[64];
            std::snprintf(cmdbuf, sizeof(cmdbuf), "Odebrano %s - koncze prace", cmdName);
            log_raport(g_semid, "MAGAZYN", cmdbuf);
            g_stop = 1;
            continue;
        }

        // --- 2. Żądania od stanowisk produkcyjnych ---
        WorkerRequestMessage req{};
        ssize_t r = msgrcv(g_msgid, &req, sizeof(req) - sizeof(long),
                           static_cast<long>(MsgType::WorkerRequest), IPC_NOWAIT);
        if (r >= 0) {
            process_worker_request(req);
            continue;
        } else if (errno != ENOMSG) {
            perror("msgrcv worker");
        }

        // --- 3. Raporty od dostawców ---
        SupplierReportMessage rep{};
        r = msgrcv(g_msgid, &rep, sizeof(rep) - sizeof(long),
                   static_cast<long>(MsgType::SupplierReport), IPC_NOWAIT);
        if (r >= 0) {
            process_supplier_report(rep);
            continue;
        } else if (errno != ENOMSG) {
            perror("msgrcv report");
        }

        // --- 4. Nic do roboty - krótka przerwa ---
        sleep(1);
    }
}

}  // namespace

/*
 * main() - Punkt wejścia procesu magazynu
 * 
 * 1. Parsowanie argumentu pojemności (strtol + walidacja)
 * 2. Utworzenie pliku klucza IPC
 * 3. Inicjalizacja zasobów IPC
 * 4. Wczytanie poprzedniego stanu (jeśli istnieje)
 * 5. Ustawienie obsługi sygnałów
 * 6. Główna pętla
 * 7. Zapis stanu i sprzątanie
 */
int main(int argc, char **argv) {
    int capacity = kDefaultCapacity;
    if (argc > 1) {
        // Walidacja wejścia z strtol() 
        char *endptr = nullptr;
        long val = std::strtol(argv[1], &endptr, 10);
        if (endptr == argv[1] || *endptr != '\0') {
            std::cerr << "Błąd: '" << argv[1] << "' nie jest poprawną liczbą.\n";
            return 1;
        }
        if (val <= 0 || val > 10000) {
            std::cerr << "Błąd: capacity musi być w zakresie 1-10000.\n";
            return 1;
        }
        capacity = static_cast<int>(val);
    }

    // Konfiguracja sygnałów (SIGINT, SIGTERM, SIGUSR1, SIGUSR2)
    setup_sigaction(handle_signal);

    // Tworzenie pliku klucza jeśli nie istnieje
    ensure_ipc_key();

    // Inicjalizacja pamięci dzielonej, kolejki i semaforów
    init_ipc(capacity);

    // Log startu
    char startbuf[64];
    std::snprintf(startbuf, sizeof(startbuf), "Start magazynu (capacity=%d)", capacity);
    log_raport(g_semid, "MAGAZYN", startbuf);

    // Odtworzenie stanu z poprzedniego uruchomienia
    if (access(g_stateFile.c_str(), F_OK) == 0) {
        std::cout << "[MAGAZYN] wczytuje stan z pliku...\n";
        load_state_from_file();

        // Log o odtworzonym stanie
        P_mutex(g_semid);
        int usedUnits = g_state->a + g_state->b + 2*g_state->c + 3*g_state->d;
        char loadbuf[128];
        std::snprintf(loadbuf, sizeof(loadbuf),
                      "Odtworzono stan z pliku: A=%d B=%d C=%d D=%d (zajetosc: %d/%d jednostek)",
                      g_state->a, g_state->b, g_state->c, g_state->d, usedUnits, g_state->capacity);
        V_mutex(g_semid);
        log_raport(g_semid, "MAGAZYN", loadbuf);
    }

    // Główna pętla obsługi
    loop();

    // Przed zakończeniem - zapis stanu do pliku
    P_mutex(g_semid);
    int usedUnits = g_state->a + g_state->b + 2*g_state->c + 3*g_state->d;
    char savebuf[128];
    std::snprintf(savebuf, sizeof(savebuf),
                  "Zapisuje stan do pliku: A=%d B=%d C=%d D=%d (zajetosc: %d/%d jednostek)",
                  g_state->a, g_state->b, g_state->c, g_state->d, usedUnits, g_state->capacity);
    V_mutex(g_semid);
    log_raport(g_semid, "MAGAZYN", savebuf);

    save_state_to_file();
    cleanup_ipc();

    return 0;
}