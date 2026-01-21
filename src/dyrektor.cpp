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

namespace {

// Zmienne globalne
std::vector<pid_t> g_children;  // PIDy wszystkich procesów
int g_semid = -1;   // ID semaforów
int g_shmid = -1;   // ID pamięci dzielonej

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

// Usuwa wszystkie zasoby IPC
void remove_ipcs() {
    if (g_semid != -1) semctl(g_semid, 0, IPC_RMID);
    if (g_shmid != -1) shmctl(g_shmid, IPC_RMID, nullptr);
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
    
    for (int i = 0; i < timeout_sec * 2; ++i) {  // 500ms intervals
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
        usleep(500000);
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
            // StopMagazyn - SIGTERM = zakończ BEZ zapisu stanu
            log_raport(g_semid, "DYREKTOR", "Wysyłam SIGTERM do magazynu (bez zapisu)");
            if (g_children.size() > 0 && g_children[0] > 0) {
                kill(g_children[0], SIGTERM);
            }
        }
        else if (choice == '3') {
            log_raport(g_semid, "DYREKTOR", "Wysyłam SIGTERM do dostawców");
            send_signal_to_range(SIGTERM, 1, 5);  // dostawcy [1,2,3,4]
        }
        else if (choice == '4') {
            // StopAll - DETERMINISTYCZNY zapis stanu
            // Sekwencja: stanowiska -> dostawcy -> magazyn (z zapisem)
            log_raport(g_semid, "DYREKTOR", "StopAll - zatrzymuję stanowiska...");
            
            // 1) Zatrzymaj stanowiska (konsumentów)
            send_signal_to_range(SIGTERM, 5, 7);
            if (!wait_for_range(5, 7, 5)) {
                std::cout << "[DYREKTOR] Timeout stanowisk - SIGKILL\n";
                for (size_t j = 5; j < 7 && j < g_children.size(); ++j) {
                    if (g_children[j] > 0) kill(g_children[j], SIGKILL);
                }
                wait_for_range(5, 7, 2);
            }
            
            // 2) Zatrzymaj dostawców (producentów)
            log_raport(g_semid, "DYREKTOR", "StopAll - zatrzymuję dostawców...");
            send_signal_to_range(SIGTERM, 1, 5);
            if (!wait_for_range(1, 5, 5)) {
                std::cout << "[DYREKTOR] Timeout dostawców - SIGKILL\n";
                for (size_t j = 1; j < 5 && j < g_children.size(); ++j) {
                    if (g_children[j] > 0) kill(g_children[j], SIGKILL);
                }
                wait_for_range(1, 5, 2);
            }
            
            // 3) Teraz magazyn może bezpiecznie zapisać stan
            log_raport(g_semid, "DYREKTOR", "StopAll - zapisuję stan magazynu...");
            if (g_children.size() > 0 && g_children[0] > 0) {
                kill(g_children[0], SIGUSR1);  // magazyn zapisze stan i zakończy
                
                // WAŻNE: Czekaj na zakończenie magazynu PRZED graceful_shutdown()
                // Zapobiega wyścigowi SIGUSR1 vs SIGTERM
                if (!wait_for_range(0, 1, 5)) {
                    std::cout << "[DYREKTOR] Timeout magazynu - SIGKILL\n";
                    kill(g_children[0], SIGKILL);
                    wait_for_range(0, 1, 2);
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

    std::cout << "[DYREKTOR] Start fabryki dla " << targetChocolates 
              << " czekolad na pracownika\n";
    std::cout << "[DYREKTOR] Pamięć: " << calc_shm_size(targetChocolates) << " bajtów\n";

    // Uruchom procesy potomne
    start_processes(targetChocolates);

    // Dołącz do IPC
    attach_ipc(targetChocolates);

    // Pętla menu
    menu_loop();

    // Zakończenie
    graceful_shutdown();

    // Usuń zasoby IPC
    remove_ipcs();

    std::cout << "[DYREKTOR] Zakończono.\n";
    return 0;
}
