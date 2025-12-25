
#include "../include/common.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
	int g_msgid = -1;
	int g_semid = -1;
	int g_shmid = -1;
	WarehouseState *g_state = nullptr;
	volatile sig_atomic_t g_stop = 0;
	int g_workerType =1; // 1 albo 2

	void check_command();

	void handle_signal(int){g_stop = 1;}

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


		   if (msgsnd(g_msgid, &req, sizeof(req) - sizeof(long), 0) == -1) {
        perror("msgsnd worker request");
        return;
    }


    WarehouseReplyMessage rep{};
    while (!g_stop) {
        ssize_t r = msgrcv(g_msgid, &rep, sizeof(rep) - sizeof(long), req.pid, IPC_NOWAIT);

        if (r >= 0) {
            std::cout << "[PRACOWNIK] got reply: pid=" << getpid()
                      << " granted=" << rep.granted << "\n";
            break;
        }

        if (errno == ENOMSG) {
            check_command();
            usleep(100000);
            continue;
        }
        if (errno == EINTR) {
            if (g_stop) return;
            continue;
        }

        perror("msgrcv worker reply");
        return;
    }

    if(!rep.granted){
        std::cerr << "Brak surowcow dla pracownika, czekam...\n";
        usleep(1000000 + (rand() % 1000000));
        return;
    }

    // Logowanie do pliku
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Stanowisko %d wyprodukowano czekolade (pid=%d)", 
                  g_workerType, getpid());
    log_raport(g_semid, "STANOWISKO", buf);

    std::cout << "Pracownik " << g_workerType << " Produkuje czekolade ...\n";
    int delay = (rand() % 2) + 1;
    sleep(delay);
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

	srand(static_cast<unsigned>(time(nullptr)) ^ getpid());
	setup_sigaction(handle_signal);

	attach_ipc();

	while(!g_stop){

		request_and_produce();
		check_command();


	}
	if(g_state && shmdt(g_state)==-1) perror("shmdt");




	return 0;
}