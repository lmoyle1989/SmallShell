
/***************************************************************************************
* Program: SmallShell 
* Author: Lucas Moyle
* Description: This program is a basic unix shell that demonstrates basic features of the
*	unix operating system process and signal API. The program requirements are described:
* 
* Notes: This came out pretty well, I think.
*	That said, it's a little rough around the edges and there is some inefficient stuff
*	going on in a few functions but there shouldn't be any leaks. Output formatting is a
*	a bit off sometimes, a few of the arrays should probably be dynamic, I'm not entirely
*	sure if I handled input/output redirection 'correctly' (but it works for the test script), 
*	and I'm not entirely sure my 'exit' command does everything it is supposed to.
*	The signal handlers were probably the hardest thing to get working and I still can't figure 
*	out a way to get a exit status to print when immediately when ^C SIGINT cancels a foreground 
*	process...
***************************************************************************************/

#include <sys/wait.h> 
#include <sys/types.h>
#include <stdio.h>    
#include <stdlib.h>   
#include <unistd.h>   
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>


#define MAX_ARGUMENTS 512 //this the size of the array that holds each argument string. it could be a dynamic array but nawwwwww im lazy and the assigment instructions say max 512
#define DELIMITERS " \n" //for use with strtok_r
#define PID_VAR_EXPANSION "$$" //if we wanted to replace something other than '$$' with the PID, we could do that here
#define MAX_BACKGROUND_PIDS 200 //this is the size of the array that holds all currently running background PIDs, my understaing is that the os1 machine itself has a limit at 200, so that's what we're rolling with


int foregroundOnlyMode = 0; //we have to use a global variable for foreground only mode because its tied to a signal, unfortunately

//this is our command struct that an input line will be parsed in to
struct smallshCommand {
	char* fullInput; //the full unparsed input line with $$ expanded into the PID
	char* command; //the first space separated token
	char* arguments[MAX_ARGUMENTS]; //the rest of the space separated tokens, note index [0] will be the same as command
	int argCount; //the number of non-null entries in the arguments array
	char* inputFile; //the token that immediately follows a '<' in the command line, this will not be placed in the args array
	char* outputFile; //the token that immediately follows a '>' in the command line, not placed in arg array
	int ampersand; //a bool that if true tells the program to run the command in the background
	int exitShell; //a bool that if true exits the shell
};


//this function clears any allocated memory within a command struct
//the struct itself must be free'd seperately
void freeCommandStructMembers(struct smallshCommand* input) {
	if (input != NULL) {
		free(input->fullInput);
		free(input->command);
		free(input->inputFile);
		free(input->outputFile);
		for (int i = 0; i < input->argCount; i++) {
			free(input->arguments[i]);
		}
	}
}


/*	FUNCTION: expandPidVar
this function takes a c string parameter and, if the string contains an instance of the substring "$$" it will return a pointer 
to a newly allocated c string with the "$$" substring expanded into the process ID of the calling process. if the input string 
has no instance of the substring, the function will return the input string and NOT allocate any memory. the function must be 
called an additional time for each instance of the substring.

its not very efficient because it allocates and frees memory for every instance of '$$' but it works and doesnt leak! Probably could 
be better if all the instances were counted first and a single new block of memory was created once for the whole line, also getpid() 
could be called only once in the calling function and passed to this function instead of calling it for every loop...
*/
char* expandPidVar(char* stringIn) {

	char* substring = strstr(stringIn, PID_VAR_EXPANSION);

	if (substring == NULL) {
		return stringIn;
	}

	//get our pid
	int pid = getpid();
	//find out how many chars our pid int will need when converted to string. snprintf will return the number of characters that need to be allocated in memory for the PID string when we pass NULL as the first arg
	int pidStringLen = snprintf(NULL, 0, "%d", pid);
	//allocate memory for our pid string
	char* pidString = malloc((sizeof(char) * pidStringLen) + 1);
	//convert our pid int to a string
	sprintf(pidString, "%d", pid);

	//calculate the size of our new string with the first instance of '$$' expanded into the PID, which will be equal to the size of the original string plus the length of the PID string minus the length of "$$"
	int newStringLen = strlen(stringIn) + pidStringLen - strlen(PID_VAR_EXPANSION);
	char* newString = malloc((sizeof(char) * newStringLen) + 1);
	newString[0] = '\0';

	//since substring and stringIn are pointers to the same string in different positions we use pointer arithmetic here to get the position of the first occurence of substring
	int substringPos = substring - stringIn;

	//assemble our new string
	strncat(newString, stringIn, substringPos);
	strcat(newString, pidString);
	strcat(newString, substring + strlen(PID_VAR_EXPANSION));

	free(pidString);

	return newString;
}


/*	FUNCTION: smallshGetInput
this function uses getline() to take user input from the command line. it also calls our '$$' expansion function on that input 
line before parsing otherwise. the return value will be passed to our command parsing function which will turn it into a command struct
*/
char* smallshGetInput() {
	char* inputLine;
	char* expandedInputLine;
	size_t inputLength = 0;


	if (getline(&inputLine, &inputLength, stdin) != -1) {
		//call our "$$" substring expansion function on the raw input line, if no instance of the substring is found it will simply return the input line with no extra allocated memory, and we take care of the getline allocated memory in the main shell loop function
		expandedInputLine = expandPidVar(inputLine);
		//check equivalency of the raw input against the same input run through the substring expansion function, if they're the same, no substring was found and we can move on
		//if they're different, we found and substituted and instance of the substring and we must call it consecutive times to find all instances of the substring if there are more than one
		//we use the temp pointer here to free up the memory we allocated for each successful call of the substring expansion function.
		while (strcmp(inputLine, expandedInputLine) != 0) {
			char* temp = inputLine;
			inputLine = expandedInputLine;
			expandedInputLine = expandPidVar(inputLine);
			free(temp);
		}

		return inputLine;
	}
	else {
		fflush(stdout);
		//clearerr(stdin);
		return NULL;
	}
}


/*	FUNCTION: smallshParseInput
this function parses the full command (with '$$' expanded into the PID already) into tokens which are then placed into 
their corresponding member variables in the command struct and then returns the newly allocated struct which must be free'd later
*/
struct smallshCommand* smallshParseInput(char* inputLine) {
	
	if (inputLine != NULL) {
		//create a copy of the full input line so we can store it before strtok_r manipulates the original, in case we need it for something...
		char* fullInputLine = malloc((sizeof(char) * strlen(inputLine)) + 1);
		strcpy(fullInputLine, inputLine);

		char* saveptr;
		char* token = strtok_r(inputLine, DELIMITERS, &saveptr);

		if (token == NULL) {
			//not sure if this free() is necessary but i'm pretty sure it would allocate memory for just the newline character if the command line was otherwise blank, with the way i have it setup
			free(fullInputLine);
			return NULL;
		}

		//initialize our new command struct
		struct smallshCommand* newCommand = malloc(sizeof(struct smallshCommand));
		newCommand->fullInput = fullInputLine;
		newCommand->command = NULL;
		newCommand->inputFile = NULL;
		newCommand->outputFile = NULL;
		newCommand->ampersand = 0;
		newCommand->exitShell = 0;
		newCommand->argCount = 0;
		for (int i = 0; i < MAX_ARGUMENTS; i++) {
			newCommand->arguments[i] = NULL;
		}

		//the first string token of the input line will be our command, so assign it to our 'command' member variable (ultimately this 'command' member variable is unnecessary because arguments[0] will always be identical, i just didn't realize that until after I had set up a lot of other stuff)
		newCommand->command = calloc(strlen(token) + 1, sizeof(char));
		strcpy(newCommand->command, token);
		//our command also needs to be the first element of our argument array if we're passing the command to an exec() function so also put the first token of the input line into the first index of the argument array
		newCommand->arguments[newCommand->argCount] = calloc(strlen(token) + 1, sizeof(char));
		strcpy(newCommand->arguments[newCommand->argCount], token);
		newCommand->argCount++;

		token = strtok_r(NULL, DELIMITERS, &saveptr);

		while (token != NULL) {

			//if we find < surrounded by spaces we know the next token will be our input file
			if (strcmp(token, "<") == 0) {
				token = strtok_r(NULL, DELIMITERS, &saveptr);
				newCommand->inputFile = calloc(strlen(token) + 1, sizeof(char));
				strcpy(newCommand->inputFile, token);
			}
			//if we find > we know the next token is our output file
			else if (strcmp(token, ">") == 0) {
				token = strtok_r(NULL, DELIMITERS, &saveptr);
				newCommand->outputFile = calloc(strlen(token) + 1, sizeof(char));
				strcpy(newCommand->outputFile, token);
			}
			//otherwise, we'll treat the token as an argument
			else {
				newCommand->arguments[newCommand->argCount] = calloc(strlen(token) + 1, sizeof(char));
				strcpy(newCommand->arguments[newCommand->argCount], token);
				newCommand->argCount++;
			}

			token = strtok_r(NULL, DELIMITERS, &saveptr);
		}

		//check if the last argument is an ampersand, and if the last character on the whole input line is an ampersand to determine if the command should run in background
		//if it is an ampersand, free the allocated memory for it in the argument array and remove it from the arguments list, and set the 'ampersand' member variable to true
		if ((newCommand->argCount > 1) && (newCommand->fullInput[strlen(newCommand->fullInput) - 2] == '&')) { //note that the full input line ends with a '\n' so we must subtract 2 instead of 1 to get the last array index
			if (strcmp(newCommand->arguments[newCommand->argCount - 1], "&") == 0) {
				newCommand->ampersand = 1;
				free(newCommand->arguments[newCommand->argCount - 1]);
				newCommand->arguments[newCommand->argCount - 1] = NULL;
				newCommand->argCount--;
			}
		}

		return newCommand;
	}
	else {
		return NULL;
	}
}


/*	FUNCTION: smallshCD
pretty self explanatory change directory function. we use the chdir() and pass it either the environment variable for HOME, 
or whatever the first argument was after 'cd', if there is one
*/
void smallshCD(struct smallshCommand* inputCommand) {
	
	int chdirStatus;
	
	if (inputCommand->argCount == 1) {
		chdirStatus = chdir(getenv("HOME"));
	}
	else if (inputCommand->argCount > 1) {
		chdirStatus = chdir(inputCommand->arguments[1]);
	}
	
	if (chdirStatus == -1) {
		perror("chdir");
	}
}


/*	FUNCTION: callExecForeground
this function takes our command struct as input and forks off a new process that calls an exec() function which is passed our 
command structs arguments. the child process created here runs in the foreground and thus smallsh will be blocked until the child is finished
*/
void callExecForeground(struct smallshCommand* input, int* childStatus, struct sigaction sa) {
	
	int inputFD;
	int outputFD;
	pid_t newPid = fork();

	switch(newPid) {
		case -1:
			perror("fork()\n");
			exit(1);
			break;
		case 0:
			//this child process is being run in the foreground so we want SIGINT to stop it, so we change the handler back to SIG_DFL because that will actually be inherited by exec() 
			sa.sa_handler = SIG_DFL;
			sigaction(SIGINT, &sa, NULL);

			//check for input file from the command struct. if there is one, open it and redirect stdin to the input file
			//i'm not sure if the file should be opened in the parent or the child (??), obviously dup2() must be done in the child process, but this way seems to work with the test script and makes it so if it fails the parent doesn't exit
			if (input->inputFile != NULL) {
				inputFD = open(input->inputFile, O_RDONLY);
				if (inputFD == -1) {
					perror("source open()");
					exit(1);
				}
				int result = dup2(inputFD, 0);
				if (result == -1) {
					perror("source dup2()");
					exit(1);

				}
			}
			//same as above but for an output file
			if (input->outputFile != NULL) {
				outputFD = open(input->outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
				if (outputFD == -1) {
					perror("target open()");
					exit(1);
				}
				int result = dup2(outputFD, 1);
				if (result == -1) {
					perror("target dup2()");
					exit(1);
				}
			}
			//call exec() using our command struct members
			execvp(input->arguments[0], input->arguments);
			perror(input->arguments[0]);
			exit(2);
			break;
		default:
			//since this child process is being run in the foreground, we want the childStatus to work with our 'status' function and persist to the next command, so the childStatus variable here is passed by reference and exists in the scope of the main loop
			//alternatively, childStatus could be the return value of this function
			newPid = waitpid(newPid, childStatus, 0);
			break;
	}
}


/*	FUNCTION: callExecBackground
this function is almost identical to our foreground exec function above but, uses the WNOHANG arugment when it calls waitpid() function 
to return foreground to smallsh and run the new child process in the background. it stores the background child's PID in an array 
that exists outside the scope of this function
*/
void callExecBackground(struct smallshCommand* input, pid_t* backgroundPids) {

	int status;
	int inputFD;
	int outputFD;
	pid_t newPid = fork();

	switch (newPid) {
		case -1:
			perror("fork()\n");
			exit(1);
			break;
		case 0:
			//open our input file if there is one, otherwise exit the child with status 0
			if (input->inputFile != NULL) {
				inputFD = open(input->inputFile, O_RDONLY);
				if (inputFD == -1) {
					perror("source open()");
					exit(1);
				}
				int result = dup2(inputFD, 0);
				if (result == -1) {
					perror("source dup2()");
					exit(1);
				}
			}
			//if the process is run in the background with no input file, we redirect stdin to dev/null
			else {
				inputFD = open("/dev/null", O_RDONLY);
				int result = dup2(inputFD, 0);
				if (result == -1) {
					perror("source dup2()");
					exit(1);
				}
			}
			//open our output file if there is one, otherwise exit the child with status 0
			if (input->outputFile != NULL) {
				outputFD = open(input->outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
				if (outputFD == -1) {
					perror("target open()");
					exit(1);
				}
				int result = dup2(outputFD, 1);
				if (result == -1) {
					perror("target dup2()");
					exit(1);
				}
			}
			//likewise we redirect stdout to dev/null if there is no output file specified
			else {
				outputFD = open("/dev/null", O_WRONLY);
				int result = dup2(outputFD, 1);
				if (result == -1) {
					perror("source dup2()");
					exit(1);
				}
			}
			execvp(input->arguments[0], input->arguments);
			perror(input->arguments[0]);
			exit(2);
			break;
		default:
			printf("Background process started with PID: %d\n", newPid);
			//put the PID of our new child in our array of background PIDs that will be checked everytime the shell resfreshes the command line
			for (int i = 0; i < MAX_BACKGROUND_PIDS; i++) {
				if (backgroundPids[i] == 0) {
					backgroundPids[i] = newPid;
					break;
				}
			}
			newPid = waitpid(newPid, &status, WNOHANG);
			break;
	}
}


/*	FUNCTION: smallshExecuteInput
this function interprets the command struct and decides whether to run a built-in command or pass the command to an exec() function 
with pointers to the necessary status PID variables
*/
void smallshExecuteInput(struct smallshCommand* inputCommand, int* childStatus, pid_t* backgroundPids, struct sigaction sa) {

	if (inputCommand != NULL) {
		//if our command is exit, set the exitShell var in our command struct to true 
		if (strcmp(inputCommand->command, "exit") == 0) {
			inputCommand->exitShell = 1;
			return;
		}
		//a comment line shoud effectively do nothing, so we just return out of this execute function and the shell loop will start over
		//here we check if the first character of the first token is a '#' to account for it being surrounded by spaces or just the first character of a string followed by non whitespace
		else if (inputCommand->arguments[0][0] == '#') {
			return;
		}
		//built-in cd command
		else if (strcmp(inputCommand->command, "cd") == 0) {
			smallshCD(inputCommand);
		}
		//check the status of the last foreground exec() call. childStatus exists in the scope of the main loop so we can keep track of it between commands
		else if (strcmp(inputCommand->command, "status") == 0) {
			if (WIFEXITED(*childStatus)) {
				printf("Child exit status: %d\n", WEXITSTATUS(*childStatus));
			}
			else {
				printf("Child exited abnormally due to signal: %d\n", WTERMSIG(*childStatus));
			}
		}
		//check for the ampersand member variable which will be set by our parsing function, if its true we know we want to run this command in the background
		else if ((inputCommand->ampersand == 1) && (foregroundOnlyMode != 1)) {
			callExecBackground(inputCommand, backgroundPids);
		}
		//pass all other commands & arguments to an exec() function to be called in the foreground
		else {
			callExecForeground(inputCommand, childStatus, sa);
		}
	}
}


/*	FUNCTION: checkBackgroundPids
this function gets called right before our main shell loop prints our command line prompt. it checks every PID in our 
background PID array and if the process is done (ie. when waitpid() returns the process ID of the process we're checking, 
not 0) we clear that entry in the background PID array and waitpid() clears the zombie and prints out the PID of what it just cleaned up
*/
void checkBackgroundPids(pid_t* backgroundArray) {
	int childStatus;
	pid_t childPid;
	for (int i = 0; i < MAX_BACKGROUND_PIDS; i++) {
		if (backgroundArray[i] != 0) {
			
			childPid = waitpid(backgroundArray[i], &childStatus, WNOHANG);
			
			if (childPid > 0) {
				if (WIFEXITED(childStatus)) {
					printf("Background process %d exited with status: %d\n", backgroundArray[i], WEXITSTATUS(childStatus));
				}
				else {
					printf("Background process %d exited abnormally due to signal: %d\n", backgroundArray[i], WTERMSIG(childStatus));
				}
				backgroundArray[i] = 0;
			}
		}
	}
}


/*	FUNCTION: smallshMainLoop
our main command line shell loop, basically does: 
1. check for background PIDs and kills any that are done
2. prints our command line prompt and waits for user input
3. parses user input into a struct
4. executes the struct
5. cleans up the allocated memory in the struct and then repeats
the only parameter is the signal handler for SIGINT, so we can pass it to our callExecForeground() function, this way we can 
change it when it creates a child process to its default behavior
*/
void smallshMainLoop(struct sigaction sa) {
	
	int exitShell = 0;
	char* rawLine = NULL;
	int childStatus = 0; //this is storage for the childstatus of the last foreground process that was run
	pid_t backgroundPids[MAX_BACKGROUND_PIDS]; //this array holds the PIDs of all currently running background processes
	for (int i = 0; i < MAX_BACKGROUND_PIDS; i++) {
		backgroundPids[i] = 0;
	}

	struct smallshCommand* parsedLine = NULL;

	do {
		checkBackgroundPids(backgroundPids);
		fflush(stdout);
		
		printf(": ");
		fflush(stdout);
		rawLine = smallshGetInput();
		parsedLine = smallshParseInput(rawLine);
		
		if (parsedLine != NULL) {
			smallshExecuteInput(parsedLine, &childStatus, backgroundPids, sa);
			exitShell = parsedLine->exitShell;
			freeCommandStructMembers(parsedLine);
			free(parsedLine);
		}
		
		fflush(stdout);
		free(rawLine);

	} while (!exitShell);
} 


/*	FUNCTION: handler_SIGTSTOP
this is our signal handler for SIGTSTP, it basically just toggles a global variable (I have no idea how to pass another value here) 
that when true will run all commands using callExecForeground()
*/
void handler_SIGTSTP(int signo) {

	if (foregroundOnlyMode == 0) {
		char* foregroundMessage = "\nEntering foreground-only mode (& is now ignored)\n:";
		write(1, foregroundMessage, 51);
		fflush(stdout);
		foregroundOnlyMode = 1;
	}
	else {
		char* foregroundMessage = "\nExiting foreground-only mode\n:";
		write(1, foregroundMessage, 31);
		fflush(stdout);
		foregroundOnlyMode = 0;
	}
}




int main() {

	//set up out signal handler to ignore sigint in the main shell
	struct sigaction SIGINT_action = {0};
	SIGINT_action.sa_handler = SIG_IGN;
	sigfillset(&SIGINT_action.sa_mask);
	SIGINT_action.sa_flags = SA_RESTART; //we need to set SA_RESTART here so getline() doesn't get screwed up
	sigaction(SIGINT, &SIGINT_action, NULL);

	struct sigaction SIGTSTP_action = {0};
	SIGTSTP_action.sa_handler = handler_SIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = SA_RESTART; //we need to set SA_RESTART again
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);

	smallshMainLoop(SIGINT_action);

	return EXIT_SUCCESS;
}