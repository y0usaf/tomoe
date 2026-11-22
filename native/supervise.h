#ifndef TOMOE_SUPERVISE_H
#define TOMOE_SUPERVISE_H

#include <stddef.h>

struct execution;

struct execution *tomoe_process_start_dependent(char *const argv[], char *const envp[], int *error);
int tomoe_exec_fd(struct execution *job, int which);
int tomoe_exec_poll(struct execution *job);
int tomoe_exec_code(struct execution *job);
int tomoe_exec_stop(struct execution *job, int sig);
int tomoe_exec_release(struct execution *job);
int tomoe_service_run_begin(const char *runtime, int *cleared, char *error, size_t error_size);
void tomoe_service_run_end(void);

#endif
