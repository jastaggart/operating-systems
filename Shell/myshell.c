#include <stdio.h>
#include <unistd.h> 
#include <string.h> 
#include <stdlib.h> 
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>

/*
  Name: Jasmine Taggart
  ID: 261056534
*/

struct JOB {
    int jobnum;
    int pid;
    char* jobname[20];
} joblist[16];

int getcmd (char *prompt, char *args[], int *background)
{
    int length, i = 0; char *token, *loc; char *line = NULL; size_t linecap = 0;
    printf("%s", prompt);
    length = getline(&line, &linecap, stdin);

    if (length <= 0) {
        exit(-1); 
    }

    // Check if background is specified.. 
    if ((loc = index(line, '&')) != NULL) {
        *background = 1;
        *loc = ' '; 
    } else
        *background = 0;

    while ((token = strsep(&line, " \t\n")) != NULL) { 
        for (int j = 0; j < strlen(token); j++) {
            if (token[j] <= 32) 
            token[j] = '\0';
        }
        if (strlen(token) > 0) args[i++] = token;
    }
    args[i] = NULL;
    return i; 
}

void echo(char *args[], int numArgs) {
    for (int i = 1; i < numArgs; i++) {
        printf("%s ", args[i]);
    }
    printf("\n");
}

// Prints current working directory
void pwd() {
    char cwd[100];
    if (getcwd(cwd, sizeof(cwd)) == NULL) 
        perror("getcwd() error");
    else 
        printf("%s\n", cwd);
}

// Goes into the specified directory
void cd(char *args[], int numArgs) {

    // if no directory is given, simply print the current directory
    if (numArgs == 1)
        pwd();

    // if more than one destination is given, error
    else if (numArgs > 2) {
        printf("cd: Too many arguments\n");
    }

    // otherwise, the input is correct
    // so change to the directory given and print an error if it fails
    else if (chdir(args[1]) != 0) {
        perror("chdir() error");
    }
}

// Terminates the shell
void exitShell(int jobcount) {

    // go through list of background jobs and terminate them
    for (int i = 0; i < jobcount; i++) {
        int killret = kill(joblist[i].pid, SIGKILL);
        if (killret) perror("kill() error");
    }
    // exit the program
    exit(0);
}

// Brings a background job to the foreground
void fg(int* jobcount, char* args[], int numArgs) {
    int fgJob;
    int isFound = 0;

    // if a job number isn't specified, 
    // bring the first job in list to the foreground
    if (numArgs == 1) 
        fgJob = 0;

    // otherwise convert the given job number into an integer
    else fgJob = atoi(args[1]);

    // search for job number in job list array
    for (int i = 0; i < *jobcount && !isFound; i++) {

        // if job is found
        if (joblist[i].jobnum == fgJob) {

            // wait for child to complete
            waitpid(joblist[i].pid, NULL, 0);
            isFound = 1;

            // remove foreground job from background job list
            for (int j = i; j < *jobcount - 1; j++) {
                joblist[j] = joblist[j+1]; //shift the following jobs forward
                joblist[j].jobnum = j; //shift the job numbers forward
            }

            //decrement number of background jobs
            (*jobcount)--;
        }
    }
    if (!isFound)
        puts("Job not found");
}

// Lists all background jobs
void jobs(int jobcount, int numArgs) {
    puts("-ID- --PID-- --------NAME--------");
    // iterate through job list
    for (int i = 0; i < jobcount; i++) {
        // print job's number and pid
        printf("%-4d %-7d", joblist[i].jobnum, joblist[i].pid);

        // print job's command 
        for (int j = 0; joblist[i].jobname[j]; j++) {
            printf(" %s", joblist[i].jobname[j]);
        }
        puts(""); // print new line
    }
}

int main(void)
{
    char *args[20];
    int bg;
    int jobcount = 0;

    while(1) {
        bg = 0;
        int cnt = getcmd("\n>> ", args, &bg);
        if (cnt == 0) 
            continue;
        args[cnt] = NULL;
        int *status_p;

        // First, check if command is built in:

        // check if cmd is 'echo'
        if (strcmp(args[0], "echo") == 0) 
            echo(args, cnt);

        // check if command is 'cd'
        else if (strcmp(args[0], "cd") == 0) 
            cd(args, cnt);

        // check if command is 'pwd'
        else if (strcmp(args[0], "pwd") == 0) 
            pwd();

        // check if command is 'exit'
        else if (strcmp(args[0], "exit") == 0) {
            exitShell(jobcount);
        }

        // check if command is 'fg'
        else if (strcmp(args[0], "fg") == 0) 
            fg(&jobcount, args, cnt);

        // check if command is 'jobs'
        else if (strcmp(args[0], "jobs") == 0) 
            jobs(jobcount, cnt);

        // If not, fork a child process to execute the external cmd
        else {
            int pid = fork();

            // if fork fails, exit
            if (pid < 0) {
                printf("Fork failed");
                return 1;
            } 

            // if in child process, execute the inputted command
            else if (pid == 0) {

                // check if redirection or piping occurs
                int isRedir = 0;
                int isPipe = 0;
                for (int i = 0; i < cnt; i++) {

                    // if '>' is present, do output redirection
                    if (strcmp(args[i], ">") == 0)  {
                        isRedir = 1;
                        char* argsRedir[20];

                        // copy command args preceding '>' to new arr
                        memcpy(argsRedir, args, sizeof(char*)*(i));
                        argsRedir[i] = NULL; //set last element to null to define end of args

                        // close stdout
                        close(1);

                        // open file to redirect output to
                        int fd = open(args[i+1], O_RDWR | O_CREAT);
                        
                        // execute command and report error if it fails
                        if (execvp(argsRedir[0], argsRedir)) {
                            perror("execvp() error");
                            exit(1);
                        }
                        break;
                    }

                    // if | is present, do piping
                    if (strcmp(args[i], "|") == 0) {
                        isPipe = 1;
                        int fds[2];
                        pipe(fds);
                        int pipepid = fork();

                        // if fork fails, exit
                        if (pipepid < 0) {
                            printf("Fork failed");
                            return 1;
                        } 

                        // if we're in parent process, execute left side of pipe
                        else if (pipepid > 0) {
                            
                            char* argsLeftPipe[20];

                            // copy command args preceding '|' to new arr
                            memcpy(argsLeftPipe, args, sizeof(char*)*(i));
                            argsLeftPipe[i] = NULL; //set last element to null to define end of args
                            
                            close(1); //close stdout
                            dup(fds[1]); //connect stdout to write end of pipe
                            close(fds[0]); //close read end of pipe
                            
                            // execute command and report error if it fails
                            if (execvp(argsLeftPipe[0], argsLeftPipe)) {
                                perror("execvp() error");
                                exit(1);
                            }
                            waitpid(pipepid, NULL, 0); // wait for child to complete
                        }

                        // if we're in child, execute right side of pipe
                        else {
                            char* argsRightPipe[20];

                            // copy command args following '|' to new arr
                            memcpy(argsRightPipe, &args[i+1], sizeof(char*)*(cnt - i - 1));
                            argsRightPipe[cnt - i - 1] = NULL; //set last element to null to define end of args
                            
                            close(0); //close stdin
                            dup(fds[0]); //connect stdin to read end of pipe
                            close(fds[1]); //close write end of pipe

                            // execute command and report error if it fails
                            if (execvp(argsRightPipe[0], argsRightPipe)) {
                                perror("execvp() error");
                                exit(1);
                            }
                        }
                    } 
                }

                // if redirection and piping are not present, execute normally
                if (!isRedir && !isPipe) {
                    if (execvp(args[0], args)) {
                        perror("execvp() error");
                        exit(1);
                    }
                }
            }

            // if in parent process and background is not specified, 
            // wait for child to continue
            else if (bg == 0) 
                waitpid(pid, NULL, 0);

            // if background is specified, add child process to job list
            else {
                joblist[jobcount].jobnum = jobcount;
                memcpy(joblist[jobcount].jobname, args, sizeof(args));
                joblist[jobcount].pid = pid;
                jobcount++;
            }
        }
    }
}
