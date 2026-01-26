// Dyrektor - główny proces sterujący fabryką
// Uruchamia wszystkie procesy (magazyn, dostawcy, stanowiska)
// Odbiera polecenia od użytkownika

#include "../include/common.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <atomic>

namespace {

// Zmienne globalne
std::vector<pid_t> g_children;  // PIDy wszystkich procesów
int g_semid = -1;   // ID semaforów
int g_shmid = -1;   // ID pamięci dzielonej
int g_msqid = -1;   // ID kolejki komunikatów
std::thread g_monitor_thread;     // monitor zmian stanu magazynu
std::atomic_bool g_monitor_running{false};

void die_exec(const char *what) {
    perror(what);
    _exit(EXIT_FAILURE);
}

// Uruchamia nowy proces i wykonuje program
pid_t spawn(const std::vector<std::string> &args) {
    pid_t pid = fork();
    
    if (pid < 0) {
        die_exec("fork");
    }
    
    if (pid == 0) {
        std::vector<char*> cargs;
        cargs.reserve(args.size() + 1);
        
        for (const auto &s : args) {
            cargs.push_back(const_cast<char*>(s.c_str()));
        }
        cargs.push_back(nullptr);
        
        execv(args[0].c_str(), cargs.data());
        die_exec(args[0].c_str());
    }
    
    g_children.push_back(pid);
    return pid;
}

// Tworzy klucz IPC
key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_exec("ftok");
    return key;
}

// Łączy się do zasobów IPC z retry
void attach_ipc([[maybe_unused]] int targetChocolates) {
    key_t key = make_key();
    
    // Retry przez 3 sekundy
    constexpr int maxRetries = 30;
    constexpr int retryDelayUs = 100000;
    
    for (int i = 0; i < maxRetries; ++i) {
        // Próba dołączenia
        g_semid = semget(key, SEM_COUNT, 0600);
        if (g_semid != -1) {
            g_shmid = shmget(key, 0, 0600);
            if (g_shmid != -1) return;
        }
        
        // Jeśli błąd inny niż "nie istnieje" - wyjść
        if (errno != ENOENT && errno != EINVAL) {
            die_exec("attach_ipc");
        }
        
        usleep(retryDelayUs);
    }
    
    std::cerr << "[DYREKTOR] Timeout: magazyn nie utworzył zasobów IPC.\n";
    die_exec("attach_ipc timeout");
}

// Wysyła sygnał do zakresu procesów
void send_signal_to_range(int sig, size_t from, size_t to) {
    for (size_t i = from; i < to && i < g_children.size(); ++i) {
        if (g_children[i] > 0) {
            kill(g_children[i], sig);
        }
    }
}

void send_signal_to_all(int sig) {
    for (pid_t pid : g_children) {
        if (pid > 0) {
            kill(pid, sig);
        }
    }
}

// Wyślij stan magazynu (0 = closed, 1 = open) do wszystkich dzieci (wiadomości po pid)
void send_state_to_children(int state) {
    if (g_msqid == -1) return;
    for (pid_t pid : g_children) {
        if (pid <= 0) continue;
        if (msq_send_pid(g_msqid, pid, state) == -1) {
            std::perror("msq_send_pid");
        }
    }
}

// Monitoruje stan magazynu (stop/continue) i rozgłasza go
void monitor_magazyn(pid_t magazyn_pid) {
    int status = 0;

    while (g_monitor_running) {
        pid_t r = waitpid(magazyn_pid, &status, WUNTRACED | WCONTINUED);
        if (r == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            continue; // brak zmian
        }

        if (WIFSTOPPED(status)) {
            std::cout << "[DYREKTOR] Magazyn zatrzymany (SIGSTOP) - zamykam bramkę i wysyłam powiadomienia\n";
            union semun arg; arg.val = 0;
            if (g_semid != -1) semctl(g_semid, SEM_WAREHOUSE_ON, SETVAL, arg);
            send_state_to_children(0);
        } else if (WIFCONTINUED(status)) {
            std::cout << "[DYREKTOR] Magazyn wznowiony (SIGCONT) - otwieram bramkę i wysyłam powiadomienia\n";
            union semun arg; arg.val = 1;
            if (g_semid != -1) semctl(g_semid, SEM_WAREHOUSE_ON, SETVAL, arg);
            send_state_to_children(1);
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            std::cout << "[DYREKTOR] Magazyn zakończył pracę - oznaczam zamknięcie.\n";
            send_state_to_children(0);
            break;
        }
    }
}

// Usuwa wszystkie zasoby IPC
void remove_ipcs() {
    if (g_semid != -1) semctl(g_semid, 0, IPC_RMID);
    if (g_shmid != -1) shmctl(g_shmid, IPC_RMID, nullptr);
    if (g_msqid != -1) msgctl(g_msqid, IPC_RMID, nullptr);
}

// Czyści stare zasoby IPC (jeśli pozostały po poprzednim uruchomieniu)
void cleanup_old_ipcs() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) return;
    
    // Spróbuj usunąć stare semafory
    int old_semid = semget(key, 0, 0600);
    if (old_semid != -1) {
        semctl(old_semid, 0, IPC_RMID);
        std::cout << "[DYREKTOR] Usunięto stare semafory.\n";
    }
    
    // Spróbuj usunąć starą pamięć dzieloną
    int old_shmid = shmget(key, 0, 0600);
    if (old_shmid != -1) {
        shmctl(old_shmid, IPC_RMID, nullptr);
        std::cout << "[DYREKTOR] Usunięto starą pamięć dzieloną.\n";
    }

    // Spróbuj usunąć starą kolejkę komunikatów
    int old_msqid = msgget(key, 0);
    if (old_msqid != -1) {
        msgctl(old_msqid, IPC_RMID, nullptr);
        std::cout << "[DYREKTOR] Usunięto starą kolejkę komunikatów.\n";
    }
}

// Zatrzymuje wszystkie procesy gracefully
void graceful_shutdown() {
    // Wysyłanie SIGTERM
    std::cout << "[DYREKTOR] Wysyłam SIGTERM do wszystkich procesów...\n";
    send_signal_to_all(SIGTERM);
    
    // 2) Grace period: czekaj do 5 sekund
    std::vector<bool> reaped(g_children.size(), false);

    for (int i = 0; i < 10; ++i) {
        bool all_done = true;
        
        for (size_t j = 0; j < g_children.size(); ++j) {
            pid_t pid = g_children[j];
            if (pid <= 0 || reaped[j]) continue;

            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            
            if (r == 0) {
                all_done = false;
            } else if (r == pid) {
                reaped[j] = true;
            }
        }
        
        if (all_done) {
            std::cout << "[DYREKTOR] Wszystkie procesy zakończone.\n";
            return;
        }
        
        usleep(500000);
    }
    
    // 3) Timeout - wyślij SIGKILL
    std::cout << "[DYREKTOR] Timeout - wysyłam SIGKILL.\n";
    for (size_t j = 0; j < g_children.size(); ++j) {
        pid_t pid = g_children[j];
        if (pid <= 0 || reaped[j]) continue;
        kill(pid, SIGKILL);
    }
    
    // 4) Zbierz pozostałe zombie
    for (size_t j = 0; j < g_children.size(); ++j) {
        pid_t pid = g_children[j];
        if (pid <= 0 || reaped[j]) continue;
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

// Uruchamia wszystkie procesy
void start_processes(int targetChocolates) {
    // Magazyn na pierwszym miejscu
    spawn({"./magazyn", std::to_string(targetChocolates)});
    
    sleep(1);
    
    // Dostawcy
    spawn({"./dostawca", "A"});
    spawn({"./dostawca", "B"});
    spawn({"./dostawca", "C"});
    spawn({"./dostawca", "D"});
    
    // Stanowiska
    spawn({"./stanowisko", "1"});
    spawn({"./stanowisko", "2"});
}

// Czeka na zakończenie procesów z zakresu
bool wait_for_range(size_t from, size_t to, int timeout_sec) {
    std::vector<bool> reaped(g_children.size(), false);
    
    // mniejszy interwał sprawdzania, krótsze timeouty
    int checks = timeout_sec * 10; // 10 sprawdzeń na sekundę
    for (int i = 0; i < checks; ++i) {
        bool all_done = true;
        
        for (size_t j = from; j < to && j < g_children.size(); ++j) {
            pid_t pid = g_children[j];
            if (pid <= 0 || reaped[j]) continue;
            
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            
            if (r == 0) {
                all_done = false;
            } else if (r == pid) {
                reaped[j] = true;
                g_children[j] = -1;  // oznacz jako zakończony
            }
        }
        
        if (all_done) return true;
        usleep(100000); // 100ms
    }
    return false;
}

// Główna pętla menu
void menu_loop() {
    std::cout << "Polecenie dyrektora (1-4, q=quit):\n";
    std::cout << "  1 - StopFabryka (zatrzymaj stanowiska)\n";
    std::cout << "  2 - StopMagazyn\n";
    std::cout << "  3 - StopDostawcy\n";
    std::cout << "  4 - StopAll (zapisz stan i zakończ)\n";
    std::cout << "  q - Quit\n";
    
    std::string line;

    while (true) {
        std::cout << ">  ";
        if (!std::getline(std::cin, line)) break;
        
        if (line.empty()) continue;
        
        char choice = line[0];

        // Układ g_children: [0]=magazyn, [1-4]=dostawcy A,B,C,D, [5-6]=stanowiska 1,2
        if (choice == '1') {
            log_raport(g_semid, "DYREKTOR", "Wysyłam SIGTERM do stanowisk");
            send_signal_to_range(SIGTERM, 5, 7);  // stanowiska [5,6]
        }
        else if (choice == '2') {
            // StopMagazyn - ustaw SEM_WAREHOUSE_ON=0 (magazyn sam się zakończy)
            log_raport(g_semid, "DYREKTOR", "Ustawiam SEM_WAREHOUSE_ON=0 (zamykam magazyn)");
            semun arg{};
            arg.val = 0;
            if (semctl(g_semid, SEM_WAREHOUSE_ON, SETVAL, arg) == -1) {
                perror("semctl SEM_WAREHOUSE_ON=0");
            }
        }
        else if (choice == '3') {
            log_raport(g_semid, "DYREKTOR", "Wysyłam SIGTERM do dostawców");
            send_signal_to_range(SIGTERM, 1, 5);  // dostawcy [1,2,3,4]
        }
        else if (choice == '4') {
            // StopAll - zapis stanu
            // Sekwencja: stanowiska -> dostawcy -> magazyn (z zapisem)
            // Najpierw zamknij bramkę aby zabronić nowych dostaw i wznow zatrzymane procesy
            log_raport(g_semid, "DYREKTOR", "StopAll - zamykam bramkę i wznowię zatrzymane procesy...");
            {
                union semun arg; arg.val = 0;
                if (g_semid != -1) semctl(g_semid, SEM_WAREHOUSE_ON, SETVAL, arg);
                send_state_to_children(0);
                // Wznow wszystkie dzieci, żeby przyjęły SIGTERM natychmiast
                send_signal_to_all(SIGCONT);
            }

            // 1) Zatrzymaj stanowiska (konsumentów)
            send_signal_to_range(SIGTERM, 5, 7);
            if (!wait_for_range(5, 7, 3)) {
                std::cout << "[DYREKTOR] Timeout stanowisk - SIGKILL\n";
                for (size_t j = 5; j < 7 && j < g_children.size(); ++j) {
                    if (g_children[j] > 0) kill(g_children[j], SIGKILL);
                }
                wait_for_range(5, 7, 1);
            }
            
            // 2) Zatrzymaj dostawców (producentów)
            log_raport(g_semid, "DYREKTOR", "StopAll - zatrzymuję dostawców...");
            // Wznow również na wszelki wypadek
            send_signal_to_range(SIGCONT, 1, 5);
            send_signal_to_range(SIGTERM, 1, 5);
            if (!wait_for_range(1, 5, 3)) {
                std::cout << "[DYREKTOR] Timeout dostawców - SIGKILL\n";
                for (size_t j = 1; j < 5 && j < g_children.size(); ++j) {
                    if (g_children[j] > 0) kill(g_children[j], SIGKILL);
                }
                wait_for_range(1, 5, 1);
            }
            
            // 3) Teraz magazyn może bezpiecznie zapisać stan
            log_raport(g_semid, "DYREKTOR", "StopAll - zapisuję stan magazynu...");
            if (g_children.size() > 0 && g_children[0] > 0) {
                // Jeśli magazyn został zatrzymany (SIGSTOP), wznow go, żeby mógł obsłużyć SIGUSR1
                std::cout << "[DYREKTOR] Wysyłam SIGCONT do magazynu przed SIGUSR1 (wznowienie jeśli był zatrzymany)\n";
                kill(g_children[0], SIGCONT);

                kill(g_children[0], SIGUSR1);  // magazyn zapisze stan i zakończy

                // Zapobiega wyścigowi SIGUSR1 vs SIGTERM
                if (!wait_for_range(0, 1, 3)) {
                    std::cout << "[DYREKTOR] Timeout magazynu - SIGKILL\n";
                    kill(g_children[0], SIGKILL);
                    wait_for_range(0, 1, 1);
                }
            }
            break;
        }
        else if (choice == 'q' || choice == 'Q') {
            break;
        }
        else {
            std::cout << "Nieznana opcja: '" << choice << "'" << std::endl;
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    int targetChocolates = kDefaultChocolates;
    
    if (argc > 1) {
        char *endptr = nullptr;
        long val = std::strtol(argv[1], &endptr, 10);
        
        if (endptr == argv[1] || *endptr != '\0') {
            std::cerr << "Błąd: '" << argv[1] << "' nie jest poprawną liczbą.\n";
            std::cerr << "Użycie: " << argv[0] << " [liczba_czekolad]\n";
            return 1;
        }
        
        if (val <= 0 || val > 10000) {
            std::cerr << "Błąd: liczba czekolad musi być w zakresie 1-10000.\n";
            return 1;
        }
        
        targetChocolates = static_cast<int>(val);
    }

    ensure_ipc_key();
    
    // Usuń stare IPC z poprzedniego uruchomienia (jeśli istnieją)
    cleanup_old_ipcs();

    std::cout << "[DYREKTOR] Start fabryki dla " << targetChocolates 
              << " czekolad na pracownika\n";
    std::cout << "[DYREKTOR] Pamięć: " << calc_shm_size(targetChocolates) << " bajtów\n";

    // Uruchom procesy potomne
    start_processes(targetChocolates);

    // Dołącz do IPC
    attach_ipc(targetChocolates);

    // Utwórz kolejkę komunikatów (po pid) do powiadomień
    key_t key = make_key();
    g_msqid = msgget(key, IPC_CREAT | 0600);
    if (g_msqid == -1) {
        die_exec("msgget");
    }

    // Uruchom monitor magazynu (będzie obserwował pierwszego potomka - magazyn)
    if (!g_children.empty()) {
        g_monitor_running = true;
        g_monitor_thread = std::thread(monitor_magazyn, g_children[0]);
    }

    // Pętla menu
    menu_loop();

    // Zakończenie
    graceful_shutdown();

    // Zatrzymaj i dołącz monitor (jeśli działa)
    if (g_monitor_running) {
        g_monitor_running = false;
    }
    if (g_monitor_thread.joinable()) g_monitor_thread.join();

    // Usuń zasoby IPC
    remove_ipcs();

    std::cout << "[DYREKTOR] Zakończono.\n";
    return 0;
}
