#include <unistd.h>
#include "listener_socket.h"
#include "connection.h"
#include "response.h"
#include "request.h"
#include "queue.h"
#include "rwlock.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>

// -- Constants --
#define DEFAULT_THREAD_NUM 4
#define QUEUE_SIZE         1024
#define HASH_TABLE_SIZE    1024
#define N_WAY_BATCH                                                                                \
    DEFAULT_THREAD_NUM // Maximum number of concurrent readers before a waiting writer is admitted.

// -- Global Audit Synchronization Variables --
static pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;
//static pthread_cond_t audit_cond = PTHREAD_COND_INITIALIZER;
// static int next_log_seq = 0;
// static int seq_counter = 0;

// -- Structure Definitions --
typedef struct {
    int connfd;
    //int seqNum; // Sequence number assigned when enqueued.
} requestEntry_t;

typedef struct {
    char *oper; // e.g., "GET", "PUT", "UNSUP", "ERROR"
    char *uri; // The requested URI
    int status; // Intended HTTP status code (e.g., 200, 404, etc.)
    char *reqid; // The Request-Id header value, or "0" if not provided.
} logEntry_t;

// -- Global Dispatch Queue --
static queue_t *dispatch_queue = NULL;

// -- Hash Table for URI Locks --
// Each bucket holds a linked list of lock entries.
// --- Revised Lock Table Structure ---
typedef struct lock_entry {
    char *uri;
    rwlock_t *lock;
    struct lock_entry *next;
} lock_entry_t;

typedef struct {
    lock_entry_t **buckets;
    size_t size;
    pthread_mutex_t *bucket_mutexes; // One mutex per bucket
} lock_table_t;

static lock_table_t *uri_lock_table = NULL;

//using the dj2b hashing function for the hash table implementation
//refer to https://theartincode.stanis.me/008-djb2/
unsigned long hash_uri(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

//locks per bucket implementation
//refer to https://cboard.cprogramming.com/c-programming/121295-locking-threads-hash-table.html
lock_table_t *lock_table_init(size_t size) {
    lock_table_t *table = malloc(sizeof(lock_table_t));
    if (!table)
        return NULL;
    table->size = size;
    table->buckets = calloc(size, sizeof(lock_entry_t *));
    if (!table->buckets) {
        free(table);
        return NULL;
    }
    table->bucket_mutexes = malloc(size * sizeof(pthread_mutex_t));
    if (!table->bucket_mutexes) {
        free(table->buckets);
        free(table);
        return NULL;
    }
    for (size_t i = 0; i < size; i++) {
        pthread_mutex_init(&table->bucket_mutexes[i], NULL);
    }
    return table;
}

rwlock_t *lock_table_lookup(lock_table_t *table, const char *uri) {
    unsigned long index = hash_uri(uri) % table->size;
    rwlock_t *result = NULL;
    pthread_mutex_lock(&table->bucket_mutexes[index]);
    lock_entry_t *entry = table->buckets[index];
    while (entry) {
        if (strcmp(entry->uri, uri) == 0) {
            result = entry->lock;
            break;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&table->bucket_mutexes[index]);
    return result;
}

int lock_table_insert(lock_table_t *table, const char *uri, rwlock_t *lock) {
    unsigned long index = hash_uri(uri) % table->size;
    lock_entry_t *new_entry = malloc(sizeof(lock_entry_t));
    if (!new_entry)
        return -1;
    new_entry->uri = strdup(uri);
    if (!new_entry->uri) {
        free(new_entry);
        return -1;
    }
    new_entry->lock = lock;
    pthread_mutex_lock(&table->bucket_mutexes[index]);
    new_entry->next = table->buckets[index];
    table->buckets[index] = new_entry;
    pthread_mutex_unlock(&table->bucket_mutexes[index]);
    return 0;
}

void lock_table_destroy(lock_table_t *table) {
    if (!table)
        return;
    for (size_t i = 0; i < table->size; i++) {
        pthread_mutex_lock(&table->bucket_mutexes[i]);
        lock_entry_t *entry = table->buckets[i];
        while (entry) {
            lock_entry_t *tmp = entry;
            entry = entry->next;
            free(tmp->uri);
            rwlock_delete(&tmp->lock);
            free(tmp);
        }
        pthread_mutex_unlock(&table->bucket_mutexes[i]);
        pthread_mutex_destroy(&table->bucket_mutexes[i]);
    }
    free(table->bucket_mutexes);
    free(table->buckets);
    free(table);
}

rwlock_t *getFileLock(const char *uri) {
    rwlock_t *result = lock_table_lookup(uri_lock_table, uri);
    if (result == NULL) {
        result = rwlock_new(N_WAY, N_WAY_BATCH);
        if (!result) {
            fprintf(stderr, "Failed to create lock for URI: %s\n", uri);
            return NULL;
        }
        if (lock_table_insert(uri_lock_table, uri, result) != 0) {
            rwlock_delete(&result);
            return NULL;
        }
    }
    return result;
}

void *fulfill_request(void *arg);
void enqueue_request(int connfd);
void dispatch(requestEntry_t *entry);

// Server operations
void handle_connection(int connfd);
void get(conn_t *conn);
void put(conn_t *conn);
void unsupported(conn_t *conn);

// -- Main Function --
int main(int argc, char **argv) {
    int threads = DEFAULT_THREAD_NUM;
    int opt;

    // Parse command-line options.
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
        case 't': threads = atoi(optarg); break;
        default: fprintf(stderr, "Usage: %s [-t threads] <port>\n", argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "Expected port argument after options\n");
        fprintf(stderr, "Usage: %s [-t threads] <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Use argv[optind] for the port.
    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[optind]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    Listener_Socket_t *sock = ls_new(port);
    if (!sock) {
        warnx("cannot open socket");
        return EXIT_FAILURE;
    }

    dispatch_queue = queue_new(QUEUE_SIZE);
    if (dispatch_queue == NULL) {
        perror("Dispatch Queue Allocation Failed!\n");
        exit(EXIT_FAILURE);
    }

    // Initialize the URI lock table (hash table).
    uri_lock_table = lock_table_init(HASH_TABLE_SIZE);
    if (uri_lock_table == NULL) {
        perror("URI Lock Table Allocation Failed!\n");
        exit(EXIT_FAILURE);
    }

    pthread_t *workers = malloc(sizeof(pthread_t) * threads);
    if (workers == NULL) {
        perror("Thread allocation failed!\n");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < threads; i++) {
        if (pthread_create(&workers[i], NULL, fulfill_request, NULL) != 0) {
            perror("Worker Thread creation Failed");
            exit(EXIT_FAILURE);
        }
    }

    // Dispatcher loop: accept connections and enqueue them.
    while (1) {
        int connfd = ls_accept(sock);
        if (connfd < 0) {
            perror("Connection Accept Error...Retrying");
            continue;
        }
        enqueue_request(connfd);
        // Do NOT close(connfd) here; the worker thread will handle it.
    }

    // Unreachable cleanup code.
    ls_delete(&sock);
    free(workers);
    queue_delete(&dispatch_queue);
    lock_table_destroy(uri_lock_table);
    return EXIT_SUCCESS;
}

// -- Enqueue a Connection --
void enqueue_request(int connfd) {
    requestEntry_t *request = malloc(sizeof(requestEntry_t));
    if (request == NULL) {
        close(connfd);
        return;
    }
    request->connfd = connfd;

    if (!queue_push(dispatch_queue, request)) {
        free(request);
        close(connfd);
    }
}

// -- Worker Thread Function --
void *fulfill_request(void *arg) {
    (void) arg; // Unused parameter.
    while (1) {
        void *entry_ptr = NULL;
        if (!queue_pop(dispatch_queue, &entry_ptr)) {
            fprintf(stderr, "queue_pop failed in worker thread\n");
            return NULL;
        }
        //fprintf(stderr, "Fulfilling");
        requestEntry_t *req_entry = (requestEntry_t *) entry_ptr;
        dispatch(req_entry);
    }
    return NULL;
}

// -- Dispatch a Request --
void dispatch(requestEntry_t *elem) {
    // Process the connection and capture audit info.
    handle_connection(elem->connfd);
    //free(audit);
    free(elem);
}

// -- Process a Connection and Capture Audit Info --
void handle_connection(int connfd) {

    conn_t *conn = conn_new(connfd);
    if (conn == NULL) {
        //free(audit);
        close(connfd);
        return;
    }
    const Response_t *res = conn_parse(conn);

    if (res != NULL) {

        conn_send_response(conn, res);
    } else {
        const Request_t *req = conn_get_request(conn);

        if (req == &REQUEST_PUT) {
            //strcpy(audit->oper, "PUT");
            put(conn);
            //audit->status = 200;
        } else if (req == &REQUEST_GET) {

            //strcpy(audit->oper, "GET");
            get(conn);
            //audit->status = 200;
        } else if (req == &REQUEST_UNSUPPORTED) {
            //strcpy(audit->oper, "UNSUP");
            unsupported(conn);
            //audit->status = 501;
        }
    }
    // fprintf(stderr, "Dispatching");
    close(connfd);
    conn_delete(&conn);
    //return audit;
}


//Testing has revealed Current PUT operation leaves tmp dirs
//Need clean up after each complete PUT operation
void put(conn_t *conn) {
    const Response_t *response = NULL;

    // Create a unique temporary filename in a dedicated temp directory.
    // Using a constant prefix (e.g., "/tmp/put") that is independent of audit->uri.
    char tempTemplate[512];
    snprintf(tempTemplate, sizeof(tempTemplate), "/tmp/putXXXXXX");
    int tmpfd = mkstemp(tempTemplate);
    if (tmpfd < 0) {
        //fprintf(stderr, "Error creating temp file for %s: %d\n", audit->uri, errno);
        if (errno == EACCES || errno == EISDIR || errno == ENOENT)
            response = &RESPONSE_FORBIDDEN;
        else
            response = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, response);
        //free(audit);
        return;
    }

    // Write the file contents to the temporary file concurrently.
    response = conn_recv_file(conn, tmpfd);
    close(tmpfd);

    logEntry_t *audit = malloc(sizeof(logEntry_t));
    audit->oper = "PUT";
    audit->uri = conn_get_uri(conn);

    // Get the lock associated with this URI.
    rwlock_t *fileLock = getFileLock(audit->uri);
    if (fileLock == NULL) {
        response = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, response);
        free(audit);
        return;
    }

    // Acquire writer lock (this call blocks until available).
    writer_lock(fileLock);
    // Retrieve Request-Id after acquiring the writer lock.
    audit->reqid = conn_get_header(conn, "Request-Id");

    // Check if the target file already exists.
    bool exists = (access(audit->uri, F_OK) == 0);

    if (rename(tempTemplate, audit->uri) < 0) {
        response = &RESPONSE_INTERNAL_SERVER_ERROR;
        writer_unlock(fileLock);
        unlink(tempTemplate);
        conn_send_response(conn, response);
        free(audit);
        return;
    }

    // Set appropriate response based on existence of the file.
    if (exists) {
        response = &RESPONSE_OK;
        audit->status = 200;
    } else {
        response = &RESPONSE_CREATED;
        audit->status = 201;
    }

    // Log the operation.
    pthread_mutex_lock(&audit_lock);
    fprintf(stderr, "%s,%s,%d,%s\n", audit->oper, audit->uri, audit->status, audit->reqid);
    pthread_mutex_unlock(&audit_lock);

    // Release writer lock and clean up.
    writer_unlock(fileLock);
    free(audit);
    conn_send_response(conn, response);
}

// -- GET Operation with Per-URI Locking --
void get(conn_t *conn) {
    logEntry_t *audit = malloc(sizeof(logEntry_t));
    const Response_t *response = NULL;
    audit->uri = conn_get_uri(conn);
    audit->oper = "GET";
    rwlock_t *fileLock = getFileLock(audit->uri);

    if (fileLock == NULL) {
        response = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, response);
        return;
    }

    // Acquire reader lock.
    reader_lock(fileLock);
    audit->reqid = conn_get_header(conn, "Request-Id");
    bool exists = (access(audit->uri, F_OK) == 0);
    if (!exists) {
        response = &RESPONSE_NOT_FOUND;
        conn_send_response(conn, response);
    } else {
        int fd = open(audit->uri, O_RDONLY, 0666);
        if (fd < 0) {
            fprintf(stderr, "%s : %d\n", audit->uri, errno);
            response = &RESPONSE_INTERNAL_SERVER_ERROR;
        } else {
            off_t file_size = lseek(fd, 0, SEEK_END);
            //uint64_t count = (uint64_t) file_size;
            lseek(fd, 0, SEEK_SET);
            response = &RESPONSE_OK;
            if ((conn_send_file(conn, fd, file_size)) == NULL)
                close(fd);
            else {
                conn_send_response(conn, &RESPONSE_INTERNAL_SERVER_ERROR);
            }
        }
    }
    audit->status = 200;
    pthread_mutex_lock(&audit_lock);
    fprintf(stderr, "%s,%s,%d,%s\n", audit->oper, audit->uri, audit->status, audit->reqid);

    pthread_mutex_unlock(&audit_lock);
    reader_unlock(fileLock);
    free(audit);
}

void unsupported(conn_t *conn) {
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}
