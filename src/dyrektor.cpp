/*
 * dyrektor.cpp - Główny proces sterujący fabryką czekolady
 * 
 * Dyrektor jest odpowiedzialny za:
 * - uruchomienie wszystkich procesów potomnych (magazyn, dostawcy, stanowiska)
 * - przyjmowanie poleceń od użytkownika (1-4)
 * - wysyłanie komend do procesów przez kolejkę komunikatów
 * - graceful shutdown i sprzątanie zasobów IPC
 * 
 * Autor: Krzysztof Pietrzak (156721)
 */

#include "../include/common.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// --- Zmienne globalne (w anonimowej przestrzeni nazw = prywatne dla tego pliku) ---

std::vector<pid_t> g_children;  // PIDy wszystkich procesów potomnych
                                 // Kolejność: [0]=magazyn, [1-4]=dostawcy A,B,C,D, [5-6]=stanowiska 1,2

int g_msgid = -1;   // ID kolejki komunikatów
int g_semid = -1;   // ID zestawu semaforów  
int g_shmid = -1;   // ID segmentu pamięci dzielonej

volatile sig_atomic_t g_stop = 0;  // Flaga zakończenia (ustawiana przez handler sygnału)
                                    // volatile sig_atomic_t jest bezpieczne w handlerze

// Handler sygnału - tylko ustawia flagę, nic więcej
// (w handlerze nie wolno robić skomplikowanych rzeczy, nawet printf może zawiesić program)
void handle_signal(int) { g_stop = 1; }

// Wypisz błąd i zakończ proces potomny
// Używam _exit() zamiast exit() bo w procesie po fork() nie chcę wywoływać atexit handlers
void die_exec(const char *what) {
    perror(what);
    _exit(EXIT_FAILURE);
}

/**
 * Tworzy nowy proces potomny i uruchamia w nim podany program.
 * 
 * Używa fork() + execv() zgodnie z wymaganiami projektu.
 * PID nowego procesu jest dodawany do g_children.
 * 
 * @param args wektor argumentów: args[0] = ścieżka do programu, args[1..] = argumenty
 * @return PID utworzonego procesu
 */
pid_t spawn(const std::vector<std::string> &args) {
    pid_t pid = fork();
    
    if (pid < 0) {
        // fork się nie udał
        die_exec("fork");
    }
    
    if (pid == 0) {
        // Jesteśmy w procesie potomnym - wykonaj exec
        // Muszę przekonwertować vector<string> na char*[] dla execv
        std::vector<char*> cargs;
        cargs.reserve(args.size() + 1);
        
        for (const auto &s : args) {
            cargs.push_back(const_cast<char*>(s.c_str()));
        }
        cargs.push_back(nullptr);  // execv wymaga nullptr na końcu
        
        execv(args[0].c_str(), cargs.data());
        
        // Jeśli execv powróciło, to znaczy że się nie udało
        die_exec(args[0].c_str());
    }
    
    // Jesteśmy w procesie rodzica - zapamiętaj PID dziecka
    g_children.push_back(pid);
    return pid;
}

// Generuje klucz IPC - wszystkie procesy muszą używać tego samego klucza
key_t make_key() {
    key_t key = ftok(kIpcKeyPath, kProjId);
    if (key == -1) die_exec("ftok");
    return key;
}

// Dołącza do istniejących zasobów IPC (utworzonych przez magazyn)
void attach_ipc() {
    key_t key = make_key();
    
    // Dołącz do kolejki komunikatów (bez IPC_CREAT - musi już istnieć)
    g_msgid = msgget(key, 0600);
    if (g_msgid == -1) die_exec("msgget");
    
    // Dołącz do zestawu semaforów
    g_semid = semget(key, SEM_COUNT, 0600);
    if (g_semid == -1) die_exec("semget");
    
    // Dołącz do pamięci dzielonej
    g_shmid = shmget(key, sizeof(WarehouseState), 0600);
    if (g_shmid == -1) die_exec("shmget");
}

// --- Wysyłanie komend przez kolejkę komunikatów ---

/**
 * Wysyła komendę do konkretnego procesu (per-PID targeting).
 * 
 * To jest kluczowe! Wcześniej próbowałem broadcastu z mtype=1, ale wtedy
 * tylko jeden proces odbierał wiadomość. Teraz każda komenda idzie do
 * konkretnego PID i każdy proces słucha tylko na swoim PID.
 */
void send_command_to_pid(Command cmd, pid_t pid) {
    CommandMessage msg{};
    msg.mtype = pid;  // mtype = PID procesu docelowego
    msg.cmd = cmd;
    
    if (msgsnd(g_msgid, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
        perror("msgsnd command");
    }
}

// Wysyła komendę do wszystkich procesów potomnych
void send_command_to_all(Command cmd) {
    for (pid_t pid : g_children) {
        if (pid > 0) {
            send_command_to_pid(cmd, pid);
        }
    }
}

/**
 * Wysyła komendę do zakresu procesów (np. tylko dostawcy lub tylko stanowiska).
 * 
 * Układ g_children:
 *   [0]     = magazyn
 *   [1-4]   = dostawcy A, B, C, D
 *   [5-6]   = stanowiska 1, 2
 */
void send_command_to_range(Command cmd, size_t from, size_t to) {
    for (size_t i = from; i < to && i < g_children.size(); ++i) {
        if (g_children[i] > 0) {
            send_command_to_pid(cmd, g_children[i]);
        }
    }
}

// --- Sprzątanie zasobów IPC ---

// Usuwa wszystkie zasoby IPC - wywoływane tylko przez dyrektora na koniec!
// Inne procesy tylko odłączają się (shmdt), ale nie usuwają.
void remove_ipcs() {
    if (g_msgid != -1) msgctl(g_msgid, IPC_RMID, nullptr);  // usuń kolejkę
    if (g_semid != -1) semctl(g_semid, 0, IPC_RMID);        // usuń semafory
    if (g_shmid != -1) shmctl(g_shmid, IPC_RMID, nullptr);  // usuń pamięć dzieloną
}

// Czeka na zakończenie wszystkich procesów potomnych (blokujące)
void wait_children() {
    for (pid_t pid : g_children) {
        if (pid <= 0) continue;
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

/**
 * Graceful shutdown - zamyka wszystkie procesy w kontrolowany sposób.
 * 
 * Algorytm:
 * 1. Wyślij StopAll do wszystkich procesów
 * 2. Czekaj do 5 sekund aż same się zakończą (grace period)
 * 3. Jeśli któryś nie odpowiedział - wyślij SIGTERM
 * 4. Zbierz wszystkie zombie (waitpid)
 * 
 * Grace period jest ważny! Daje czas magazynowi na zapisanie stanu do pliku
 * przed zakończeniem. Bez tego mógłby dostać SIGTERM w trakcie zapisu.
 */
void graceful_shutdown() {
    // 1) Wyślij StopAll do wszystkich procesów
    send_command_to_all(Command::StopAll);
    
    // 2) Grace period: czekaj do 5 sekund (10 x 500ms)
    std::vector<bool> reaped(g_children.size(), false);  // które procesy już się zakończyły

    for (int i = 0; i < 10; ++i) {
        bool all_done = true;
        
        // Sprawdź każdy proces czy się już zakończył
        for (size_t j = 0; j < g_children.size(); ++j) {
            pid_t pid = g_children[j];
            if (pid <= 0 || reaped[j]) continue;

            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);  // WNOHANG = nie blokuj
            
            if (r == 0) {
                all_done = false;  // ten proces jeszcze żyje
            } else if (r == pid) {
                reaped[j] = true;  // ten się zakończył
            }
        }
        
        if (all_done) {
            std::cout << "[DYREKTOR] Wszystkie procesy zakończone gracefully.\n";
            return;
        }
        
        usleep(500000);  // czekaj 500ms przed kolejnym sprawdzeniem
    }
    
    // 3) Timeout - niektóre procesy nie odpowiedziały, wyślij SIGTERM
    std::cout << "[DYREKTOR] Timeout - wysyłam SIGTERM do pozostałych procesów.\n";
    for (size_t j = 0; j < g_children.size(); ++j) {
        pid_t pid = g_children[j];
        if (pid <= 0 || reaped[j]) continue;
        kill(pid, SIGTERM);
    }
    
    // 4) Zbierz pozostałe zombie
    for (size_t j = 0; j < g_children.size(); ++j) {
        pid_t pid = g_children[j];
        if (pid <= 0 || reaped[j]) continue;
        int status = 0;
        waitpid(pid, &status, 0);  // teraz blokująco
    }
}

/**
 * Uruchamia wszystkie procesy potomne w odpowiedniej kolejności.
 * 
 * WAŻNE: Magazyn musi wystartować pierwszy i zainicjalizować zasoby IPC!
 * Dlatego jest sleep(1) przed uruchomieniem pozostałych procesów.
 */
void start_processes(int capacity) {
    // Najpierw magazyn - on tworzy pamięć dzieloną, semafory i kolejkę
    spawn({"./magazyn", std::to_string(capacity)});
    
    sleep(1);  // daj magazynowi czas na inicjalizację IPC
    
    // Teraz dostawcy - każdy dostarcza inny składnik
    spawn({"./dostawca", "A"});
    spawn({"./dostawca", "B"});
    spawn({"./dostawca", "C"});
    spawn({"./dostawca", "D"});
    
    // Na końcu stanowiska produkcyjne
    spawn({"./stanowisko", "1"});  // produkuje czekoladę z A+B+C
    spawn({"./stanowisko", "2"});  // produkuje czekoladę z A+B+D
}

/**
 * Główna pętla menu - przyjmuje polecenia od użytkownika.
 * 
 * Polecenia zgodne z treścią zadania:
 * 1 - StopFabryka (zatrzymaj stanowiska)
 * 2 - StopMagazyn
 * 3 - StopDostawcy
 * 4 - StopAll (zapisz stan i zakończ wszystko)
 */
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
        if (!std::getline(std::cin, line)) break;  // EOF lub błąd
        
        if (line.empty()) continue;  // pomiń puste linie
        
        char choice = line[0];

        // Układ g_children: [0]=magazyn, [1-4]=dostawcy A,B,C,D, [5-6]=stanowiska 1,2
        if (choice == '1') {
            log_raport(g_semid, "DYREKTOR", "Wysyłam StopFabryka (zatrzymanie stanowisk)");
            send_command_to_range(Command::StopFabryka, 5, 7);  // stanowiska [5,6]
        }
        else if (choice == '2') {
            log_raport(g_semid, "DYREKTOR", "Wysyłam StopMagazyn");
            send_command_to_pid(Command::StopMagazyn, g_children[0]);  // magazyn [0]
        }
        else if (choice == '3') {
            log_raport(g_semid, "DYREKTOR", "Wysyłam StopDostawcy");
            send_command_to_range(Command::StopDostawcy, 1, 5);  // dostawcy [1,2,3,4]
        }
        else if (choice == '4') {
            log_raport(g_semid, "DYREKTOR", "Wysyłam StopAll (zapis stanu i zakończenie)");
            send_command_to_all(Command::StopAll);
            break;  // wyjdź z pętli menu
        }
        else if (choice == 'q' || choice == 'Q') {
            break;
        }
        else {
            std::cout << "Nieznana opcja: '" << choice << "'" << std::endl;
        }
    }
}

}  // koniec anonimowej przestrzeni nazw

// ============================================================================
// FUNKCJA MAIN
// ============================================================================

int main(int argc, char **argv) {
    // --- Parsowanie argumentów ---
    int capacity = kDefaultCapacity;
    
    if (argc > 1) {
        // Walidacja wejścia z użyciem strtol (wymaganie 4.1.b)
        // strtol jest bezpieczniejsze niż atoi bo pozwala wykryć błędy
        char *endptr = nullptr;
        long val = std::strtol(argv[1], &endptr, 10);
        
        // Sprawdź czy cały string był liczbą
        if (endptr == argv[1] || *endptr != '\0') {
            std::cerr << "Błąd: '" << argv[1] << "' nie jest poprawną liczbą.\n";
            std::cerr << "Użycie: " << argv[0] << " [capacity]\n";
            return 1;
        }
        
        // Sprawdź zakres
        if (val <= 0 || val > 10000) {
            std::cerr << "Błąd: capacity musi być w zakresie 1-10000.\n";
            return 1;
        }
        
        capacity = static_cast<int>(val);
    }

    // --- Inicjalizacja ---
    setup_sigaction(handle_signal);  // obsługa SIGINT, SIGTERM
    ensure_ipc_key();  // utwórz plik ipc.key jeśli nie istnieje

    // --- Uruchom procesy potomne ---
    start_processes(capacity);

    // --- Dołącz do IPC (utworzonych przez magazyn) ---
    attach_ipc();

    // --- Pętla menu ---
    menu_loop();

    // --- Zakończenie ---
    // graceful_shutdown() daje czas magazynowi na zapisanie stanu
    graceful_shutdown();

    // Usuń zasoby IPC - tylko dyrektor to robi!
    remove_ipcs();

    return 0;
}
