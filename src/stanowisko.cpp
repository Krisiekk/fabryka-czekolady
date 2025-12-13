
#include "../include/common.h"


#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
	int g_msgid = -1;
	int g_semid = -1;
	int g_shmid = -1;
	WarehouseState *g_state = nullptr;
	bool g_stop = false;
	int g_workerType =1; // 1 albo 2

	void handle_signal(int){g_stop = true;}

	key_t make_key(){
		key_t key = ftok(kIpcKeyPath,kProjId);
		if(key == -1) die_perror("ftok");
		return key;

	}

	void attach_ipc(){
		key_t key =make_key();
		g_shmid =shmget(key,sizeof(WarehouseState),0600);
		if(g_shmid == -1) die_perror("shmget");
		g_state = static_cast<WarehouseState*>(shmat(g_shmid,nullptr,0));
		if(g_state ==reinterpret_cast<void*>(-1))die_perror("shmat");
		g_semid= semget(key,SEM_COUNT,0600);
		if(g_semid == -1) die_perror("semget");
		g_msgid =msgget(key,0600);
		if(g_msgid == -1) die_perror("msgget");



	}

	void request_and_produce(){
		WorkerRequestMessage req{};
		req.mtype = static_cast<long>(MsgType::WorkerRequest);
		req.workerType = g_workerType;
		req.pid = getpid();
		if(g_workerType ==1){
			req.needA = 1;
			req.needB = 1;
			req.needC = 1;
			req.needD = 0;


		}
		else{
			req.needA = 1;
			req.needB = 1;
			req.needC = 0;
			req.needD = 1;	


		}
		if(msgsnd(g_msgid,&req,sizeof(req)- sizeof(long),0)== -1){
			perror("msgsnd worker request");
			return;
		}

		WarehouseReplyMessage rep{};

		ssize_t r = msgrcv(g_msgid,&rep, sizeof(rep)- sizeof(long), static_cast<long>(MsgType::WarehouseReply),0);
		if (r == -1){
			perror("msgrcv worker replay");
			return;

		}
		if(!rep.granted){
			std::cerr<<"Brak surowcow dla pracownika \n";
			sleep(1);
			return;

		}

		std::cout<<"Pracownik"<<g_workerType<<"Produkuje czekolade ... "<<std::endl;
		sleep(1);




	}

	void check_command() {
    CommandMessage cmd{};
    ssize_t r = msgrcv(
        g_msgid,
        &cmd,
        sizeof(cmd) - sizeof(long),
        static_cast<long>(MsgType::CommandBroadcast),
        IPC_NOWAIT
    );

    if (r == -1) {
        if (errno == ENOMSG) return;
        perror("msgrcv command");
        return;
    }

    if (cmd.cmd == Command::StopFabryka || cmd.cmd == Command::StopAll) {
        g_stop = true;
    }
}




}

int main (int argc, char **argv){

	if(argc<2){
		std::cerr<<"Uzycie stanowisko <1/2> \n";
		return 1;
	}

	g_workerType = std::atoi(argv[1]);
	if(g_workerType !=1 && g_workerType !=2 ) g_workerType=1;

	std::signal(SIGTERM, handle_signal);
	std::signal(SIGINT, handle_signal);

	attach_ipc();

	while(!g_stop){

		request_and_produce();
		check_command();


	}
	if(g_state && shmdt(g_state)==-1) perror("shmdt");




	return 0;
}