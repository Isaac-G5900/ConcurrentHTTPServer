/**
 * @File rwlock.c
 *
 * Reader/Writer Lock Implementation
 *
 * @author Isaac Garibay
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "rwlock.h"
#include <pthread.h>
#include <semaphore.h>

//[Usage Note]: 
// N_WAY rwlock configuration
//  implementation currently does not pass with 100% coverage
// Full reliability of httpserver confirmed assuming use of 2 other configurations

typedef struct rwlock {
    PRIORITY p;
    uint32_t nway;
    //tracking active/waiting readers and waiting writers
    //use these based on PRIORITY p and make decisions on the behavior for the
    //lock mode
    int waiting_readers;
    int waiting_writers;
    int active_readers;
    uint32_t nway_batch;
    pthread_cond_t admitRead;
    bool active_writer; //either is or isn't a writer
    //can probably just check if admitWrite high to track an active writer
    pthread_cond_t admitWrite;
    pthread_mutex_t lock;

} rwlock_t;

//constructor
rwlock_t *rwlock_new(PRIORITY p, uint32_t n) {
    if (p != READERS && p != WRITERS && p != N_WAY) {
        fprintf(stderr, "Invalid Priority set for new lock\n");
        return NULL;
    }

    if (n == 0 && p == N_WAY) {
        fprintf(stderr, "Invalid argument n when p is set to N_WAY\n");
        return NULL;
    }

    rwlock_t *rw;
    if ((rw = malloc(sizeof(rwlock_t))) == NULL) {
        fprintf(stderr, "Lock allocation Failed\n");
        return NULL;
    }
    rw->p = p;
    //ignored when p == READERS or WRITERS
    rw->nway = n;
    rw->nway_batch = 0;
    rw->active_readers = 0;
    rw->waiting_readers = 0;
    rw->waiting_writers = 0;
    rw->active_writer = false;
    if (pthread_mutex_init(&rw->lock, NULL) != 0) {
        fprintf(stderr, "Lock init failed\n");
        free(rw);
        return NULL;
    }

    //we'll have to check based off what p is to determine what we are initializing
    //the n argument based off of whether or not p==N_WAY

    if (pthread_cond_init(&rw->admitRead, NULL) != 0) {
        fprintf(stderr, "Read CV init failed\n");
        pthread_mutex_destroy(&rw->lock);
        free(rw);
        return NULL;
    }

    if (pthread_cond_init(&rw->admitWrite, NULL) != 0) {
        fprintf(stderr, "Write CV init failed\n");
        pthread_mutex_destroy(&rw->lock);
        pthread_cond_destroy(&rw->admitRead);
        free(rw);
        return NULL;
    }

    return rw;
}

//deconstructor

void rwlock_delete(rwlock_t **rw) {
    if (rw == NULL || *rw == NULL)
        return;

    rwlock_t *ptr = *rw;
    pthread_mutex_destroy(&ptr->lock);
    pthread_cond_destroy(&ptr->admitRead);
    pthread_cond_destroy(&ptr->admitWrite);
    free(ptr);
    *rw = NULL;
}

//reader lock mechanisms

void reader_lock(rwlock_t *rw) {
    //protect critical region
    pthread_mutex_lock(&rw->lock);
    rw->waiting_readers += 1;

    if (rw->p == READERS) {
        //while a write is admitted, make the readers standby
        while (rw->active_writer) {
            pthread_cond_wait(&rw->admitRead, &rw->lock);
        }

    } else if (rw->p == WRITERS) {
        while (rw->waiting_writers > 0 || rw->active_writer) {
            pthread_cond_wait(&rw->admitRead, &rw->lock);
        }
    } else if (rw->p == N_WAY) {
        while (rw->active_writer) {
            pthread_cond_wait(&rw->admitRead, &rw->lock);
        }
        if (rw->waiting_writers > 0) {
            while (rw->nway_batch == rw->nway) {
                pthread_cond_wait(&rw->admitRead, &rw->lock);
            }
            rw->nway_batch += 1;
        } else {
            rw->nway_batch = 0;
        }
    }

    rw->waiting_readers -= 1;
    rw->active_readers += 1;
    pthread_mutex_unlock(&rw->lock);
}

void reader_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->lock);
    rw->active_readers -= 1;
    if (rw->p == READERS) {
        if (rw->waiting_readers > 0) {
            pthread_cond_signal(&rw->admitRead);
        } else {
            pthread_cond_signal(&rw->admitWrite);
        }
    } else if (rw->p == WRITERS) {
        if (rw->waiting_writers > 0 && rw->active_writer == false) {
            pthread_cond_signal(&rw->admitWrite);
        } else {
            if (rw->waiting_readers > 0) {
                pthread_cond_signal(&rw->admitRead);
            }
        }
    } else if (rw->p == N_WAY) {
        if (rw->active_readers == 0) {
            rw->nway_batch = 0;
            if (rw->waiting_writers > 0) {
                pthread_cond_signal(&rw->admitWrite);
            } else if (rw->waiting_readers > 0) {
                pthread_cond_signal(&rw->admitRead);
            }
        }
    }
    pthread_mutex_unlock(&rw->lock);
}

//writer lock mechanisms

void writer_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->lock);
    rw->waiting_writers += 1;
    if (rw->p == READERS) {
        while (rw->waiting_readers > 0 || rw->active_readers > 0 || rw->active_writer) {
            pthread_cond_wait(&rw->admitWrite, &rw->lock);
        }
    } else if (rw->p == WRITERS) {
        while (rw->active_writer || rw->active_readers > 0) {
            pthread_cond_wait(&rw->admitWrite, &rw->lock);
        }
    } else if (rw->p == N_WAY) {
        while (rw->active_readers > 0 || rw->active_writer) {
            pthread_cond_wait(&rw->admitWrite, &rw->lock);
        }
        rw->nway_batch = 0;
    }

    rw->waiting_writers -= 1;
    rw->active_writer = true;
    pthread_mutex_unlock(&rw->lock);
}

void writer_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->lock);
    rw->active_writer = false;
    if (rw->p == READERS) {
        if (rw->waiting_readers > 0) {
            pthread_cond_signal(&rw->admitRead);
        } else {
            if (rw->waiting_writers > 0) {
                pthread_cond_signal(&rw->admitWrite);
            }
        }
    } else if (rw->p == WRITERS) {
        if (rw->waiting_writers > 0) {
            pthread_cond_signal(&rw->admitWrite);
        } else {
            if (rw->waiting_readers > 0) {
                pthread_cond_signal(&rw->admitRead);
            }
        }
    } else if (rw->p == N_WAY) {
        if (rw->waiting_readers > 0) {
            pthread_cond_broadcast(&rw->admitRead);
        } else if (rw->waiting_writers > 0) {
            pthread_cond_signal(&rw->admitWrite);
        }
    }
    pthread_mutex_unlock(&rw->lock);
}
