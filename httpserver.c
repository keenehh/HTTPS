#include "asgn4_helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "request.h"
#include "response.h"
#include "queue.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

typedef struct queue {
    void **array;
    int size;
    int capacity;
    int front;
    int back;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} queue_t;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; //to lock the temp files.
queue_t *g_queue; //global queue

void handle_connection(int);
void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);

int main(int argc, char **argv) {
    if (argc < 2) {
        warnx("wrong arguments: %s port_num", argv[0]);
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int opt;
    int threads = 4; //default number of threads
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
        case 't': threads = atoi(optarg); break;
        }
    }
    /*
    ./httpserver -t 4 <port>
    Or ./httpserver <port> -t 4
    Both should work
    */
    //printf("threads %d\n", threads);
    char *endptr = NULL;
    ssize_t port;
    if (optind < argc) {
        port = strtoull(argv[optind], &endptr, 10);
    } else {
        fprintf(stderr, "Error: Port number is missing.\n");
        fprintf(stderr, "usage: %s [-t threads] <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[1]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    //initialize the global queue with the threads
    g_queue = queue_new(threads);
    pthread_t threads_ids[threads];
    for (int i = 0; i < threads; i++) {
        if (pthread_create(&threads_ids[i], NULL, worker_thread, NULL) != 0) {
            warnx("pthread_create");
            return EXIT_FAILURE;
        }
    }
    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        //handle_connection(connfd);
        //close(connfd);
        queue_push(g_queue, (void *) connfd);
    }

    for (int i = 0; i < threads; i++) {
        if (pthread_join(threads_ids[i], NULL) != 0) {
            warnx("pthread_join");
            return EXIT_FAILURE;
        }
    }
    queue_delete(&g_queue);
    return EXIT_SUCCESS;
}

void handle_connection(int connfd) {
    conn_t *conn = conn_new(connfd);
    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
        // debug("%s", conn_str(conn));
        const Request_t *req = conn_get_request(conn);

        if (req == &REQUEST_GET) { //get
            handle_get(conn);
        } else if (req == &REQUEST_PUT) { //put
            handle_put(conn);
        } else {
            handle_unsupported(conn); //didnt use get/put
        }
    }

    conn_delete(&conn);
    return;
}

void handle_get(conn_t *conn) {
    // TODO: Implement GET

    char *uri = conn_get_uri(conn);
    //debug("GET %s", uri);
    //check if the file exists
    pthread_mutex_lock(&mutex);
    struct stat fileStat; //https://man7.org/linux/man-pages/man2/stat.2.html
    if (stat(uri, &fileStat) == -1) {
        conn_send_response(conn, &RESPONSE_NOT_FOUND);
        uint16_t status_code = response_get_code(&RESPONSE_NOT_FOUND);
        audity_entry(request_get_str(conn_get_request(conn)), uri, status_code,
            conn_get_header(conn, "Request-Id"));
        return;
    }
    //check if the file is readable
    if (access(uri, R_OK) == -1) {
        conn_send_response(conn, &RESPONSE_FORBIDDEN);
        uint16_t status_code = response_get_code(&RESPONSE_FORBIDDEN);
        audity_entry(request_get_str(conn_get_request(conn)), uri, status_code,
            conn_get_header(conn, "Request-Id"));
        return;
    }
    /*
// send a message body from the file (fd)
const Response_t *conn_send_file(conn_t *conn, int fd, uint64_t count);
*/
    //lock the temporary file
    //pthread_mutex_lock(&mutex);
    int open_get = open(uri, O_RDONLY); //open the file for reading
    if (open_get <= 0) { //if we can't open the file then invalid
        pthread_mutex_unlock(&mutex);
        conn_send_response(conn, &RESPONSE_INTERNAL_SERVER_ERROR);
        uint16_t status_code = response_get_code(
            &RESPONSE_INTERNAL_SERVER_ERROR); //honestly this might not even be the right error codei forgot
        audity_entry(request_get_str(conn_get_request(conn)), uri, status_code,
            conn_get_header(conn, "Request-Id"));
        close(open_get);
        return;
    }
    //todo queue stuff lock mutex then unlock flock stuff someone give mitch a raise
    flock(open_get, LOCK_SH); //apply the shared lock on the file https://linux.die.net/man/2/flock
    pthread_mutex_unlock(&mutex);

    conn_send_file(conn, open_get, fileStat.st_size);
    uint16_t status_code = response_get_code(&RESPONSE_OK);
    audity_entry(request_get_str(conn_get_request(conn)), uri, status_code,
        conn_get_header(conn, "Request-Id"));
    flock(open_get, LOCK_UN); //mitch
    close(open_get);
    return;
}

void handle_put(conn_t *conn) {
    // TODO: Implement PUT

    char *uri = conn_get_uri(conn);
    // debug("PUT %s", uri);
    const Response_t *res;
    pthread_mutex_lock(&mutex);
    bool existed = false;
    //check if the file exists
    struct stat fileStat; //https://man7.org/linux/man-pages/man2/stat.2.html
    if (stat(uri, &fileStat)
        == 0) { //i was cooking with this too tired now. send the response message of that we craeted it. OK->CREATED If the file already exists then the response is ok else its created
        //when stat == 0 return value means it already exists so the response will be RESPONSE_OK
        res = &RESPONSE_OK;
        existed = true; //the file already existed
    }
    if (existed != true) {
        res = &RESPONSE_CREATED;
    }
    int open_file = open(uri, O_WRONLY | O_CREAT | O_TRUNC, 0644); //create open the file
    if (open_file <= 0) { //open failed
        if (access(uri, W_OK) != 0) {
            pthread_mutex_unlock(&mutex);
            conn_send_response(conn, &RESPONSE_FORBIDDEN);
            uint16_t status_code = response_get_code(
                &RESPONSE_FORBIDDEN); //honestly this might not even be the right error codei forgot
            audity_entry(request_get_str(conn_get_request(conn)), uri, status_code,
                conn_get_header(conn, "Request-Id"));
            close(open_file);
            return;
        } else {
            pthread_mutex_unlock(&mutex);
            conn_send_response(conn, &RESPONSE_INTERNAL_SERVER_ERROR);
            uint16_t status_code = response_get_code(
                &RESPONSE_INTERNAL_SERVER_ERROR); //honestly this might not even be the right error codei forgot
            audity_entry(request_get_str(conn_get_request(conn)), uri, status_code,
                conn_get_header(conn, "Request-Id"));
            close(open_file);
            return;
        }
    }
    flock(open_file, LOCK_EX); //exclusive lock https://linux.die.net/man/2/flock
    pthread_mutex_unlock(&mutex);
    ftruncate(open_file, 0); //piazza
    conn_recv_file(conn, open_file);
    uint16_t status_code = response_get_code(res);
    audity_entry(request_get_str(conn_get_request(conn)), uri, status_code,
        conn_get_header(conn, "Request-Id"));
    conn_send_response(conn, res);
    flock(open_file, LOCK_UN); //mitch
    close(open_file);
    return;
}

//shoudnt need to change these two functions below
void handle_unsupported(conn_t *conn) {
    //debug("Unsupported request");
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
    return;
}

//<Oper>,<URI>,<Status-Code>,<RequestID header value>\n
void audity_entry(
    const char *operation, const char *uri, uint16_t status_code, const char *request_id) {
    fprintf(stderr, "%s,%s,%hu,%s\n", operation, uri, status_code, request_id);
}

void *worker_thread(void *args) {
    (void) args; //ok chatgpt helped me fixed this error
    while (1) {
        intptr_t connfd;
        queue_pop(g_queue, (void *) &connfd);
        handle_connection(connfd);
        close(connfd);
    }
}
