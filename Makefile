all:
	gcc -Wall -pthread systemmanager_1.c -o offload_simulator
	gcc -Wall mobilenode.c -o mobilenode
clean:
	rm -f offload_simulator
	rm -f mobilenode