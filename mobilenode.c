#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#define PIPE_NAME   "TASK_PIPE"

int main(int argc, char *argv[]) {
    if (argc !=5) {
        printf("mobile_node {nº pedidos a gerar} {intervalo entre pedidos em ms} "
               "{milhares de instruções de cada pedido} {tempo máximo para execução}\n");
        exit(-1);
    }
    char *ptr;
    int pedidos=(strtol(argv[1],&ptr,10));
    int segundos=(strtol(argv[2],&ptr,10));
    //int instrucoes=(strtol(argv[3],&ptr,10));
    //int tempo_maximo=(strtol(argv[4],&ptr,10));
    // Opens the pipe for writing
    int fd;
    char tarefa[1024];
    if ((fd=open(PIPE_NAME, O_WRONLY)) < 0)
    {
        perror("Cannot open pipe for writing: ");
        exit(0);
    }
    for(int i=0;i<pedidos;i++) {
    	tarefa[0]=0;
        sprintf(tarefa,"%d %s %s /0",i,argv[3],argv[4]);
        write(fd,tarefa, sizeof(tarefa));
        usleep(1000*segundos);
    }
}
