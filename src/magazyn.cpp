#include "../include/common.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <cerrno>

namespace {
int g_msgid = -1;
int g_semid = -1;
int g_shmid = -1;
WarehouseState *g_state = nullptr;
volatile sig_atomic_t g_stop = 0;
bool g_fresh = false;

// void add_supply(char type, int amount);

std::string g_stateFile = "magazyn_state.txt";

union semun{
	int val;
	struct semid_ds *buf;
	unsigned short *array;
	struct seminfo *__buf;

};

void handle_signal (int) {g_stop = 1;}

key_t make_key(){
	key_t key = ftok(kIpcKeyPath,kProjId);
	if(key == -1) die_perror("ftok");
	return key;

}

void init_ipc(int capacity){

	key_t  key = make_key();
	// tworzenie segmentu pamieci o danym kluczu

	g_shmid = shmget(key,sizeof(WarehouseState),IPC_CREAT | 0600);
	if(g_shmid == -1) die_perror("shmget");

	// podpinanie sie  procesu do sharedmemmory
	g_state = static_cast<WarehouseState*>(shmat(g_shmid,nullptr,0));
	if(g_state == reinterpret_cast<void*>(-1)) die_perror("shmat");

	// tworzy kolejke wiadomosci
	g_msgid = msgget(key,IPC_CREAT | 0600);
	if(g_msgid == -1) die_perror("msgget");

	g_semid= semget(key,SEM_COUNT,IPC_CREAT | IPC_EXCL | 0600);

	bool fresh = true;

	 // jesli istnieja to dolaczamy do istniejacego juz zbioru
	if(g_semid == -1){
		if(errno !=EEXIST) die_perror("semget");
		fresh = false;
		g_semid = semget(key,SEM_COUNT,0600);
		if(g_semid == -1) die_perror("semget attach");

	}

	g_fresh = fresh;

	if(fresh){
		g_state->capacity =capacity;
		g_state->a = g_state->b = g_state->c = g_state->d = 0;
	

	semun arg{};
	arg.val =1;
	if(semctl(g_semid,SEM_MUTEX,SETVAL,arg)== -1) die_perror("semctl mutex");

	arg.val  = capacity;
	if(semctl(g_semid,SEM_CAPACITY,SETVAL,arg)==-1) die_perror("semctl capacity");

	arg.val = 0;
	if(semctl(g_semid,SEM_A,SETVAL,arg)== -1) die_perror("semctl SEM_A");
	if(semctl(g_semid,SEM_B,SETVAL,arg)== -1) die_perror("semctl SEM_B");
	if(semctl(g_semid,SEM_C,SETVAL,arg)== -1) die_perror("semctl SEM_C");
	if(semctl(g_semid,SEM_D,SETVAL,arg)== -1) die_perror("semctl SEM_D");

	arg.val = 1;  // SEM_RAPORT = mutex do pliku raportu
	if(semctl(g_semid,SEM_RAPORT,SETVAL,arg)== -1) die_perror("semctl SEM_RAPORT");

	}


}

void load_state_from_file(){
	// Używamy syscalli open/read/close (wymaganie 5.2)
	int fd = open(g_stateFile.c_str(), O_RDONLY);
	if (fd == -1) return;  // plik nie istnieje
	
	char buf[128];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	
	if (n <= 0) return;
	buf[n] = '\0';
	
	int cap, a, b, c, d;
	if (sscanf(buf, "%d %d %d %d %d", &cap, &a, &b, &c, &d) != 5) return;

	P_mutex(g_semid);

	g_state->capacity =cap;
	g_state->a=a;
	g_state->b=b;
	g_state->c=c;
	g_state->d=d;

	V_mutex(g_semid);

	semun arg{};

	arg.val =1;
	if(semctl(g_semid,SEM_MUTEX,SETVAL,arg)== -1) die_perror("semctl mutex");

	arg.val = a;
	if(semctl(g_semid,SEM_A,SETVAL,arg)== -1)die_perror("semctl SEM_A");

	arg.val = b;
	if(semctl(g_semid,SEM_B,SETVAL,arg)== -1)die_perror("semctl SEM_B");

	arg.val = c;
	if(semctl(g_semid,SEM_C,SETVAL,arg)== -1)die_perror("semctl SEM_C");

	arg.val = d;
	if(semctl(g_semid,SEM_D,SETVAL,arg)== -1)die_perror("semctl SEM_D");

	int usedUnits = a +b +2*c + 3*d;
	int freeUnits =cap - usedUnits;

	if(freeUnits<0) freeUnits =0;

	arg.val =freeUnits;
	if(semctl(g_semid,SEM_CAPACITY,SETVAL,arg)== -1) die_perror("semctl capacity");


}

void save_state_to_file(){
	P_mutex(g_semid);
	
	// Używamy syscalli open/write/close (wymaganie 5.2)
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

// Pomocnicza funkcja do nieblokującego P (zwraca true jeśli sukces)
bool try_P(int semid, int semnum, int delta) {
    if (delta <= 0) return true;
    sembuf op{static_cast<unsigned short>(semnum), static_cast<short>(-delta), IPC_NOWAIT};
    return semop(semid, &op, 1) == 0;
}

void cleanup_ipc() {
	if (g_state && shmdt(g_state) == -1) perror("shmdt");
	
}



void process_worker_request(const WorkerRequestMessage &req){
    // Logowanie żądania do pliku
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Żądanie od stanowiska %d (pid=%d): A=%d B=%d C=%d D=%d",
                  req.workerType, req.pid, req.needA, req.needB, req.needC, req.needD);
    log_raport(g_semid, "MAGAZYN", buf);

    std::cout << "[MAGAZYN] request od PID=" << req.pid
              << " A=" << req.needA << " B=" << req.needB
              << " C=" << req.needC << " D=" << req.needD << "\n";


    bool gotA = try_P(g_semid, SEM_A, req.needA);
    if (!gotA) {
     
        WarehouseReplyMessage reply{};
        reply.mtype = req.pid;
        reply.granted = false;
        std::cout << "[MAGAZYN] brak A, odmowa dla PID=" << req.pid << "\n";
        msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0);
        return;
    }

    bool gotB = try_P(g_semid, SEM_B, req.needB);
    if (!gotB) {
        if (req.needA) V(g_semid, SEM_A, req.needA);
        WarehouseReplyMessage reply{};
        reply.mtype = req.pid;
        reply.granted = false;
        std::cout << "[MAGAZYN] brak B, odmowa dla PID=" << req.pid << "\n";
        msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0);
        return;
    }

    bool gotC = try_P(g_semid, SEM_C, req.needC);
    if (!gotC) {
        if (req.needA) V(g_semid, SEM_A, req.needA);
        if (req.needB) V(g_semid, SEM_B, req.needB);
        WarehouseReplyMessage reply{};
        reply.mtype = req.pid;
        reply.granted = false;
        std::cout << "[MAGAZYN] brak C, odmowa dla PID=" << req.pid << "\n";
        msgsnd(g_msgid, &reply, sizeof(reply) - sizeof(long), 0);
        return;
    }

    bool gotD = try_P(g_semid, SEM_D, req.needD);
    if (!gotD) {
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

    
    P_mutex(g_semid);
    g_state->a -= req.needA;
    g_state->b -= req.needB;
    g_state->c -= req.needC;
    g_state->d -= req.needD;
    V_mutex(g_semid);

    int freed = req.needA + req.needB + 2*req.needC + 3*req.needD;
    if (freed > 0) V(g_semid, SEM_CAPACITY, freed);

  
    WarehouseReplyMessage reply{};
    reply.mtype = req.pid;
    reply.granted = true;

   
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


void process_supplier_report(const SupplierReportMessage &rep){
	std::cout << "[MAGAZYN]" << rep.text << std::endl;

}



void loop(){

	while(!g_stop){


		CommandMessage cmd{};
		ssize_t r = msgrcv(g_msgid,&cmd,sizeof(cmd)-sizeof(long),static_cast<long>(MsgType::CommandBroadcast),IPC_NOWAIT);
		
		if(r>=0){
			if(cmd.cmd == Command::StopMagazyn ||cmd.cmd  == Command::StopAll) {				const char* cmdName = (cmd.cmd == Command::StopAll) ? "StopAll" : "StopMagazyn";
				char cmdbuf[64];
				std::snprintf(cmdbuf, sizeof(cmdbuf), "Odebrano %s - koncze prace", cmdName);
				log_raport(g_semid, "MAGAZYN", cmdbuf);
				g_stop = true;
				continue;
			}

		}	else if (errno !=ENOMSG){
				perror("msgrcv command");
			}


		WorkerRequestMessage req{};
		r = msgrcv(g_msgid,&req,sizeof(req)-sizeof(long),static_cast<long>(MsgType::WorkerRequest),IPC_NOWAIT);

		if(r>=0){
			process_worker_request(req);
			continue;

		} else if(errno !=ENOMSG){
			perror("msgrcv worker");

		}

		SupplierReportMessage rep{};

		r =msgrcv(g_msgid,&rep,sizeof(rep)- sizeof(long),static_cast<long>(MsgType::SupplierReport),IPC_NOWAIT);

		if(r>=0){
			process_supplier_report(rep);
			continue;
		}

		else if(errno !=ENOMSG){
			perror("msgrcv report");
		}


		sleep(1);




	}

}


}

int main (int argc, char **argv){

	int capacity = kDefaultCapacity;
	if(argc>1) capacity =std::atoi(argv[1]);

	setup_sigaction(handle_signal);

	init_ipc(capacity);

	// Logowanie startu magazynu
	char startbuf[64];
	std::snprintf(startbuf, sizeof(startbuf), "Start magazynu (capacity=%d)", capacity);
	log_raport(g_semid, "MAGAZYN", startbuf);

	// Sprawdź czy plik stanu istnieje (syscall access)
	if (access(g_stateFile.c_str(), F_OK) == 0) {
		std::cout << "[MAGAZYN] wczytuje z pliku\n";
		load_state_from_file();
		
		
		P_mutex(g_semid);
		int usedUnits = g_state->a + g_state->b + 2*g_state->c + 3*g_state->d;
		char loadbuf[128];
		std::snprintf(loadbuf, sizeof(loadbuf), 
			"Odtworzono stan z pliku: A=%d B=%d C=%d D=%d (zajetosc: %d/%d jednostek)",
			g_state->a, g_state->b, g_state->c, g_state->d, usedUnits, g_state->capacity);
		V_mutex(g_semid);
		log_raport(g_semid, "MAGAZYN", loadbuf);
	}

	loop();

	// Log przed zapisem stanu
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




