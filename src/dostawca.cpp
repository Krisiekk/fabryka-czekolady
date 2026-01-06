#include "../include/common.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <unistd.h>


namespace{
int g_msgid =-1;  // id kolejki komunikatow
int g_semid = -1; // id zestawu semaforow
int g_shmid = -1; // id pamieci dzielonej
WarehouseState  *g_state = nullptr; // wskaznik  na stan magazynu 
volatile sig_atomic_t g_stop = 0; // flaga zakonczenia petli (sig_atomic_t dla bezpieczenstwa handlera)
char g_type = 'A'; // jaki dostawca

//gdy dostaniemy sygnal SIGTERM lub SIGINT g stop na 1 koniec petli
void handle_signal(int) {g_stop = 1;}

static int units_per_item(char t){
    switch(t){
        case 'A': return 1;
        case 'B': return 1;
        case 'C': return 2;
        case 'D': return 3;
        default: return 1;
    }
}

static int sem_index_for(char t){
    switch(t){
        case 'A': return SEM_A;
        case 'B': return SEM_B;
        case 'C': return SEM_C;
        case 'D': return SEM_D;
        default:  return SEM_A;
    }
}


static int target_units(char t, int capUnits){
    switch(t){
        case 'A': return (2 * capUnits) / 9;
        case 'B': return (2 * capUnits) / 9;
        case 'C': return (2 * capUnits) / 9;
        case 'D': return (3 * capUnits) / 9;
        default:  return capUnits / 4;
    }
}


// tworzenie wspolnego klucza
key_t make_key(){
	key_t key = ftok(kIpcKeyPath,kProjId);
	if (key == -1 ){
		die_perror("ftok");
	}
	return (key);


}
 //dolaczenie do istniejacych zasobow
void attach_ipc() {
	key_t key = make_key();
	g_shmid = shmget(key,sizeof(WarehouseState),0600); // zanleznienie istniejacej pameci wspoldzielonej
	if(g_shmid == -1) die_perror("shmget"); 
	g_state = static_cast<WarehouseState*>(shmat(g_shmid,nullptr,0));
	if(g_state == reinterpret_cast<void*>(-1)) die_perror("shmat"); // podlaczenie do tej istniejacej pamieci
	g_semid =semget(key,SEM_COUNT,0600); // dolaczenie do istniejacego zestawu semaforow
	if(g_semid ==-1) die_perror("semget");
	g_msgid = msgget(key,0600);
	if(g_msgid == -1) die_perror("msgget");

};


void check_command();

void deliver_once(int amount){
    int uPer = units_per_item(g_type);
    int units = amount * uPer;

    // 1) Sprawdź miks bez trzymania mutexa podczas semop na pojemność
    P_mutex(g_semid);
    int cap = g_state->capacity;

    int a = g_state->a, b = g_state->b, c = g_state->c, d = g_state->d;
    int usedUnits = a + b + 2*c + 3*d;
    int freeUnits = cap - usedUnits;
    if (freeUnits < 0) freeUnits = 0;

    int myUnitsNow = 0;
    if(g_type=='A') myUnitsNow = a * 1;
    if(g_type=='B') myUnitsNow = b * 1;
    if(g_type=='C') myUnitsNow = c * 2;
    if(g_type=='D') myUnitsNow = d * 3;

    int myTarget = target_units(g_type, cap);

    V_mutex(g_semid);


    if (freeUnits < 10 && myUnitsNow > myTarget) {
        std::cout << "[DOSTAWCA " << g_type << "] miks OK? mam " << myUnitsNow
                  << "u > target " << myTarget << "u, czekam\n";
        // Krótkie czekanie ze sprawdzaniem komend
        for (int i = 0; i < 3 && !g_stop; i++) {
            usleep(100000);
            check_command();
        }
        return;
    }

  
    sembuf op{static_cast<unsigned short>(SEM_CAPACITY), static_cast<short>(-units), IPC_NOWAIT};
    if (semop(g_semid, &op, 1) == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::cout << "[DOSTAWCA " << g_type << "] magazyn pełny, czekam\n";
            // Krótkie czekanie ze sprawdzaniem komend
            for (int i = 0; i < 3 && !g_stop; i++) {
                usleep(100000);
                check_command();
            }
            return;
        }
        perror("semop capacity");
        return;
    }


    P_mutex(g_semid);
    if(g_type =='A') g_state->a += amount;
    if(g_type =='B') g_state->b += amount;
    if(g_type =='C') g_state->c += amount;
    if(g_type =='D') g_state->d += amount;
    V_mutex(g_semid);

    V(g_semid, sem_index_for(g_type), amount);

    // Logowanie do pliku
    char buf[128];
    P_mutex(g_semid);
    std::snprintf(buf, sizeof(buf), "Dostarczono %d x %c (stan: A=%d B=%d C=%d D=%d)",
                  amount, g_type, g_state->a, g_state->b, g_state->c, g_state->d);
    V_mutex(g_semid);
    log_raport(g_semid, "DOSTAWCA", buf);

    std::cout << "[DOSTAWCA " << g_type << "] +" << amount << "\n";

   
    SupplierReportMessage rep{};
    rep.mtype = static_cast<long>(MsgType::SupplierReport);
    std::snprintf(rep.text, sizeof(rep.text), "Dostawca %c + %d", g_type, amount);
    if (msgsnd(g_msgid, &rep, sizeof(rep) - sizeof(long), IPC_NOWAIT) == -1 && errno != EAGAIN) {
        perror("msgsnd report");
    }
}

void check_command() {
    Command cmd = ::check_command(g_msgid);  // używa PID jako mtype

    if (cmd == Command::StopDostawcy || cmd == Command::StopAll) {
        g_stop = 1;
    }
}
	


}








int main(int argc, char **argv){
	if(argc<2){
		std::cerr<<"Uzycie dostawca <A|B|C|D> [amount] \n";
		return 1;
	}


	g_type = argv[1][0];
	if (g_type != 'A' && g_type != 'B' && g_type != 'C' && g_type != 'D') {
		std::cerr << "Błąd: typ dostawcy musi być A, B, C lub D.\n";
		return 1;
	}
	
	// Walidacja amount z strtol
	int amount = 1;
	if (argc >= 3) {
		char *endptr = nullptr;
		long val = std::strtol(argv[2], &endptr, 10);
		if (endptr == argv[2] || *endptr != '\0' || val <= 0 || val > 100) {
			std::cerr << "Błąd: amount musi być liczbą 1-100.\n";
			return 1;
		}
		amount = static_cast<int>(val);
	}
	setup_sigaction(handle_signal);

	attach_ipc();

	srand(static_cast<unsigned>(time(nullptr)) ^ getpid());  // seed dla losowych przerw

	while(!g_stop){
		check_command();  
		if (g_stop) break;
		
		deliver_once(amount);
		
		check_command();  
		if (g_stop) break;
		
		
		int delay = (rand() % 3) + 1;  // losowa przerwa 1-3 sekundy
		for (int i = 0; i < delay * 10 && !g_stop; i++) {
			usleep(100000);  
			check_command();
		}
	}
	
	
	char endbuf[64];
	std::snprintf(endbuf, sizeof(endbuf), "Dostawca %c konczy prace (pid=%d)", g_type, getpid());
	log_raport(g_semid, "DOSTAWCA", endbuf);
	
	if(g_state &&shmdt(g_state)==-1) perror("shmdt");

	return 0;




}