// for struct sigaction for signal handling
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>

struct Command {
  bool exit;
  bool change_directory;
  bool status;
  bool other_command;
  bool input_redirect;
  bool output_redirect;
  // as per the requirements
  char arguments[2048];
  char input_file[256];
  char output_file[256];
  bool background;
  bool process_id_called;
  bool background_processes_allowed;
  bool echo_command;
};

struct BackgroundPIDs {
  int size;
  pid_t pids[100];
};

struct Status {
  bool fg_process_status;
  pid_t fg_process_pid;
  bool fg_process_exit;
  bool fg_process_terminated;
  int fg_process_exit_or_term_reason;
};

void print_to_console(char string_text[]);
char* get_input_from_user();
void lower_case_string(char string_text[]);
void assign_user_values_to_command_struct(char text_string[], struct Command *command_ptr);
bool is_blank(char text_string[]);
bool set_exit_flag(char* token_ptr, struct Command *command_ptr);
bool set_change_directory_flag(char* token_ptr, struct Command *command_ptr);
bool set_status_flag(char* token_ptr, struct Command *command_ptr);
bool set_output_file(char* token_ptr, struct Command *command_ptr);
bool set_input_file(char* token_ptr, struct Command *command_ptr);
bool set_background_flag(char* token_ptr, struct Command *command_ptr);
bool set_process_id_called_flag(char* token_ptr, struct Command *command_ptr);
void remove_newline_from_string(char string_text[]);
void log_command_struct(struct Command *command_ptr);
void initialize_command_struct(struct Command *command_ptr);
void set_other_command_and_arguments(char* token_ptr, struct Command *command_ptr);
bool check_if_token_is_actually_a_test_comment(char* token_ptr, struct Command *command_ptr);
void change_directory(struct Command *command_ptr);
void execute_command(struct Command *command_ptr, struct BackgroundPIDs *background_pids, struct Status *status_ptr);
void set_output_redirect_fg(char* file_name_ptr);
void set_input_redirect_fg(char* file_name_ptr);
void set_output_redirect_bg();
void set_input_redirect_bg();
void set_any_redirects(struct Command *command_ptr);
void create_arguments_array(char arguments[2048], char* arg_array[512]);
void initialize_background_pids_struct(struct BackgroundPIDs *background_pids);
void reap_terminated_child_processes(struct BackgroundPIDs *background_pids);
void print_foreground_process_status(struct Status *status);
void set_ignore_sigint();
void set_default_sigint();
void set_sigtstp_handler();
void SIGTSTP_handler(int signo);
void set_ignore_sigtstp();
void child_process_ignore_sigtstp();
int perform_variable_expansion(char argument_str[], char* new_str);
bool set_echo_command(char* token_ptr, struct Command *command_ptr);

bool turn_off_background = false;
bool SIGTSTP_called = false;
pid_t smallsh_pid;

int main() {
  smallsh_pid = getpid();
  
  // set IGNORE signal handler for SIG_INT
  set_ignore_sigint();

  // set custom behavior for SIGTSTP
  set_sigtstp_handler();

  // used to keep track of background processes
  struct BackgroundPIDs background_pids;
  initialize_background_pids_struct(&background_pids);

  // keeps track of foreground exit statuses
  struct Status status;
  status.fg_process_status = false;
  
  bool keep_console_on = true;
  while(keep_console_on) {

    // Command struct abstracts user entry string from
    // all of the various built-in commands
    // provides a safer way to start and stop commands
    struct Command *command_ptr;
    
    // using dynamic allocation here because otherwise
    // the command_ptr doesn't reset with each loop
    command_ptr = malloc(sizeof(struct Command));
    initialize_command_struct(command_ptr);
    
    // manage bg processes
    if (background_pids.size) {
      reap_terminated_child_processes(&background_pids);
    }
    
    // get input from user and verify it has no errors
    // SIGTSTP was causing errors in stdin - this should avoid that!
    bool input_error = false;
    char* input_text_ptr;
    do {
      // for SIGTSTP signals
      if (SIGTSTP_called) {
        if (turn_off_background) {
          printf("\nEntering foreground-only mode (& is now ignored)\n");
          fflush(stdout);
          command_ptr->background_processes_allowed = false;
        } else if (!turn_off_background) {
          printf("\nExiting foreground-only mode\n");
          fflush(stdout);
          command_ptr->background_processes_allowed = true;
        }
        SIGTSTP_called = false;
      }

      print_to_console(": ");
      input_text_ptr = get_input_from_user();
      
      // checking if stdin has errors
      // weird characters cause the program to misfire invalid commands!
      if (ferror(stdin)) {
        clearerr(stdin);
        free(input_text_ptr);
        input_error = true;
      } else {
        input_error = false;
      }
    } while (input_error);

    remove_newline_from_string(input_text_ptr);
    assign_user_values_to_command_struct(input_text_ptr, command_ptr);

    // checking if '$$' was used in command, if so, change string to PID #
    if (command_ptr->process_id_called) {
      // store the altered string in a new string array,
      // then once the process is complete, erase whatever is
      // in command_ptr->arguments and string copy the altered string
      // into command_ptr->arguments.
      char new_test_str[strlen(command_ptr->arguments) + 7];
      memset(new_test_str, '\0', sizeof(new_test_str));

      // keep calling the perform_variable_expansion function
      // if the function returns 0, which means there are
      // additional characters to check after the first '$$' was found
      int result = 0;
      while (result == 0) {
        result = perform_variable_expansion(command_ptr->arguments, new_test_str);
        if (result == 0) {
          memset(command_ptr->arguments, '\0', sizeof(command_ptr->arguments));
          strcpy(command_ptr->arguments, new_test_str);
          memset(new_test_str, '\0', sizeof(new_test_str));

        } else {
          // anything other than 0 is returned, means there are no
          // more characters to check for '$$'
          break;
        }
      }
      // store the newly altered string into command_ptr->arguments
      memset(command_ptr->arguments, '\0', sizeof(command_ptr->arguments));
      strcpy(command_ptr->arguments, new_test_str);
    }

    // handle comment lines
    if (command_ptr->arguments[0] == '#') {
      continue;

    // handle blank lines
    } else if (is_blank(input_text_ptr)) {
      continue;

    // handle change directory call
    } else if (command_ptr->change_directory) {
      change_directory(command_ptr);

    // handle status call
    } else if (command_ptr->status) {
      // prints out either the exit status or
      // the terminating signal of the last foreground process ran by your shell.
      // If this command is run before any foreground command is run,
      // then it should simply return the exit status 0.
      if (status.fg_process_status) {
        print_foreground_process_status(&status);
      } else {
        printf("No status set: 0\n");
        fflush(stdout);
      }

    // exit while loop if user says so
    } else if (command_ptr->exit) {
      // release resources
      free(input_text_ptr);
      free(command_ptr);
      // terminate all child background processes
      if (background_pids.size) {
        int sig = 15;
        for (int i = 0; i < background_pids.size; i++) {
          kill(background_pids.pids[i], sig);
        }
      }
      break;

    } else {
      // anything that doesn't match the above conditions,
      // means its probably a command to execute!
      execute_command(command_ptr, &background_pids, &status);
    }
    // release resources
    free(input_text_ptr);
    free(command_ptr);
  }

  return 0;
}

void execute_command(
  struct Command *command_ptr,
  struct BackgroundPIDs *background_pids,
  struct Status *status_ptr
) {
  int child_status;
  pid_t spawn_pid = fork();

  switch(spawn_pid) {
    case -1: {
      perror("fork()\n");
      exit(1);
      break;
    }
    case 0: {
      // In the child process now     
      // setting any redirects for fg and bg commands
      set_any_redirects(command_ptr);

      // default SIGINT behavior only for foreground processes
      if (!command_ptr->background) {
        set_default_sigint();
      }

      // creating the command for execution with exec
      // as per the requirements, we can take up to 512 arguments
      char* arg_array[512];
      create_arguments_array(command_ptr->arguments, arg_array);

      // execute it!
      int status_code = execvp(arg_array[0], arg_array);
      // this piece only runs if a failure happens in exec
      if (status_code < 0) {
        perror("execvp");
        exit(EXIT_FAILURE);
      }
      break;
    }
    default: {     
      // check if process is a background process 
      if (command_ptr->background && command_ptr->background_processes_allowed) {
        fflush(stdout);
        // add the child's pid to the background_pids array
        background_pids->pids[background_pids->size] = spawn_pid;
        // print out something helpful similar to bash
        printf(
          "[%d] %d\n",
          (background_pids->size + 1),
          background_pids->pids[background_pids->size]
        );
        fflush(stdout);
        // keep track of the size of the background_pids_array
        background_pids->size += 1;
      
      // if not a background process, handle normally
      } else {
        spawn_pid = waitpid(spawn_pid, &child_status, 0);
        if (spawn_pid == -1) {
          if (WIFSIGNALED(child_status)) {
            printf("waitpid() interrupted: term signal %d\n", WTERMSIG(child_status));
            fflush(stdout);
          }
        } 
        if (WIFEXITED(child_status)) {
          status_ptr->fg_process_status = true;
          status_ptr->fg_process_pid = spawn_pid;
          status_ptr->fg_process_exit = true;
          status_ptr->fg_process_terminated = false;
          status_ptr->fg_process_exit_or_term_reason = WEXITSTATUS(child_status);
          // printf("Child %d exited normally with status %d\n", spawn_pid, WEXITSTATUS(child_status));
          // fflush(stdout);

        } else {
          status_ptr->fg_process_status = true;
          status_ptr->fg_process_pid = spawn_pid;
          status_ptr->fg_process_exit = false;
          status_ptr->fg_process_terminated = true;
          status_ptr->fg_process_exit_or_term_reason = WTERMSIG(child_status);
          // printf("Child %d exited abnormally due to signal %d\n", spawn_pid, WTERMSIG(child_status));
          // fflush(stdout);
        }
      }      
    }
  }
}

void print_foreground_process_status(struct Status *status) {
  if (status->fg_process_exit) {
    printf(
      "Child PID=%d | Exit Status: %d\n",
      status->fg_process_pid,
      status->fg_process_exit_or_term_reason
    );
    fflush(stdout);
  } else if (status->fg_process_terminated) {
    printf(
      "Child PID=%d | Abnormal Termination Status: %d\n",
      status->fg_process_pid,
      status->fg_process_exit_or_term_reason
    );
    fflush(stdout);
  }
}

void reap_terminated_child_processes(struct BackgroundPIDs *background_pids) {
  int to_be_removed[background_pids->size];
  int to_be_removed_count = 0;
  
  // for every bg PID captured in the background_pid array,
  // perform a non-blocking waitpid call
  // if waitpid returns 0, the process is still working on its thing
  // else reap the stopped/term'd child process and keep track of
  // removals via the to_be_removed array
  for (int i = 0; i < background_pids->size; i++) {
    // printf("Checking if [%d] %d is still alive.\n", (i + 1), background_pids->pids[i]);
    // fflush(stdout);
    
    int child_status;
    
    pid_t child_pid = waitpid(background_pids->pids[i], &child_status, WNOHANG);
    if (child_pid < 0) {
      perror("background PID waitpid()");
      exit(1);
    } else if (child_pid == 0) {
      // printf("Child process [%d] %d still working.\n", (i + 1), background_pids->pids[i]);
      // fflush(stdout);
      continue;

    } else {
      printf("Background pid [%d] %d is done: ", (i + 1), background_pids->pids[i]);
      fflush(stdout);
      to_be_removed[to_be_removed_count] = i;
      to_be_removed_count += 1;
      if (WIFEXITED(child_status)) {
        printf("exit value %d\n", WEXITSTATUS(child_status));
        fflush(stdout);
      } else {
        printf("terminated by signal %d\n", WTERMSIG(child_status));
        fflush(stdout);
      }
    }
  }

  // for every index captured in the to_be_removed array,
  // get the index and shift the bg pids array to skip over the index
  for (int i = 0; i < to_be_removed_count; i++) {
    int index_for_removal = to_be_removed[i];
    for (; index_for_removal < background_pids->size; index_for_removal++) {
      background_pids->pids[index_for_removal] = background_pids->pids[index_for_removal + 1];
    }
    // keep track of the number of removed bg pids
    background_pids->size -= 1;
  }
  return;
}

void initialize_background_pids_struct(struct BackgroundPIDs *background_pids) {
  background_pids->size = 0;
}

void set_any_redirects(struct Command *command_ptr) {
  if (!command_ptr->background) {
    if (command_ptr->output_redirect) {
      set_output_redirect_fg(command_ptr->output_file); 
    }
    if(command_ptr->input_redirect) {
      set_input_redirect_fg(command_ptr->input_file);
    }
  } else {
    if (command_ptr->output_redirect) {
      set_output_redirect_bg();
    }
    if (command_ptr->input_redirect) {
      set_input_redirect_bg();
    }
  }
}

void set_output_redirect_bg() {
  int output_fd = open("/dev/null", O_WRONLY);
  if (output_fd == -1) {
    perror("output_fd_bg open()");
    exit(6);
  }
  int result = dup2(output_fd, STDOUT_FILENO);
  if (result == -1) {
    perror("output_fd_bg dup2()");
    exit(7);
  }
}

void set_input_redirect_bg() {
  int input_fd = open("/dev/null", O_RDONLY);
  if (input_fd == -1) {
    perror("input_fd_bg open()");
    exit(8);
  }
  int result = dup2(input_fd, STDIN_FILENO);
  if (result == -1) {
    perror("input_fd_bg dup2()");
    exit(9);
  }
}

void set_output_redirect_fg(char* file_name_ptr) {
  int output_fd = open(file_name_ptr, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (output_fd == -1) {
    perror("output_fd_fg open()");
    exit(2);
  }
  int result = dup2(output_fd, STDOUT_FILENO);
  if (result == -1) {
    perror("output_fd_fg dup2()");
    exit(3);
  }
}

void set_input_redirect_fg(char* file_name_ptr) {
  int input_fd = open(file_name_ptr, O_RDONLY);
  if (input_fd == -1) {
    perror("input_fd open()");
    exit(4);
  }
  int result = dup2(input_fd, STDIN_FILENO);
  if (result == -1) {
    perror("input_fd dup2()");
    exit(5);
  }
}

void create_arguments_array(char arguments[2048], char* arg_array[512]) {
  char* token_ptr = strtok(arguments, " ");
  int index = 0;
  while (token_ptr != NULL) {
    arg_array[index] = token_ptr;
    index += 1;
    token_ptr = strtok(NULL, " ");
  }
  arg_array[index] = NULL;
}

void change_directory(struct Command *command_ptr) {
  char pathname[2048];
  strcpy(pathname, command_ptr->arguments);
  if (strlen(pathname) > 0) {
    chdir(pathname);
  } else {
    char *HOME_env = getenv("HOME");
    chdir(HOME_env);
  }
  // testing purposes
  /*
  char current_directory[2048];
  getcwd(current_directory, 2048);
  printf("getcwd() =%s\n", current_directory);
  fflush(stdout);
  */
}

void initialize_command_struct(struct Command *command_ptr) {
  command_ptr->exit = false;
  command_ptr->change_directory = false;
  command_ptr->status = false;
  command_ptr->other_command = false;
  command_ptr->input_redirect = false;
  command_ptr->output_redirect = false;
  memset(command_ptr->arguments, '\0', sizeof(command_ptr->arguments));
  memset(command_ptr->input_file, '\0', sizeof(command_ptr->input_file));
  memset(command_ptr->output_file, '\0', sizeof(command_ptr->output_file));
  command_ptr->background = false;
  command_ptr->process_id_called = false;
  if (turn_off_background) {
    command_ptr->background_processes_allowed = false;
  } else if (!turn_off_background) {
    command_ptr->background_processes_allowed = true;
  }
  command_ptr->echo_command = false;
}

void print_to_console(char string_text[]) {
  printf(string_text);
  fflush(stdout);
}

char* get_input_from_user() {
  // as per the requirements, set to capture 2048 characters
  int BUFFER_SIZE = 2048;
  char* input_buffer = malloc(sizeof(char) * BUFFER_SIZE);
  
  fgets(input_buffer, BUFFER_SIZE, stdin);

  return input_buffer;
}

void remove_newline_from_string(char string_text[]) {
  int result = strlen(string_text);
  string_text[result - 1] = '\0';
  return;
}

void assign_user_values_to_command_struct(char text_string[], struct Command *command_ptr) {
  // Here we check if the different commands/arguments in the input string
  // match certain scenarios that we want to catch
  // if they match, update the command struct so it can relay the info 
  // to other pieces of code
  char* token_ptr = strtok(text_string, " ");
  for (; token_ptr != NULL;) {
    // return from these immediately because they're fairly
    // self contained
    if (set_echo_command(token_ptr, command_ptr)) {
      while (token_ptr != NULL) {
        set_other_command_and_arguments(token_ptr, command_ptr);
        token_ptr = strtok(NULL, " ");
      }
      return;
    }
    if (set_exit_flag(token_ptr, command_ptr))
      return;
    if (set_change_directory_flag(token_ptr, command_ptr)) {
      token_ptr = strtok(NULL, " ");
      if (token_ptr) {
        set_process_id_called_flag(token_ptr, command_ptr);
        strcpy(command_ptr->arguments, token_ptr);
      }
      return;
    }
    if (set_status_flag(token_ptr, command_ptr))
      return;
    // for the p3testscript file
    if (check_if_token_is_actually_a_test_comment(token_ptr, command_ptr))
      return;

    // We dont want to immediately return from these,
    // because they're used in combination with other arguments
    if (set_output_file(token_ptr, command_ptr)) {
      token_ptr = strtok(NULL, " ");
      continue;
    }
    if (set_input_file(token_ptr, command_ptr)) {
      token_ptr = strtok(NULL, " ");
      continue;
    }
    if (set_background_flag(token_ptr, command_ptr)) {
      token_ptr = strtok(NULL, " ");
      continue;
    }

    // if token doesnt match any of the qualifiers above, 
    // its probably a regular ol' command!
    set_other_command_and_arguments(token_ptr, command_ptr);
    // don't forget to capture calls for the shell process id - $$
    set_process_id_called_flag(token_ptr, command_ptr);
    
    // progress token_ptr forward
    token_ptr = strtok(NULL, " ");
  }
}

bool set_echo_command(char* token_ptr, struct Command *command_ptr) {
  if (strcmp(token_ptr, "echo") == 0) {
    command_ptr->echo_command = true;
    return true;
  } else {
    return false;
  }
}

bool check_if_token_is_actually_a_test_comment(char* token_ptr, struct Command *command_ptr) {
  if (token_ptr[0] == '(') {
    return true;
  } else {
    return false;
  }
}

void set_other_command_and_arguments(char* token_ptr, struct Command *command_ptr) {
  command_ptr->other_command = true;
  strcat(command_ptr->arguments, token_ptr);
  char* space = " ";
  strcat(command_ptr->arguments, space);
}

bool set_process_id_called_flag(char* token_ptr, struct Command *command_ptr) {
  if (strstr(token_ptr, "$$") != NULL) {
    command_ptr->process_id_called = true;
    return true;
  } else {
    return false;
  }
}

bool set_exit_flag(char* token_ptr, struct Command *command_ptr) {
  if (strcmp(token_ptr, "exit") == 0) {
    command_ptr->exit = true;
    return true;
  } else {
    return false;
  }
}

bool set_change_directory_flag(char* token_ptr, struct Command *command_ptr) {
  if (strcmp(token_ptr, "cd") == 0) {
    command_ptr->change_directory = true;
    /*
    token_ptr = strtok(NULL, " ");
    if (token_ptr) {
      strcpy(command_ptr->arguments, token_ptr);
    }
    */
    return true;
  } else {
    return false;
  }
}

bool set_status_flag(char* token_ptr, struct Command *command_ptr) {
  if (strcmp(token_ptr, "status") == 0) {
    command_ptr->status = true;
    return true;
  } else {
    return false;
  }
}

bool set_output_file(char* token_ptr, struct Command *command_ptr) {
  if (strcmp(token_ptr, ">") == 0) {
    token_ptr = strtok(NULL, " ");
    strcpy(command_ptr->output_file, token_ptr);
    command_ptr->output_redirect = true;
    return true;
  } else {
    return false;
  }
}

bool set_input_file(char* token_ptr, struct Command *command_ptr) {
  if (strcmp(token_ptr, "<") == 0) {
    token_ptr = strtok(NULL, " ");
    strcpy(command_ptr->input_file, token_ptr);
    command_ptr->input_redirect = true;
    return true;
  } else {
    return false;
  }
}

bool set_background_flag(char* token_ptr, struct Command *command_ptr) {
  if (strcmp(token_ptr, "&") == 0) {
    if (command_ptr->background_processes_allowed) {
      command_ptr->background = true;
    } else {
      command_ptr->background = false;
    }
    return true;
  } else {
    return false;
  }
}

bool is_blank(char text_string[]) {
  if (strlen(text_string) == 0) {
    return true;
  } else {
    return false;
  }
}

void set_ignore_sigint() {
  struct sigaction ignore_action = {0};
  ignore_action.sa_handler = SIG_IGN;
  sigfillset(&ignore_action.sa_mask);
  sigaction(SIGINT, &ignore_action, NULL);
}

void set_default_sigint() {
  struct sigaction default_action = {0};
  default_action.sa_handler = SIG_DFL;
  sigfillset(&default_action.sa_mask);
  default_action.sa_flags = 0;
  sigaction(SIGINT, &default_action, NULL);
}

void SIGTSTP_handler(int signo) {
  SIGTSTP_called = true;
  pid_t pid = getpid();
  if (pid == smallsh_pid && !turn_off_background) {
    // turn off ability to exec bg commands
    turn_off_background = true;
  } else if (pid == smallsh_pid && turn_off_background) {
    // turn on ability to exec bg commands
    turn_off_background = false;
  }
  return;
}

void set_sigtstp_handler() {
  struct sigaction SIGTSTP_action = {0};
  SIGTSTP_action.sa_handler = SIGTSTP_handler;
  sigfillset(&SIGTSTP_action.sa_mask);
  SIGTSTP_action.sa_flags = 0;
  sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

void set_ignore_sigtstp() {
  struct sigaction ignore_action = {0};
  ignore_action.sa_handler = SIG_IGN;
  sigfillset(&ignore_action.sa_mask);
  sigaction(SIGTSTP, &ignore_action, NULL);
}

void child_process_ignore_sigtstp() {
  /*********
  * Per Michael Slater's recommendation
  *********/
  struct sigaction saz = {0};
  sigset_t mask;
  sigfillset(&mask);
  sigdelset(&mask, SIGBUS);
  sigdelset(&mask, SIGFPE);
  sigdelset(&mask, SIGILL);
  sigdelset(&mask, SIGSEGV);
  saz.sa_mask = mask;
  saz.sa_handler = SIG_IGN;
  sigaction(SIGTSTP, &saz, NULL);
}

int perform_variable_expansion(char argument_str[], char* new_str) {
  // check if string has $$
  // else, copy existing string into new_str
  // function returns 0 if there's additional characters to check after '$$'
  // otherwise if '$$' are the last 2 characters in the string, return -1
  if(strstr(argument_str, "$$") != NULL) {
    // get the index of the first '$'
    int len = strlen(argument_str);
    int expansion_index = -1;
    for (int c = 0; c < len; c++) {
      if (argument_str[c] == '$') {
        expansion_index = c;
        break;
      }
    }

    // create char array to store the value of pid
    char pid_str[7];
    memset(pid_str, '\0', sizeof(pid_str));
    pid_t program_pid = getpid();
    // special way to write the pid_t to char array
    sprintf(pid_str, "%d", program_pid);

    // just double checking here that '$$' was found... really not necessary
    if (expansion_index >= 0) {
      // copy all of the characters just before '$$' into the new string
      strncpy(new_str, argument_str, expansion_index);
      
      // if there are additional characters after '$$',
      // place those additional characters into a buffer
      // then concat the pid string to the new string,
      // as well as the buffer string to the new string
      if ((expansion_index + 2) < len) {
        char buffer[(len + 1)];
        memset(buffer, '\0', (len + 1)*sizeof(char));
        
        int buffer_index = 0;
        for (int i = (expansion_index + 2); i < len; i++) {
          buffer[buffer_index] = argument_str[i];
          buffer_index += 1;
        }
        
        strcat(new_str, pid_str);
        strcat(new_str, buffer);
        return 0;

      } else {
        // if '$$' are the last 2 characters in the string,
        // simply add the pid string to the new string
        // return -1 to indicate there's nothing else left to check
        strcat(new_str, pid_str);
        return -1;
      }
    }
  } else {
    strcat(new_str, argument_str);
  }
  return -1;
}

void lower_case_string(char string_text[]) {
  int string_length = strlen(string_text);
  for (int i = 0; i < string_length; i++) {
    string_text[i] = tolower(string_text[i]);
  }
}

void log_command_struct(struct Command *command_ptr) {
  if (command_ptr->exit) {
    printf("command.exit=%d\n", command_ptr->exit);
    fflush(stdout);
  }
  if (command_ptr->change_directory) {
    printf("command.change_directory=%d\n", command_ptr->change_directory);
    fflush(stdout);
  }
  if (command_ptr->status) {
    printf("command.status=%d\n", command_ptr->status);
    fflush(stdout);
  }
  if (command_ptr->other_command) {
    printf("command.other_command=%d\n", command_ptr->other_command);
    fflush(stdout);
  }
  if (command_ptr->input_redirect) {
    printf("command.input_redirect=%d\n", command_ptr->input_redirect);
    fflush(stdout);
  }
  if (command_ptr->output_redirect) {
    printf("command.output_redirect=%d\n", command_ptr->output_redirect);
    fflush(stdout);
  }
  if (strlen(command_ptr->arguments) > 0) {
    printf("command.arguments=%s\n", command_ptr->arguments);
    fflush(stdout);
  }
  if (strlen(command_ptr->input_file) > 0) {
    printf("command.input_file=%s\n", command_ptr->input_file);
    fflush(stdout);
  }
  if (strlen(command_ptr->output_file) > 0) {
    printf("command.output_file=%s\n", command_ptr->output_file);
    fflush(stdout);
  }
  if (command_ptr->background) {
    printf("command.background=%d\n", command_ptr->background);
    fflush(stdout);
  }
  if (command_ptr->process_id_called) {
    printf("command.process_id_called=%d\n", command_ptr->process_id_called);
    fflush(stdout);
  }
  if (command_ptr->echo_command) {
    printf("command.echo_command=%d\n", command_ptr->echo_command);
    fflush(stdout);
  }
}