
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>

#define ERROR_CODE_WRONG_ARG 1
#define ERROR_CODE_CANNOT_OPEN_FILE 2
#define ERROR_CODE_PARALLEL_ADDRESS 3
#define ERROR_CODE_PARALLEL_PERMISSION 4

int main(int argc, char *argv[]){

	if(argc < 3){
		printf("Insufficient arguments: Require first parallel port address, value to write\n");
		return ERROR_CODE_WRONG_ARG;
	}

	char * parallelPortAddressStr = argv[1];
	char * valueToWriteStr = argv[2];

	printf("Attempting to open parallel port at %s\n", parallelPortAddressStr);

	int parallelPortBaseAddress = (int)strtol(parallelPortAddressStr, NULL, 0);
	int valueToWrite = (int)strtol(valueToWriteStr, NULL, 0);

	//Unable to convert given address to hex number
	if(parallelPortBaseAddress == 0L){
		printf("Invalid parallel port base address.\n");
		return ERROR_CODE_PARALLEL_ADDRESS;
	}

	if(ioperm(parallelPortBaseAddress, 8, 1) == -1)	{ //Set permissions to access port
		printf("Cannot open parallel port address. Do you have root privileges?\n");
		return ERROR_CODE_PARALLEL_PERMISSION;
	}

	printf("Attempting to write %d to %s\n", valueToWrite, parallelPortAddressStr);

	outb(valueToWrite, parallelPortBaseAddress);

	//Take away permissions to access port
	if (ioperm(parallelPortBaseAddress, 8, 0)) {
		printf("Error closing parallel port\n");
		return ERROR_CODE_PARALLEL_ADDRESS;
	}

	return 0;
}
