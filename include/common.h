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
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// zmienne do generowania klucza bazowego ftok
constexpr const char *kIpcKeyPath="/home/krzysztof/fabryka-czekolady/CMakeLists.txt";
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
	SEM_COUNT = 6


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




#endif  // COMMON_H
