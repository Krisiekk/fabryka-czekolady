#include "../include/common.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

    std::vector<pid_t> g_children;

    int g_msgid = -1;
    int g_semid = -1;
    int g_shmid = -1;
    volatile sig_atomic_t g_stop = 0;

    void handle_signal(int) { g_stop = 1; }

    void die_exec(const char *what){
        perror(what);
        _exit(EXIT_FAILURE);


    }

    pid_t spawn(const std::vector<std::string> &args){

        pid_t pid = fork();
        if(pid<0) die_exec("fork");
        if(pid ==0){

            std::vector<char*> cargs;
            cargs.reserve(args.size()+1);

            for(const auto &s : args){
                cargs.push_back(const_cast<char*>(s.c_str()));
            }
            cargs.push_back(nullptr);
            execv(args[0].c_str(),cargs.data());
            die_exec(args[0].c_str());
        }
        g_children.push_back(pid);
        return pid;


    }

    key_t make_key(){
        key_t key = ftok(kIpcKeyPath,kProjId);
        if(key == -1) die_exec("ftok");
        return key;

    }

    void attach_ipc(){
        key_t key = make_key();
        g_msgid = msgget(key,0600);
        if(g_msgid == -1) die_exec("msgget");
        g_semid = semget(key,SEM_COUNT,0600);
        if(g_semid == -1) die_exec("semget");
        g_shmid = shmget(key,sizeof(WarehouseState),0600);
        if(g_shmid == -1) die_exec("shmget");

    }

    void send_command(Command cmd){
        CommandMessage msg{};
        msg.mtype = static_cast<long>(MsgType::CommandBroadcast);
        msg.cmd =cmd;
        if(msgsnd(g_msgid,&msg,sizeof(msg)-sizeof(long),0)==-1){
            perror("msgsnd command");
        }
    }

    void send_command_to_all(Command cmd, int count) {
    for (int i = 0; i < count; ++i) {
        send_command(cmd);
    }
}

    void remove_ipcs(){
        if(g_msgid!=-1)msgctl(g_msgid,IPC_RMID,nullptr);
        if(g_semid!=-1)semctl(g_semid,0,IPC_RMID);
        if(g_shmid!=-1)shmctl(g_shmid,IPC_RMID,nullptr);


    }

    void wait_children(){
        for(pid_t pid : g_children){
            if(pid<=0) continue;
            int status =0;
            waitpid(pid,&status,0);

        }


    }

    void start_processes(int capacity){
        spawn({"./magazyn",std::to_string(capacity)});

        sleep(1);

        spawn({"./dostawca","A"});
        spawn({"./dostawca","B"});
        spawn({"./dostawca","C"});
        spawn({"./dostawca","D"});

        spawn({"./stanowisko","1"});
        spawn({"./stanowisko","2"});


    }

    void menu_loop(){
        std::cout<<"Polecenie dyrektora:  \n";
        char choice;

        while(true){
            std::cout<< ">  ";
            std::cin >> choice;

            if(!std::cin) break;

            if (choice == '1'){
                send_command_to_all(Command::StopFabryka,2);

            }

            else if (choice =='2'){
                send_command_to_all(Command::StopMagazyn,1);

            }

            else if (choice == '3'){

                send_command_to_all(Command::StopDostawcy,4);
            }

            else if (choice == '4'){
                send_command_to_all(Command::StopAll, 7);
                break;
            }

            else if (choice == 'q' || choice == 'Q'){
                break;
            }

            else{
                std::cout<<"Nieznana opcja"<<std::endl;
            }

        }

    }


}

int main( int argc, char **argv){
    int capacity = kDefaultCapacity;
    if(argc >1) capacity =std::atoi(argv[1]);
    if(capacity<=0) capacity =kDefaultCapacity;

    setup_sigaction(handle_signal);

    start_processes(capacity);

    attach_ipc();

    menu_loop();

    send_command(Command::StopAll);
    sleep(1); // daj czas na odbiór polecenia

    // Awaryjne zakończenie, jeśli któryś proces wisi (np. blokujące msgrcv).
    for (pid_t pid : g_children) {
        if (pid > 0) kill(pid, SIGTERM);
    }

    wait_children();
    remove_ipcs();



    return 0;
}
