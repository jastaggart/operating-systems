#include<ucontext.h>
#include<stdio.h>
#include <stdlib.h>
#include<pthread.h>
#include<unistd.h> 
#include<time.h>
#include <errno.h>
#include "sut.h"
#include "queue.h"

/* Name: Jasmine Taggart
   ID: 261056534
   Assignment 2 - Thread Scheduler
*/

#define STACK_SIZE 1024*1024

// task structure for queue
struct task {
	char *threadstack;
	void *threadfunc;
	ucontext_t *threadcontext;
	bool done;
};

// structure for open files
struct openedFile {
	int fd;
	FILE *file;
	bool wait;
} files[30];

// c-exec and i-exec kernel and user threads
pthread_t *t_cexec, *t_iexec;
ucontext_t *uc_cexec, *uc_iexec;

// pointer to current running task
struct task *cur_c_task, *cur_i_task;

// ready queue and wait queue 
struct queue *readyq, *waitq;

// locks for ready queue, wait queue, current task
pthread_mutex_t *rq_lock, *wq_lock, *cur_c_lock, *cur_i_lock, *file_lock;

int numFiles = 0, shutd = 0, cexec_empty = 0, iexec_empty = 0;

void * cexec() {
	getcontext(uc_cexec);
	struct queue_entry *entry;

	while(shutd == 0) {
		// if there are no ready tasks, sleep
		if (queue_peek_front(readyq) == NULL) {
			nanosleep((const struct timespec[]){{0, 1000000000L}}, NULL);
		}

		else {
			// pop next ready task from queue
			pthread_mutex_lock(rq_lock);
			entry = queue_pop_head(readyq);
			pthread_mutex_unlock(rq_lock);
			
			if (entry != NULL) {
				// get and execute the current task
				pthread_mutex_lock(cur_c_lock);
				cur_c_task = entry->data;
				free(entry);
				swapcontext(uc_cexec, (*cur_c_task).threadcontext);

				// if task exited via sut_exit then free the task
				if (cur_c_task->done) {
					free(cur_c_task->threadstack);
					free(cur_c_task->threadcontext);
					free(cur_c_task);
				}
				pthread_mutex_unlock(cur_c_lock);
			}
		}
	}

	// if shutdown was called, then c-exec finishes all tasks
	// by checking if the ready queue has tasks waiting and if i-exec is 
	// executing tasks that will return to the ready queue
	while (queue_peek_front(readyq) != NULL || !iexec_empty) {
		if (queue_peek_front(readyq) != NULL) {
			cexec_empty = 0; // c-exec is not done with its tasks

			// get task from ready queue and execute it
			pthread_mutex_lock(rq_lock);
			entry = queue_pop_head(readyq);
			pthread_mutex_unlock(rq_lock);
			pthread_mutex_lock(cur_c_lock);
			cur_c_task = entry->data;
			free(entry);
			swapcontext(uc_cexec, (*cur_c_task).threadcontext);

			// if task exited via sut_exit then free the task
			if (cur_c_task->done) {
				free(cur_c_task->threadstack);
				free(cur_c_task->threadcontext);
				free(cur_c_task);
			}
			pthread_mutex_unlock(cur_c_lock);
		}
	}
	cexec_empty = 1; // c-exec is ready to shut down
}

void * iexec() {
	getcontext(uc_iexec);
	struct queue_entry *entry;

	while (shutd == 0) {
		pthread_mutex_lock(wq_lock);

		// if there is an io task to execute, then remove it from wait queue
		// and execute it
		if (queue_peek_front(waitq) != NULL) {
			entry = queue_pop_head(waitq);
			pthread_mutex_unlock(wq_lock);
			
			if (entry != NULL) {
				pthread_mutex_lock(cur_i_lock);
				cur_i_task = entry->data;
				swapcontext(uc_iexec, (*cur_i_task).threadcontext);

				// add task back into ready queue for c-exec to execute
				pthread_mutex_lock(rq_lock);
				queue_insert_head(readyq, entry);
				pthread_mutex_unlock(rq_lock);
				pthread_mutex_unlock(cur_i_lock);
			}
			pthread_mutex_lock(wq_lock);
		}
		pthread_mutex_unlock(wq_lock);
	}
	
	// if shutdown was called, then i-exec finishes all tasks
	// by checking if the wait queue has tasks waiting and if c-exec is 
	// executing tasks that may be added to wait queue
	while (queue_peek_front(waitq) != NULL || !cexec_empty) {
		if (queue_peek_front(waitq) != NULL) {
			iexec_empty = 0; // still io tasks left

			// remove task from wait queue
			pthread_mutex_lock(wq_lock);
			entry = queue_pop_head(waitq);
			pthread_mutex_unlock(wq_lock);

			// run the removed task
			pthread_mutex_lock(cur_i_lock);
			cur_i_task = entry->data;
			swapcontext(uc_iexec, (*cur_i_task).threadcontext);

			// add task back into ready queue
			pthread_mutex_lock(rq_lock);
			queue_insert_head(readyq, entry);
			pthread_mutex_unlock(rq_lock);
			pthread_mutex_unlock(cur_i_lock);
		}
		else iexec_empty = 1; // no io tasks to run
	}
	iexec_empty = 1; // i-exec is ready to shut down
}
 
void sut_init() {
	
	// initialize locks
	rq_lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
	wq_lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
	cur_c_lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
	cur_i_lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
	file_lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));

	if (pthread_mutex_init(rq_lock, NULL) != 0)
		perror("pthread_mutex_init() error");

	if (pthread_mutex_init(wq_lock, NULL) != 0)
		perror("pthread_mutex_init() error");

	if (pthread_mutex_init(cur_c_lock, NULL) != 0)
		perror("pthread_mutex_init() error");

	if (pthread_mutex_init(cur_i_lock, NULL) != 0)
		perror("pthread_mutex_init() error");

	if (pthread_mutex_init(file_lock, NULL) != 0)
		perror("pthread_mutex_init() error");
	
	// initialize pthreads
	t_cexec = (pthread_t *) malloc(sizeof(pthread_t));
	t_iexec = (pthread_t *) malloc(sizeof(pthread_t));

	// initialize contexts for c-exec and i-exec 
	uc_cexec = (ucontext_t *) malloc(sizeof(ucontext_t));
	uc_iexec = (ucontext_t *) malloc(sizeof(ucontext_t));
	
	char *stack_cexec = (char *) malloc(sizeof(char)*STACK_SIZE); 
	uc_cexec->uc_stack.ss_sp = stack_cexec;
	uc_cexec->uc_stack.ss_size = STACK_SIZE;
	uc_cexec->uc_stack.ss_flags = 0; 
	uc_cexec->uc_link = 0;

	char *stack_iexec = (char *) malloc(sizeof(char)*STACK_SIZE); 
	uc_iexec->uc_stack.ss_sp = stack_iexec;
	uc_iexec->uc_stack.ss_size = STACK_SIZE;
	uc_iexec->uc_stack.ss_flags = 0; 
	uc_iexec->uc_link = 0;

	// initialize queues
	readyq = (struct queue *) malloc(sizeof(struct queue));
	waitq = (struct queue *) malloc(sizeof(struct queue));

	*readyq = queue_create();
	*waitq = queue_create();
	queue_init(readyq);
	queue_init(waitq);

	// start pthreads
	if (pthread_create(t_cexec, NULL, cexec, NULL) != 0) {
		perror("pthread_create() t_cexec error");
		exit(1);
	}
	if (pthread_create(t_iexec, NULL, iexec, NULL) != 0) {
		perror("pthread_create() t_iexec error");
		exit(1);
	}
}

bool sut_create(sut_task_f fn) {
	
	// create a task
	struct task *new_task = (struct task *) malloc(sizeof(struct task));
	new_task->done = false;
	new_task->threadcontext = (ucontext_t *) malloc(sizeof(ucontext_t));
	new_task->threadstack = (char *) malloc(sizeof(char)*STACK_SIZE);
	new_task->threadfunc = fn;
	new_task->threadcontext->uc_stack.ss_sp = new_task->threadstack;
	new_task->threadcontext->uc_stack.ss_size = STACK_SIZE;
	new_task->threadcontext->uc_stack.ss_flags = 0; 
	new_task->threadcontext->uc_link = 0;
	
	getcontext(new_task->threadcontext);
	// make context and check if it failed
	errno = 0;
	makecontext(new_task->threadcontext, fn, 0);
	if (errno != 0) {
		perror("makecontext() error");
		return 0;
	}
	// add task to ready queue
	struct queue_entry *entry = queue_new_node(new_task);
	pthread_mutex_lock(rq_lock);
	queue_insert_tail(readyq, entry);
	pthread_mutex_unlock(rq_lock);

	return 1;
}

void sut_yield() {
	// put current task at the end of the ready queue
	struct queue_entry *entry = queue_new_node(cur_c_task);
	
	pthread_mutex_lock(rq_lock);
	if (entry != NULL)
		queue_insert_tail(readyq, entry);
	pthread_mutex_unlock(rq_lock);

	// return to c-exec
	swapcontext((*cur_c_task).threadcontext, uc_cexec);
}

void sut_exit() {
	// set done flag to 'true' to notify c-exec that this task can be freed
	cur_c_task->done = true;

	// return to c-exec
	swapcontext((*cur_c_task).threadcontext, uc_cexec);
}

int sut_open(char *file_name) {
	// add task to wait queue for i-exec to execute
	struct queue_entry *entry = queue_new_node(cur_c_task);
	pthread_mutex_lock(wq_lock);
	queue_insert_tail(waitq, entry);
	pthread_mutex_unlock(wq_lock);
	
	// flag that i-exec still has tasks
	iexec_empty = 0;

	// return to c-exec while saving the task's context so that i-exec can resume here
	swapcontext((*cur_c_task).threadcontext, uc_cexec);

	// i-exec resumes the task, attempt to open file
	FILE *file = fopen(file_name, "r+");

	// if file with given name does not exist, create and open a file 
	if (file == NULL) {
		file = fopen(file_name, "w+");

		// if file creation fails, error
		if (file == NULL) {
			perror("fopen() error");
			return -1;
		}
	}

	// file successfully opened so add to list of open files
	pthread_mutex_lock(file_lock);
	files[numFiles].fd = fileno(file);
	files[numFiles].file = file;
	files[numFiles].wait = false;
	numFiles += 1;
	pthread_mutex_unlock(file_lock);

	// flag that c-exec still has tasks since it needs to return here
	cexec_empty = 0;
	
	// return to i-exec
	swapcontext((*cur_i_task).threadcontext, uc_iexec);

	// c-exec resumes here to return the file's fd
	return fileno(file);
}

void sut_close(int fd) {
	// add task to wait queue for i-exec to execute
	struct queue_entry *entry = queue_new_node(cur_c_task);
	pthread_mutex_lock(wq_lock);
	queue_insert_tail(waitq, entry);
	pthread_mutex_unlock(wq_lock);

	// flag that i-exec still has tasks
	iexec_empty = 0;

	// return to c-exec while saving the task's context so that i-exec can resume here
	swapcontext((*cur_c_task).threadcontext, uc_cexec);
	
	// get file with given fd and close it
	pthread_mutex_lock(file_lock);
	for (int i = 0; i < numFiles; i++) {
		if (files[i].fd == fd) {
			// if file is still in use, can't close yet
			// so add task back to wait queue and switch back to i_exec
			while (files[i].wait) {
				pthread_mutex_unlock(file_lock);
				pthread_mutex_lock(wq_lock);
				queue_insert_tail(waitq, entry);
				pthread_mutex_unlock(wq_lock);
				swapcontext((*cur_i_task).threadcontext, uc_iexec);
				pthread_mutex_lock(file_lock);
			}
			// file is ready to be closed so close it or report error
			if (fclose(files[i].file) != 0)
				perror("fclose() error");
			break;
		}
	}
	pthread_mutex_unlock(file_lock);

	// flag that c-exec still has tasks since it needs to return here
	cexec_empty = 0;

	// return to i_exec
	swapcontext((*cur_i_task).threadcontext, uc_iexec);
}

void sut_write(int fd, char *buf, int size) {
	// add task to wait queue for i-exec to execute
	struct queue_entry *entry = queue_new_node(cur_c_task);
	pthread_mutex_lock(wq_lock);
	queue_insert_tail(waitq, entry);
	pthread_mutex_unlock(wq_lock);

	// set wait to true so that sut_close does not close the file
	pthread_mutex_lock(file_lock);
	int i;
	for (i = 0; i < numFiles; i++) {
		if (files[i].fd == fd) {
			files[i].wait = true;
			break;
		}
	}
	pthread_mutex_unlock(file_lock);

	// flag that i-exec still has tasks
	iexec_empty = 0;

	// return to c-exec while saving the task's context so that i-exec can resume here
	swapcontext((*cur_c_task).threadcontext, uc_cexec);

	// write to file with given fd
	pthread_mutex_lock(file_lock);
	if (write(fd, buf, size) < 0) 
		perror("write() error");
	files[i].wait = false; // file no longer in use by write
	pthread_mutex_unlock(file_lock);
	
	// flag that c-exec still has tasks since it needs to return here
	cexec_empty = 0;

	// return to i_exec
	swapcontext((*cur_i_task).threadcontext, uc_iexec);
}

char* sut_read(int fd, char *buf, int size) {
	// add task to wait queue for i-exec to execute
	struct queue_entry *entry = queue_new_node(cur_c_task);
	pthread_mutex_lock(wq_lock);
	queue_insert_tail(waitq, entry);
	pthread_mutex_unlock(wq_lock);

	// set wait to true so that sut_close does not close the file
	pthread_mutex_lock(file_lock);
	int i;
	for (i = 0; i < numFiles; i++) {
		if (files[i].fd == fd) {
			files[i].wait = true;
			break;
		}
	}
	
	pthread_mutex_unlock(file_lock);
	
	// flag that i-exec still has tasks
	iexec_empty = 0;
	
	// return to c-exec while saving the task's context so that i-exec can resume here
	swapcontext((*cur_c_task).threadcontext, uc_cexec);

	// read to file with given fd
	pthread_mutex_lock(file_lock);
	if (read(fd, buf, size) < 0) {
		files[i].wait = false;
		pthread_mutex_unlock(file_lock);
		perror("read() error");
		return NULL;
	}
	files[i].wait = false; // read no longer needs file to be open
	pthread_mutex_unlock(file_lock);

	// flag that c-exec still has tasks since it needs to continue this one
	cexec_empty = 0;

	// return to i-exec
	swapcontext((*cur_i_task).threadcontext, uc_iexec);

	// c-exec resumes here to return buf
	return buf;
}

void sut_shutdown() {

	// set shutd to '1' to notify c-exec and i-exec to shutdown the program
	shutd = 1;

	// join c-exec and i-exec back to the main thread
	pthread_join(*t_cexec, NULL);
	pthread_join(*t_iexec, NULL);

	// free locks
	free(rq_lock);
	free(wq_lock);
	free(cur_c_lock);
	free(cur_i_lock);
	free(file_lock);
	
	// free pthreads
	free(t_cexec);
	free(t_iexec);

	// free contexts
	free(uc_cexec->uc_stack.ss_sp);
	free(uc_iexec->uc_stack.ss_sp);
	free(uc_cexec);
	free(uc_iexec);

	// free queues
	free(readyq);
	free(waitq);
}