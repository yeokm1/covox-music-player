#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/io.h>
#include <sndfile.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <termios.h>
#include <stdbool.h>
#include <ieee1284.h>

#define MESSAGE_INITIAL "\nCovox Music Player, Copyright 2017 Yeo Kheng Meng, MIT License\nSource Code: https://github.com/yeokm1/covox-music-player\n"

#define ERROR_CODE_WRONG_ARG 1
#define ERROR_CODE_CANNOT_OPEN_FILE 2
#define ERROR_CODE_PARALLEL_ADDRESS 3
#define ERROR_CODE_PARALLEL_PERMISSION 4
#define ERROR_CODE_PARALLEL_PORTS_DETECTION 5
#define ERROR_CODE_PARALLEL_PORTS_NOT_FOUND 6
#define ERROR_CODE_IEEE1284_UNKNOWN_ERROR 7

#define COMMAND_FFMPEG_FORMAT_MAX_LENGTH 1000

#define FILENAME_WAV_CONVERT "/tmp/covox-wav-convert.wav"

//Store and force overwrite converted wav file in tmp directory as libsndfile can only process wav files
#define FORMAT_COMMAND_FFMPEG "ffmpeg -loglevel error -y -i %s /tmp/covox-wav-convert.wav"

#define CODE_SPACEBAR ' '
#define CODE_ESCAPE 27

const char * formatDurationStr (double seconds);
const char * generateDurationStr (SF_INFO *sfinfo);
const char * getFilenameExtension(const char *filename);
uint8_t mapShortTo8bit(short input);
long long getCurrentNanoseconds();
void *playbackThreadFunction(void *inputPtr);
void setUnblockKeyboard(bool newState);
int logIeee1284ResultAndDetermineExitCode(enum E1284 errorCode);

bool pausePlayback = false;
bool endPlayback = false;

struct parport *selectedPort = NULL;

short * dataBuffer;
int totalFramesToPlay;

long long nanosecondsPerFrame;
long long startSpecTime;
long long currentSpecTime;
long long timeSinceStart;
long long pauseTime;

int currentFrameNumber;
int previousFrameNumber;
int channels;

long framesSkippedCumulativeUIThread = 0;
long framesSkippedCumulativePlaybackThread = 0;

int main(int argc, char *argv[]){

	struct parport_list parports = {};

	enum E1284 parportDetectionResult = ieee1284_find_ports(&parports, 0);

	if (parportDetectionResult != E1284_OK) {
		printf("Parallel port detection failed. ");
		return logIeee1284ResultAndDetermineExitCode(parportDetectionResult);
	}

	if (parports.portc < 1) {
		printf("This system doesn't appear to have any parallel ports.\n");
		return ERROR_CODE_PARALLEL_PORTS_NOT_FOUND;
	}

	if(argc < 3){
		printf("Insufficient arguments: Require music file and first parallel port address eg: ./linux-covox-player 0x378 file.mp3\n");
		return ERROR_CODE_WRONG_ARG;
	}

	puts(MESSAGE_INITIAL);

	char * parallelPortAddressStr = argv[1];
	char * filename = argv[2];
	const char * fileExtension = getFilenameExtension(filename);

	printf("Parallel ports detected on this system:\n");
	for (int i = 0; i < parports.portc; i++) {
		printf(" * Address/name: %s", parports.portv[i]->name);
		if (strcmp(parallelPortAddressStr, parports.portv[i]->name) == 0) {
			printf(" (selected)");
			selectedPort = parports.portv[i];
		}
		printf("\n");
	}
	printf("\n");

	if (selectedPort == NULL) {
		printf("None of the parallel ports on this system have an address or name \"%s\".\n"
			"Please pick one from the list of detected ports above.\n", parallelPortAddressStr);
		return ERROR_CODE_PARALLEL_ADDRESS;
	}

	//remove the previous temp file to avoid playing back this file should the ffmpeg conversion fail
	remove(FILENAME_WAV_CONVERT);

	//If file does not have wav extension, call FFMPEG to convert it to wav before proceeding
	if(strcmp(fileExtension, "wav") != 0){
		printf("File is not wav, converting to wav using FFMPEG\n");

		char conversionCommand[COMMAND_FFMPEG_FORMAT_MAX_LENGTH];

		//Generate conversion command
		snprintf(conversionCommand, COMMAND_FFMPEG_FORMAT_MAX_LENGTH, FORMAT_COMMAND_FFMPEG, filename);
		system(conversionCommand);

		filename = FILENAME_WAV_CONVERT;

		printf("FFMPEG program ended\n");
	}



	printf("Attempting to open parallel port at %s\n", parallelPortAddressStr);

	int desiredCapabilities = CAP1284_RAW;
	enum E1284 parPortOpenResult = ieee1284_open(selectedPort, F1284_EXCL, &desiredCapabilities);
	if (parPortOpenResult != E1284_OK) {
		printf("Could not open parallel port %s. ", selectedPort->name);
		printf("Do you have root privileges? ");
		return logIeee1284ResultAndDetermineExitCode(parPortOpenResult);
	}

	// After opening the selected port, we no longer need the list of ports.
	// See https://linux.die.net/man/3/libieee1284
	ieee1284_free_ports(&parports);

	enum E1284 parPortClaimResult = ieee1284_claim(selectedPort);
	if (parPortClaimResult != E1284_OK) {
		printf("Could not claim parallel port %s.", selectedPort->name);
		printf("Do you have root privileges? ");
		return logIeee1284ResultAndDetermineExitCode(parPortClaimResult);
	}

	printf("Attempting to play file %s to port at %s\n", filename, parallelPortAddressStr);


	//Open file using lsndfile API
	SF_INFO sfinfo;
	memset (&sfinfo, 0, sizeof (sfinfo)) ;

	SNDFILE* soundFile = sf_open(filename, SFM_READ, &sfinfo);

	if(soundFile == NULL){

		int errorCode = sf_error(soundFile) ;

		switch(errorCode){
			case SF_ERR_NO_ERROR:
			printf("No error huh?\n");
			break;
			case SF_ERR_UNRECOGNISED_FORMAT:
			printf("Unrecognised file format\n");
			break;
			case SF_ERR_SYSTEM:
			printf("System error, probably a missing file\n");
			break;
			case SF_ERR_MALFORMED_FILE:
			printf("Malformed File\n");
			break;
			case SF_ERR_UNSUPPORTED_ENCODING:
			printf("Unsupported encoding\n");
			break;
			default:
			printf("Unknown error code from libsnd library\n");
		}

		return ERROR_CODE_CANNOT_OPEN_FILE;
	}


	setUnblockKeyboard(true);


	printf("\nFile details:\n");

	int sampleRate = sfinfo.samplerate;

	printf ("Sample Rate : %d\n", sampleRate) ;

	long frames = sfinfo.frames;

	if (frames == SF_COUNT_MAX){
		printf ("Frames      : unknown\n") ;
	} else {
		printf ("Frames      : %ld\n", frames) ;
	}

	channels = sfinfo.channels;

	printf ("Channels    : %d\n", channels) ;
	printf ("Format      : 0x%08X\n", sfinfo.format) ;
	printf ("Sections    : %d\n", sfinfo.sections) ;
	printf ("Seekable    : %s\n", (sfinfo.seekable ? "TRUE" : "FALSE")) ;
	printf ("Duration    : %s\n", generateDurationStr (&sfinfo)) ;

	int totalItems = frames * channels;

	dataBuffer = (short *) malloc(totalItems * sizeof(short));
	totalFramesToPlay = sf_readf_short (soundFile, dataBuffer, frames);
	sf_close(soundFile);

	printf("Total Frames Read from file: %d\n\n", totalFramesToPlay);

	printf("Press spacebar to pause, Escape to exit\n\n");


	//Calculate how many nanoseconds to play each frame
	nanosecondsPerFrame = 1E9 / sampleRate;

	startSpecTime = getCurrentNanoseconds();
	currentFrameNumber = 0;
	previousFrameNumber = 0;


	pthread_t playBackThread;
	pthread_create(&playBackThread, NULL, playbackThreadFunction, NULL);

  while(true){
		usleep(100000);

		if(!pausePlayback){

			if(currentFrameNumber >= totalFramesToPlay){
				break;
			}

			//Multiply 1.0 is to convert to double
			double secondsPlayed = 1.0 * currentFrameNumber / sampleRate;

			const char * currentPlayTime = formatDurationStr(secondsPlayed);

			int framesSkipped = 0;

			if(framesSkippedCumulativePlaybackThread != framesSkippedCumulativeUIThread){
				framesSkipped = framesSkippedCumulativePlaybackThread - framesSkippedCumulativeUIThread;
				framesSkippedCumulativeUIThread = framesSkippedCumulativePlaybackThread;
			}

			printf("\rPosition: %s, framesSkipped: %04d", currentPlayTime, framesSkipped);

			if(framesSkipped > 0){
				printf("\n");
			}
		}

		int readChar = getchar();

		if(readChar == CODE_SPACEBAR){

			if(pausePlayback){
				//We have to unpause here

				long long currentTime = getCurrentNanoseconds();

				//We add the time we spent on pause to the time since start so the playback thread will be able to pace itself
				long long timeOnPause = currentTime - pauseTime;
				startSpecTime += timeOnPause;

				pausePlayback = false;
			} else {
				//We need to pause here
				pausePlayback = true;
				pauseTime = getCurrentNanoseconds();
				printf("\nPaused: Press space to resume, Esc to end");
			}


		} else if(readChar == CODE_ESCAPE){
			endPlayback = true;
			break;
		}

		fflush(stdout);


	}


	pthread_join(playBackThread, NULL);

	free(dataBuffer);

	//Revert parallelPort state to all 0
	ieee1284_write_data(selectedPort, 0);

	printf("\n");

	setUnblockKeyboard(false);

	//Take away permissions to access port
	enum E1284 parPortClosureResult = ieee1284_close(selectedPort);
	if (parPortClosureResult != E1284_OK) {
		printf("Could not close parallel port %s.", selectedPort->name);
		printf("Do you have root privileges? ");
		return logIeee1284ResultAndDetermineExitCode(parPortClosureResult);
	}

	return 0;
	}

void *playbackThreadFunction(void *inputPtr){
	while(true){

		if(endPlayback){
			break;
		}

		if(pausePlayback){
			usleep(10000);
			continue;
		}

		currentSpecTime = getCurrentNanoseconds();
		timeSinceStart = currentSpecTime - startSpecTime;
		currentFrameNumber = timeSinceStart / nanosecondsPerFrame;

		if(currentFrameNumber >= totalFramesToPlay){
			break;
		}

		//Only accumulate the skipping if the difference is greater than the usual 1 increment
		if((currentFrameNumber - previousFrameNumber) > 1){
				framesSkippedCumulativePlaybackThread += currentFrameNumber - previousFrameNumber - 1;
		}

		previousFrameNumber = currentFrameNumber;

		//Average values to merge all channels to mono
		int sum = 0;
		for (int m = 0 ; m < 1 ; m++) {
			sum += dataBuffer[currentFrameNumber * channels + m];
		}

		short value = sum / channels;


		uint8_t smallValue = mapShortTo8bit(value);
		ieee1284_write_data(selectedPort, smallValue);
	}

	return NULL;

}

const char * formatDurationStr (double seconds){
	static char str [128] ;
	int hrs, min ;
	double sec ;

	memset (str, 0, sizeof (str)) ;

	hrs = (int) (seconds / 3600.0) ;
	min = (int) ((seconds - (hrs * 3600.0)) / 60.0) ;
	sec = seconds - (hrs * 3600.0) - (min * 60.0) ;

	snprintf (str, sizeof (str) - 1, "%02d:%02d:%04.1f", hrs, min, sec) ;

	return str ;
}


const char * generateDurationStr (SF_INFO *sfinfo){
	double seconds ;

	if (sfinfo->samplerate < 1)
	return NULL ;

	if (sfinfo->frames / sfinfo->samplerate > 0x7FFFFFFF)
	return "unknown" ;

	seconds = (1.0 * sfinfo->frames) / sfinfo->samplerate ;


	return formatDurationStr(seconds) ;
}

//From: http://stackoverflow.com/questions/5309471/getting-file-extension-in-c
const char * getFilenameExtension(const char *filename) {
	const char *dot = strrchr(filename, '.');
	if(!dot || dot == filename) return "";
	return dot + 1;
}

uint8_t mapShortTo8bit(short input){
	double slope = 1.0 * (UINT8_MAX - 0) / (SHRT_MAX - SHRT_MIN);
	uint8_t output = 0 + slope * (input -  SHRT_MIN);
	return output;
}

long long getCurrentNanoseconds(){
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);
	long long specTime = (spec.tv_sec * 1E9) + spec.tv_nsec;
	return specTime;
}

//Source https://gist.github.com/whyrusleeping/3983293
void setUnblockKeyboard(bool newState){
	static struct termios initialSettings;

	if(newState){
		tcgetattr(0, &initialSettings);

		struct termios newSettings;

		//Disable delay on getchar
		newSettings = initialSettings;
		newSettings.c_lflag &= ~ICANON;
		newSettings.c_lflag &= ~ECHO;
		newSettings.c_lflag &= ~ISIG;
		newSettings.c_cc[VMIN] = 0;
		newSettings.c_cc[VTIME] = 0;

		tcsetattr(0, TCSANOW, &newSettings);
	} else {
		tcsetattr(0, TCSANOW, &initialSettings);
	}


}

int logIeee1284ResultAndDetermineExitCode(enum E1284 errorCode) {
	printf("IEEE1284: ");
	switch (errorCode) {
		case E1284_OK:
			printf("Everything went fine\n");
			return 0;
		case E1284_NOTIMPL:
			printf("Not implemented in libieee1284\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_NOTAVAIL:
			printf("Not available on this system\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_TIMEDOUT:
			printf("Operation timed out\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_REJECTED:
			printf("IEEE 1284 negotiation rejected\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_NEGFAILED:
			printf("Negotiation went wrong\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_NOMEM:
			printf("No memory left\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_INIT:
			printf("Error initialising port\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_SYS:
			printf("Error interfacing system\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_NOID:
			printf("No IEEE 1284 ID available\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		case E1284_INVALIDPORT:
			printf("Invalid port\n");
			return ERROR_CODE_PARALLEL_PORTS_DETECTION;
		default:
			printf("Unknown error\n");
			return ERROR_CODE_IEEE1284_UNKNOWN_ERROR;
	}
}
