// Wspólne definicje dla procesów fabryki czekolady
#ifndef COMMON_H
#define COMMON_H
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

// zmienne do generowania klucza bazowego ftok
constexpr const char *kIpcKeyPath="./ipc.key";
constexpr int kProjId = 0x42;

// domyslna pojemnosc magazunu
constexpr int kDefaultCapacity = 100;


// typy polecen dyrektora 
enum class Command : int{
	None = 0,
	StopFabryka = 1,
	StopMagazyn = 2,
	StopDostawcy = 3,
	StopAll =4

};

// wiadomosci do kolejek komunikatow
// long gwaratnuje odpowieni rozmiar zgodny z oczekiwaniami jadra 
enum class MsgType : long {

	CommandBroadcast = 1,
	SupplierReport  = 2,
	WorkerRequest = 3,
	WarehouseReply =4


};

// polecenia dyrektora
struct  CommandMessage{
	long mtype;
	Command cmd;

};

// struktura zadan  pracownika o zestaw surowcow
struct WorkerRequestMessage{

	long mtype;
	int workerType;
	int needA;
	int needB;
	int needC;
	int needD;
	pid_t pid;

};

struct WarehouseReplyMessage{
	long mtype; 
	bool granted; // true jak wydano surowce


};

struct SupplierReportMessage{
	long mtype;
	char text[64];
};


// pamiec wspodzielona
struct WarehouseState{
	int capacity;
	int a;
	int b;
	int c;
	int d;


};


enum SemaphoreIndex{
	SEM_MUTEX = 0,
	SEM_CAPACITY = 1,
	SEM_A = 2,
	SEM_B = 3,
	SEM_C = 4,
	SEM_D = 5,
	SEM_RAPORT = 6,
	SEM_COUNT = 7
};

inline void die_perror(const char *msg){
	perror(msg);
	std::exit(EXIT_FAILURE);

}


inline int sem_up(int semid, int semnum, int delta = 1, short flg = 0) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(delta), flg};
	return semop(semid, &op, 1);
}

inline int sem_down(int semid, int semnum, int delta = 1, short flg = 0) {
	sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(-delta), flg};
	return semop(semid, &op, 1);
}

// P/V z SEM_UNDO dla mutexa
inline void P_mutex(int semid) {
	if (sem_down(semid, SEM_MUTEX, 1, SEM_UNDO) == -1) die_perror("P_mutex");
}

inline void V_mutex(int semid) {
	if (sem_up(semid, SEM_MUTEX, 1, SEM_UNDO) == -1) die_perror("V_mutex");
}

// P/V 
inline void P(int semid, int semnum, int delta = 1) {
	if (sem_down(semid, semnum, delta, 0) == -1) die_perror("P");
}

inline void V(int semid, int semnum, int delta = 1) {
	if (sem_up(semid, semnum, delta, 0) == -1) die_perror("V");
}

// Ścieżka do pliku raportu
constexpr const char *kRaportPath = "raport.txt";

// Logowanie do wspólnego pliku z ochroną semaforem SEM_RAPORT
inline void log_raport(int semid, const char* proces, const char* msg) {
	// Używamy osobnego semafora SEM_RAPORT (nie SEM_MUTEX) żeby nie blokować całego systemu podczas I/O
	if (sem_down(semid, SEM_RAPORT, 1, SEM_UNDO) == -1) {
		perror("P SEM_RAPORT");
		return;
	}
	
	FILE* f = fopen(kRaportPath, "a");
	if (f) {
		time_t now = time(nullptr);
		char timebuf[64];
		strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime(&now));
		fprintf(f, "[%s] %s: %s\n", timebuf, proces, msg);
		fflush(f);
		fclose(f);
	}
	
	if (sem_up(semid, SEM_RAPORT, 1, SEM_UNDO) == -1) {
		perror("V SEM_RAPORT");
	}
}

// Ustawienie obsługi sygnałów przez sigaction (zalecane nad signal())
// handler - funkcja obsługi sygnału, przyjmuje int (numer sygnału)
// Obsługiwane sygnały: SIGINT, SIGTERM, SIGUSR1, SIGUSR2
inline void setup_sigaction(void (*handler)(int)) {
	struct sigaction sa{};
	sa.sa_handler = handler;
	sa.sa_flags = 0;  // bez SA_RESTART - przerwane syscalle zwrócą EINTR
	sigemptyset(&sa.sa_mask);  // nie blokuj innych sygnałów podczas obsługi
	
	if (sigaction(SIGTERM, &sa, nullptr) == -1) {
		perror("sigaction SIGTERM");
	}
	if (sigaction(SIGINT, &sa, nullptr) == -1) {
		perror("sigaction SIGINT");
	}
	if (sigaction(SIGUSR1, &sa, nullptr) == -1) {
		perror("sigaction SIGUSR1");
	}
	if (sigaction(SIGUSR2, &sa, nullptr) == -1) {
		perror("sigaction SIGUSR2");
	}
}

#endif  // COMMON_H
