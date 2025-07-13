/**
 * @File queue.c
 *
 * Thread-safe Queue Implementation
 *
 * @author Isaac Garibay
 */

#include <stdio.h>
#include "queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

//queue_t definition
typedef struct queue {
    int size;
    int head; //out
    int tail; //in
    void **buffer;
    pthread_mutex_t lock;
    sem_t emptySem; //how many spaces left?
    sem_t fullSem; //how many spaces filled?
} queue_t;

//constructor
queue_t *queue_new(int size) {
    if (size <= 0)
        return NULL; //because why?

    queue_t *queue;
    if ((queue = malloc(sizeof(queue_t))) == NULL) {
        fprintf(stderr, "Queue struct allocation failed\n");
        return NULL;
    }

    if (sem_init(&queue->emptySem, 0, size) != 0 || sem_init(&queue->fullSem, 0, 0) != 0
        || pthread_mutex_init(&queue->lock, NULL) != 0) {
        fprintf(stderr, "Semaphore or Mutex init failed\n");
        free(queue);
        return NULL;
    }

    if ((queue->buffer = malloc(size * sizeof(void *))) == NULL) {
        fprintf(stderr, "Queue buffer allocation failed\n");
        sem_destroy(&queue->emptySem);
        sem_destroy(&queue->fullSem);
        pthread_mutex_destroy(&queue->lock);
        free(queue);
        return NULL;
    }

    queue->head = 0;
    queue->tail = 0;
    queue->size = size;

    return queue;
}

//deconstructor
void queue_delete(queue_t **q) {
    if (q == NULL || *q == NULL)
        return; //why?

    queue_t *ptr = *q; //point to the queue ptr

    //pass what is at the address of queue ptr's reference and free respectively
    pthread_mutex_destroy(&ptr->lock);
    sem_destroy(&ptr->emptySem);
    sem_destroy(&ptr->fullSem);
    free(ptr->buffer);
    free(ptr);
    *q = NULL;
}

bool queue_push(queue_t *q, void *elem) {
    if (q == NULL)
        return false;
    sem_wait(&q->emptySem); //decrement empty space count

    //enter critical region
    pthread_mutex_lock(&q->lock);
    q->buffer[q->tail] = elem;
    q->tail = (q->tail + 1) % q->size;
    pthread_mutex_unlock(&q->lock);
    //exit critical region

    sem_post(&q->fullSem); //increment filled space count
    return true;
}

bool queue_pop(queue_t *q, void **elem) {
    if (q == NULL || elem == NULL)
        return false;
    sem_wait(&q->fullSem); //decrement filled space count
    //enter critical region
    pthread_mutex_lock(&q->lock);
    *elem = q->buffer[q->head];
    q->head = (q->head + 1) % q->size;
    pthread_mutex_unlock(&q->lock);
    //exit critical region

    sem_post(&q->emptySem); //increment empty space count
    return true;
}
