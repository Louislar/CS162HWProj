/*
 * Word count application with one thread per input file.
 *
 * You may modify this file in any way you like, and are expected to modify it.
 * Your solution must read each input file from a separate thread. We encourage
 * you to make as few changes as necessary.
 */

/*
 * Copyright © 2021 University of California, Berkeley
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <pthread.h>

#include "word_count.h"
#include "word_helpers.h"

/*
 * main - handle command line, spawning one thread per file.
 */

// argument structure for count_words() in pthread_create()
struct thread_arg {
  word_count_list_t* word_counts;
  char* file_name;
};

void* count_words_wrapper(void* args) {
  struct thread_arg* t_arg;
  t_arg = (struct thread_arg*) args;
  // Open the file 
  FILE* infile = fopen(t_arg->file_name, "r");
  if(!infile) {
    printf("File opening fail\n");
    pthread_exit(NULL);
  }

  //printf("Working in count words wrapper function.\n");
  count_words(t_arg->word_counts, infile);
  //printf("Child thread end\n");
  fclose(infile);
  pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
  /* Create the empty data structure. */
  word_count_list_t* word_counts = (word_count_list_t*) malloc(sizeof(word_count_list_t));
  init_words(word_counts);
  long t;
  pthread_t threads[argc-1];
  struct thread_arg* t_arg[argc-1];


  if (argc <= 1) {
    /* Process stdin in a single thread. */
    count_words(word_counts, stdin);
  } else {

    // Create multiple threads for counting words 
    for(t = 0;t<argc-1;++t) {
      // read in file related to t^th command line argument 
      // printf("main: creating thread %ld\n", t);

      t_arg[t]  = (struct thread_arg*) malloc(sizeof(struct thread_arg));
      t_arg[t]->word_counts = word_counts;
      t_arg[t]->file_name = argv[t+1];

      int rc;
      rc = pthread_create(&threads[t], NULL, count_words_wrapper, (void*)t_arg[t]);
      if (rc) {
        printf("ERROR; return code from pthread_create() is %d\n", rc);
        exit(-1);
      }
    }  /* if argc > 1 */

  }
  
  /* Wait all the child threads end */
  for(t=0;t<argc-1;++t) {
    void* retval;
    pthread_join(threads[t], &retval);
  }

  /* Output final result of all threads' work. */
  // printf("Output final result of all threas' work\n");
  wordcount_sort(word_counts, less_count);
  fprint_words(word_counts, stdout);
  pthread_exit(NULL);
  return 0;
}
