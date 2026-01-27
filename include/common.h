/**
 * @file include/common.h
 * @brief Wspólne definicje i helpery używane przez wszystkie procesy.
 *
 * Zawiera definicje struktur SHM, indeksy semaforów, funkcje pomocnicze do
 * operacji na semaforach, kolejkach komunikatów oraz logowania.
 *
 * Autor: Krzysztof Pietrzak (156721)
 * Projekt: Fabryka Czekolady - Systemy Operacyjne 2025/2026
 */

#ifndef COMMON_H
#define COMMON_H

// --- Nagłówki systemowe dla IPC ---
#include <sys/ipc.h>    // klucze IPC (ftok)
#include <sys/sem.h>    // semafory System V
#include <sys/shm.h>    // pamięć dzielona
#include <sys/types.h>  // typy systemowe (pid_t, key_t)
#include <sys/stat.h>   // stałe dla uprawnień plików
#include <fcntl.h>      // flagi open() - O_CREAT, O_RDONLY itp.
#include <unistd.h>     // syscalle: read, write, close, getpid
#include <sys/msg.h>    // kolejki komunikatów System V (msgrcv, msgsnd)

// --- Nagłówki C++ ---
#include <cerrno>       // errno - kody błędów
#include <csignal>      // obsługa sygnałów (sigaction)
#include <cstdint>      // typy o stałym rozmiarze
#include <cstdio>       // perror, snprintf
#include <cstdlib>      // exit, strtol
#include <cstring>      // memset, memcpy
#include <ctime>        // timestampy do logów

// ============================================================================
// UNION SEMUN - wymagany przez semctl() na Linuxie
// ============================================================================
// Na większości Linuxów union semun nie jest zdefiniowany w sys/sem.h,
// trzeba go zdefiniować samodzielnie.
union semun {
    int val;               // wartość dla SETVAL
    struct semid_ds *buf;  // bufor dla IPC_STAT, IPC_SET
    unsigned short *array; // tablica dla GETALL, SETALL
};

// ============================================================================
// KONFIGURACJA IPC
// ============================================================================

// Ścieżka do pliku używanego przez ftok() do generowania klucza IPC.
constexpr const char *kIpcKeyPath = "./ipc.key";

// Identyfikator projektu dla ftok()
constexpr int kProjId = 0x42;

/**
 * Tworzy plik używany przez ftok() jeśli go jeszcze nie ma.
 *
 * Funkcja zapewnia, że plik `kIpcKeyPath` istnieje — to umożliwia
 * generowanie stabilnego klucza IPC w kolejnych wywołaniach `ftok()`.
 * W razie błędu wypisuje komunikat przez `perror()` i kontynuuje.
 */
inline void ensure_ipc_key() {
	int fd = open(kIpcKeyPath, O_CREAT | O_WRONLY, 0600);
	if (fd == -1) {
		perror("open ipc.key");
	} else {
		close(fd);
	}
} 

// Domyślna liczba czekolad na pracownika gdy nie podano argumentu
constexpr int kDefaultChocolates = 100;

// ============================================================================
// ROZMIARY SKŁADNIKÓW W PAMIĘCI
// ============================================================================

// Każdy składnik zajmuje określoną ilość bajtów w pamięci
constexpr int kSizeA = 1;  // składnik A = 1 bajt
constexpr int kSizeB = 1;  // składnik B = 1 bajt
constexpr int kSizeC = 2;  // składnik C = 2 bajty
constexpr int kSizeD = 3;  // składnik D = 3 bajty

// ============================================================================
// PAMIĘĆ DZIELONA - MAGAZYN
// ============================================================================

/**
 * Struktura nagłówka magazynu w pamięci dzielonej.
 * 
 * Po nagłówku następują segmenty danych dla składników A, B, C, D.
 * Całkowita wielkość pamięci = sizeof(WarehouseHeader) + dataSize
 * 
 * Dla N czekolad na pracownika:
 *   capacityA = 2*N (obie receptury używają A)
 *   capacityB = 2*N (obie receptury używają B)
 *   capacityC = N   (tylko typ 1)
 *   capacityD = N   (tylko typ 2)
 *   dataSize = 2*N*1 + 2*N*1 + N*2 + N*3 = 9*N bajtów
 */
struct WarehouseHeader {
	int targetChocolates;  // ile czekolad na pracownika (argument z CLI)
	
	// Pojemności segmentów (ile sztuk max)
	int capacityA;
	int capacityB;
	int capacityC;
	int capacityD;
	
	// UWAGA: Ilości składników są trzymane w semaforach FULL_X, nie w pamięci!
	
	// Offsety do danych w pamięci (względem początku segmentu danych)
	size_t offsetA;  // = 0
	size_t offsetB;  // = capacityA * kSizeA
	size_t offsetC;  // = offsetB + capacityB * kSizeB
	size_t offsetD;  // = offsetC + capacityC ja m* kSizeC
	
	// Łączny rozmiar danych (bez nagłówka)
	size_t dataSize;
};

/**
 * Oblicza rozmiar pamięci dzielonej dla N czekolad na pracownika.
 *
 * @param n liczba czekolad na pracownika
 * @return rozmiar w bajtach (nagłówek + obszar danych)
 */
inline size_t calc_shm_size(int n) {
	size_t headerSize = sizeof(WarehouseHeader);
	size_t dataSize = static_cast<size_t>(2*n) * kSizeA   // segment A
	                + static_cast<size_t>(2*n) * kSizeB   // segment B
	                + static_cast<size_t>(n) * kSizeC     // segment C
	                + static_cast<size_t>(n) * kSizeD;    // segment D
	return headerSize + dataSize;
}

/**
 * Inicjalizuje nagłówek magazynu dla N czekolad na pracownika.
 *
 * Ustawia pojemności i offsety segmentów; nie ustawia stanów FULL/EMPTY,
 * te wartości trzymane są w semaforach.
 *
 * @param h wskaźnik na nagłówek w pamięci dzielonej
 * @param n liczba czekolad na pracownika
 */
inline void init_warehouse_header(WarehouseHeader* h, int n) {
	h->targetChocolates = n;
	
	// Pojemności
	h->capacityA = 2 * n;
	h->capacityB = 2 * n;
	h->capacityC = n;
	h->capacityD = n;
	
	// Offsety segmentów (dane zaczynają się tuż za nagłówkiem)
	h->offsetA = 0;
	h->offsetB = h->offsetA + static_cast<size_t>(h->capacityA) * kSizeA;
	h->offsetC = h->offsetB + static_cast<size_t>(h->capacityB) * kSizeB;
	h->offsetD = h->offsetC + static_cast<size_t>(h->capacityC) * kSizeC;
	
	// Łączny rozmiar danych
	h->dataSize = h->offsetD + static_cast<size_t>(h->capacityD) * kSizeD;
}

/**
 * Zwraca wskaźnik na początek obszaru danych (tuż za nagłówkiem).
 *
 * @param h wskaźnik nagłówka magazynu
 * @return wskaźnik do obszaru danych
 */
inline char* warehouse_data(WarehouseHeader* h) {
	return reinterpret_cast<char*>(h) + sizeof(WarehouseHeader);
}

/**
 * Zwraca wskaźnik na segment A.
 *
 * @param h wskaźnik nagłówka magazynu
 * @return wskaźnik do początku segmentu A
 */
inline char* segment_A(WarehouseHeader* h) { return warehouse_data(h) + h->offsetA; }
/**
 * Zwraca wskaźnik na segment B.
 *
 * @param h wskaźnik nagłówka magazynu
 * @return wskaźnik do początku segmentu B
 */
inline char* segment_B(WarehouseHeader* h) { return warehouse_data(h) + h->offsetB; }
/**
 * Zwraca wskaźnik na segment C.
 *
 * @param h wskaźnik nagłówka magazynu
 * @return wskaźnik do początku segmentu C
 */
inline char* segment_C(WarehouseHeader* h) { return warehouse_data(h) + h->offsetC; }
/**
 * Zwraca wskaźnik na segment D.
 *
 * @param h wskaźnik nagłówka magazynu
 * @return wskaźnik do początku segmentu D
 */
inline char* segment_D(WarehouseHeader* h) { return warehouse_data(h) + h->offsetD; }

// ============================================================================
// SEMAFORY
// ============================================================================

/**
 * Indeksy semaforów w zestawie — model ring buffer z offsetami bajtowymi.
 *
 * Dla każdego składnika X (A,B,C,D) mamy semafory EMPTY/FULL oraz IN/OUT
 * (offsety bajtowe). Opis działania: P(EMPTY) -> zapis -> IN update -> V(FULL)
 * oraz P(FULL) -> odczyt -> OUT update -> V(EMPTY).
 *
 * Mutexy: SEM_MUTEX (ochrona SHM), SEM_RAPORT (ochrona pliku raportu).
 */
enum SemaphoreIndex {
	SEM_MUTEX = 0,      // mutex do ochrony pamięci dzielonej
	SEM_RAPORT = 1,     // mutex do pliku raportu
	// EMPTY - wolne miejsca
	SEM_EMPTY_A = 2,
	SEM_EMPTY_B = 3,
	SEM_EMPTY_C = 4,
	SEM_EMPTY_D = 5,
	// FULL - zajęte miejsca
	SEM_FULL_A = 6,
	SEM_FULL_B = 7,
	SEM_FULL_C = 8,
	SEM_FULL_D = 9,
	// IN - OFFSET BAJTOWY zapisu (gdzie dostawca wpisuje)
	SEM_IN_A = 10,
	SEM_IN_B = 11,
	SEM_IN_C = 12,
	SEM_IN_D = 13,
	// OUT - OFFSET BAJTOWY odczytu (skąd stanowisko czyta)
	SEM_OUT_A = 14,
	SEM_OUT_B = 15,
	SEM_OUT_C = 16,
	SEM_OUT_D = 17,
	// Flaga czy magazyn działa (1=ON, 0=OFF)
	SEM_WAREHOUSE_ON = 18,
	SEM_COUNT = 19      // łączna liczba semaforów
};

// ============================================================================
// FUNKCJE POMOCNICZE
// ============================================================================

/**
 * Wypisuje komunikat o błędzie i kończy program.
 *
 * Wrapper nad `perror()` kończący program z kodem `EXIT_FAILURE`.
 * Używamy tego, gdy błąd jest krytyczny i nie można kontynuować.
 *
 * @param msg komunikat przekazywany do perror
 */
inline void die_perror(const char *msg) {
	perror(msg);
	std::exit(EXIT_FAILURE);
} 

// --- Operacje na semaforach ---

/**
 * Zwiększa semafor (V) i powtarza przy EINTR.
 *
 * Używaj tam, gdzie podbicie semafora musi się wykonać niezależnie od sygnałów
 * (np. oddawanie EMPTY/FULL). Funkcja pętlą ponawia `semop` jeśli wystąpi
 * EINTR.
 *
 * @param semid id zestawu semaforów
 * @param semnum indeks semafora
 * @param delta ile dodać (domyślnie 1)
 * @return 0 przy sukcesie, -1 przy błędzie (errno ustawione)
 */
inline int sem_V_retry(int semid, int semnum, int delta = 1) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(delta), 0};
	while (true) {
		if (semop(semid, &op, 1) == 0) return 0;
		if (errno == EINTR) continue;  // sygnał - ponów
		return -1;  // prawdziwy błąd
	}
}

/**
 * Czeka na semafor (P) — wywołanie przerwalne przez sygnały.
 *
 * Funkcja NIE powtarza `semop` przy EINTR — zwraca -1 i ustawia errno==EINTR
 * gdy wywołanie przerwie sygnał. Używaj gdy chcesz, żeby oczekiwanie było
 * możliwe do przerwania.
 *
 * @param semid id zestawu semaforów
 * @param semnum indeks semafora
 * @param delta ile zmniejszyć (domyślnie 1)
 * @return 0 przy sukcesie, -1 przy błędzie (sprawdź errno dla EINTR)
 */
inline int sem_P_intr(int semid, int semnum, int delta = 1) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(-delta), 0};
	return semop(semid, &op, 1);
} 

/**
 * P z flagą SEM_UNDO — przydatne dla mutexów (auto-zwolnienie przy crashu).
 *
 * @param semid id zestawu semaforów
 * @param semnum indeks semafora
 * @return 0 przy sukcesie, -1 przy błędzie
 */
inline int sem_P_undo(int semid, int semnum) {
	sembuf op{static_cast<unsigned short>(semnum), -1, SEM_UNDO};
	return semop(semid, &op, 1);
}

/**
 * V z flagą SEM_UNDO.
 *
 * @param semid id zestawu semaforów
 * @param semnum indeks semafora
 * @return 0 przy sukcesie, -1 przy błędzie
 */
inline int sem_V_undo(int semid, int semnum) {
	sembuf op{static_cast<unsigned short>(semnum), 1, SEM_UNDO};
	return semop(semid, &op, 1);
} 

/**
 * Atomowe przejście przez bramkę (P i V w jednym wywołaniu kernela).
 *
 * Zapobiega sytuacji, w której proces zabiera bramkę i potem jest zatrzymany.
 * Operacja jest przerwalna — w razie sygnału zwraca -1 i errno==EINTR.
 *
 * @param semid id zestawu semaforów
 * @param semnum indeks bramki
 * @return 0 przy sukcesie, -1 przy błędem (errno może być EINTR)
 */
inline int pass_gate_intr(int semid, int semnum) {
	sembuf ops[2];

	ops[0] = {static_cast<unsigned short>(semnum), -1, 0};
	ops[1] = {static_cast<unsigned short>(semnum), +1, 0};
	return semop(semid, ops, 2);
}

// ---------------------------------------------------------------------------
// Kolejka komunikatów (po pid) - prosta implementacja helperów
// ---------------------------------------------------------------------------
struct msq_msg {
	long mtype; // pid odbiorcy
	int state;  // 0 = closed, 1 = open
};

/**
 * Wyślij wiadomość do konkretnego pida (kolejka System V), retry na EINTR.
 *
 * @param msqid id kolejki
 * @param pid pid odbiorcy (mtype)
 * @param state payload (0 = zamknięte, 1 = otwarte)
 * @return 0 przy sukcesie, -1 przy błędzie
 */
inline int msq_send_pid(int msqid, pid_t pid, int state) {
	msq_msg m{};
	m.mtype = static_cast<long>(pid);
	m.state = state;

	while (true) {
		if (msgsnd(msqid, &m, sizeof(m.state), 0) == 0) return 0;
		if (errno == EINTR) continue;
		return -1;
	}
}

/**
 * Odbierz wiadomość dedykowaną dla konkretnego pida (blokująco, przerwalne).
 *
 * Jeśli wywołanie zostanie przerwane sygnałem, funkcja zwróci -1 i errno==EINTR.
 *
 * @param msqid id kolejki
 * @param pid pid odbiorcy (mtype)
 * @param out_state wskaźnik, do którego zapisany zostanie otrzymany stan
 * @return 0 przy sukcesie, -1 przy błędzie
 */
inline int msq_recv_pid_intr(int msqid, pid_t pid, int *out_state) {
	msq_msg m{};
	ssize_t r = msgrcv(msqid, &m, sizeof(m.state), static_cast<long>(pid), 0);

	if (r == -1) return -1;
	*out_state = m.state;
	return 0;
}

/**
 * Wrapper do zdobycia globalnego mutexu (SEM_MUTEX) - retry na EINTR.
 *
 * Funkcja pętlą próbuje wykonać P z SEM_UNDO; w razie błędu kończy program.
 *
 * @param semid id zestawu semaforów
 */
inline void P_mutex(int semid) {
	while (sem_P_undo(semid, SEM_MUTEX) == -1) {
		if (errno == EINTR) continue;  // sygnał - ponów
		die_perror("P_mutex");
	}
}

/**
 * Wrapper do zwolnienia globalnego mutexu (SEM_MUTEX) - retry na EINTR.
 *
 * @param semid id zestawu semaforów
 */
inline void V_mutex(int semid) {
	while (sem_V_undo(semid, SEM_MUTEX) == -1) {
		if (errno == EINTR) continue;  // sygnał - ponów
		die_perror("V_mutex");
	}
} 

// ============================================================================
// LOGOWANIE DO PLIKU RAPORTU
// ============================================================================

constexpr const char *kRaportPath = "raport.txt";

/**
 * Zapisuje linię do pliku raportu chronionego SEM_RAPORT.
 *
 * Funkcja zdobywa mutex SEM_RAPORT (retry na EINTR), dopisuje czas + komunikat
 * i zwalnia mutex (także retry). W razie błędu przy lock wypisuje perror i
 * wychodzi bez logowania.
 *
 * @param semid id zestawu semaforów (używany SEM_RAPORT)
 * @param proces nazwa procesu (np. "DYREKTOR")
 * @param msg wiadomość do dopisania
 */
inline void log_raport(int semid, const char* proces, const char* msg) {
	// Wejście do sekcji krytycznej (retry na EINTR)
	while (sem_P_undo(semid, SEM_RAPORT) == -1) {
		if (errno == EINTR) continue;  // sygnał - ponów
		perror("P SEM_RAPORT");
		return;  // prawdziwy błąd - nie logujemy
	}
	
	int fd = open(kRaportPath, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd != -1) {
		time_t now = time(nullptr);
		struct tm tmnow{};
		localtime_r(&now, &tmnow);
		char timebuf[64];
		strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmnow);
		
		char linebuf[256];
		int len = snprintf(linebuf, sizeof(linebuf), "[%s] %s: %s\n", timebuf, proces, msg);
		
		if (len > 0 && len < static_cast<int>(sizeof(linebuf))) {
			write(fd, linebuf, len);
		}
		close(fd);
	}
	
	// Zwolnienie sekcji krytycznej (retry na EINTR)
	while (sem_V_undo(semid, SEM_RAPORT) == -1) {
		if (errno == EINTR) continue;
		perror("V SEM_RAPORT");
		break;  // prawdziwy błąd - wychodzimy
	}
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

/**
 * Ustawia podstawowe handler-y sygnałów (SIGTERM, SIGINT, SIGUSR1).
 *
 * Nie ustawia SA_RESTART, dzięki czemu wywołania blokujące przerwą się i
 * zwrócą EINTR — to ułatwia bezpieczne zakończenie procesów po sygnale.
 *
 * @param handler funkcja obsługi sygnału (void(int))
 */
inline void setup_sigaction(void (*handler)(int)) {
	struct sigaction sa{};
	sa.sa_handler = handler;
	sa.sa_flags = 0;  // bez SA_RESTART!
	sigemptyset(&sa.sa_mask);

	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGUSR1, &sa, nullptr);
	// SIGUSR2 - zarezerwowany na przyszłość, obecnie niewykorzystywany
}

#endif  // COMMON_H
