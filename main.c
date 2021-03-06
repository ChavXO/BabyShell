/******************************************************************************\
 * Baby Shell                                                                 *
 *                                                                            *
 * Author: Michael Chavinda                                                   *
 *                                                                            *
 * Purpose: Unix shell simulation project for my operating systems class      *
 *                                                                            *
 * Usage: Runs a REPL that calls commands Unix shell                          *
\******************************************************************************/

#include <stropts.h>
#include <stdio_ext.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>

/* known issues
 * prompt prints twice over in parallel mode sometimes *** it was for some built in commands because they returned. *** fixed
 * chain of bogus commands fills process list *** fixed
 * crtl + d combination does not show an error message for exit while tasks are running *** fixed
 * does not run files in the current path using my other implementation of path 
 * invalid write of size 4 when resuming job *** fixed *** resurfaced *** fixed by passing around sate
 * sequence of invalid command causes memory leaks in address space of program in parallel execution behaviour is inconsistent *** fixed
 * some inconsistent behaviour when swiching between modes ** fixed by making state a variable that is passed between functions
 */
 
 /* still to-do
  * access command history through bangs
  * free memory on ctrl +c?
  * reduce memory consumption
  */

/*>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Function and variable declarations <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<*/
/* shell constants */
static const int SEQUENTIAL = 0;
static const int PARALLEL   = 1;

/* shell state (there was too much state specific information to pass around) */
typedef struct _prog_state {
    bool do_exit;
    bool in_parallel;
    int mode;
} program_state;

// plan to implement bangs
typedef struct _history {
    struct _history *previous;
    char command [1024];
    struct _history *next;
} history;

/* linked list for storing shell environmenr directory */
typedef struct _path {
    char path_var [1024];
    struct _path *next;
} path;


typedef enum {RUNNING, PAUSED, DEAD} state;

// doubly linked list for keeping track of history
typedef struct _processes {
    struct _processes *previous;
    pid_t id;
    char prc_name [128];
    state process_state;
    struct _processes *next;
} processes;

processes* head_jobs;
bool shell_printed = false;

/*_________________________________________________________*
 *           Functions for initialising the shell          *
 *_________________________________________________________*/
int run_shell(path* head);
int _inc_jobs(int n);
void show_prompt();
void poll_results();
void sig_comm();
path* load_environment(); //sets envronment from computer's $PATH variable
path* load_path_from_list(char** environment); //helper function for load_environment
path *load_path(const char *filename); //load path from file
void free_path(path* head);

/*_________________________________________________________*
 *           Functions for parsing input                   *
 *_________________________________________________________*/
char** tokenify(char* buffer, char* split);
char** splitCommands(char* buffer);
char* is_valid_command(char* command, path* head);
void remove_comments(char* buffer);
bool is_built_in_command(char* command);
void run_builtin(char** params, char* buffer, program_state** p_state);

/*_________________________________________________________*
 *           Functions for running shell commands          *
 *_________________________________________________________*/
void run_commands(char** commands, path* head, program_state** p_state);
void execute_command(char** params, char* commands, path* head, program_state** p_state);
char* previous_directory(char* dir);
void change_directory(char* dir);
bool change_mode(char* mode_str, program_state** p_state);
void print_processes(processes* head);
void pause_process(char* id);
void resume_process(char* id);
void run_from_system(char* buffer, program_state* p_state);
void manage_state();
void set_process_state(pid_t pid, state process_state);

/*_________________________________________________________*
 *           Functions cleaning up and debugging           *
 *_________________________________________________________*/
void print_path(path* head, int num_words); //debugging
void list_clear(path *list);
void clean_up_processes(); // defensive mechanism because delete is asynchronous
path* list_append(char* curr, path *list);
void free_tokens(char** tokens);
void add_process(pid_t pid, char* process_name);
void delete_process(pid_t id);
void delete_process_by_name(char* process_name);

int main() {
    system("reset"); //run reset in parallel to reduce lag time (removed ampersand because the behaviour was inconsistent)
    printf("%c]0;%s%c", '\033', "Shelby the Shell", '\007'); // window title
    path* head = load_environment();
    //path* head = load_path("shell-config");
    int res = run_shell(head);	
    free_path(head);
    return res;
}

/* run shell contains a loop that gets user input and then runs the appropriate file
 * exits when user types "exit" or presses ctrl + d to signify the end of the fil
 * The signal listener is initialised so the processes can be updated asynchronously */
int run_shell(path* head) {
    // initialisation
    program_state* p_state = (program_state*) calloc(1, sizeof(program_state));
    p_state->do_exit = false;
    p_state->in_parallel = false;
    p_state->mode = SEQUENTIAL;
    head_jobs = (processes*) calloc(1, sizeof(processes));
    head_jobs->next = NULL;
    head_jobs->previous = NULL;
    show_prompt();
    signal(SIGCHLD, sig_comm); //signal handler notifies user asynchronously. I like that feature better than the waitpid notification after input so I'll run both in tandem
    while(!feof(stdin) || _inc_jobs(0) != 0) {
        char buffer [1024];
        shell_printed = false;
	    if (fgets(buffer, 1024, stdin) != NULL) {
	        remove_comments(buffer);
	        char** commands = splitCommands(buffer);
            run_commands(commands, head, &p_state);
	        free_tokens(commands); 
	    } else continue; 
	    if (p_state->do_exit) break; //check background
	      
	    if (feof(stdin)) printf("\nYou cannot exit while there are processes running.\n"); //unindented single line if statements help make the code readable for me
	    manage_state(&p_state);   
    }
    free(p_state);
    free(head_jobs);
    return 0;
}

void manage_state(program_state** p_state) {
    //change mode
    //if signal handler fails clean with wait
    clean_up_processes(); // collect zombie children if not caught by signal wait. chains of short commands had this problem
    shell_printed = false;
    if ((*p_state)->in_parallel) (*p_state)->mode = PARALLEL;
    else (*p_state)->mode = SEQUENTIAL; 
    
    if ((*p_state)->mode == PARALLEL) {
        poll_results(); //using this as a non-blocking wait
        if (shell_printed == false) show_prompt();
    	shell_printed = true;
    } else show_prompt();
}

void clean_up_processes() {
    if (_inc_jobs(0) > 0) {
		int pid = waitpid(-1, NULL, WNOHANG); //non-blocking wait
		while (pid > 0) {	
			printf("\nProcess %d finished running.\n", pid);
			shell_printed = true;
            show_prompt();
			delete_process(pid);
			pid = waitpid(-1, NULL, WNOHANG);
		}
	} //only ran on the next command input so I used it in conjunction with signal
}

void run_commands(char** commands, path* head, program_state** p_state) {
    char whitespace [4] = "\n\t\r ";
    int i;
    for (i = 0; commands[i] != NULL; i++) {
        remove_comments(commands[i]);
        char** params = tokenify(commands[i], whitespace);
        execute_command(params, commands[i], head, p_state); 
		free_tokens(params);
	}
	return;
}

/* executes $PATH commands using execv. "builtin" commands are either handled explicity
 * or sent to the system.*/
void execute_command(char** params, char* command, path* head, program_state** p_state) {
    if (params[0] == NULL) return;
    if (is_built_in_command(params[0])) { //handle builtin commands
		run_builtin(params, command, p_state);
	} else {
        char* curr_command = is_valid_command(params[0], head); // checks if valid, attaches path to code
        if (curr_command == NULL) {
            printf("Invalid command: %s\n", params[0]);
            return;
        }
        params[0] = realloc(params[0], (strlen(curr_command) + 1) * sizeof(char));
        strcpy(params[0], curr_command);
        free(curr_command);
        shell_printed = false;
        pid_t pid = fork();
		if (pid == 0) {
            execv(params[0], params);
            if ((*p_state)->mode == PARALLEL)
                delete_process_by_name(command); // delete from list by name since there is not pid associated with the process
            printf("Command %s failed to run.\n", params[0]); 
            (*p_state)->do_exit = true;
		} else if (pid < 0) printf("Failed to start process.\n");
		else {
		    if ((*p_state)->mode == SEQUENTIAL) waitpid(0, NULL, 0);
    		else {
    		    shell_printed = false; //making sure that the program knows that the shell has not been printed
    		    add_process(pid, command);
    		}	
		}
    }
}

void show_prompt() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL)
    	printf("%s> ", cwd);
    else
	    perror("getcwd() error");
	fflush(stdout);
	
}

//splits a string by a delimiter
char** tokenify(char* buffer, char* split) {
	char* string = strdup(buffer);
	char* token = NULL;
	int i = 0;    
    //count the number of delimiters
	for (token = strtok(string, split); token != NULL; token = strtok(NULL, split)) i += 1;

	char** final_tok = calloc(i + 1, sizeof(char**));
	i = 0;
	free(string);
	string = strdup(buffer);
	
	//put strings into final_tok array
	for (token = strtok(string, split); token != NULL; token = strtok(NULL, split)) {
		final_tok[i] = calloc((strlen(token) + 1), sizeof(char));
		strcpy(final_tok[i], token);
		i++;
	}
	
	//NULL-terminate array
	final_tok[i] = NULL;
	free(string);
	return final_tok;
}

char** splitCommands(char* buffer) {
    return tokenify(buffer, ";");
}

char* is_valid_command(char* command, path* head) {
    path* temp = head;
    
    while (temp != NULL) {        
	    struct stat statresult;
	    // add string to each path
        char comm[1024]= "";
        int stat_res = 0;    
        strcat(comm, temp->path_var);
        strcat(comm, "/");
        strcat(comm, command);
        stat_res = stat(comm, &statresult);
        if (stat_res == 0) {
            char* ret = calloc(strlen(comm) + 1, sizeof(char));
            strcpy(ret, comm);
            return ret;
        }        		        
        temp = temp->next;
    }
    return NULL;
}

void remove_comments(char* buffer) {
    int i;
    for (i=0; i<strlen(buffer); i++){
        if (buffer[i] == '#' || buffer[i] == '\n') {
			buffer[i] = '\0';
			return;
		}
	}
}

//free arrays of arrays
void free_tokens(char** tokens) {
    int j;
    for (j = 0; tokens[j] != NULL; j++) {
	    free(tokens[j]);
    }
    free(tokens);
}	

//find previous directory
char* previous_directory(char* dir) {
    char* prev = calloc(strlen(dir), sizeof(char));
    int i;
    
    //search from the back for first slash
    for (i = strlen(dir) - 1; i >= 0; i--) {
        if(dir[i] == '/') {
            strncpy(prev, dir, i);
            prev[i] = '\0';
            break;
        }
    }
    return prev;
}

void change_directory(char* dir) {
    if (dir == NULL) {
        chdir("/home/chav"); //go to root directory
        return;
    }
    
    char cwd[1024];
   	if (getcwd(cwd, sizeof(cwd)) == NULL)
       perror("getcwd() error");
    
    if (strcmp(dir, ".") == 0) {
        //current directory
        return;
    } else if (strcmp(dir, "..") == 0)  { //go to previous directory
        char* prev_dir = previous_directory(cwd);
        chdir(prev_dir);
        free(prev_dir);
        return;
    } else if ((strstr("/", dir) != NULL)) { //if the string contains no slashes assume it is the cwd
        char * new_str ;
        int i;
        for (i = 0; cwd[i] != '\0'; i++);

        cwd[i] = '/';
        cwd[i + 1] = '\0';
     
        //concatenate string to directory
        if((new_str = calloc(strlen(cwd)+strlen(dir)+1, sizeof(char*))) != NULL){
            new_str[0] = '\0';   // ensures the memory is an empty string
            strcat(new_str,cwd);
            strcat(new_str,dir);
            chdir(new_str);
            free(new_str);
            return;
        } else {
            return; //error
        }
    } else {
        chdir(dir);
        return;
    }
    printf("Directory %s not found.\n", dir);
}

bool is_built_in_command(char* command) {
    char* builtin [] = {"exit", "pwd", "mode", "echo", "type", "resume", "pause", "jobs", "help", "history", "time", "cd", NULL};
    int i;
    for (i = 0; builtin[i] != NULL; i++) {
        if (strncmp(command, builtin[i], strlen(command)) == 0) return true;
    }
    
    return false;
}

path* list_append(char* curr, path *list) {
    path *current = list;
    path *newNode = (path*) calloc(1, sizeof(path));
    if (newNode == NULL) { //case where malloc fails 
        fprintf(stderr, "Failed to create new node to add.\n");
        return list;
    }
    strcpy(newNode->path_var, curr);
    newNode->next = NULL;
    
    // adding to an empty list
    if (list == NULL) {
        //list = newNode; unnecessary assignement
        return newNode;
    }
    
    // general case
    while (current != NULL) {
        current = current->next;
    }
    
    current = newNode;
    return list;
}

void list_clear(path *list) {
    while (list != NULL) {
        path *tmp = list;
        list = list->next;
        free(tmp);
    }
}

bool change_mode(char* mode_str, program_state** p_state) {
    if (mode_str == NULL){
        if ((*p_state)->mode == 0) {
            printf("Running in sequential mode.\n");
        }
        if ((*p_state)->mode == 1) {
            printf("Running in parallel mode.\n");
        }
    } else if (strcmp(mode_str, "parallel") == 0 || strcmp(mode_str, "p") == 0) {
        return true;
    } else if (strcmp(mode_str, "sequential") == 0 || strcmp(mode_str, "s") == 0) {
        return false;
    } else {
        printf("Unrecognised mode: %s.\nValid entires are parallel or p, or sequential or s.\n", mode_str);
    }
    // default case is to return the current mode
    return (*p_state)->in_parallel;
}

void sig_comm() {
    // did some work in sig_comm. The while loops approach was a bit tricky
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid < 0 && errno != ECHILD)
        printf("Waitpid failed\n"); 
    else if (pid > 0) {
        printf("\nProcess %d finished running.\n", pid);
        shell_printed = true;
        show_prompt();
        delete_process(pid);
    }
}

path* load_path_from_list(char** environment) {
    path* head = (path*) calloc(1, sizeof(path)); //calloc so the compiler doesn't complain about unitialised variables
    
    path* current = head;
    int i;
    
    for (i = 0; environment[i] != NULL; i++) {
        int len = strlen(environment[i]);
        strncpy(current->path_var, environment[i], len); // copy everything but the empty space
        current->next = (path*) calloc(1, sizeof(path));
        current = current->next;
    }
    current->next = NULL;  
    
    return head; 
}

path *load_path(const char *filename) {
    FILE* file = fopen(filename, "r"); 
    if (file == NULL) {
        printf("Failed to open %s. Now terminating program.\n", filename);
        exit(1);
    }
    
    path* head = (path*) calloc(1, sizeof(path*)); //calloc so the compiler doesn't complain about unitialised variables
    
    path* current = head;
    
    char current_word[256]; // a bit large so the user can add their own directories into the path
    
    while (fgets(current_word, 256, file) != NULL) {
        int len = strlen(current_word);
        strncpy(current->path_var, current_word, len - 1); // copy everything but the empty space
        current->next = (path*) calloc(1, sizeof(path));
        current = current->next;
    }
    current->next = NULL;
    
    fclose(file);
    return head;
}

void free_path(path* head) {
    while (head != NULL) {
        path* tmp = head;
        head = head->next;
        free(tmp);
    }
}

void run_builtin(char** params, char* buffer, program_state** p_state) {
    shell_printed = false;
    if (strcmp(params[0], "cd") == 0) {
        change_directory(params[1]);
    } else if (strcmp(params[0], "jobs") == 0) {
        print_processes(head_jobs);
    } else if (strcmp(params[0], "mode") == 0) {
        (*p_state)->in_parallel = change_mode(params[1], p_state);
    } else if (strcmp(params[0], "resume") == 0) {
        if (params[1] ==  NULL) printf("resume takes in the process ID as an argument.\n");
        else resume_process(params[1]);
    } else if (strcmp(params[0], "pause") == 0) {
        if (params[1] ==  NULL) printf("pause takes in the process ID as an argument.\n");
        else pause_process(params[1]);
    } else if (strcmp(params[0], "exit") == 0) {
        if (_inc_jobs(0) > 0) printf("You cannot exit while there are processes running.\n");
        else (*p_state)->do_exit = true;
    } else {
        run_from_system(buffer, (*p_state));
    }
    return;
}

void pause_process(char* id) {
    pid_t pid = strtol(id, NULL, 10);  
    if (pid == 0) {
        printf("Invalid process id.\n");
        return;
    }
    if (kill(pid, SIGSTOP) == 0)
        set_process_state(pid, PAUSED);
}

void resume_process(char* id) {
    pid_t pid = strtol(id, NULL, 10);
    if (pid == 0) {
        printf("Invalid process id.\n");
        return;
    }
    if (kill(pid, SIGCONT) == 0) {
        set_process_state(pid, RUNNING);
    }
}

void run_from_system(char* buffer, program_state* p_state) {
    if (p_state->mode == SEQUENTIAL) {
        system(buffer);
        return;
    }
    
    char* tmp = calloc(strlen(buffer) + 3, sizeof(char));
    // add ampersand to make parallel
    strcat(tmp, buffer);
    strcat(tmp, " &");
    system(tmp);
    free(tmp);
    shell_printed = false;
}

void poll_results() {
    struct pollfd pfd[1];
    pfd[0].fd = 0; 
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    
    int rv = poll(&pfd[0], 1, 200);
    if (rv < 0) printf("");
    return;
}


void print_path(path* head, int num_words) {
    path* current = head;
    int i;
    for (i = 0; i < num_words; i++) {
        printf("%s ", current->path_var);
        current = current->next;
    }
    printf("\n");
    return;
}

path* load_environment() {
    char path_env[1024];
    FILE *fp;
    
    /* Open the command for reading. */
    fp = popen("echo $PATH", "r");
    if (fp == NULL) {
        printf("Failed to run command\n" );
        exit(1);
    }

    /* Read the output a line at a time - output it. */
    if (fgets(path_env, sizeof(path_env)-1, fp) != NULL) {
        printf("");
    }

    /* close */
    pclose(fp);
    char** environment = tokenify(path_env, ":");
    path* head = load_path_from_list(environment);
    free_tokens(environment);
    return head;
}

void set_process_state (pid_t pid, state process_state) {
    processes* current = head_jobs;
    while (current != NULL && current->id != pid) {
        current = current->next;
    }
    
    if (current == NULL) {
        printf("Could not find the job with id: %d.\n", pid);
        return;
    }
    
    processes** tmp = &current;
    if (process_state == PAUSED) {
        printf("Job %d paused.\n", pid);
        (*tmp)->process_state = PAUSED;
        return;
    }
        
    if (process_state == RUNNING) {
        printf("Job %d resumed.\n", pid);
        (*tmp)->process_state = RUNNING;
        return;
    }
    
    return;
}

void add_process(pid_t pid, char* process_name) {
    processes* current = head_jobs;
    while(current->next != NULL) {
        current = current->next;
    }
    current->next = (processes*) malloc(sizeof(processes));
    (current->next)->previous = current;
    current = current->next;
    current-> id = pid;
    strcpy(current->prc_name, process_name);
    current->process_state = RUNNING;
    current->next = NULL;
    _inc_jobs(1);
}

void delete_process(pid_t process_id) {
    if (head_jobs == NULL) {
        return; //case when head is empty
    }
    processes* current = head_jobs;
    while (current->next != NULL && (current->next)->id != process_id) {
        current = current->next;
    }
    
    if (current->next == NULL) {
        return;
    }
    
    processes* tmp = current->next;
    if (tmp->next == NULL) {
        current->next = NULL;   
    } else {
        current->next = tmp->next;
        (current->next)->previous = tmp->previous;
    }
    tmp->previous = current;
    _inc_jobs(-1);
    free(tmp);
    return;
}

void delete_process_by_name(char* process_name) {
    if (head_jobs == NULL) {
        return; //case when head is empty
    }
    processes* current = head_jobs;
    // iterate through to desired job
    while (current->next != NULL && strcmp((current->next)->prc_name, process_name) != 0) {
        current = current->next;
    }
    
    if (current->next == NULL) {
        return;
    }
    
    processes* tmp = current->next;
    if (tmp->next == NULL) {
        current->next = NULL;   
    } else {
        current->next = tmp->next;
        (current->next)->previous = tmp->previous;
    }
    tmp->previous = current;
    _inc_jobs(-1);
    free(tmp);
    return;
}

void print_processes(processes* head) {
    processes* current = head_jobs->next;

    while(current != NULL) {
        char p_state [32] = "";
        if (current->process_state == PAUSED) {
            strcpy(p_state, "PAUSED");
        } else if (current->process_state == DEAD) {
            strcpy(p_state, "DEAD"); // plan to keep a list of active and inactive processes
        } else {
            strcpy(p_state, "RUNNING");
        }
        
        printf("[%d]: %s - STATUS: %s\n", current->id, current->prc_name, p_state);
        current = current->next;
    }
    return;
}

// function with static variable that keeps track of the number of jobs
int _inc_jobs(int n){
    static int total_jobs = 0;
    total_jobs += n;
    return total_jobs;
}