#include "../include/common.h"

#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
int g_msgid = -1;
int g_semid = -1;
int g_shmid = -1;
WarehouseState *g_state = nullptr;
bool g_stop = false;
bool g_fresh = false;

std::string g_stateFile = "magazyn_state.txt";

union semun{
	int val;
	struct semid_ds *buf;
	unsigned short *array;
	struct seminfo *__buf;

};

void handle_signal (int) {g_stop = true;}

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

	}


}

void load_state_from_file(){
	std::ifstream in(g_stateFile);
	if(!in) return;
	int cap, a, b, c, d;
	in>>cap>>a>>b>>c>>d;
	if(!in) return;

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
	std::ofstream out(g_stateFile, std::ios::trunc);
	if(out){
		out << g_state->capacity << ' ' << g_state->a << ' ' << g_state->b << ' ' << g_state->c << ' ' << g_state->d << "\n";

	};
	V_mutex(g_semid);



}

void cleanup_ipc() {
	if (g_state && shmdt(g_state) == -1) perror("shmdt");
	
}

void process_worker_request(const WorkerRequestMessage &req){
	WarehouseReplyMessage reply{};
	reply.mtype = req.pid;
	reply.granted =false;

	P(g_semid,SEM_A,req.needA);
	P(g_semid,SEM_B,req.needB);
	P(g_semid,SEM_C,req.needC);
	P(g_semid,SEM_D,req.needD);

	P_mutex(g_semid);
	
	g_state->a -= req.needA;
	g_state->b -= req.needB;
	g_state->c -= req.needC;
	g_state->d -= req.needD;
	V_mutex(g_semid);

	int freed = req.needA + req.needB + 2*req.needC + 3*req.needD;

	V(g_semid,SEM_CAPACITY,freed);

	reply.granted =true;

	if(msgsnd(g_msgid,&reply,sizeof(reply)-sizeof(long),0)== -1) perror("msgsend reply");



}

void process_supplier_report(const SupplierReportMessage &rep){
	std::cout<<"[MAGAZYN]"<<rep.text<<std::endl;


}

void add_supply(char type, int amount){
	int units =0;
	int semIndex = SEM_A;
	
	switch (type)
	{
		case 'A' : units = amount *1; semIndex = SEM_A; break;
		case 'B' : units = amount *1; semIndex = SEM_B; break;
		case 'C' : units = amount *2; semIndex = SEM_C; break;
		case 'D' : units = amount *3; semIndex = SEM_D; break;
		default  : return;

	}

	P(g_semid, SEM_CAPACITY, units);

	P_mutex(g_semid);
	if(type == 'A') g_state->a +=amount;
	if(type == 'B') g_state->b +=amount;
	if(type == 'C') g_state->c +=amount;
	if(type == 'D') g_state->d +=amount;
	V_mutex(g_semid);

	V(g_semid,semIndex,amount);


}

void loop(){

	while(!g_stop){
		CommandMessage cmd{};
		ssize_t r = msgrcv(g_msgid,&cmd,sizeof(cmd)-sizeof(long),static_cast<long>(MsgType::CommandBroadcast),IPC_NOWAIT);
		
		if(r>=0){
			if(cmd.cmd == Command::StopMagazyn ||cmd.cmd  == Command::StopAll) {
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

	std::signal(SIGTERM, handle_signal);
	std::signal(SIGINT,handle_signal);

	init_ipc(capacity);
	if(g_fresh){
	load_state_from_file();
	}

	loop();

	save_state_to_file();

	cleanup_ipc();




	return 0;
}




