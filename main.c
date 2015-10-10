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
 * invalid write of size 4 when resuming job *** fixed *** resurfaced
 * sequence of invalid command causes memory leaks in address space of program in parallel execution behaviour is inconsistent *** fixed
 * someinconsistent behaviour when swiching between modes
 */

// shell constants
static const int SEQUENTIAL = 0;
static const int PARALLEL   = 1;

// shell state as global variables (there was too much state specific information to pass around)
typedef struct _prog_state {
    bool do_exit;
    bool in_parallel;
    int mode;
    bool shell_printed;    
} program_state;

// global variables
bool do_exit = false;
bool in_parallel = false;
int mode = SEQUENTIAL;
bool shell_printed = false;

// plan to implement bangs
typedef struct _history {
    struct _history *previous;
    char command [1024];
    struct _history *next;
} history;

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

// functions for initialising the shell
int run_shell(path* head);
int _inc_jobs(int n);
void show_prompt();
void poll_results();
void sig_comm();
pid_t proc_find(char* name);
path* load_environment(); //sets envronment from computer's $PATH variable
path* load_path_from_list(char** environment); //helper function for load_environment
path *load_path(const char *filename); //load path from file
void free_path(path* head);

// functions for processing input
char** tokenify(char* buffer, char* split);
char** splitCommands(char* buffer);
char* is_valid_command(char* command, path* head);
void remove_comments(char* buffer);
bool is_built_in_command(char* command);
void run_builtin(char** params, char* buffer);

// runner functions
void run_commands(char* buffer, char** commands, path* head);
char* previous_directory(char* dir);
void change_directory(char* dir);
bool change_mode(char* mode_str);
void print_processes(processes* head);
void manage_state();
void set_process_state(pid_t pid, const char* set_state);

// clean up and debugging
void print_path(path* head, int num_words); //debugging
void list_clear(path *list);
void clean_up_processes();
path* list_append(char* curr, path *list);
void free_tokens(char** tokens);
void add_process(pid_t pid, char* process_name);
void delete_process(pid_t id);
void delete_process_by_name(char* process_name);

int main() {
    // removed ampersand because the behaviour was inconsistent
    system("reset"); //run reset in parallel to reduce lag time
    //signal(SIGCHLD, sig_comm); //moved down so that signal is only set in parallel. No need to define it in sequential
    printf("%c]0;%s%c", '\033', "Shelby the Shell", '\007'); // window title
    //path* head = load_environment();
    path* head = load_path("shell-config");
    int res = run_shell(head);	
    free_path(head);
	return res;
}

int run_shell(path* head) {
    head_jobs = (processes*) calloc(1, sizeof(processes));
    head_jobs->next = NULL;
    head_jobs->previous = NULL;
    show_prompt();
    signal(SIGCHLD, sig_comm);
	while(!feof(stdin) || _inc_jobs(0) != 0) {
		char buffer [1024];
		shell_printed = false;
		if (fgets(buffer, 1024, stdin) != NULL) {
		    remove_comments(buffer);
			char** commands = splitCommands(buffer);
            run_commands(buffer, commands, head);
	    	free_tokens(commands); 
	    } else continue; 

	    if (do_exit) break; //check background
	  
	    if (feof(stdin)) printf("\nYou cannot exit while there are processes running.\n"); //unindented single line iff statements help make the code readable for me	   
	    manage_state();   
	    if (_inc_jobs(0) > 0) clean_up_processes(); //in case some process' exit signal wasn't handled
	}
	free(head_jobs);
    return 0;
}

void manage_state() {
    //change mode
    if (in_parallel) mode = PARALLEL;
    else mode = SEQUENTIAL; 
    
    if (mode == PARALLEL) {
        poll_results(); //using this as a non-blocking wait
        if (shell_printed == false) show_prompt();
    	shell_printed = true;
    } else show_prompt();
}

void clean_up_processes() {
    processes* current = head_jobs;
    while (current != NULL) {
        pid_t pid = proc_find(current->prc_name);
        if (pid == -1) {
            processes* tmp = current->next;
            delete_process_by_name(current->prc_name);
            current = tmp;
            continue;
        }
        current = current->next;
    }
}

void run_commands(char* buffer, char** commands, path* head) {
    char whitespace [4] = "\n\t\r ";
    int i;
    for (i = 0; commands[i] != NULL; i++) {
        char** params = tokenify(commands[i], whitespace);
        remove_comments(commands[i]);
        bool abort = false;
		if (params[0] == NULL) goto NEXT; // go to the end and free if there are no commands. Preferred over big else statement
		
        if (is_built_in_command(params[0])) { //handle builtin commands
            shell_printed = false;
    		run_builtin(params, buffer);
    	} else {
            char* command = is_valid_command(params[0], head);
            if (command == NULL) {
                printf("Invalid command: %s\n", params[0]);
                goto NEXT; // rather than a big else statement
            }
            // copies updated command into params[0]
            params[0] = realloc(params[0], (strlen(command) + 1) * sizeof(char));
            strcpy(params[0], command);
            free(command);
            shell_printed = false;
            
            pid_t pid = fork();
    		if (pid == 0) {
		        execv(params[0], params);
		        delete_process_by_name(commands[i]); // delete from list by name since there is not pid associated with the process
		        printf("Command %s failed to run.\n", params[0]); 
		        abort = true;
		        do_exit = true;
		        
    		} else if (pid < 0) printf("Failed to start process.\n");
    		else {
    		    if (mode == SEQUENTIAL) waitpid(0, NULL, 0);
        		else {
        		    shell_printed = false; //making sure that the program knows that the shell has not been printed
        		    add_process(pid, commands[i]);
        		}	
    		}
        } 
		NEXT:free_tokens(params);
		if (abort) return;
	}
	return;
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
	for (token = strtok(string, split); token != NULL; token = strtok(NULL, split)) {
		i += 1;
	}

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
        // duplicate code for this and execute. might just return the string
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
        } else {
            return; //error
        }
    } else {
        chdir(dir);
    }
}

bool is_built_in_command(char* command) {
    char* builtin [] = {"exit", "pwd", "cd", "mode", "echo", "type", "resume", "pause", "jobs", "help", NULL};
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

bool change_mode(char* mode_str) {
    if (mode_str == NULL){
        if (mode == 0) {
            printf("Running in sequential mode.\n");
        }
        if (mode == 1) {
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
    return in_parallel;
}

pid_t proc_find(char* name) {
    DIR* dir;
    struct dirent* ent;
    char* endptr;
    char buf[512];
    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }

    while((ent = readdir(dir)) != NULL) {
        /* if endptr is not a null character, the directory is not
         * entirely numeric, so ignore it */
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0') {
            continue;
        }
        /* try to open the cmdline file */
        snprintf(buf, sizeof(buf), "/proc/%ld/cmdline", lpid);
        FILE* fp = fopen(buf, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                /* check the first token in the file, the program name */
                char* first = strtok(buf, " ");
                if (!strcmp(first, name)) {
                    fclose(fp);
                    closedir(dir);
                    return (pid_t)lpid;
                }
            }
            fclose(fp);
        }

    }
    closedir(dir);
    return -1;
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
    
    //asssumes last line of file is a new line or that there is an empty space
    char current_word[256]; //
    
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

void run_builtin(char** params, char* buffer) {
    //multiple arguments vs global variables
    if (strcmp(params[0], "cd") == 0) {
        change_directory(params[1]);
    } else if (strcmp(params[0], "jobs") == 0) {
        print_processes(head_jobs);
    } else if (strcmp(params[0], "mode") == 0) {
        in_parallel = change_mode(params[1]);
    } else if (strcmp(params[0], "resume") == 0) {
        if (params[1] ==  NULL) {
            printf("resume takes in the process ID as an argument.\n");
        } else {
            pid_t pid = strtol(params[1], NULL, 10);
            if (pid == 0) {
                printf("Invalid process id.\n");
                return;
            }
            if (kill(pid, SIGCONT) == 0)
                set_process_state(pid, "running");
        }
    } else if (strcmp(params[0], "pause") == 0) {
        if (params[1] ==  NULL) {
            printf("pause takes in the process ID as an argument.\n");
        } else {
            pid_t pid = strtol(params[1], NULL, 10);  
            if (pid == 0) {
                printf("Invalid process id.\n");
                return;
            }
            if (kill(pid, SIGSTOP) == 0)
                set_process_state(pid, "paused");
        }
    } else if (strcmp(params[0], "exit") == 0) {
        if (_inc_jobs(0) > 0) {
            printf("You cannot exit while there are processes running.\n");
        }
        else
            do_exit = true;
    } else {
        if (mode == SEQUENTIAL)
            system(buffer);
        else {
            //might need to pass in commands for this to be fully functional
            int comm = 0;
            int arguments = 0;
            if (params[0] != NULL) {
                comm = strlen(params[0]);
            }
            if (params[1] != NULL) {
                arguments = strlen(params[1]);
            }
            char* tmp = calloc(comm + arguments + 5, sizeof(char));
            // add ampersand to make parallel
            strcat(tmp, params[0]);
            strcat(tmp, " ");
            if (arguments > 0)
                strcat(tmp, params[1]);
            strcat(tmp, " &");
            system(tmp);
            free(tmp);
            shell_printed = false;
            //show_prompt(); //built_in commands don't return in signal
        }
    }
    return;
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
    char path_env[1035];
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

void set_process_state (pid_t pid, const char* set_state) {
    processes* current = head_jobs;
    while (current != NULL && current->id != pid) {
        current = current->next;
    }
    
    if (current == NULL) {
        printf("Could not find the job with id: %d.\n", pid);
        return;
    }
    
    processes** tmp = &current;
    if (strcmp(set_state, "paused") == 0) {
        printf("Job paused.\n");
        (*tmp)->process_state = PAUSED;
        return;
    }
        
    if (strcmp(set_state, "running") == 0) {
        printf("Job resumed.\n");
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
            strcpy(p_state, "DEAD");
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