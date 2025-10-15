// 
// zk Need a file header with description and author name.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

#define MAX_MESSAGE_LEN 100 //max length of message is 100 characters

//message header for pipes
struct msg_hdr {
    int dest;   //destination node id or -1 for empty apple 
    int length; //length in bytes
};

//globals for sig handlers 
static pid_t *child_pids = NULL;
static int total_k = 0;
static int is_parent = 0;

//writes n bytes
ssize_t writen(int fd, const void *buf, size_t n) {
    
    size_t left = n;
    const char *ptr = buf;

    while (left > 0) {
        ssize_t w = write(fd, ptr, left);

        if (w <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        
        left -= w;
        ptr += w;
    }

    return n;
}

//reads n bytes
ssize_t readn(int fd, void *buf, size_t n) {
    
    size_t left = n;
    char *ptr = buf;

    while (left > 0) {
        ssize_t r = read(fd, ptr, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return (ssize_t)(n - left);

        left -= r;
        ptr += r;
    }

    return n;
}

//parent sigint handler, terminates children then exits
void parent_sigint_handler(int signo) {

    (void)signo;
    if (!is_parent) return;
    printf("\nParent: user typed ^C, shutting down children...\n");

    if (child_pids) {

        //terminates child
        for (int i = 1; i < total_k; ++i) {
            if (child_pids[i] > 0) {
                kill(child_pids[i], SIGTERM);
            }
        }

        //waits for children to terminate
        for (int i = 1; i < total_k; ++i) {
            if (child_pids[i] > 0) {
                int status;
                waitpid(child_pids[i], &status, 0);
                printf("Parent: child %d exited (status=%d)\n", child_pids[i], status);
            }
        }
    }

    free(child_pids);
    exit(0);
}

//child sigterm handler, terminates child when done
void child_sigterm_handler(int signo) {
    (void)signo;
    exit(0);
}

//trim trailing new line from string
void trim_newline(char *s) {
    size_t n = strlen(s);
    if (n == 0) return;
    if (s[n-1] == '\n') s[n-1] = '\0';
}

//process handling for nodes
int node_main(int node_id, int k, int read_fd, int write_fd) {
    struct msg_hdr header;
    char *message = NULL;

    printf("Node %d: started. Read FD=%d, Write FD=%d\n", node_id, read_fd, write_fd);

    for (;;) {
        //blocking read on header
        ssize_t read_n = readn(read_fd, &header, sizeof(header));

        if (read_n <= 0) {

            if (read_n == 0) {
                //close writer
                fprintf(stderr, "Node %d: read returned 0.\n", node_id);
                break;
            } 

            else {
                perror("read header error");
                break;
            }
        }

        //read if there is a message
        if (header.length > 0) {

            if (header.length > MAX_MESSAGE_LEN) { //makes sure message does not exceed allowed length
                fprintf(stderr, "Node %d: message too large (%d).\n", node_id, header.length);
                header.length = MAX_MESSAGE_LEN;
            }

            message = malloc(header.length + 1);

            if (!message) { //make sure message is there
                perror("malloc error for message");
                return 1;
            }

            ssize_t read_n2 = readn(read_fd, message, header.length);
            if (read_n2 != header.length) { //make sure length of message is the same as what headers length is
                fprintf(stderr, "Node %d: failed to read full message (got %zd expected %d)\n", node_id, read_n2, header.length);
                free(message);
                message = NULL;
                break;
            }
            message[header.length] = '\0';
        } 

        else {
            free(message);
            message = NULL;
        }

        int prev = (node_id - 1 + k) % k;
        int next = (node_id + 1) % k;

        printf("Node %d: received apple from node %d. Contains message (dest=%d, len=%d).\n",
               node_id, prev, header.dest, header.length);

        if (header.dest == node_id) { //destination node recieves message
            if (header.length > 0) {
                printf("Node %d: I am the destination. Message: \"%s\" \n", node_id, message);
            } else {
                printf("Node %d: I am destination, but message empty.\n", node_id);
            }

            //make header empty apple and clear message, new destination is parent node
            header.dest = -1;
            header.length = 0;
            free(message);
            message = NULL;

            printf("Node %d: received message, setting apple to EMPTY and forwarding to next node %d\n\n", node_id, next);
        } 
        else {
            //not destination node, continue to next node
            if (header.dest == -1) {//empty apple, continues till node 0
                
                printf("Node %d: apple is EMPTY; forwarding to node %d\n\n", node_id, next);
            } 
            else { //not destinantion node but apple not empty
                printf("Node %d: forwarding message to node %d. (Destination Node = %d)\n\n", node_id, next, header.dest);
            }
        }

        //write the header
        if (writen(write_fd, &header, sizeof(header)) != sizeof(header)) {
            perror("writen(header)");
            break;
        }

        //write message if there
        if (header.length > 0) {

            if (!message) {
                char empty = '\0';
                if (writen(write_fd, &empty, 1) != 1) {
                    perror("mesasge write error");
                    break;
                }
            } 

            else {
                if (writen(write_fd, message, header.length) != header.length) {
                    perror("write message error");
                    break;
                }
            }
        }
    }

    //cleanup when finished
    close(read_fd);
    close(write_fd);
    printf("Node %d: closing and exiting.\n", node_id);
    return 0;
}

int main(void) {

    int k;
    printf("Parent: enter number of nodes in ring (k >= 2): ");

    if (scanf("%d", &k) != 1) {
        fprintf(stderr, "Failed to read integer k.\n");
        return 1;
    }
    if (k < 2) {
        fprintf(stderr, "k must be >= 2\n");
        return 1;
    }

    //clear leftover newline
    int c;
    while ((c = getchar()) != EOF && c != '\n');

    total_k = k;
    is_parent = 1;

    //allocate pipes for k nodes
    // zk Newer C compiler will allow you to statically define int pipe_fds[k][2]
    int (*pipe_fds)[2] = malloc(sizeof(int[2]) * k);
    if (!pipe_fds) { perror("malloc pipes"); return 1; }

    for (int i = 0; i < k; ++i) {
        if (pipe(pipe_fds[i]) < 0) {
            perror("pipe");
            return 1;
        }
    }

    //array to holde child PIDs where the parent is node 0
    child_pids = calloc(k, sizeof(pid_t));
    if (!child_pids) { 
        perror("calloc child_pids"); 
        return 1; 
    }

    child_pids[0] = getpid();

    //fork children
    int node_id = 0;
    for (int i = 1; i < k; ++i) {

        pid_t pid = fork();

        if (pid < 0) { //make sure fork worked
            perror("fork"); 
            return 1; 
        }
        if (pid == 0) { //child
            node_id = i;
            is_parent = 0;
            break;
        } 
        else {
            //parent holds child's pid in array
            child_pids[i] = pid;
        }
    }

    //signals
    if (is_parent) {

        struct sigaction sa;
        sa.sa_handler = parent_sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
    } 
    
    else {
        struct sigaction sa;
        sa.sa_handler = child_sigterm_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, NULL);
        signal(SIGINT, SIG_IGN);
    }


    int read_fd = pipe_fds[(node_id - 1 + k) % k][0];
    int write_fd = pipe_fds[node_id][1];

    for (int j = 0; j < k; ++j) {
        if (j == node_id) { //pipe's write end is kept and closes the read end
            close(pipe_fds[j][0]);
            continue;
        } 
        
        else if (j == (node_id - 1 + k) % k) { //pipe's read end is kept and closes the write end
            close(pipe_fds[j][1]);
            continue;
        } 
        
        else { //close ends
            close(pipe_fds[j][0]);
            close(pipe_fds[j][1]);
        }
    }

    //Free the pipe_fds container
    free(pipe_fds);

    //child processes run node_main
    if (!is_parent) { //child runs node_main and exits when it returns
        int returnc = node_main(node_id, k, read_fd, write_fd);
        return returnc;
    }

    //parent loop, prompt user, and writing the message into the ring
    printf("Parent (node 0): spawned %d children. Ready.\n", k - 1);
    printf("Parent: verbose diagnostics will show node activity.\n");

    char input_buf[MAX_MESSAGE_LEN];
    while (1) {

        int dest;
        //prompt destination
        printf("\nParent (node 0): enter destination node id or ^C to exit: ");
        fflush(stdout);

        if (scanf("%d", &dest) != 1) {
            fprintf(stderr, "Parent: failed to read destination.\n");
            break;
        }

        while ((c = getchar()) != EOF && c != '\n'); //clear rest of user input

        if (dest == -1) {
            //user exitsa and sends SIGTERM to children and breaks
            printf("Parent: exiting. Sending SIGTERM to children...\n");
            for (int i = 1; i < k; ++i) kill(child_pids[i], SIGTERM);
            for (int i = 1; i < k; ++i) waitpid(child_pids[i], NULL, 0);
            printf("Parent: all children terminated. Exiting.\n");
            break;
        }
        if (dest < 0 || dest >= k) {
            printf("Parent: invalid destination %d. Must be 0 %d\n", dest, k - 1);
            continue;
        }

        //prompt message line
        printf("Parent (node 0): enter message to send to node %d: ", dest);

        if (!fgets(input_buf, sizeof(input_buf), stdin)) {
            fprintf(stderr, "Parent: fgets failed.\n");
            break;
        }

        trim_newline(input_buf); //trim_newline from earlier comes into use

        int msg_len = (int)strlen(input_buf);
        if (msg_len > MAX_MESSAGE_LEN) msg_len = MAX_MESSAGE_LEN;

        //creates header and write to write_fd (send to node 1) */
        struct msg_hdr header;
        header.dest = dest;
        header.length = msg_len;

        printf("Parent (node 0): sending apple to node %d (dest=%d, len=%d)\n", (0 + 1) % k, header.dest, header.length);

        if (writen(write_fd, &header, sizeof(header)) != sizeof(header)) {
            perror("Parent writen(header)");
            break;
        }
        if (header.length > 0) {
            if (writen(write_fd, input_buf, header.length) != header.length) {
                perror("Parent writen(message)");
                break;
            }
        }

        //blocks reading till empty apple
        for (;;) {
            struct msg_hdr incoming;
            ssize_t rr = readn(read_fd, &incoming, sizeof(incoming));
            if (rr <= 0) {
                if (rr == 0) {
                    fprintf(stderr, "Parent: read returned 0 (pipe closed). Exiting.\n");
                    goto cleanup;
                } else {
                    perror("Parent readn(header)");
                    goto cleanup;
                }
            }
            char *recv_message = NULL;
            if (incoming.length > 0) {
                recv_message = malloc(incoming.length + 1);
                if (!recv_message) { perror("malloc"); goto cleanup; }
                ssize_t rr2 = readn(read_fd, recv_message, incoming.length);
                if (rr2 != incoming.length) {
                    fprintf(stderr, "Parent: failed to read message fully\n");
                    free(recv_message);
                    goto cleanup;
                }
                recv_message[incoming.length] = '\0';
            }

            int prev = (0 - 1 + k) % k;
            printf("Parent (node 0): received apple from node %d. (dest=%d, len=%d)\n",
                   prev, incoming.dest, incoming.length);

            if (incoming.dest == -1) {
                printf("Parent (node 0): apple returned empty. Ready for next message.\n");
                free(recv_message);
                break; /* go prompt for next */
            } else {
                /* Parent must forward it on the ring (parent is node 0) */
                printf("Parent (node 0): forwarding message to node %d. (dest=%d)\n", (0 + 1) % k, incoming.dest);

                if (writen(write_fd, &incoming, sizeof(incoming)) != sizeof(incoming)) {
                    perror("Parent forward writen(header)");
                    free(recv_message);
                    goto cleanup;
                }
                if (incoming.length > 0) {
                    if (writen(write_fd, recv_message, incoming.length) != incoming.length) {
                        perror("Parent forward writen(message)");
                        free(recv_message);
                        goto cleanup;
                    }
                }
                free(recv_message);
                //continue till
            }
        }
    }

cleanup:
    //cleaup helper
    close(read_fd);
    close(write_fd);
    free(child_pids);
    return 0;
}
