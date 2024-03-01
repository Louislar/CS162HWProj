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

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_print(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);

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
  }
}

/* A task in command line with program's path, process_id, redirection FILE pointers */
struct command_task{
pid_t pid;
FILE* redirect_in;
FILE* redirect_out;
char** args;  // including programs path at the first element, and a NULL at the last element
int args_len;
};

/* Parse tokens into command line task struct 
 * The end_index is pointing to position next to the last element 
 * Return NULL if error occurs while parsing */
struct command_task* tokens_to_task(struct tokens* tokens, int start_index, int end_index) {
  struct command_task* com_task = (struct command_task*) malloc(sizeof(struct command_task));
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
        com_task->redirect_out = fopen(next_token, "w+");
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
        com_task->redirect_in = fopen(next_token, "r");
        // todo: remember to close the file when ending the parent process 
      }
      else {
        free(com_task->args);
        free(com_task);
        return NULL;
      }
      ++i;
    }
  } // for loop end
  // Not every token is in args array, thus the length can be smaller  
  char** tmp_command = realloc(com_task->args, sizeof(char*) * (com_task->args_len+1));
  if(!tmp_command) {perror("realloc failed"); exit(0);}
  com_task->args = tmp_command;
  com_task->args[com_task->args_len] = NULL; // execv needs the last arg being NULL 

  return com_task;
}

/* Create a child process by giving args
 * args is a array of strings (char**) and the first string must be program's path, 
 * and the last pointer must be NULL 
 * todo: the argument need to be a struct contains redirection pointers 
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

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* REPLACE this to run commands as programs. */
      /* Command parsing should be here 
       * Since the child processes is created after parsing the command */ 

      struct command_task** all_com_tasks = malloc(512 * sizeof(struct command_task*));
      int com_tasks_count = 0;
      
      /* Separate by pipe symbol first, 
       * and each of the separated command will be executed in a different child process. 
       * The resulting command and corresponding stdin/stdout redirection 
       * will be stored in a struct command_task */
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
      }

      // todo: codes below including checking path, parse multiple arguments, 
      // and create and execute process need to be done in a loop
      // todo: AFTER checking all the programs' path exits, then we can execute those programs 
      // todo: redirecting the stdout and stdin in the child processes 

      for(int i=0;i<com_tasks_count;++i) {
        struct command_task* com_task = all_com_tasks[i];
        char* program_path = com_task->args[0];
        if(access(program_path, F_OK) == -1) 
          program_path = find_file_path(program_path);
      
        if(!program_path) {
          printf("Program file not found\n");
          continue; // This will break sth, so its not correct 
          //exit(0);
        }
        else {
          printf("Program file found at %s\n", program_path);
          com_task->args[0] = program_path;
        }
      
        // create process and execute 
        com_task->pid = create_process_and_exec(com_task);
      }
      
      /* Wait all the child processes to terminate */ 
      for(int i=0;i<com_tasks_count;++i) {
        waitpid(all_com_tasks[i]->pid, NULL, 0);
      }


        
      // todo: free all the child processes' resources in the struct

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
