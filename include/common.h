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

// Tworzy plik ipc.key jeśli nie istnieje - potrzebny dla ftok()
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
	size_t offsetD;  // = offsetC + capacityC * kSizeC
	
	// Łączny rozmiar danych (bez nagłówka)
	size_t dataSize;
};

/**
 * Oblicza rozmiar pamięci dzielonej dla N czekolad na pracownika.
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
 * Zwraca wskaźnik na początek danych (tuż za nagłówkiem).
 */
inline char* warehouse_data(WarehouseHeader* h) {
	return reinterpret_cast<char*>(h) + sizeof(WarehouseHeader);
}

/**
 * Zwraca wskaźnik na segment A/B/C/D.
 */
inline char* segment_A(WarehouseHeader* h) { return warehouse_data(h) + h->offsetA; }
inline char* segment_B(WarehouseHeader* h) { return warehouse_data(h) + h->offsetB; }
inline char* segment_C(WarehouseHeader* h) { return warehouse_data(h) + h->offsetC; }
inline char* segment_D(WarehouseHeader* h) { return warehouse_data(h) + h->offsetD; }

// ============================================================================
// SEMAFORY
// ============================================================================

/*
 * Indeksy semaforów w zestawie - model RING BUFFER z OFFSETAMI BAJTOWYMI.
 * 
 * Dla każdego składnika X (A, B, C, D) mamy 4 semafory:
 *   SEM_EMPTY_X - liczba WOLNYCH miejsc (init = capacity)
 *   SEM_FULL_X  - liczba ZAJĘTYCH miejsc / dostępnych sztuk (init = 0)
 *   SEM_IN_X    - OFFSET BAJTOWY zapisu (init = 0)
 *   SEM_OUT_X   - OFFSET BAJTOWY odczytu (init = 0)
 * 
 * Dostawca:
 *   P(EMPTY_X) -> offset = IN_X -> wpisz[offset] -> IN = (IN + itemSize) % segmentSize -> V(FULL_X)
 * 
 * Stanowisko:
 *   P(FULL_X) -> offset = OUT_X -> czytaj[offset] -> OUT = (OUT + itemSize) % segmentSize -> V(EMPTY_X)
 * 
 * IN/OUT to OFFSETY BAJTOWE (nie numery elementów)!
 * 
 * Mutexy:
 *   SEM_MUTEX - ochrona sekcji krytycznych w pamięci dzielonej
 *   SEM_RAPORT - ochrona zapisu do pliku raportu
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

inline void die_perror(const char *msg) {
	perror(msg);
	std::exit(EXIT_FAILURE);
}

// --- Operacje na semaforach ---

/*
 * sem_V_retry (signal) - zwiększa wartość semafora
 * 
 * RETRY na EINTR - operacja MUSI się udać!
 * Używaj do oddawania EMPTY/FULL po zakończeniu operacji.
 * Sygnał NIE może "zgubić" podbicia semafora.
 * 
 * Zwraca: 0 przy sukcesie, -1 przy błędzie innym niż EINTR
 */
inline int sem_V_retry(int semid, int semnum, int delta = 1) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(delta), 0};
	while (true) {
		if (semop(semid, &op, 1) == 0) return 0;
		if (errno == EINTR) continue;  // sygnał - ponów
		return -1;  // prawdziwy błąd
	}
}

/*
 * sem_P_intr (wait) - zmniejsza wartość semafora (PRZERYWALNE!)
 * 
 * NIE robi retry na EINTR - pozwala procesowi wyjść po sygnale.
 * Używaj do czekania na FULL/EMPTY.
 * 
 * Zwraca: 0 przy sukcesie, -1 przy błędzie (w tym EINTR!)
 * Sprawdź errno==EINTR żeby rozróżnić sygnał od błędu.
 */
inline int sem_P_intr(int semid, int semnum, int delta = 1) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(-delta), 0};
	return semop(semid, &op, 1);
}

// P z SEM_UNDO (dla mutexów - automatyczne zwolnienie przy crashu)
inline int sem_P_undo(int semid, int semnum) {
	sembuf op{static_cast<unsigned short>(semnum), -1, SEM_UNDO};
	return semop(semid, &op, 1);
}

// V z SEM_UNDO (dla mutexów)
inline int sem_V_undo(int semid, int semnum) {
	sembuf op{static_cast<unsigned short>(semnum), 1, SEM_UNDO};
	return semop(semid, &op, 1);
}

// Wygodne wrappery dla mutexów
// PĘTLA NA EINTR - mutex MUSI być zdobyty/zwolniony!
inline void P_mutex(int semid) {
	while (sem_P_undo(semid, SEM_MUTEX) == -1) {
		if (errno == EINTR) continue;  // sygnał - ponów
		die_perror("P_mutex");
	}
}

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
 * Konfiguruje obsługę sygnałów.
 * 
 * Bez SA_RESTART - blokujące semop() zwróci EINTR po sygnale,
 * co pozwala procesowi zakończyć się.
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
