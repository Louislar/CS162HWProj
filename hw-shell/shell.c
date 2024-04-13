#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

/* Foreground process group id for *this* shell 
 * -1 means no process running in foreground 
 *  We also record the foreground child pids */
pid_t fgPgid=-1;
pid_t* fgPids=NULL; 
int fgPidNum=0;

/* Background process group ids for *this* shell, its an array 
 * We limit the # of background process group to 1 
 * Initialization is in init_shell() */
pid_t bgPgid=-1;
pid_t* bgPids=NULL;
int bgPidNum=0;

/* Signals should be sent to foreground process */
int foreground_signals[5] = {SIGINT, SIGQUIT, SIGKILL, SIGTERM, SIGTSTP};

/* Signals should be sent to background process */
int background_signals[3] = {SIGCONT, SIGTTIN, SIGTTOU};

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_print(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);
int cmd_parentFgPgid(struct tokens* tokens);
int cmd_fg(struct tokens* tokens);
int cmd_fgPgid(struct tokens* tokens);
int cmd_bgPgid(struct tokens* tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_print, "cmdprint", "print the line"},
    {cmd_pwd, "pwd", "print current working directory"},
    {cmd_cd, "cd", "change current working directory"}, 
    {cmd_parentFgPgid, "pfgpgid", "print parent terminal's foreground pgid"},
    {cmd_fg, "fg", "resumes a paused program"},
    {cmd_fgPgid, "fgpgid", "print current shell's foreground pgid"},
    {cmd_bgPgid, "bgpgid", "print current shell's background pgid"},
};

/* Change current working directory */ 
int cmd_cd(struct tokens* tokens) {
  chdir(tokens_get_token(tokens, 1));
  return 1;
}

/* Print current working directory */ 
int cmd_pwd(struct tokens* tokens) {
  long size = 4096;
  char* buf;
  buf = (char*) malloc((size_t)size);
  getcwd(buf, (size_t)size);
  printf("%s\n", buf);

  return 1;
}

/* Prints the tokens input to the terminal in a single line */
int cmd_print(struct tokens* tokens) {
  for(unsigned i = 0; i < tokens_get_length(tokens); ++i) {
    printf("%s\n", tokens_get_token(tokens, i));
  }
  return 1;
}


/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Prints parent terminal's foreground process group id */
int cmd_parentFgPgid(unused struct tokens* tokens) {
  printf("%d\n", tcgetpgrp(STDIN_FILENO));
  return 1;
}

/* Resume the background process group to foreground */
int cmd_fg(unused struct tokens* tokens) {
  if(bgPgid>0) {
    fgPgid=bgPgid;
    fgPids=bgPids;
    fgPidNum=bgPidNum;
    bgPgid=-1;
    bgPids=NULL;
    bgPidNum=0;

    int sig_result = killpg(fgPgid, SIGCONT);
    if(sig_result == -1) {
      perror("killpg (SIGCONT)");
      return 1;
    }
    tcsetpgrp(0, fgPgid);

    // I need to wait all the resumed process finished
    int status;
    for(unsigned int i=0;i<fgPidNum;++i) {
      int return_pid = waitpid(fgPids[i], &status, WUNTRACED);
      printf("After waiting pid: %d\n", fgPids[i]);
      printf("status: %d\n", status);
      printf("return pid: %d\n", return_pid);
      if(return_pid == -1) perror("waitpid");
    }

    /* If child processes are terminated not stopped, clean their pids 
     * If they're stopped, move them to background */
    if(WIFEXITED(status) || WIFSIGNALED(status)) {
      fgPgid = -1;
      free(fgPids);
      fgPidNum=0;
    }
    else if(WIFSTOPPED(status)) {
      bgPgid=fgPgid;
      bgPids = fgPids;
      bgPidNum=fgPidNum;
      fgPgid=-1;
      fgPids=NULL;
      fgPidNum=0;
    }
    tcsetpgrp(0, shell_pgid);
  }
  return 1;
}

/* print foreground process group id */
int cmd_fgPgid(unused struct tokens* tokens) {
  printf("%d\n", fgPgid);
  return 1;
}

/* print background process group id */
int cmd_bgPgid(unused struct tokens* tokens) {
  printf("%d\n", bgPgid);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Find specific symbol (e.g. '>' or '<' or '|') from tokens 
 * Return found symbol indices in a array. 
 * If not finding the symbol return -1 */
int find_symbol_from_tokens(struct tokens* tokens, int start_index, char* const symbol) {
  if(!symbol) return -1;
  int symbol_index = -1;
  for(unsigned int i = start_index; i<tokens_get_length(tokens); ++i) {
    char* const a_token = tokens_get_token(tokens, i);
    if(strcmp(a_token, symbol) == 0) { symbol_index = i; break; }
  }
  return symbol_index;
}

/* Find accessible file path from PATH 
 * Input: string of a file path
 * Return NULL if not finding a valid file path, otherwise return the found path  
 * */
char* find_file_path(char* const filepath) {
  /* Use getenv(PATH) to get the PATH environment variable */
  char* pathenv = malloc(strlen(getenv("PATH")));
  strcpy(pathenv, getenv("PATH"));
  const char* const delim = ":";
  char* saveptr = NULL;
  char* result = NULL;

  // printf("%s\n", pathenv);
  pathenv = strtok_r(pathenv, delim, &saveptr);
  while(pathenv) {
    /* See if the file exist in this directory */
    char* dup_path = malloc(strlen(pathenv) + strlen(filepath) + 1);
    strcpy(dup_path, pathenv);
    strcat(dup_path, "/");
    strcat(dup_path, filepath);
    if(access(dup_path, F_OK) != -1) {
      // printf("File exist: %s\n", dup_path);
      result = dup_path;
      break;
    }
    else {
      // printf("File doesn't exist: %s\n", dup_path);
    }

      pathenv = strtok_r(NULL, delim, &saveptr);
  }
  return result;
}

/* Check if every process in a specific process group are all terminated */
bool isProcessAlive(int pgid_in){
  int status;
  pid_t wait_return = waitpid(-pgid_in, &status, WNOHANG);
  int return_error = errno;
  if(wait_return == -1){
    perror("waitpid (isProcessAlive)");
    if(return_error == ECHILD) printf("No background child process found\n");
    return false;
  }
  else if(wait_return == 0) return true;
  else { // if it returns a valid pid
    return true;
  }
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);

    /* Change the signal handler of shell process to ignore */
    for(unsigned int i=0;i<5;++i) {
      struct sigaction* tmp = (struct sigaction*) malloc(sizeof(struct sigaction));
      tmp->sa_flags = SA_RESTART;
      tmp->sa_handler = SIG_IGN;
      sigaction(foreground_signals[i], tmp, NULL);
    }
    for(unsigned int i=0;i<3;++i) {
      struct sigaction* tmp = (struct sigaction*) malloc(sizeof(struct sigaction));
      tmp->sa_flags = SA_RESTART;
      tmp->sa_handler = SIG_IGN;
      sigaction(background_signals[i], tmp, NULL);
    }
    /* Shell process need to deal with SIGCHLD, 
     * not making the terminated child processes into zombie by using the SA_NOCLDWAIT option */
    struct sigaction* tmp = (struct sigaction*) malloc(sizeof(struct sigaction));
    tmp->sa_flags = SA_RESTART|SA_NOCLDWAIT;
    tmp->sa_handler = SIG_DFL;
    sigaction(SIGCHLD, tmp, NULL);

  } // if(shell_is_interactive)
}

/* A task in command line with program's path, process_id, redirection FILE pointers */
struct command_task{
pid_t pid;
pid_t pgid;
int redirect_in;
int redirect_out;
char** args;  // including programs path at the first element, and a NULL at the last element
int args_len;
};

/* Initialize command_task structure */ 
void init_command_task(struct command_task* com_task) {
  com_task->pid = -1;
  com_task->pgid = -1;
  com_task->redirect_in = -1;
  com_task->redirect_out = -1;
  com_task->args = NULL;
  com_task->args_len = 0;
}

/* Parse tokens into command line task struct 
 * The end_index is pointing to position next to the last element 
 * Return NULL if error occurs while parsing */
struct command_task* tokens_to_task(struct tokens* tokens, int start_index, int end_index) {
  struct command_task* com_task = (struct command_task*) malloc(sizeof(struct command_task));
  init_command_task(com_task);
  // Fetch commands until pipe symbol and be aware of the redirection symbols 
  // The length of command may need to be adjust by realloc() after removing > and < 
  com_task->args = malloc(sizeof(char*) * (end_index - start_index));
  com_task->args_len = 0;
  // if there's nothing after redirection symbol, return error 
  for(unsigned int i = start_index; i < end_index; ++i) {
    char* cur_token = tokens_get_token(tokens, i);
    char* next_token = tokens_get_token(tokens, i+1);

    if(strcmp(cur_token, ">") != 0 && strcmp(cur_token, "<") != 0) {
      com_task->args[com_task->args_len] = cur_token;
      ++com_task->args_len;
    }
    else if(strcmp(cur_token, ">") == 0) {
      // If next symbol is '|', '>', '<', or nothing's over there, return error
      if(i+1 < end_index && 
          strcmp(next_token, "|") != 0 && 
          strcmp(next_token, ">") != 0 && 
          strcmp(next_token, "<") != 0) {
        com_task->redirect_out = open(next_token, O_WRONLY|O_CREAT|O_CLOEXEC);
      }
      else {
        free(com_task->args);
        free(com_task);
        return NULL; // This is a invalid command, 
                        // need to let user enter another command again in the parent process
      }
      ++i;
    }
    else if(strcmp(cur_token, "<") == 0) {
      if(i+1 < end_index && 
          strcmp(next_token, "|") != 0 && 
          strcmp(next_token, ">") != 0 && 
          strcmp(next_token, "<") != 0) {
        com_task->redirect_in = open(next_token, O_RDONLY|O_CLOEXEC);
        if(com_task->redirect_in == -1) { // if no such file, return NULL and print error code out
          perror(next_token);
          return NULL;
        }
      }
      else {
        free(com_task->args);
        free(com_task);
        return NULL;
      }
      ++i;
    }
  } // for loop end
  //  If it is a empty command, its still a invalid command (this is what I defined)
  if(com_task->args_len == 0) return NULL;
  // Not every token is in args array, thus the length can be smaller  
  char** tmp_command = realloc(com_task->args, sizeof(char*) * (com_task->args_len+1));
  if(!tmp_command) {perror("realloc failed"); exit(0);}
  com_task->args = tmp_command;
  com_task->args[com_task->args_len] = NULL; // execv needs the last arg being NULL 

  return com_task;
}

/* Create a child process by giving args
 * The argument is a struct contains redirection pointers and other necessary info of a program  
 * Use dup2 to redirect stdin and stdout
 * Return pid of the created child process */
pid_t create_process_and_exec(struct command_task* com_task) {
  pid_t cpid; // child pid
  cpid = fork();
  if(cpid > 0) {
    /* parent process */
    /* wait for the child process to finish */
    // wait(&status);
    // Not using this is because we want to create multiple processes 
    // and try to wait all of them, and allowing them to run concurrently 
  }
  else if(cpid == 0) {
    /* child process */
    /* Run a new process image by using a execv() system call */
    /* change all the signal handlers to default handler */
    for(unsigned int i=0;i<5;++i) {
      struct sigaction* tmp = (struct sigaction*) malloc(sizeof(struct sigaction));
      tmp->sa_handler = SIG_DFL;
      sigaction(foreground_signals[i], tmp, NULL);
    }
    for(unsigned int i=0;i<3;++i) {
      struct sigaction* tmp = (struct sigaction*) malloc(sizeof(struct sigaction));
      tmp->sa_flags = SA_RESTART;
      tmp->sa_handler = SIG_DFL;
      sigaction(background_signals[i], tmp, NULL);
    }

    if(com_task->pgid > 0) setpgid(getpid(), com_task->pgid);
    else setpgid(getpid(), getpid());
    if(com_task->redirect_in >= 0)
      dup2(com_task->redirect_in, STDIN_FILENO);
    if(com_task->redirect_out >= 0)
      dup2(com_task->redirect_out, STDOUT_FILENO);
    execv(com_task->args[0], com_task->args);
    perror("execv");
    exit(0);
  }
  else {
    perror("Fork failed");
  }
  return cpid;
}

int main(unused int argc, unused char* argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    /* Before running any command, check if all the background processes are terminated
     * If it is, then modify the corresponding variables, e.g. bgPgid */
    if(bgPgid!=-1 && !isProcessAlive(bgPgid)) {
      bgPgid = -1;
      free(bgPids);
      bgPidNum=0;
    }


    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* REPLACE this to run commands as programs. */

      /* Command parsing should be here 
       * Since the child processes are created after parsing the command */ 

      struct command_task** all_com_tasks = malloc(512 * sizeof(struct command_task*));
      int com_tasks_count = 0;
      bool is_commands_valid = true;
      bool is_command_background = false;

      
      /* Separate by pipe symbol first, 
       * and each of the separated command will be executed in a different child process. 
       * The resulting command and corresponding stdin/stdout redirection 
       * will be stored in a data structure 'command_task'
       * The process before PIPE and after PIPE need to redirect their stdout to stdin */
      int next_proc_redirect_stdin_fd = -1;
      int pipe_index = find_symbol_from_tokens(tokens, 0, "|");
      int cur_index = 0;
      while(pipe_index != -1) {
        struct command_task* com_task = 
          tokens_to_task(tokens, cur_index, pipe_index);
        if(com_task) {
          printf("Length: %d\n", com_task->args_len);
          for(int i=0;i<com_task->args_len;++i) printf("%s\n", com_task->args[i]);
          printf("=== next process ===\n");
          all_com_tasks[com_tasks_count] = com_task;
          ++com_tasks_count;
        }
        else {
          // When error occurs while parsing, need to allow user to enter next command, 
          // just like nothing happened
          is_commands_valid = false;
          break;
        }

        // PIPE setting
        int pipefd[2];
        if(pipe(pipefd) == -1) perror("pipe");
        com_task->redirect_in = next_proc_redirect_stdin_fd;
        com_task->redirect_out = pipefd[1];
        next_proc_redirect_stdin_fd = pipefd[0];
        if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) == -1) perror("fcntl");
        if (fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == -1) perror("fcntl");


        cur_index = pipe_index + 1;
        pipe_index = find_symbol_from_tokens(tokens, pipe_index+1, "|");
      } // while pipe_index != -1 loop end

      // Do above process again, 
      // this is for the command that does not end with a pipe symbol
      struct command_task* com_task = 
        tokens_to_task(tokens, cur_index, (int) tokens_get_length(tokens));
      if(com_task) {
        printf("Length: %d\n", com_task->args_len);
        for(int i=0;i<com_task->args_len;++i) printf("%s\n", com_task->args[i]);
        printf("=== next process ===\n");
        all_com_tasks[com_tasks_count] = com_task;
        ++com_tasks_count;

        // PIPE setting 
        com_task->redirect_in = next_proc_redirect_stdin_fd;
      }
      else is_commands_valid = false;

      /* check if the last character is '&' 
       * ignore it when executing the command, don't count it as a argument */
      if(com_task->args[com_task->args_len-1][0] == '&') {
        is_command_background=true;
        com_task->args[com_task->args_len-1] = NULL;
      }
      if(is_command_background) 
        printf("This is a background command\n");

      // Every separated command's path needs to be checked and need to do path resolution, 
      // after that create a child process to execute it. 

      // If any path resolution failed, do not execute any program and allows user to type in next command.
      for(int i=0;i<com_tasks_count;++i) {
        struct command_task* com_task = all_com_tasks[i];
        char* program_path = com_task->args[0];
        if(access(program_path, F_OK) == -1) 
          program_path = find_file_path(program_path);
      
        if(!program_path) {
          printf("%s: Program file not found\n", com_task->args[0]);
          is_commands_valid = false;
          //exit(0);
        }
        else {
          printf("Program file found at %s\n", program_path);
          com_task->args[0] = program_path;
        }
      }
      
      /* Execute the commands only after parsing all the commands and check there're all valid */
      if(is_commands_valid) {
        for(int i=0;i<com_tasks_count;++i) {
          // create process and execute 
          all_com_tasks[i]->pgid = all_com_tasks[0]->pid;
          all_com_tasks[i]->pid = create_process_and_exec(all_com_tasks[i]);

          /* All the PIPE's need to be closed here, otherwise child porcesses will never ends 
           * Also freeing all the resources in the data structure command_task */
          struct command_task* com_task = all_com_tasks[i];
          if(com_task->redirect_in) close(com_task->redirect_in);
          if(com_task->redirect_out) close(com_task->redirect_out);
        }

        // If this is a background process, then it must record in bgPid and bgPgid
        if(is_command_background) {
          bgPgid = all_com_tasks[0]->pid;
          bgPidNum = com_tasks_count;
          bgPids = (pid_t*) malloc(bgPidNum * sizeof(pid_t));
          for(unsigned int i=0;i<bgPidNum;++i) {
            *(bgPids+i) = all_com_tasks[i]->pid;
          }
          printf("Current background group id: %d\n", bgPgid);
        }
        else {
          fgPgid = all_com_tasks[0]->pid;
          // record all foreground pids
          fgPidNum = com_tasks_count;
          fgPids = (pid_t*) malloc(fgPidNum * sizeof(pid_t));
          for(unsigned int i=0;i<fgPidNum;++i) {
            *(fgPids+i) = all_com_tasks[i]->pid;
          }
          printf("Current foreground group id: %d\n", fgPgid);

          /* Switch current foreground pgid */
          tcsetpgrp(0, fgPgid);

          /* Wait all the child processes to terminate */
          int status=0;
          for(int i=0;i<com_tasks_count;++i) {
            /* WUNTRACED flag means this will return when child process stops */
            pid_t return_pid = waitpid(all_com_tasks[i]->pid, &status, WUNTRACED);
            printf("After waiting pid: %d\n", all_com_tasks[i]->pid);
            printf("status: %d\n", status);
            printf("return pid: %d\n", return_pid);
            if(return_pid == -1) perror("waitpid");
          }
          
          // When the child processes are terminated not stopped, clean the forground pids
          // If processes are stopped, they are going to be moved to background pids,
          if(WIFEXITED(status) || WIFSIGNALED(status)) {
            free(fgPids);
            fgPidNum=0;
            fgPgid=-1;
          }
          else if(WIFSTOPPED(status)) {
            bgPgid=fgPgid;
            bgPids = fgPids;
            bgPidNum=fgPidNum;
            fgPgid=-1;
            fgPids=NULL;
            fgPidNum=0;
          }
          tcsetpgrp(0, shell_pgid);
        } // if !is_command_background
      } // if commands_is_valid

        
      // Free all the child processes' resources in the struct
      // The strings in args don't need to be freed, since we're just pointing them to the original tokens
      for(unsigned int i=0;i<com_tasks_count;++i) {
        free(all_com_tasks[i]);
      }

      // fprintf(stdout, "This shell doesn't know how to run programs.\n");
    } // if fundex < 0 end

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
