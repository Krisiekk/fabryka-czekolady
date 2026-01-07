/*
 * common.h - Wspólne definicje dla wszystkich procesów fabryki czekolady
 * 
 * Ten plik zawiera struktury danych, stałe i funkcje pomocnicze używane
 * przez wszystkie procesy: dyrektor, magazyn, dostawca, stanowisko.
 * 
 * Autor: Krzysztof Pietrzak (156721)
 * Projekt: Fabryka Czekolady - Systemy Operacyjne 2025/2026
 */

#ifndef COMMON_H
#define COMMON_H

// --- Nagłówki systemowe dla IPC ---
#include <sys/ipc.h>    // klucze IPC (ftok)
#include <sys/msg.h>    // kolejki komunikatów
#include <sys/sem.h>    // semafory System V
#include <sys/shm.h>    // pamięć dzielona
#include <sys/types.h>  // typy systemowe (pid_t, key_t)
#include <sys/stat.h>   // stałe dla uprawnień plików
#include <fcntl.h>      // flagi open() - O_CREAT, O_RDONLY itp.
#include <unistd.h>     // syscalle: read, write, close, getpid

// --- Nagłówki C++ ---
#include <cerrno>       // errno - kody błędów
#include <csignal>      // obsługa sygnałów (sigaction)
#include <cstdint>      // typy o stałym rozmiarze
#include <cstdio>       // perror, snprintf
#include <cstdlib>      // exit, strtol
#include <ctime>        // timestampy do logów

// ============================================================================
// KONFIGURACJA IPC
// ============================================================================

// Ścieżka do pliku używanego przez ftok() do generowania klucza IPC.
// Wszystkie procesy muszą używać tego samego pliku żeby dostać ten sam klucz.
constexpr const char *kIpcKeyPath = "./ipc.key";

// Identyfikator projektu dla ftok() - może być dowolny, byle ten sam wszędzie
constexpr int kProjId = 0x42;

// Tworzy plik ipc.key jeśli nie istnieje - potrzebny dla ftok()
// Bez tego ftok() zwróci błąd bo nie znajdzie pliku
inline void ensure_ipc_key() {
	int fd = open(kIpcKeyPath, O_CREAT | O_WRONLY, 0600);
	if (fd == -1) {
		perror("open ipc.key");
	} else {
		close(fd);
	}
}

// Domyślna pojemność magazynu gdy nie podano argumentu
constexpr int kDefaultCapacity = 100;

// ============================================================================
// TYPY POLECEŃ I WIADOMOŚCI
// ============================================================================

// Komendy wysyłane przez dyrektora do procesów potomnych.
// Numeracja 1-4 odpowiada poleceniom z treści zadania.
enum class Command : int {
	None = 0,         // brak komendy (domyślnie)
	StopFabryka = 1,  // polecenie_1: zatrzymaj stanowiska produkcyjne
	StopMagazyn = 2,  // polecenie_2: zatrzymaj magazyn
	StopDostawcy = 3, // polecenie_3: zatrzymaj dostawców
	StopAll = 4       // polecenie_4: zapisz stan i zakończ wszystko
};

// Typy wiadomości w kolejce komunikatów.
// mtype w strukturze wiadomości musi być > 0, stąd numeracja od 1.
// UWAGA: dla poleceń dyrektora używamy PID jako mtype (per-PID targeting),
//        te wartości są tylko dla innych typów wiadomości.
enum class MsgType : long {
	CommandBroadcast = 1,  // (nieużywane - komendy idą per-PID)
	SupplierReport = 2,    // raport od dostawcy do magazynu
	WorkerRequest = 3,     // żądanie surowców od stanowiska
	WarehouseReply = 4     // odpowiedź magazynu do stanowiska
};

// ============================================================================
// STRUKTURY WIADOMOŚCI (dla kolejki komunikatów)
// ============================================================================

// Wiadomość z komendą od dyrektora.
// mtype = PID procesu docelowego (dzięki temu każdy proces odbiera tylko swoje)
struct CommandMessage {
	long mtype;      // PID procesu docelowego
	Command cmd;     // jaka komenda
};

// Żądanie surowców od stanowiska produkcyjnego do magazynu.
// Stanowisko wysyła ile potrzebuje każdego składnika.
struct WorkerRequestMessage {
	long mtype;       // MsgType::WorkerRequest
	int workerType;   // 1 lub 2 (typ stanowiska)
	int needA;        // ile sztuk składnika A potrzebuje
	int needB;        // ile sztuk składnika B
	int needC;        // ile sztuk składnika C (tylko stanowisko 1)
	int needD;        // ile sztuk składnika D (tylko stanowisko 2)
	pid_t pid;        // PID stanowiska - na ten PID magazyn wyśle odpowiedź
};

// Odpowiedź magazynu na żądanie stanowiska
struct WarehouseReplyMessage {
	long mtype;       // PID stanowiska które pytało
	bool granted;     // true = wydano surowce, false = brak (spróbuj później)
};

// Raport od dostawcy (informacja tekstowa)
struct SupplierReportMessage {
	long mtype;       // MsgType::SupplierReport
	char text[64];    // treść raportu
};

// ============================================================================
// PAMIĘĆ DZIELONA - STAN MAGAZYNU
// ============================================================================

// Struktura przechowywana w pamięci dzielonej - aktualny stan magazynu.
// Dostęp chroniony semaforem SEM_MUTEX.
struct WarehouseState {
	int capacity;     // max pojemność w jednostkach (A,B=1, C=2, D=3)
	int a;            // ile sztuk składnika A w magazynie
	int b;            // ile sztuk składnika B
	int c;            // ile sztuk składnika C
	int d;            // ile sztuk składnika D
};

// ============================================================================
// SEMAFORY
// ============================================================================

// Indeksy semaforów w zestawie. Używamy jednego zestawu z 7 semaforami.
enum SemaphoreIndex {
	SEM_MUTEX = 0,     // mutex do ochrony pamięci dzielonej (sekcja krytyczna)
	SEM_CAPACITY = 1,  // zlicza wolne miejsce w magazynie (producent-konsument)
	SEM_A = 2,         // zlicza dostępne sztuki składnika A
	SEM_B = 3,         // zlicza dostępne sztuki składnika B
	SEM_C = 4,         // zlicza dostępne sztuki składnika C
	SEM_D = 5,         // zlicza dostępne sztuki składnika D
	SEM_RAPORT = 6,    // mutex do pliku raportu (żeby logi się nie mieszały)
	SEM_COUNT = 7      // łączna liczba semaforów
};

// ============================================================================
// FUNKCJE POMOCNICZE
// ============================================================================

// Wypisz błąd i zakończ program - używane gdy coś pójdzie nie tak z IPC
inline void die_perror(const char *msg) {
	perror(msg);
	std::exit(EXIT_FAILURE);
}


// --- Operacje na semaforach ---
// Wrapper na semop() dla zwiększania wartości semafora (operacja V / signal)
inline int sem_up(int semid, int semnum, int delta = 1, short flg = 0) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(delta), flg};
	return semop(semid, &op, 1);
}

// Wrapper na semop() dla zmniejszania wartości semafora (operacja P / wait)
inline int sem_down(int semid, int semnum, int delta = 1, short flg = 0) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(-delta), flg};
	return semop(semid, &op, 1);
}

// Operacja P (wait) na mutexie z flagą SEM_UNDO.
// SEM_UNDO sprawia że jeśli proces umrze, semafor zostanie automatycznie zwolniony
// (zapobiega deadlockom gdy proces crashuje trzymając mutex).
inline void P_mutex(int semid) {
	if (sem_down(semid, SEM_MUTEX, 1, SEM_UNDO) == -1) die_perror("P_mutex");
}

// Operacja V (signal) na mutexie - zwalnia mutex
inline void V_mutex(int semid) {
	if (sem_up(semid, SEM_MUTEX, 1, SEM_UNDO) == -1) die_perror("V_mutex");
}

// Zwykłe P/V bez SEM_UNDO - używane dla semaforów zliczających (pojemność, składniki)
inline void P(int semid, int semnum, int delta = 1) {
	if (sem_down(semid, semnum, delta, 0) == -1) die_perror("P");
}

inline void V(int semid, int semnum, int delta = 1) {
	if (sem_up(semid, semnum, delta, 0) == -1) die_perror("V");
}

// ============================================================================
// LOGOWANIE DO PLIKU RAPORTU
// ============================================================================

constexpr const char *kRaportPath = "raport.txt";

/**
 * Zapisuje linię do pliku raportu z timestampem.
 * 
 * Format: [HH:MM:SS] PROCES: wiadomość
 * 
 * Używa semafora SEM_RAPORT żeby logi z różnych procesów się nie mieszały.
 * Mógłbym użyć SEM_MUTEX ale wtedy zapis do pliku blokowałby cały magazyn.
 */
inline void log_raport(int semid, const char* proces, const char* msg) {
	// Wejście do sekcji krytycznej zapisu do pliku
	if (sem_down(semid, SEM_RAPORT, 1, SEM_UNDO) == -1) {
		perror("P SEM_RAPORT");
		return;
	}
	
	// Otwieram plik w trybie append - każdy proces dopisuje na końcu
	int fd = open(kRaportPath, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd != -1) {
		// Przygotuj timestamp
		time_t now = time(nullptr);
		struct tm tmnow{};
		localtime_r(&now, &tmnow);  // wersja thread-safe localtime
		char timebuf[64];
		strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmnow);
		
		// Sformatuj i zapisz linię
		char linebuf[256];
		int len = snprintf(linebuf, sizeof(linebuf), "[%s] %s: %s\n", timebuf, proces, msg);
		
		if (len > 0 && len < static_cast<int>(sizeof(linebuf))) {
			if (write(fd, linebuf, len) == -1) {
				perror("write raport");
			}
		}
		close(fd);
	} else {
		perror("open raport");
	}
	
	// Zwolnij semafor
	if (sem_up(semid, SEM_RAPORT, 1, SEM_UNDO) == -1) {
		perror("V SEM_RAPORT");
	}
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

/**
 * Konfiguruje obsługę sygnałów przez sigaction().
 * 
 * sigaction() jest lepsze od signal() bo:
 * - zachowanie jest przenośne między systemami
 * - można kontrolować czy syscalle mają być restartowane
 * - można blokować inne sygnały podczas obsługi
 * 
 * Używam flag = 0 (bez SA_RESTART) żeby sleep() i inne blokujące funkcje
 * zwracały EINTR po otrzymaniu sygnału - dzięki temu proces może się zakończyć.
 */
inline void setup_sigaction(void (*handler)(int)) {
	struct sigaction sa{};
	sa.sa_handler = handler;
	sa.sa_flags = 0;  // NIE ustawiamy SA_RESTART - chcemy żeby syscalle były przerywane
	sigemptyset(&sa.sa_mask);
	
	// Obsługujemy 4 sygnały (wymaganie: co najmniej 2 różne)
	if (sigaction(SIGTERM, &sa, nullptr) == -1) perror("sigaction SIGTERM");
	if (sigaction(SIGINT, &sa, nullptr) == -1) perror("sigaction SIGINT");
	if (sigaction(SIGUSR1, &sa, nullptr) == -1) perror("sigaction SIGUSR1");
	if (sigaction(SIGUSR2, &sa, nullptr) == -1) perror("sigaction SIGUSR2");
}

// ============================================================================
// ODBIERANIE POLECEŃ
// ============================================================================

/**
 * Sprawdza czy przyszła komenda dla tego procesu (nieblokujące).
 * 
 * Kluczowa rzecz: używamy getpid() jako mtype!
 * Dyrektor wysyła komendę z mtype = PID procesu docelowego,
 * a każdy proces odbiera tylko wiadomości ze swoim PID.
 * 
 * Wcześniej próbowałem broadcastu (mtype=1) ale wtedy tylko jeden proces
 * odbierał wiadomość i reszta jej nie widziała. Per-PID rozwiązuje ten problem.
 */
inline Command check_command(int msqid) {
	CommandMessage msg{};
	// IPC_NOWAIT = nie blokuj jeśli nie ma wiadomości, zwróć błąd ENOMSG
	ssize_t ret = msgrcv(msqid, &msg, sizeof(msg) - sizeof(long), getpid(), IPC_NOWAIT);
	if (ret != -1) {
		return msg.cmd;
	}
	return Command::None;
}

#endif  // COMMON_H
