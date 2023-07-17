#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/msg.h>
#define MAX_LETTERS 512
#define PIPE_NAME   "TASK_PIPE"
struct tm *data_hora_atual;

int max_queue;
int max_wait;
int edge_server_number;
int pid_system_manager;

struct server{
    char server_name[128];
    int mips_vcpus[2];
    bool vcpu_ocupado[2];
    char performance[32];
    int tarefas_exec;
    int ops_manutencao;
};
struct task{
    int task_id;
    int num_inst;
    int time_max;
    int vcpu_escolhido;
    time_t begin;
};
struct no_fila {
    struct task tar;
    int prioridade;
    struct no_fila* tarefa_seguinte;
};

struct messages
{
    long msgtype;
    int recv_msgtype;
    char mens[MAX_LETTERS];
} ;

struct  info{
    sem_t *mutex;
    bool acabar_prog;
    bool pronto_a_ler[124];
    int tam_fila_tarefas;
    bool flag_monitor;
    long total_tempo;
	int tarefas_apagadas;
    bool ativar_dispatcher;
    pthread_cond_t cond_dispatcher;
    pthread_mutex_t mutex_dispatcher;
    pthread_cond_t cond_edge_server;
    pthread_mutex_t mutex_edge_server;
    pthread_cond_t cond_monitor;
    pthread_mutex_t mutex_monitor;
};
struct cpu_info{
    int m;
    int ind_cpu;
    int server_n;
};
int shmid;
struct info * shared_var;
struct server * servers;

sem_t * mutex_shared_mem;
struct no_fila * fila_tarefas;
struct task tarefa_a_executar;
int ** unamed_fd;
int fd_fila;
int id_fila;


pthread_mutex_t scheduler_mutex = PTHREAD_MUTEX_INITIALIZER; // protects condition
pthread_cond_t scheduler_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t cpu_mutex = PTHREAD_MUTEX_INITIALIZER; // protects condition
pthread_cond_t cpu_cond = PTHREAD_COND_INITIALIZER;

bool ativar_scheduler;

void printer(char *str);

void stats(){
    int tarefas_totais=0;
    for(int i=0;i<edge_server_number;i++){
        tarefas_totais+=servers[i].tarefas_exec;
    }
    printf("Número total de tarefas executas = %d\n",tarefas_totais);
    printf("Tempo médio por tarefa = %ld\n",(shared_var->total_tempo/tarefas_totais));
    for(int i=0;i<edge_server_number;i++){
        printf("Número de tarefas executadas no server %d=%d\n",i,servers[i].tarefas_exec);

    }
    for(int i=0;i<edge_server_number;i++){
        printf("Número de operacoes de manutencao no server %d=%d\n",i,servers[i].ops_manutencao);
    }
    printf("Número de tarefas apagadas=%d\n",shared_var->tarefas_apagadas);

}

void sigint(int signum) {
	if (getpid() == pid_system_manager){
        printer("SIGNAL SIGINT RECEIVED\n");
    int fd;
    if ((fd=open(PIPE_NAME, O_WRONLY)) < 0)
    {
        perror("Cannot open pipe for writing:\n ");
        exit(0);
    }
    shared_var->acabar_prog=true;
    write(fd,"EXIT", sizeof("EXIT"));
    }
}

void sigstp(int signum){
	if (getpid() == pid_system_manager){
        printer("SIGNAL SIGSTP RECEIVED\n");
        stats();
    }
}

bool colocar(struct no_fila *FILA, struct task tarefa);
struct no_fila * retira(struct no_fila *FILA);
void exibe(struct no_fila *FILA);
void apaga_fila(struct no_fila *FILA);
void* maintenance_reader(void*server_n);
void printer(char *str){
    sem_wait(shared_var->mutex);
    FILE *plog;
    time_t segundos;
    time(&segundos);
    data_hora_atual = localtime(&segundos);
    plog=fopen("log.txt","a");
    fprintf(plog,"%d:%d:%d %s",data_hora_atual->tm_hour,data_hora_atual->tm_min,data_hora_atual->tm_sec,str);
    printf("%d:%d:%d %s",data_hora_atual->tm_hour,data_hora_atual->tm_min,data_hora_atual->tm_sec,str);
    fclose(plog);
    sem_post(shared_var->mutex);
}

void * vcpu(void * n) {
    char mensagem[MAX_LETTERS];
    struct cpu_info cpuInfo= *(struct cpu_info *) n;
    int inst=0;

    while (!shared_var->acabar_prog) {
        pthread_mutex_lock(&cpu_mutex);
        while (tarefa_a_executar.vcpu_escolhido!=cpuInfo.m && ((strcmp(servers[cpuInfo.server_n].performance, "Stopped") == 0) || !shared_var->acabar_prog)) {
        	if((servers[cpuInfo.server_n].mips_vcpus[0]==cpuInfo.m || shared_var->flag_monitor) && (strcmp(servers[cpuInfo.server_n].performance, "Stopped") != 0)){
                pthread_mutex_lock(&shared_var->mutex_dispatcher);
                shared_var->ativar_dispatcher=true;
                pthread_cond_signal(&shared_var->cond_dispatcher);
                pthread_mutex_unlock(&shared_var->mutex_dispatcher);
            }
            pthread_cond_wait(&cpu_cond, &cpu_mutex);
        }

        	if (!shared_var->acabar_prog) {
            servers[cpuInfo.server_n].vcpu_ocupado[cpuInfo.ind_cpu] = true;
            inst = tarefa_a_executar.num_inst;
            tarefa_a_executar.vcpu_escolhido = 0;
            pthread_mutex_unlock(&cpu_mutex);
            sleep(inst / cpuInfo.m);
            sprintf(mensagem,"%s: TASK %d COMPLETED\n",servers[cpuInfo.server_n].server_name,tarefa_a_executar.task_id);
            printer(mensagem);
            servers[cpuInfo.server_n].vcpu_ocupado[cpuInfo.ind_cpu] = false;
            if (shared_var->acabar_prog) {
                pthread_mutex_unlock(&cpu_mutex);
                break;
            }
			}
			pthread_mutex_unlock(&cpu_mutex);

    }

    pthread_exit(NULL);
}
void edge_server(int num_server){
    char server_name[1024];
    struct cpu_info cpu_1,cpu_2;
    cpu_1.m=servers[num_server].mips_vcpus[0];
    cpu_1.server_n=num_server;
    cpu_1.ind_cpu=0;
    char mensagem[MAX_LETTERS];
    pthread_t vCPU1;
    pthread_t vCPU2;
    pthread_t maintenance;
    pthread_create(&maintenance, NULL, maintenance_reader, (void *) &num_server);
    pthread_create(&vCPU1, NULL,vcpu,(void *)&cpu_1);
    cpu_2.m=servers[num_server].mips_vcpus[1];
    cpu_2.server_n=num_server;
    cpu_2.ind_cpu=1;
    pthread_create(&vCPU2, NULL,vcpu,(void *)&cpu_2);
    close(unamed_fd[num_server][1]);
    sprintf(server_name,"%s READ\n",servers[num_server].server_name);
    printer(server_name);
    while (1) {
        pthread_mutex_lock(&shared_var->mutex_edge_server);
        while(!shared_var->acabar_prog && !shared_var->pronto_a_ler[num_server]){
            if (shared_var->flag_monitor && strcmp(servers[num_server].performance,"Normal")==0){
                strcpy(servers[num_server].performance,"High");
                sprintf(mensagem,"%s PERFORMANCE: %s\n",servers[num_server].server_name,servers[num_server].performance);
                printer(mensagem);
                pthread_cond_broadcast(&cpu_cond);
            }
            if(!shared_var->flag_monitor && strcmp(servers[num_server].performance,"High")==0){
                strcpy(servers[num_server].performance,"Normal");
                sprintf(mensagem,"%s PERFORMANCE: %s\n",servers[num_server].server_name,servers[num_server].performance);
                printer(mensagem);
            }
            pthread_cond_wait(&shared_var->cond_edge_server,&shared_var->mutex_edge_server);
        }

        if (shared_var->acabar_prog){
            pthread_mutex_unlock(&shared_var->mutex_edge_server);
            break;
        }else {
            pthread_mutex_lock(&cpu_mutex);
            read(unamed_fd[num_server][0], &tarefa_a_executar, sizeof(struct task));
            servers[num_server].tarefas_exec += 1;
            shared_var->pronto_a_ler[num_server]=false;
            pthread_cond_broadcast(&cpu_cond);
            pthread_mutex_unlock(&cpu_mutex);
            pthread_mutex_unlock(&shared_var->mutex_edge_server);
        }
    }
    struct messages mensagem_ter;
    mensagem_ter.msgtype=404;
    msgsnd(id_fila, &mensagem_ter, sizeof(struct messages), 0);
    pthread_kill(maintenance,SIGTERM);
    pthread_mutex_lock(&cpu_mutex);
    pthread_cond_broadcast(&cpu_cond);
    pthread_mutex_unlock(&cpu_mutex);
    pthread_join(vCPU1, NULL);
    pthread_join(vCPU2, NULL);
    pthread_join(maintenance,NULL);
    close(unamed_fd[num_server][0]);
}
void * scheduler();
void * dispatcher();

void task_manager(int val){
    printer("PROCESS TASK MANAGER CREATED\n");
    fila_tarefas = (struct no_fila *) malloc(sizeof(struct no_fila));
    if(!fila_tarefas){
        printf("Sem memoria disponivel!\n");
        exit(1);
    }else {
        fila_tarefas->tarefa_seguinte = NULL;
    }
    unamed_fd= (int **)malloc(sizeof(int*)*edge_server_number);
    if(unamed_fd==NULL) {
        printf("Sem memoria disponivel!\n");
        exit(1);
    }
    for(int f=0; f < edge_server_number; f++){
        unamed_fd[f]=(int*)malloc(sizeof(int)*2);
        if(unamed_fd[f]==NULL) {
            printf("Sem memoria disponivel!\n");
            exit(1);
        }
    }
    for(int e=0; e < edge_server_number;e++){
        pipe(unamed_fd[e]);
    }

    pthread_t t_scheduler, t_dispatcher;

    pthread_create(&t_scheduler, NULL,scheduler, NULL);
    pthread_create(&t_dispatcher,NULL,dispatcher,NULL);

    for(int i=0;i<val;i++){
        if (fork() == 0){
            edge_server(i);
            exit(0);
        }
    }

    for(int e=0; e < edge_server_number;e++){
        close(unamed_fd[e][0]);
    }
    int fd;
    if ((fd=open(PIPE_NAME, O_RDWR)) < 0)
    {
        perror("Cannot open pipe for reading: ");
        exit(0);
    }

    int r;
    struct task tarefa;
    char mens_pipe[1024];
    while(!shared_var->acabar_prog){
        strcpy(mens_pipe,"");
        r=read(fd, mens_pipe, sizeof(mens_pipe));
        mens_pipe[r-1]=0;
        char mensagem[1024];
        if (r<0) perror("Named Pipe");
        pthread_mutex_lock(&scheduler_mutex);
        if(strcmp(mens_pipe,"EXIT")== 0){
        	printf("yfyfy");
            shared_var->acabar_prog=true;
        }
        else if(strcmp(mens_pipe,"STATS")== 0){
            stats();
        }
        else {
            char*token=NULL;
            token= strtok(mens_pipe," ");
            tarefa.task_id= atoi(token);
            token = strtok(NULL, " ");
            tarefa.num_inst= atoi(token)*1000;
            token = strtok(NULL, " ");
            tarefa.time_max=atoi(token);
            tarefa.begin = time(NULL);
            tarefa.vcpu_escolhido=0;

            if (!colocar(fila_tarefas, tarefa)) {
                sprintf(mensagem, "TASK_MANAGER: TASK %d DELETED\n", tarefa.task_id);
                printer(mensagem);
                shared_var->tarefas_apagadas+=1;
            }
        }
        ativar_scheduler = true;
        pthread_cond_signal(&scheduler_cond);
        pthread_mutex_unlock(&scheduler_mutex);
    }

    pthread_join(t_scheduler, NULL);
    pthread_join(t_dispatcher, NULL);
    printer("SIMULATOR WAITING FOR LAST TASKS TO FINISH\n");
    for (int e=0; e<val; e++) wait(NULL);
    for(int b=0; b < edge_server_number;b++){
        free(unamed_fd[b]);
    }
    free(unamed_fd);
    apaga_fila(fila_tarefas);
    free(fila_tarefas);
    stats();
}
void * scheduler() {
    while(!shared_var->acabar_prog){
        pthread_mutex_lock(&scheduler_mutex);
        ativar_scheduler= false;
        while(!ativar_scheduler){
            pthread_cond_wait(&scheduler_cond,&scheduler_mutex);
        }
        if(!shared_var->acabar_prog){
            //Node current will point to head
            struct no_fila *current = fila_tarefas->tarefa_seguinte, *index = NULL;
            struct task temp;


            if(fila_tarefas == NULL) {
                return current;
            }
            else {
                while(current != NULL) {

                    index = current->tarefa_seguinte;

                    while(index != NULL) {
                        if(index->tar.time_max < current->tar.time_max) {
                            temp = current->tar;
                            current->tar = index->tar;
                            index->tar = temp;
                        }
                        index = index->tarefa_seguinte;
                    }
                    current = current->tarefa_seguinte;
                }
            }
            bool verifica_servers = false;
            for (int ind = 0; ind < edge_server_number;ind++){
                if ((strcmp(servers[ind].performance, "Stopped")) == 0){
                    continue;
                }else if (!servers[ind].vcpu_ocupado[0] && (strcmp(servers[ind].performance, "Normal")==0)){
                    verifica_servers = true;
                }else if((!servers[ind].vcpu_ocupado[0] || !servers[ind].vcpu_ocupado[1]) && strcmp(servers[ind].performance, "High") == 0){
                    verifica_servers = true;
                }
            }
            if(verifica_servers) {
                pthread_mutex_lock(&shared_var->mutex_dispatcher);
                shared_var->ativar_dispatcher=true;
                pthread_cond_signal(&shared_var->cond_dispatcher);
                pthread_mutex_unlock(&shared_var->mutex_dispatcher);
            }
            pthread_mutex_unlock(&scheduler_mutex);
        }
    }
    pthread_mutex_lock(&shared_var->mutex_dispatcher);
    shared_var->ativar_dispatcher=true;
    pthread_cond_signal(&shared_var->cond_dispatcher);
    pthread_mutex_unlock(&shared_var->mutex_dispatcher);
    pthread_exit(NULL);
}
void monitor(){
    printer("PROCESS MONITOR CREATED\n");
    while (!shared_var->acabar_prog){
        pthread_mutex_lock(&shared_var->mutex_monitor);
        while ((shared_var->tam_fila_tarefas <= 0.8*max_queue || shared_var->flag_monitor ) && !shared_var->acabar_prog ){
            if(shared_var->tam_fila_tarefas < 0.2* max_queue) {
                shared_var->flag_monitor = false;
            }
            pthread_cond_wait(&shared_var->cond_monitor,&shared_var->mutex_monitor);
        }
        shared_var->flag_monitor=true;
        pthread_mutex_unlock(&shared_var->mutex_monitor);
    }
}

void* maintenance_reader(void* server_n){
    char mensagem[MAX_LETTERS];
    int server_number = *(int *) server_n + 1;
    struct messages read;
    struct messages comeca;
    while (1) {
        msgrcv(id_fila, &read, sizeof(struct messages), server_number, 0);
        comeca.msgtype = read.recv_msgtype;
        if (shared_var->acabar_prog) {
            strcpy(comeca.mens, "TERMINAR");
            msgsnd(id_fila, &comeca, sizeof(struct messages), 0);
            break;
        }else {
            strcpy(comeca.mens, "O SERVIDOR ESTA PRONTO PARA INTERVENCAO");
            msgsnd(id_fila, &comeca, sizeof(struct messages), 0);
            strcpy(servers[*(int *) server_n].performance, "Stopped");
            sprintf(mensagem,"O %s ENTROU EM MANUTENCAO\n",servers[*(int *) server_n].server_name);
            printer(mensagem);
            sprintf(mensagem,"%s PERFORMANCE: %s\n",servers[*(int *) server_n].server_name,servers[*(int *) server_n].performance);
            printer(mensagem);
            msgrcv(id_fila, &read, sizeof(struct messages), server_number, 0);
            if(!shared_var->flag_monitor){
            strcpy(servers[*(int *) server_n].performance, "Normal");
            sprintf(mensagem,"%s PERFORMANCE: %s\n",servers[*(int *) server_n].server_name,servers[*(int *) server_n].performance);
            printer(mensagem);}
            else{strcpy(servers[*(int *) server_n].performance, "High");
            sprintf(mensagem,"%s PERFORMANCE: %s\n",servers[*(int *) server_n].server_name,servers[*(int *) server_n].performance);
            printer(mensagem);}

        }
    }
    pthread_exit(NULL);
}



void * maintenance_thread(void * my_msgtype){
	struct messages ms;
    struct messages inter;
    srand(time(NULL));
    strcpy(ms.mens,"O SERVIDOR VAI ENTRAR EM MANUTENCAO\n");
    ms.recv_msgtype=*(int *)my_msgtype;
    while(1){
        sleep(1+ rand()% 5);
        int server_number=1+rand()% (edge_server_number);
        ms.msgtype = server_number;
        msgsnd(id_fila,&ms,sizeof(struct messages),0);
        msgrcv(id_fila,&inter,sizeof(struct messages),*(int*)my_msgtype,0);
        if(strcmp(inter.mens,"TERMINAR")==0){
            break;
        }else {
            sleep(1 + rand() % 5);
            servers[server_number - 1].ops_manutencao += 1;
            strcpy(ms.mens, "O SERVIDOR PODE REENTRAR EM ATIVIDADE\n");
            msgsnd(id_fila, &ms, sizeof(struct messages), 0);
        }
		}
	pthread_exit(NULL);
}

void maintenance_manager(){
    printer("PROCESS MAINTENANCE MANAGER CREATED\n");
    pthread_t threads[edge_server_number-1];
    int id[edge_server_number-1];

    for(int i=0;i<edge_server_number-1;i++){
    	id[i]= i+edge_server_number+1;
    	printf("%d\n",id[i]);
    	pthread_create(&threads[i],NULL,maintenance_thread,(void*)&id[i]);
    }

    struct messages term;
    msgrcv(id_fila,&term,sizeof(struct messages),404,0);
    pthread_kill(threads[0],SIGTERM);
    pthread_kill(threads[1],SIGTERM);

    }
int main(int argc, char *argv[]) {
	pid_system_manager=getpid();

	// Redirects SIGINT to sigint()
    signal(SIGINT, sigint);
    signal(SIGTSTP,sigstp);
    signal(SIGHUP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    
    pthread_mutexattr_t attrmutex;
    pthread_condattr_t attrcondv;
    (id_fila = msgget(IPC_PRIVATE, IPC_CREAT|0700));
    if (argc !=2 ) {
        printf("{ficheiro de configuração}\n");
        exit(-1);
    }
    FILE *fptr;
    fptr = fopen(argv[1],"r");
    if(fptr == NULL)
    {
        printf("Error!");
        exit(1);
    }

    size_t len=0;
    char * line=NULL;

    getline(&line,&len,fptr);
    max_queue= atoi(line);

    getline(&line,&len,fptr);
    max_wait= atoi(line);

    getline(&line,&len,fptr);
    edge_server_number=atoi(line);


    // Create shared memory
    if ((shmid = shmget(IPC_PRIVATE, sizeof(struct info )+sizeof(struct server)*edge_server_number, IPC_CREAT | 0766)) < 0){
        perror("Error in shmget with IPC_CREAT\n");
        exit(1);
    }

    // Attach shared memory
    if ((shared_var = (struct info *) shmat(shmid, NULL, 0)) == (struct info *) - 1){
        perror("Shmat error!");
        exit(1);
    }
    servers = (struct server*)(shared_var+1);
    for(int j=0;j<edge_server_number;j++){
        getline(&line,&len,fptr);
        char*token= strtok(line,",");
        strcpy(servers[j].server_name,token);
        token = strtok(NULL, ",");
        servers[j].mips_vcpus[0]= atoi(token)*1000000;
        token = strtok(NULL, ",");
        servers[j].mips_vcpus[1]= atoi(token)*1000000;
        servers[j].vcpu_ocupado[0]= false;
        servers[j].vcpu_ocupado[1]= false;
        strcpy(servers[j].performance,"Normal");
    }
    fclose(fptr);
    // Create semaphore
    sem_unlink("MUTEX");
    shared_var->mutex = sem_open("MUTEX", O_CREAT | O_EXCL, 0700,1);

    sem_unlink("MUTEX_MEM");
    mutex_shared_mem = sem_open("MUTEX_MEM",O_CREAT|O_EXCL,0700,1);
    printer("OFFLOAD SIMULATOR STARTING\n");
    for (int s =0; s<edge_server_number;s++){
        shared_var->pronto_a_ler[s]=false;
    }
    shared_var->acabar_prog=false;
    shared_var->tam_fila_tarefas=0;
    shared_var->flag_monitor=false;
    shared_var->ativar_dispatcher=false;
    // Creates the named pipe if it doesn't exist yet
    if ((mkfifo(PIPE_NAME, O_CREAT|O_EXCL|0600)<0) && (errno!= EEXIST))
    {
        perror("Cannot create pie: ");
        exit(0);
    }

    /* Initialize attribute of mutex. */
    pthread_mutexattr_init(&attrmutex);
    pthread_mutexattr_setpshared(&attrmutex, PTHREAD_PROCESS_SHARED);

    /* Initialize attribute of condition variable. */
    pthread_condattr_init(&attrcondv);
    pthread_condattr_setpshared(&attrcondv, PTHREAD_PROCESS_SHARED);

    /* Initialize mutex. */
    pthread_mutex_init(&shared_var->mutex_dispatcher, &attrmutex);
    pthread_mutex_init(&shared_var->mutex_edge_server, &attrmutex);
    pthread_mutex_init(&shared_var->mutex_monitor, &attrmutex);

    /* Initialize condition variables. */
    pthread_cond_init(&shared_var->cond_dispatcher, &attrcondv);
    pthread_cond_init(&shared_var->cond_edge_server, &attrcondv);
    pthread_cond_init(&shared_var->cond_monitor, &attrcondv);

  

    if (fork() == 0){
        task_manager(edge_server_number);
        exit(0);
    }
    if (fork() == 0){
        monitor();
        exit(0);
    }
    if (fork() == 0){
        maintenance_manager();
        exit(0);
    }

    for (int i=0; i<3; i++) wait(NULL);
    printer("OFFLOAD CLOSING\n");
    sem_close(shared_var->mutex);
    sem_close(mutex_shared_mem);
    sem_unlink("MUTEX");
    sem_unlink("MUTEX_MEM");
    pthread_mutex_destroy(&scheduler_mutex);
    pthread_cond_destroy(&scheduler_cond);
    pthread_mutex_destroy(&shared_var->mutex_dispatcher);
    pthread_mutexattr_destroy(&attrmutex);
    pthread_cond_destroy(&shared_var->cond_dispatcher);
    pthread_condattr_destroy(&attrcondv);
    msgctl(id_fila, IPC_RMID, 0);
    unlink("TASK_PIPE");
    shmdt(shared_var);
    shmctl(shmid, IPC_RMID, NULL);
}

bool colocar(struct no_fila *FILA, struct task tarefa){
    struct no_fila *novo= (struct no_fila *) malloc(sizeof(struct no_fila));
    novo->tarefa_seguinte = NULL;
    novo->tar = tarefa;
    novo->prioridade =0;
    if(FILA->tarefa_seguinte == NULL){
        FILA->tarefa_seguinte=novo;
        shared_var->tam_fila_tarefas +=1;
        return true;
    }
    else{
        struct no_fila *tmp = FILA->tarefa_seguinte;

        while(tmp->tarefa_seguinte != NULL){
            tmp = tmp->tarefa_seguinte;
        }
        if (shared_var->tam_fila_tarefas <= max_queue){
            tmp->tarefa_seguinte = novo;
            sem_wait(mutex_shared_mem);
            shared_var->tam_fila_tarefas +=1;
            sem_post(mutex_shared_mem);
            return true;
        }else{
            return false;
        }
    }
}
void exibe(struct no_fila *FILA)
{
    if(FILA->tarefa_seguinte == NULL){
        printf("Fila vazia!\n\n");
        return ;
    }

    struct no_fila *tmp;
    tmp = FILA->tarefa_seguinte;
    printf("Fila :");
    while( tmp != NULL){
        tmp = tmp->tarefa_seguinte;
    }

}

void apaga_fila(struct no_fila *FILA){
    if(FILA->tarefa_seguinte!=NULL){
        char mensagem[1024];
        struct no_fila *proxNode,*atual;

        atual = FILA->tarefa_seguinte;
        while(atual != NULL){
            mensagem[0]=0;
            proxNode = atual->tarefa_seguinte;
            sprintf(mensagem,"TASK_MANAGER: TASK %d DELETED\n",atual->tar.task_id);
            printer(mensagem);
            shared_var->tarefas_apagadas +=1;
            free(atual);
            sem_wait(mutex_shared_mem);
            shared_var->tam_fila_tarefas --;
            sem_post(mutex_shared_mem);
            atual = proxNode;
        }
    }
}

void * dispatcher(){

    char mensagem[1024];
    int indice_server=-1;
    struct no_fila * tarefa_a_enviar;
    while (!shared_var->acabar_prog) {
        pthread_mutex_lock(&shared_var->mutex_dispatcher);
        if (indice_server >= 0) {
            shared_var->ativar_dispatcher = false;
        }
        while ( ((shared_var->tam_fila_tarefas == 0) || !shared_var->ativar_dispatcher ) && !shared_var->acabar_prog) {
            pthread_cond_wait(&shared_var->cond_dispatcher, &shared_var->mutex_dispatcher);
        }
        indice_server=-1;
        if(shared_var->tam_fila_tarefas < 0.2* max_queue){
            pthread_mutex_lock(&shared_var->mutex_monitor);
            pthread_cond_signal(&shared_var->cond_monitor);
            pthread_mutex_unlock(&shared_var->mutex_monitor);
        }
        tarefa_a_enviar = retira(fila_tarefas);
        if(!shared_var->acabar_prog) {
            long time_espera=(time(NULL) - tarefa_a_enviar->tar.begin);
            shared_var->total_tempo += time_espera;
            for (int i = 0; i < edge_server_number; i++) {
                if (strcmp(servers[i].performance, "Stopped") == 0) {
                    continue;
                } else if (strcmp(servers[i].performance, "Normal") == 0) {
                    if (!servers[i].vcpu_ocupado[0] &&
                        (tarefa_a_enviar->tar.time_max - (time(NULL) - tarefa_a_enviar->tar.begin)) >=
                        (tarefa_a_enviar->tar.num_inst / servers[i].mips_vcpus[0])) {
                        indice_server = i;
                        tarefa_a_enviar->tar.vcpu_escolhido = servers[i].mips_vcpus[0];
                        break;
                    }
                } else {
                    if (!servers[i].vcpu_ocupado[0] &&
                        (tarefa_a_enviar->tar.time_max - (time(NULL) - tarefa_a_enviar->tar.begin)) >=
                        (tarefa_a_enviar->tar.num_inst / servers[i].mips_vcpus[0])) {
                        indice_server = i;
                        tarefa_a_enviar->tar.vcpu_escolhido = servers[i].mips_vcpus[0];
                        break;
                    } else if (!servers[i].vcpu_ocupado[1] &&
                               (tarefa_a_enviar->tar.time_max - (time(NULL) - tarefa_a_enviar->tar.begin)) >=
                               (tarefa_a_enviar->tar.num_inst / servers[i].mips_vcpus[1])) {
                        indice_server = i;
                        tarefa_a_enviar->tar.vcpu_escolhido = servers[i].mips_vcpus[1];
                        break;
                    }
                }
            }
            if (indice_server == -1) {
                sprintf(mensagem, "TASK_MANAGER: TASK %d DELETED\n", tarefa_a_enviar->tar.task_id);
                printer(mensagem);
                shared_var->tarefas_apagadas+=1;
            } else {
                if(time(NULL)-tarefa_a_enviar->tar.begin >= max_wait){
                    pthread_mutex_lock(&shared_var->mutex_monitor);
                    pthread_cond_signal(&shared_var->cond_monitor);
                    pthread_mutex_unlock(&shared_var->mutex_monitor);
                }
                pthread_mutex_lock(&shared_var->mutex_edge_server);
                shared_var->pronto_a_ler[indice_server] = true;
                sprintf(mensagem, "DISPATCHER: TASK %d SELECTED FOR EXECUTION ON %s\n", tarefa_a_enviar->tar.task_id,servers[indice_server].server_name);
                printer(mensagem);
                write(unamed_fd[indice_server][1], &tarefa_a_enviar->tar, sizeof(struct task));
                pthread_cond_broadcast(&shared_var->cond_edge_server);
                pthread_mutex_unlock(&shared_var->mutex_edge_server);
            }

            free(tarefa_a_enviar);
            pthread_mutex_unlock(&shared_var->mutex_dispatcher);
        }

    }
    pthread_mutex_lock(&shared_var->mutex_monitor);
    pthread_cond_signal(&shared_var->cond_monitor);
    pthread_mutex_unlock(&shared_var->mutex_monitor);

    pthread_mutex_lock(&shared_var->mutex_edge_server);
    pthread_cond_broadcast(&shared_var->cond_edge_server);
    pthread_mutex_unlock(&shared_var->mutex_edge_server);
    pthread_exit(NULL);
}

struct no_fila * retira(struct no_fila *FILA){
    if(FILA->tarefa_seguinte == NULL){
        return NULL;
    }else{
        struct no_fila *tmp = FILA->tarefa_seguinte;
        FILA->tarefa_seguinte = tmp->tarefa_seguinte;
        sem_wait(mutex_shared_mem);
        shared_var->tam_fila_tarefas -=1;
        sem_post(mutex_shared_mem);
        return tmp;
    }
}
