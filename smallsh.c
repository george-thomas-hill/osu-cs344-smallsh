// smallsh
// By George Hill
// CS 344
// 2020-05-10

// Implements a simple bash-like shell with support for (a) three built-in
// commands (status, cd, and exit), (b) file redirection (with < and >),
// (c) background processes (with &), and (d) otherwise generally calling
// GNU/Linux executables. Ignores Ctrl-C and interprets Ctrl-Z as toggling
// on and off a "foreground-only" mode in which "&" is ignored.

// 80 Columns: /////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define TRUE 1
#define FALSE 0

#define MAX_STRING_LENGTH 2048
#define MAX_COMMAND_ARRAY_SIZE 518
// We will have an array of strings. It will have one string for the command,
// 512 strings for arguments, four strings for redirection symbols and their
// files, and one final string for commands that should be run in the background.
// That adds up to 518 strings.
#define MAX_DIGITS_IN_PROCESS_ID 10 // This is a guess.
#define STATUS_REPORT_MAX_LENGTH 100

#define EXIT_VALUE 1
#define SIGNAL_RECEIVED 0
// The statusType variable in main() will track how the last foreground process
// terminated. If it exited normally, the statusType variable will have the
// value EXIT_VALUE. If it exited because of a signal, it will have the value
// SIGNAL_RECEIVED.

#define COMMAND_AND_ARGUMENT_DELIMITER " " // Must use double-quotes.
// We will use this to separate commands and arguments from each other.

#define PROCESS_NUMBER_SYMBOL '$' // Must use single-quotes because used with
                                  // character comparison.
// We will use this to search for "$$" in the user's input.

#define COMMENT_SYMBOL '#' // Must use single-quotes.
// We will use this to identify comment lines.

#define BACKGROUND_SYMBOL "&" // Must use double-quotes because used with
                              // strcmp().
// We will use this to identify commands that need to run in the background.

#define REDIRECT_INPUT "<" // Must use double-quotes.
#define REDIRECT_OUTPUT ">" // Must use double-quotes.
// We will use these to identify commands that need file redirection.

#define DEV_NULL "/dev/null"
// We will use this with certain background processes.

#define EXIT_COMMAND "exit"
#define STATUS_COMMAND "status"
#define CD_COMMAND "cd"
// These are the three built-in commands.

struct runningProcess // Will store process IDs of running proceses in a
                      // linked list.
{
    int processID;
    struct runningProcess* next;
};

int usingBackgroundIsPossible = TRUE;
int receivedSigtstp = FALSE;
int weAreWaitingForForegroundProcessToStop = FALSE;
// As far as I can tell, we need to use global variables so that the shell
// can catch SIGTSTP signal _during execution of a foreground process_ and make
// a plan to act on that signal only after the termination of the foreground
// process. That is to say, I don't know that we have any other way to pass the
// function variables to manipulate.

// The next two functions will be used throughout the rest of the program
// to safely generate output:

void outputStringWithNoNewline(char* text)
{
    printf("%s", text);
    fflush(stdout);
}

void outputStringWithANewline(char* text)
{
    printf("%s\n", text);
    fflush(stdout);
}

// This function toggles our state between the normal mode and the foreground-
// only mode:
void implementSigtstpLogic()
{
    if (usingBackgroundIsPossible == TRUE)
    {
        usingBackgroundIsPossible = FALSE;
        char* message =
            "\nEntering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, message, 50);
    }
    else
    {
        usingBackgroundIsPossible = TRUE;
        char* message = "\nExiting foreground-only mode\n";
        write(STDOUT_FILENO, message, 30);
    }
}

// This function will be called when our (parent) shell process receives
// a SIGTSTP:
void dealWithSigtstp(int signo)
{
    if (weAreWaitingForForegroundProcessToStop == FALSE)
    {
        // We aren't blocked at waitpid(), so we are (probably) at the
        // command-line prompt, so we should act on SIGTSTP right away.
        implementSigtstpLogic();
    }
    else
    {
        // We _are_ blocked at waitpid(), so we hold off on doing anything
        // about SIGTSTP until the foreground process terminates. However,
        // in order to do that, we need to update this global variable so
        // that the work can be done when we are done being blocked at
        // waitpid().
        receivedSigtstp = TRUE;
    }
    return;
}

// This function removes the given pid from the linked list that contains
// the pid's of background processes. As noted below, writing it was hard.
void forget(int processID, struct runningProcess** listOfProcesses)
{
    struct runningProcess* current = *listOfProcesses;
    // By dereferencing the pointer to a pointer, we end up with "current"
    // being a pointer that has the same value as "listOfProcesses" in main().
    // I think.
    struct runningProcess* previous = NULL;

    // Iterate over the linked list until locating the link that contains the
    // pid that we seek:
    while (current->processID != processID)
    {
        previous = current;
        current = current->next;
    }

    if (previous != NULL)
    {
        // Remove "current" from the linked list:
        previous->next = current->next;
    }
    else
    {
        // "previous" is still NULL, which means that we found the pid in the
        // very first link, which means that we need to set the head of the
        // linked list to point to the second element in the list (or to NULL,
        // which will be the value of "current->next" if there is no second
        // link).
        *listOfProcesses = current->next;
    }

    free(current);

    return;
}

// This function checks to see if the process with the given pid has finished.
// If it has, this function calls another function to remove that pid from the
// linked list of background processes. As noted below, writing this was
// difficult.
void checkStatusOfProcess
(
    int processID,
    struct runningProcess** listOfProcesses
)
{
    // printf("checkStatusOfProcess(%d)\n", processID);

    int exitedOrNot = -5;
    int childExitMethod = -5;
    int statusValue = -5;
    char statusReport[STATUS_REPORT_MAX_LENGTH];

    exitedOrNot = waitpid(processID, &childExitMethod, WNOHANG);

    if (exitedOrNot == 0) {
        return;
    }

    if (WIFEXITED(childExitMethod) != 0)
    {
        // The process exited by exit(0), exit(1), return 0, etc.
        statusValue = WEXITSTATUS(childExitMethod);
        sprintf
        (
            statusReport,
            "background pid %d is done: exit value %d",
            processID,
            statusValue
        );
        outputStringWithANewline(statusReport);
        forget(processID, listOfProcesses);
    }
    else if (WIFSIGNALED(childExitMethod) != 0)
    {
        // The process exited because of an uncaught signal.
        statusValue = WTERMSIG(childExitMethod);
        sprintf
        (
            statusReport,
            "background pid %d is done: terminated by signal %d",
            processID,
            statusValue
        );
        outputStringWithANewline(statusReport);
        forget(processID, listOfProcesses);
    }

    return;
}

// This function iterates over a linked list of process IDs from background
// processes. For each process ID encountered, it calls a separate function
// to see if that process has exited yet. Because this is a linked list using
// pointers to pointers, writing it was hard, and so I'm leaving in the
// printf() calls used for debugging (although they are commented out).
void checkForFinishedBackgroundProcesses
(
    struct runningProcess** listOfProcesses
)
{
    struct runningProcess* current = *listOfProcesses;
    // By dereferencing the pointer to a pointer, we end up with "current"
    // being a pointer that has the same value as "listOfProcesses" in main().
    // I think.
    struct runningProcess* temp = NULL;

    // printf("checkForFinishedBackgroundProcesses, %p\n", *listOfProcesses);

    // Iterate over each element in the linked list. If there are no links in
    // the list, then "current" will equal NULL and we won't look at anything.
    // If there are links in the list, then "current" will equal NULL when it
    // finally reaches the last link.
    while (current != NULL)
    {
        temp = current;
        current = current->next; // This has to be before the next line because
                                 // the next line might lead to a forget() call.
        // printf("current: %p\n", current);
        checkStatusOfProcess(temp->processID, listOfProcesses);
    }

    return;
}

// Output prompt, get line of user input, and parse each word of that input
// into an array, noting the total number of array elements used:
void getCommandArray
(
    char commandArray[MAX_COMMAND_ARRAY_SIZE][MAX_STRING_LENGTH],
    int* arrayElementsUsed
)
{
    // Sample code for using getline was provided by the instructor at:
    // http://web.engr.oregonstate.edu/~brewsteb/CS344Slides/2.4%20File%20Access%20in%20C.pdf
    // And at:
    // http://web.engr.oregonstate.edu/~brewsteb/THCodeRepository/userinput_adv.c

    int numCharsEntered = -5; // Will hold the number of characters entered.
    size_t bufferSize = 0; // Will hold how large the allocated buffer is.
    char* lineEntered = NULL; // Will point to a buffer allocated by getline()
                              // that holds the entered string + \n + \0.

    while(TRUE)
    {
        outputStringWithNoNewline(": "); // Output prompt.

        numCharsEntered = getline(&lineEntered, &bufferSize, stdin);
        // Get a line from the user.

        if (numCharsEntered == -1)
        {
            // We got an error, probably because someone sent a SIGTSTP.
            clearerr(stdin);
        }
        else
        {
            break;
        }
    }

    lineEntered[numCharsEntered - 1] = 0; // Turn ending \n into a \0.

    char* token = NULL;
    int index = 0;

    token = strtok(lineEntered, COMMAND_AND_ARGUMENT_DELIMITER);
    while (token != NULL)
    {
        strcpy(commandArray[index], token);
        index++;
        token = strtok(NULL, COMMAND_AND_ARGUMENT_DELIMITER);
    }

    free(lineEntered);

    *arrayElementsUsed = index;
    return;
}

// Replace each instance of "$$" with the process ID:
void replaceDoubleDollarSigns
(
    char commandArray[MAX_COMMAND_ARRAY_SIZE][MAX_STRING_LENGTH],
    int arrayElementsUsed
)
{
    // Iterate over each word in the array:
    int i;
    for (i = 0; i < arrayElementsUsed; i++) {

        int currentWordHasMadeItThrough = FALSE;

        // The current world will have "made it through" when it can go through
        // the entire inner iterator without triggering a substitution from
        // "$$" to "<pid>". It's not enough to run it through once, because
        // we might have multiple "$$" substrings. After hitting a "$$"
        // substring, we'll break out of the inner iterator and recheck the
        // string from the very top.
        while (currentWordHasMadeItThrough == FALSE)
        {
            int wordLength = strlen(commandArray[i]);
            int weMadeAChange = FALSE;

            // Iterate over each character in the current word:
            int j;
            for (j = 0; j < wordLength - 1; j++)
            {
                char firstChar = commandArray[i][j];
                char secondChar = commandArray[i][j + 1];

                if (firstChar == PROCESS_NUMBER_SYMBOL &&
                    secondChar == PROCESS_NUMBER_SYMBOL)
                {
                    // We hit a "$$" substring starting at character j.

                    char tempWord[MAX_STRING_LENGTH + MAX_DIGITS_IN_PROCESS_ID];

                    // Copy in the characters that come before the first "$":
                    int k;
                    for (k = 0; k < j; k++)
                    {
                        tempWord[k] = commandArray[i][k];
                    }

                    // Get the process id into a string:

                    // https://stackoverflow.com/questions/53230155/converting-pid-t-to-string

                    int pid = getpid();
                    char tempPidString[MAX_DIGITS_IN_PROCESS_ID];
                    sprintf(tempPidString, "%d", pid);

                    // Iterate over the process ID string, copying it in to the
                    // temporary word:
                    int tempPidStringLength = strlen(tempPidString);
                    for (k = 0; k < tempPidStringLength; k++)
                    {
                        tempWord[j + k] = tempPidString[k];
                    }

                    // Copy in the characters that come after the "$$"
                    // substring.
                    int m = 0;
                    for (k = j + 2; k < wordLength; k++)
                    {
                        tempWord[j + tempPidStringLength + m] =
                            commandArray[i][k];
                        m++;
                    }

                    // Make sure to mark the end of the temporary string:
                    tempWord[j + tempPidStringLength + m] = 0;

                    strcpy(commandArray[i], tempWord);

                    weMadeAChange = TRUE;

                    break; // Break out of the for-j loop.
                }
            }

            if (weMadeAChange == FALSE)
            {
                currentWordHasMadeItThrough = TRUE;
            }
        }
    }

    return;
}

// This function helps implemnent the "exit" built-in command. It needs to
// terminate any background processes. This function is similar to
// checkForFinishedBackgroundProcesses().
void prepForExit(struct runningProcess** listOfProcesses)
{
    struct runningProcess* current = *listOfProcesses;
    // By dereferencing the pointer to a pointer, we end up with "current"
    // being a pointer that has the same value as "listOfProcesses" in main().
    // I think.
    struct runningProcess* temp = NULL;

    // Iterate over each element in the linked list. If there are no links in
    // the list, then "current" will equal NULL and we won't look at anything.
    // If there are links in the list, then "current" will equal NULL when it
    // finally reaches the last link.
    while (current != NULL)
    {
        temp = current;
        current = current->next;
        kill(temp->processID, SIGKILL);

        // It seems to me that we should output a message noting that the
        // background process was terminated, so I am including the following:

        int childExitMethod;
        int statusValue;
        char statusReport[STATUS_REPORT_MAX_LENGTH];

        waitpid(temp->processID, &childExitMethod, 0);

        if (WIFEXITED(childExitMethod) != 0)
        {
            // The process exited by exit(0), exit(1), return 0, etc.
            statusValue = WEXITSTATUS(childExitMethod);
            sprintf
            (
                statusReport,
                "background pid %d is done: exit value %d",
                temp->processID,
                statusValue
            );
            outputStringWithANewline(statusReport);
            forget(temp->processID, listOfProcesses);
        }
        else if (WIFSIGNALED(childExitMethod) != 0)
        {
            // The process exited because of an uncaught signal.
            statusValue = WTERMSIG(childExitMethod);
            sprintf
            (
                statusReport,
                "background pid %d is done: terminated by signal %d",
                temp->processID,
                statusValue
            );
            outputStringWithANewline(statusReport);
            forget(temp->processID, listOfProcesses);
        }
    }

    return;
}

// This function implements the "status" built-in command:
void outputStatus(int statusType, int statusValue)
{
    if (statusType == EXIT_VALUE)
    {
        outputStringWithNoNewline("exit value ");
    }
    else
    {
        outputStringWithNoNewline("terminated by signal ");
    }

    char valueOrSignal[5];
    sprintf(valueOrSignal, "%d", statusValue);
    outputStringWithANewline(valueOrSignal);

    return;
}

// This function implements the "cd" built-in command:
void changeDirectory(char parameter[MAX_STRING_LENGTH])
{
    const char* homePath = getenv("HOME");
    // http://www0.cs.ucl.ac.uk/staff/W.Langdon/getenv/

    if (strlen(parameter) == 0 || strcmp(parameter, "~") == 0)
    // Strangely, chdir() wouldn't respond to having a "~" string as
    // its parameter. It ignored it. In order to make "cd ~" work as
    // expected, it is necessary to test the parameter for "~" and treat it
    // as if the user entered just "cd".
    {
        chdir(homePath);
    }
    else
    {
        chdir(parameter);
    }

    return;
}

// This function adds background-command pids to a linked list. It was
// extremely hard to debug, so I'm leaving my debugging printf() statements in
// (although they are commented out).
void remember(struct runningProcess** listOfProcesses, int processToRemember)
{
    // printf
    // (
    //     "remember(%p, %d) and *listOfProcesses = %p\n",
    //     listOfProcesses,
    //     processToRemember,
    //     *listOfProcesses
    // );
    if (*listOfProcesses == NULL) {

        // We don't have any links yet in our linked list, so we have to create
        // one:

        // printf("Going to make first link.\n");
        *listOfProcesses =
            (struct runningProcess*)malloc(sizeof(struct runningProcess));
        
        (*listOfProcesses)->processID = processToRemember;
        (*listOfProcesses)->next = NULL;
    } else {

        // We already have at least one link in our linked list of processes to
        // remember, so we need to add the current one at the end:

        // printf("Going to make an additional link.\n");
        struct runningProcess* current = (*listOfProcesses)->next;
        struct runningProcess* previous = *listOfProcesses;
        // printf("ccurent: %p\n", current);
        while (current != NULL) {
            previous = current;
            current = current->next;
        }
        // printf("cccurent: %p\n", current);

        // Having reached the end, we create another link. However, it's
        // not enough to know "current", because current == NULL, and that
        // doesn't help us add a link. We need to know "previous" so that we
        // can add our link to previous->next:
        previous->next =
            (struct runningProcess*)malloc(sizeof(struct runningProcess));
        
        previous->next->processID = processToRemember;
        previous->next->next = NULL;
    }

    return;
}

// This function evalutes the command array to see if there is a need for
// input/output redirection or running in the background. It then actually
// executes the command by using fork() and execvp(). It also deals with the
// aftermath of executing a command by waiting for foreground commands (and
// noting their manner of termination) and by adding background-command pids
// to a linked list.
void executeCommand
(
    char commandArray[MAX_COMMAND_ARRAY_SIZE][MAX_STRING_LENGTH],
    int arrayElementsUsed,
    int* statusType,
    int* statusValue,
    int* usingBackgroundIsPossible,
    struct runningProcess** listOfProcesses,
    struct sigaction* originalSigintAction
)
{
    int actuallyRunInBackground = FALSE;

    char fileForInputRedirection[MAX_STRING_LENGTH] = "";
    char fileForOutputRedirection[MAX_STRING_LENGTH] = "";

    // See if there is a BACKGROUND_SYMBOL as the last element in the command
    // array, and if so deal with it:

    if (strcmp(commandArray[arrayElementsUsed - 1], BACKGROUND_SYMBOL) == 0)
    {
        if (*usingBackgroundIsPossible == TRUE)
        {
            actuallyRunInBackground = TRUE;
        }
        arrayElementsUsed--;
    }

    // Check the last two arguments to see if we might be redirecting input or
    // output:

    int needToCheckOneMoreTime = FALSE;

    if (strcmp(commandArray[arrayElementsUsed - 2], REDIRECT_INPUT) == 0)
    {
        strcpy(fileForInputRedirection, commandArray[arrayElementsUsed - 1]);
        arrayElementsUsed = arrayElementsUsed - 2;
        needToCheckOneMoreTime = TRUE;
    }
    else if (strcmp(commandArray[arrayElementsUsed - 2], REDIRECT_OUTPUT) == 0)
    {
        strcpy(fileForOutputRedirection, commandArray[arrayElementsUsed - 1]);
        arrayElementsUsed = arrayElementsUsed - 2;
        needToCheckOneMoreTime = TRUE;
    }

    // If the last two elements indicated redirection, then we also need to
    // check the two elements before them:

    if (needToCheckOneMoreTime == TRUE)
    {
        if (strcmp(commandArray[arrayElementsUsed - 2], REDIRECT_INPUT) == 0)
        {
            strcpy
            (
                fileForInputRedirection,
                commandArray[arrayElementsUsed - 1]
            );
            arrayElementsUsed = arrayElementsUsed - 2;
        }
        else if
        (
            strcmp(commandArray[arrayElementsUsed - 2], REDIRECT_OUTPUT) == 0
        )
        {
            strcpy
            (
                fileForOutputRedirection,
                commandArray[arrayElementsUsed - 1]
            );
            arrayElementsUsed = arrayElementsUsed - 2;
        }
    }

    // Now we need to take commandArray and put it in a form that we can send
    // to execvp():

    char* commandArgs[arrayElementsUsed];

    int i;
    for (i = 0; i < arrayElementsUsed; i++)
    {
        commandArgs[i] = calloc(MAX_STRING_LENGTH, sizeof(char));
        strcpy(commandArgs[i], commandArray[i]);
    }
    commandArgs[arrayElementsUsed] = NULL;

    // NOW WE FORK() AND EXECVP() !!!

    // Template for forking comes from instructor at:
    // http://web.engr.oregonstate.edu/~brewsteb/CS344Slides/3.1%20Processes.pdf

    pid_t spawnPid = -5;
    int childExitMethod = -5;
 
    spawnPid = fork();
 
    if (spawnPid == -1) //  Error!
    {
        perror("Error when attempting to fork!\n");
        exit(1);
    }
    else if (spawnPid == 0) // We are in the child process!
    {
        // If the file is going to run in the background, then we will need to
        // set up input and output redirection (unless the user has already
        // specified such redirection):
        if (actuallyRunInBackground == TRUE)
        {
            if (strcmp(fileForInputRedirection, "") == 0)
            {
                strcpy(fileForInputRedirection, DEV_NULL);
            }
            if (strcmp(fileForOutputRedirection, "") == 0)
            {
                strcpy(fileForOutputRedirection, DEV_NULL);
            }
        }

        // Now actually set up input redirection, if necessary:
        if (strcmp(fileForInputRedirection, "") != 0)
        {
            // Code for file redirection derived from professor's examples at:
            // http://web.engr.oregonstate.edu/~brewsteb/CS344Slides/3.4%20More%20UNIX%20IO.pdf

            int sourceFD = open(fileForInputRedirection, O_RDONLY);

            if (sourceFD == -1) {
                perror("Error when opening file for input redirection!");
                // printf("cannot open %s for input", fileForInputRedirection);
                exit(1);
            }

            int result = dup2(sourceFD, 0);

            if (result == -1)
            {
                perror("Error when initiating input redirection!");
                exit(1);
            }
        }

        // And actualy set up output redirection, if necessary:
        if (strcmp(fileForOutputRedirection, "") != 0)
        {
            int targetFD = open
            (
                fileForOutputRedirection,
                O_WRONLY | O_CREAT | O_TRUNC,
                0644
            );

            if (targetFD == -1) {
                perror("Error when opening file for output redirection!");
                // printf("cannot open %s for output", fileForOutputRedirection);
                exit(1);
            }

            int result = dup2(targetFD, 1);

            if (result == -1)
            {
                perror("Error when initiating output redirection!");
                exit(1);
            }
        }

        // If the command is going to be run in the _foreground_, we need to
        // set sigaction(SIGINT) back to its original behavior (the behavior
        // it had before we set things to ignore SIGINT):

        if (actuallyRunInBackground == FALSE)
        {
            sigaction(SIGINT, originalSigintAction, NULL);
        }

        // Whether this is going to be a foreground process or a background
        // process--either way--we need to set this child process to ignore
        // SIGTSTP:

        struct sigaction ignoreAction = {{0}};
        ignoreAction.sa_handler = SIG_IGN;
        sigaction(SIGTSTP, &ignoreAction, NULL);

        // And finally we're ready to execvp():

        // Pattern for execvp() comes from instructor at:
        // http://web.engr.oregonstate.edu/~brewsteb/CS344Slides/3.1%20Processes.pdf

        if (execvp(*commandArgs, commandArgs) < 0)
        {
            perror("Error when attempting to execute command!");
            exit(1);
        }
    }
    
    // Otherwise, we are still in the parent process!

    if (actuallyRunInBackground == FALSE)
    {
        // We're running the command in the foreground, so we have to wait
        // for it to terminate:

        // We have to make sure that these globe variables are set correctly
        // so that we can deal with it if a SIGTSTP comes in while we are
        // blocked at waitpid().
        weAreWaitingForForegroundProcessToStop = TRUE;
        receivedSigtstp = FALSE;

        int resultPid = -1;
        
        while (resultPid == -1)
        {
            resultPid = waitpid(spawnPid, &childExitMethod, 0);
            // If we are blocked here at waitpid() and then receive a
            // SIGTSTP, waitpid() will return with -1. However, the foreground
            // process hasn't actually stopped. When that happens, we need to
            // loop back and waitpid() again until the foreground process
            // actually stops.
        }

        // We should update this global variable.
        weAreWaitingForForegroundProcessToStop = FALSE;

        // We deal with it _now_ if a SIGTSTP came in while we were blocked
        // at waitpid().
        if (receivedSigtstp == TRUE)
        {
            receivedSigtstp = FALSE;
            implementSigtstpLogic();
        }

        // Now we need to update our state variables to reflect the way that
        // the foreground process terminated:

        if (WIFEXITED(childExitMethod) != 0)
        {
            // The process exited by exit(0), exit(1), return 0, etc.
            *statusType = EXIT_VALUE;
            *statusValue = WEXITSTATUS(childExitMethod);
        }
        else if (WIFSIGNALED(childExitMethod) != 0)
        {
            // The process exited because of an uncaught signal.
            *statusType = SIGNAL_RECEIVED;
            *statusValue = WTERMSIG(childExitMethod);
            printf("terminated by signal %d\n", *statusValue);
        } else {
            perror("A process ended for reasons unknown!");
            exit(1);
        }
    } else {
        // We're running the file in the background, so we aren't going to wait
        // for it, but we do have to announce that it's in the background:

        char backgroundMessage[STATUS_REPORT_MAX_LENGTH];
        sprintf(backgroundMessage, "background pid is %d", spawnPid);
        outputStringWithANewline(backgroundMessage);

        // We also have to add it to our watch list of processes running in the
        // background:

        remember(listOfProcesses, spawnPid);
        // This use of a linked list that involves pointers to pointers almost
        // blows my mind. It was easy enough to write the linked list part,
        // but then I realized that passing pointers by value, which I did at
        // first, wasn't going to let me change what those pointers were
        // pointing to, so I had to go back and make it use pointers to
        // pointers.
    }

    for (i = 0; i < arrayElementsUsed; i++)
    {
        free(commandArgs[i]);
    }

    return;
}

int main()
{
    // The following handful of variables track the program state:

    char commandArray[MAX_COMMAND_ARRAY_SIZE][MAX_STRING_LENGTH];
    int arrayElementsUsed = 0;

    int statusType = EXIT_VALUE;
    int statusValue = 0;

    struct runningProcess* listOfProcesses = NULL;

    // Make shell ignore SIGINT:

    struct sigaction ignoreAction = {{0}};
    struct sigaction originalSigintAction = {{0}};
    // https://stackoverflow.com/questions/13746033/how-to-repair-warning-missing-braces-around-initializer
    ignoreAction.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignoreAction, &originalSigintAction);

    // Make shell handle SIGTSTP:

    struct sigaction handleSigtstp = {{0}};
    handleSigtstp.sa_handler = dealWithSigtstp;
    sigfillset(&handleSigtstp.sa_mask);
    handleSigtstp.sa_flags = 0; // I don't think this line is necessary.
    sigaction(SIGTSTP, &handleSigtstp, NULL);

    // The following is the program's main loop:

    while (TRUE)
    {
        checkForFinishedBackgroundProcesses(&listOfProcesses);
        // We have to send the _address_ of listOfProcesses, not the value
        // of the pointer, because we need to be able to change what it's
        // pointing to in the functions that we're now calling. This almost
        // blows my mind.

        getCommandArray(commandArray, &arrayElementsUsed);

        replaceDoubleDollarSigns(commandArray, arrayElementsUsed);

        // Now we can check to see if we need to invoke one of the three
        // built-in commands:
        if (strcmp(commandArray[0], EXIT_COMMAND) == 0)
        {
            prepForExit(&listOfProcesses);
            break;
        }
        else if (strcmp(commandArray[0], STATUS_COMMAND) == 0)
        {
            outputStatus(statusType, statusValue);
        }
        else if (strcmp(commandArray[0], CD_COMMAND) == 0)
        {
            if (arrayElementsUsed == 1)
            {
                changeDirectory("");
            }
            else
            {
                changeDirectory(commandArray[1]);
            }
        }
        // Or if we're doing nothing:
        else if (arrayElementsUsed == 0)
        {
            // Do nothing; it's a blank line.
        }
        else if (commandArray[0][0] == COMMENT_SYMBOL)
        {
            // Do nothing; it's a comment line.
        }
        // And if none of the above is true, then we need to try to execute
        // this command by forking and executing:
        else
        {
            executeCommand
            (
                commandArray,
                arrayElementsUsed,
                &statusType,
                &statusValue,
                &usingBackgroundIsPossible,
                &listOfProcesses,
                // We have to send the _address_ of listOfProcesses, not the
                // value of the pointer, because we need to be able to change
                // what it's pointing to in the functions that we're now
                // calling. This almost blows my mind.
                &originalSigintAction
            );
        }
    }

    return 0;
}
