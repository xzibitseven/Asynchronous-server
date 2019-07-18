#include <arpa/inet.h>
#include <errno.h>
#include <event2/event.h>
#include <netinet/in.h> 
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>

#define READ_BUFF_SIZE 16
#define WRITE_BUFF_SIZE 3

typedef struct connection_ctx_t {
    struct connection_ctx_t* next;
    struct connection_ctx_t* prev;

    evutil_socket_t fd;
    struct event_base* base;
    struct event* read_event;
    struct event* write_event;

    uint8_t read_buff[READ_BUFF_SIZE];
    uint64_t write_buff[WRITE_BUFF_SIZE];

    ssize_t read_buff_used;
    ssize_t write_buff_used;
} connection_ctx_t;

typedef struct sub_sequence{
    uint8_t  flag;
    uint16_t step;
    uint64_t part;
} sub_sequence;

sub_sequence sequence[3];
struct event* gen_and_write_seq_event = NULL;
size_t count_clients;

// error output
void error(const char* msg) {
    fprintf(stderr, "%s, errno = %d\n", msg, errno);
    exit(1);
}

void error_no_exit(const char* msg) {
    fprintf(stderr, "%s, errno = %d\n", msg, errno);
}

// called manually
void on_close(connection_ctx_t* ctx){
    printf("[%p] on_close called, fd = %d\n", ctx, ctx->fd);

    // remove ctx from the lined list
    ctx->prev->next = ctx->next;
    ctx->next->prev = ctx->prev;

    if(ctx->read_event){
        event_del(ctx->read_event);
        event_free(ctx->read_event);
    }

    if(ctx->write_event){
        event_del(ctx->write_event);
        event_free(ctx->write_event);
    }

    close(ctx->fd);
    free(ctx);
}

// checks if the string is a natural number
uint8_t is_str_digit(const char* str){

    const char* digit = str;
    while(*digit){
        if(!isdigit(*digit)){
            printf("The string is not a number!\n");
            return 0;
        }
        digit++;
    }
    return 1;
}

//
void check_sub_sequence(sub_sequence* sequence){

    char* token = strtok(NULL, " ");
    if(token && (strlen(token)<=4) && is_str_digit(token)){
        char* end;
        sequence->part = strtoul(token, &end, 10);
        token = strtok(NULL, " ");
        if(token && (strlen(token)<=4) && is_str_digit(token)){
            sequence->step = strtoul(token, &end, 10);
            sequence->flag = 1;
        }
        else
            printf("Incorrect step input of the subsequence (must match the mask yyyy)!\n");
    }
    else
        printf("Incorrect input of the initial value of the sub-sequence (must match the mask xxxx)!\n");
}

void generator_sequence(evutil_socket_t fd, short flags, void* arg){

    if((((connection_ctx_t*)arg)->next == arg) ||
            (!sequence[0].flag && !sequence[1].flag && !sequence[2].flag)){
        printf("No connected clients or all three sequences are ignored!\n");
        if(event_del(gen_and_write_seq_event) < 0)
            error("event_del() failed");
        event_free(gen_and_write_seq_event);
        gen_and_write_seq_event = NULL;
        return;
    }

    connection_ctx_t* ctx = ((connection_ctx_t*)arg)->next;
    if(!ctx->write_buff_used){
        size_t i, j=0;
        for(i=0; i<WRITE_BUFF_SIZE; ++i)
        {
            if(sequence[i].flag){
                if((ctx->write_buff[j] + sequence[i].step) > UINT64_MAX)
                    ctx->write_buff[j] = sequence[i].part;

                else
                    ctx->write_buff[j] += sequence[i].step;

                j++;
            }
        }
        ctx->write_buff_used = sizeof(ctx->write_buff[0])*j;
    }

        if(event_add(ctx->write_event, NULL) < 0)
            error("event_add(peer->write_event, ...) failed");

        count_clients++;
        if(ctx->read_event)
            event_del(ctx->read_event);

        connection_ctx_t* peer = ctx->next;
        while(peer != ctx){
            if(peer->write_event == NULL) { // list head, skipping
                peer = peer->next;
                continue;
            }

           // printf("[%p] sending a message to %p...\n", ctx, peer);

            size_t i;
            for(i=0; i<WRITE_BUFF_SIZE; ++i)
                peer->write_buff[i] = ctx->write_buff[i];
            peer->write_buff_used = ctx->write_buff_used;

            // add writing event (it's not a problem to call it multiple times)
            if(event_add(peer->write_event, NULL) < 0)
                error("event_add(peer->write_event, ...) failed");
            count_clients++;
            if(peer->read_event){
                event_del(peer->read_event);
            }
            peer = peer->next;
        }

        if(event_del(gen_and_write_seq_event) < 0)
            error("event_del() failed");
}


// command check input by user
void check_command(char* str, connection_ctx_t* ctx){

    char* token = strtok(str, " ");
    if(!token){
        printf("Incorrect input of a subsequence!\n");
        return;
    }
    if(!strcmp(token, "seq1")){
        check_sub_sequence(&sequence[0]);
    }
    else if(!strcmp(token, "seq2")){
        check_sub_sequence(&sequence[1]);
    }
    else if(!strcmp(token, "seq3")){
        check_sub_sequence(&sequence[2]);
    }
    else if(!strcmp(token, "export")){
         token = strtok(NULL, " ");
         if(!strcmp(token, "seq")){
            if(sequence[0].flag && sequence[1].flag && sequence[2].flag){

                size_t i,j=0;
                for(i=0; i<WRITE_BUFF_SIZE; ++i){
                    if(sequence[i].part && sequence[i].step)
                         ctx->write_buff[j++] = sequence[i].part;
                    else
                        sequence[i].flag = 0;
                }
                ctx->write_buff_used = sizeof(ctx->write_buff[0])*j;

                gen_and_write_seq_event = event_new(ctx->base, -1, EV_PERSIST, generator_sequence, (void*)ctx->prev);
                if(!gen_and_write_seq_event)
                    error("event_new(... EV_WRITE ...) failed");

                if(event_add(gen_and_write_seq_event, NULL) < 0)
                    error("event_add(gen_and_write_seq_event, ...) failed");

                event_active(gen_and_write_seq_event, EV_PERSIST, 0);
            }
         }
    }
    else
        printf("Incorrect input of a subsequence!\n");
}


void on_read(evutil_socket_t fd, short flags, void* arg){

    connection_ctx_t* ctx = arg;
    printf("[%p] on_read called, fd = %d\n", ctx, fd);

    ssize_t bytes;
    for(;;) {
        bytes = read(fd, ctx->read_buff + ctx->read_buff_used, READ_BUFF_SIZE - ctx->read_buff_used);
        if(bytes == 0) {
            printf("[%p] client disconnected!\n", ctx);
            on_close(ctx);
            return;
        }

        if(bytes < 0) {
            if(errno == EINTR)
                continue;

            printf("[%p] read() failed, errno = %d, closing connection.\n", ctx, errno);
            on_close(ctx);
            return;
        }

        break; // read() succeeded
    }


    ssize_t check = ctx->read_buff_used;
    ssize_t check_end = ctx->read_buff_used + bytes;
    ctx->read_buff_used = check_end;

    while(check < check_end){
        if(ctx->read_buff[check] != '\n'){
            check++;
            continue;
        }

        int length = (int)check;
        ctx->read_buff[length] = '\0';
        if((length > 0) && (ctx->read_buff[length - 1] == '\r')){
            ctx->read_buff[length - 1] = '\0';
            length--;
        }

        check_command((char*)(ctx->read_buff), ctx);
        // shift read_buff (optimize!)
        memmove(ctx->read_buff, ctx->read_buff + check, check_end - check - 1);
        ctx->read_buff_used -= check + 1;
        check_end -= check;
        check = 0;
    }

    if(ctx->read_buff_used == READ_BUFF_SIZE) {
        printf("[%p] client sent a very long string, closing connection.\n", ctx);
        on_close(ctx);
    }
}

void on_write(evutil_socket_t fd, short flags, void* arg){
    connection_ctx_t* ctx = arg;
  //  printf("[%p] on_write called, fd = %d\n", ctx, fd);

    ssize_t bytes;
    for(;;) {
        bytes = write(fd, ctx->write_buff, ctx->write_buff_used);
        if(bytes <= 0) {
            if(errno == EINTR)
                continue;

            printf("[%p] write() failed, errno = %d, closing connection.\n", ctx, errno);
            on_close(ctx);

            count_clients--;
            if(!count_clients)
                event_active(gen_and_write_seq_event, EV_PERSIST, 0);
            return;
        }

        break; // write() succeeded
    }

    // shift the write_buffer (optimize!)
    memmove(ctx->write_buff, ctx->write_buff + bytes, ctx->write_buff_used - bytes);
    ctx->write_buff_used -= bytes;

    // if there is nothing to send call event_del
    if(ctx->write_buff_used == 0) {
 //       printf("[%p] write_buff is empty, calling event_del(write_event)\n", ctx);
        if(event_del(ctx->write_event) < 0)
            error("event_del() failed");
    }

    count_clients--;
    if(!count_clients)
        //if the current sequence is passed to all existing clients, generate a new one.
        event_active(gen_and_write_seq_event, EV_PERSIST, 0);
}

void on_accept(evutil_socket_t listen_sock, short flags, void* arg){

    connection_ctx_t* head_ctx = (connection_ctx_t*)arg;
    evutil_socket_t fd = accept(listen_sock, 0, 0);
    if(fd < 0){
        if(head_ctx->next == head_ctx)
            error("accept() failed");
        else{
            error_no_exit("accept() failed");
            return;
        }
    }

    // make in nonblocking
    if(evutil_make_socket_nonblocking(fd) < 0){
        if(head_ctx->next == head_ctx)
            error("evutil_make_socket_nonblocking() failed");
        else{
            error_no_exit("evutil_make_socket_nonblocking() failed");
            close(fd);
            return;
        }
    }

    connection_ctx_t* ctx = (connection_ctx_t*)malloc(sizeof(connection_ctx_t));
    if(!ctx){
        if(head_ctx->next == head_ctx)
            error("malloc() failed");
        else{
            error_no_exit("malloc() failed");
            close(fd);
            return;
        }
    }

    // add ctx to the linked list
    ctx->prev = head_ctx;
    ctx->next = head_ctx->next;
    head_ctx->next->prev = ctx;
    head_ctx->next = ctx;

    ctx->base = head_ctx->base;
    ctx->read_event = NULL;
    ctx->write_event = NULL;

    ctx->read_buff_used = 0;
    ctx->write_buff_used = 0;

    printf("[%p] New connection! fd = %d\n", ctx, fd);

    ctx->fd = fd;

    ctx->write_event = event_new(ctx->base, fd, EV_WRITE | EV_PERSIST, on_write, (void*)ctx);
    if(!ctx->write_event){
        if(head_ctx->next == head_ctx)
            error("event_new(... EV_WRITE ...) failed");
        else{
            error_no_exit("event_new(... EV_WRITE ...) failed");
            on_close(ctx);
            return;
        }
    }

    if(event_priority_set(ctx->write_event, 1) < 0){
        if(head_ctx->next == head_ctx)
            error("event_priority_set() failed");
        else{
            error_no_exit("event_priority_set() failed");
            on_close(ctx);
            return;
        }
    }

    if(!gen_and_write_seq_event){

        ctx->read_event = event_new(ctx->base, fd, EV_READ | EV_PERSIST, on_read, (void*)ctx);
        if(!ctx->read_event){
            if(head_ctx->next == head_ctx)
                error("event_new(... EV_READ ...) failed");
            else{
                error_no_exit("event_new(... EV_READ ...) failed");
                on_close(ctx);
                return;
            }
        }

        if(event_priority_set(ctx->read_event, 1) < 0){
            if(head_ctx->next == head_ctx)
                error("event_priority_set() failed");
            else{
                error_no_exit("event_priority_set() failed");
                on_close(ctx);
                return;
            }
        }

        if(event_add(ctx->read_event, NULL) < 0){
           if(head_ctx->next == head_ctx)
                error("event_add(read_event, ...) failed");
           else{
                error_no_exit("event_add(read_event, ...) failed");
                on_close(ctx);
                return;
           }
        }
    }
}

void run(char* host, int port){
    // allocate memory for a connection ctx (used as linked list head)
    connection_ctx_t* head_ctx = (connection_ctx_t*)malloc(sizeof(connection_ctx_t));
    if(!head_ctx)
        error("malloc() failed");

    head_ctx->next = head_ctx;
    head_ctx->prev = head_ctx;
    head_ctx->write_event = NULL;
    head_ctx->read_buff_used = 0;
    head_ctx->write_buff_used = 0;

    // create a socket
    head_ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
    if(head_ctx->fd < 0)
        error("socket() failed");

    // make it nonblocking
    if(evutil_make_socket_nonblocking(head_ctx->fd) < 0)
        error("evutil_make_socket_nonblocking() failed");

    // bind and listen
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(host);
    if(bind(head_ctx->fd, (struct sockaddr*)&sin, sizeof(sin)) < 0)
        error("bind() failed");

    if(listen(head_ctx->fd, 1000) < 0)
        error("listen() failed");

    // create an event base
    struct event_base* base = event_base_new();
    if(!base)
        error("event_base_new() failed");

    if(event_base_priority_init(base, 2) < 0)
        error("event_base_priority_init() failed");


    // create a new event
    struct event* accept_event = event_new(base, head_ctx->fd, EV_READ | EV_PERSIST, on_accept, (void*)head_ctx);
    if(!accept_event)
        error("event_new() failed");

    head_ctx->base = base;
    head_ctx->read_event = accept_event;

    if(event_priority_set(accept_event, 0) < 0)
        error("event_priority_set() failed");

    // schedule the execution of accept_event
    if(event_add(accept_event, NULL) < 0)
        error("event_add() failed");

    // run the event dispatching loop
    if(event_base_dispatch(base) < 0)
        error("event_base_dispatch() failed");

    // free allocated resources
    on_close(head_ctx);
    event_free(accept_event);
    event_base_free(base);
}

/*
 * If client will close a connection send() will just return -1
 * instead of killing a process with SIGPIPE.
 */
void ignore_sigpipe(){
   /* sigset_t msk;
    if(sigemptyset(&msk) < 0)
        error("sigemptyset() failed");

    if(sigaddset(&msk, SIGPIPE) < 0)
        error("sigaddset() failed");
*/
    sigset_t mask;
    sigset_t orig_mask;
    sigemptyset(&mask);

    sigaddset(&mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &mask, &orig_mask) < 0) {
        perror ("sigprocmask");
            return;
    }
    //    if(pthread_sigmask(SIG_BLOCK, &msk, nullptr) != 0)
    //        error("pthread_sigmask() failed");
}

int main(int argc, char** argv){

    if(argc < 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }

    char* host = argv[1];
    int port = atoi(argv[2]);

    printf("Starting server on %s:%d\n", host, port);
    ignore_sigpipe();
    run(host, port);
    if(gen_and_write_seq_event)
        event_free(gen_and_write_seq_event);

    return 0;
}


