#include <unistd.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <map>
#include <algorithm>
#include <semaphore.h>

using namespace std;

#define MAX_USER 30
#define OUT_TO_PIPE 2

#define buf_size 15010

typedef struct {
    unsigned int cmd_no;
    int argc;
    char **argv;
    char *fname;
} Cmd;

enum OutputRedirection {
    out_normal,
    to_file,
    to_null,
    to_pipe,
    to_pipe_with_err,
    to_user_pipe
};

enum InputRedirection {
    in_normal,
    from_null,
    from_pipe,
    from_user_pipe
};

typedef struct {
    InputRedirection in_action;
    OutputRedirection out_action;
    int next_n;
    int from_user;
    int to_user;
} IORedirection;

typedef struct {
    int pipe_fd[2];
} Pipe_target;

struct client_info {
    sem_t sem;
    int pid;
    char IP[INET_ADDRSTRLEN];
    int port;
    char name[30];
} *client;

struct msg {
    sem_t sem;
    sem_t send_done;
    char buf[15010];
} *msg_buf;

int shmid_client, shmid_msg_buf, shmid_user_pipe;
int client_ptr, client_fd;

map<int, Pipe_target> waiting_pipes;
struct user_pipe_info {
    sem_t sem;
    bool exist;
    int waiting_pid;
    char fifo_name[20];
} *user_pipe;

void sig_handler (int signal) {
    if(signal == SIGUSR1) {
        send(client_fd, msg_buf->buf, strlen(msg_buf->buf), MSG_NOSIGNAL);
        sem_post(&msg_buf->send_done);
    }
    else if (signal == SIGINT) {
        for (int i = 0; i < MAX_USER; i++) {
            for (int j = 0; j < MAX_USER; j++) {
                unlink(user_pipe[i*MAX_USER+j].fifo_name);
            }
        }

        shmdt(client);
        shmdt(msg_buf);
        shmdt(user_pipe);
        shmctl(shmid_client, IPC_RMID, NULL); 
        shmctl(shmid_msg_buf, IPC_RMID, NULL); 
        shmctl(shmid_user_pipe, IPC_RMID, NULL); 
        exit(0);
    }
    return;
}

void broadcast(char *local_msg) {
    int send_num = 0;
    sem_wait(&msg_buf->sem);
    sprintf(msg_buf->buf, "%s", local_msg);

    for(int i = 0; i < MAX_USER; i++) {
        if (client[i].pid > 0) {            
            kill(client[i].pid, SIGUSR1);
            send_num++;
        }
    }

    while (send_num--) {
        sem_wait(&msg_buf->send_done);
    }

    sem_post(&msg_buf->sem);
}

void sendToSomeone(int pid, char *local_msg) {
    sem_wait(&msg_buf->sem);
    sprintf(msg_buf->buf, "%s", local_msg);
    kill(pid, SIGUSR1);
    sem_wait(&msg_buf->send_done);
    sem_post(&msg_buf->sem);
}

void SetupCmd(int client_ptr, char *input_cmd, Cmd *cmd, char **rest_cmd, IORedirection* io_action) {
    char *argv_ptr;

    cmd->argv = (char **)malloc(sizeof(char *) * 20);
    *rest_cmd = input_cmd;
    cmd->argc = 0;
    do {
        argv_ptr = strtok(*rest_cmd, " ");
        *rest_cmd = strtok(NULL, "\0");
        cmd->argv[cmd->argc] = argv_ptr;
        cmd->argc++;
    } while (*rest_cmd && !strchr("|!><", *rest_cmd[0]));
    cmd->argv[cmd->argc] = NULL;
    
    io_action->out_action = out_normal;
    io_action->in_action = in_normal;
    while (*rest_cmd && strchr("|!><", *rest_cmd[0])) {
        argv_ptr = strtok(*rest_cmd, " ");
        *rest_cmd = strtok(NULL, "\0");
        if(argv_ptr[0] == '|') {
            io_action->out_action = to_pipe;
            if(strlen(argv_ptr) != 1) {
                io_action->next_n = atoi(argv_ptr+1);
            }
            else {
                io_action->next_n = 1;
            }
        }
        else if(argv_ptr[0] == '!') {
            io_action->out_action = to_pipe_with_err;
            if(strlen(argv_ptr) != 1) {
                io_action->next_n = atoi(argv_ptr+1);
            }
            else {
                io_action->next_n = 1;
            }
        }
        else if(argv_ptr[0] == '>') {
            if(strlen(argv_ptr) != 1) {
                io_action->out_action = to_user_pipe;
                io_action->to_user = atoi(argv_ptr+1)-1;
            }
            else {
                io_action->out_action = to_file;
                cmd->fname = strtok(*rest_cmd, " ");
                *rest_cmd = strtok(NULL, "\0");
            }
        }
        else if(argv_ptr[0] == '<') {
            if(strlen(argv_ptr) != 1) {
                io_action->in_action = from_user_pipe;
                io_action->from_user = atoi(argv_ptr+1)-1;
            }
        }
    }

    if (waiting_pipes.find(cmd->cmd_no) != waiting_pipes.end()) {
        io_action->in_action = from_pipe;
    }
}

void ExecCmd(int client_ptr, Cmd cmd, IORedirection io_action, char *rcv_buf) {
    int file_fd;
    char cmd_buf[buf_size], err_in_buf[buf_size], err_out_buf[buf_size];

    // cout << cmd.argv[0] << io_action.in_action << io_action.out_action << endl;
    if (strcmp("printenv", cmd.argv[0]) == 0) {
        char *env_str;
        env_str = getenv(cmd.argv[1]);
        if (env_str) {
            sprintf(cmd_buf, "%s\n", env_str);
            send(client_fd, cmd_buf, strlen(cmd_buf), 0);
        }
    }
    else if (strcmp("setenv", cmd.argv[0]) == 0) {
        if (cmd.argc >= 3)
            setenv(cmd.argv[1], cmd.argv[2], 1);
    }
    else {
        int status, ret;
        Pipe_target out_target;
        map<int, Pipe_target>::iterator in_it, out_it;

        if (io_action.in_action == from_pipe) {
            in_it = waiting_pipes.find(cmd.cmd_no);
            close(in_it->second.pipe_fd[1]);
        }
        else if (io_action.in_action == from_user_pipe) {
            sem_wait(&client[io_action.from_user].sem);
            if (client[io_action.from_user].pid == -1) {
                sem_post(&client[io_action.from_user].sem);
                sprintf(err_in_buf, "*** Error: user #%d does not exist yet. ***\n", io_action.from_user+1);
                io_action.in_action = from_null;
            }
            else {
                sem_post(&client[io_action.from_user].sem);
                sem_wait(&user_pipe[client_ptr*MAX_USER+io_action.from_user].sem);
                if (user_pipe[client_ptr*MAX_USER+io_action.from_user].exist == false) {
                    sem_post(&user_pipe[client_ptr*MAX_USER+io_action.from_user].sem);
                    sprintf(err_in_buf, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", io_action.from_user+1, client_ptr+1);
                    io_action.in_action = from_null;
                }
                else {
                    sprintf(cmd_buf, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n", client[client_ptr].name, client_ptr+1, client[io_action.from_user].name, io_action.from_user+1, rcv_buf);
                    broadcast(cmd_buf);
                }
            }
        }

        if (io_action.out_action > OUT_TO_PIPE) {
            if (io_action.out_action == to_pipe || io_action.out_action == to_pipe_with_err) {
                out_it = waiting_pipes.find(cmd.cmd_no + io_action.next_n);
                if (out_it == waiting_pipes.end()) {
                    while (pipe(out_target.pipe_fd) < 0) {
                        usleep(1000);
                    }
                    out_it = waiting_pipes.insert(pair<int, Pipe_target>(cmd.cmd_no + io_action.next_n, out_target)).first;
                }
            }
            else if (io_action.out_action == to_user_pipe) {
                sem_wait(&client[io_action.to_user].sem);
                if (client[io_action.to_user].pid == -1) {
                    sem_post(&client[io_action.to_user].sem);
                    sprintf(err_out_buf, "*** Error: user #%d does not exist yet. ***\n", io_action.to_user+1);
                    io_action.out_action = to_null;
                }
                else {
                    sem_post(&client[io_action.to_user].sem);
                    sem_wait(&user_pipe[io_action.to_user*MAX_USER+client_ptr].sem);
                    if (user_pipe[io_action.to_user*MAX_USER+client_ptr].exist) {
                        sem_post(&user_pipe[io_action.to_user*MAX_USER+client_ptr].sem);
                        sprintf(err_out_buf, "*** Error: the pipe #%d->#%d already exists. ***\n", client_ptr+1, io_action.to_user+1);
                        io_action.out_action = to_null;
                    }
                    else {
                        sprintf(cmd_buf, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n", client[client_ptr].name, client_ptr+1, rcv_buf, client[io_action.to_user].name, io_action.to_user+1);
                        broadcast(cmd_buf);
                    }
                }
            }
        }
        
        pid_t pid;
        while ((pid = fork()) < 0) {
            usleep(1000);
        }
        if (pid == 0) { // child process
            // cout << cmd.argv[0] << endl;
            if (io_action.in_action == from_null) {
                file_fd = open("/dev/null", (O_RDWR));
                dup2(file_fd, STDIN_FILENO);
                close(file_fd);
            }
            else if (io_action.in_action == from_pipe) {
                dup2(in_it->second.pipe_fd[0], STDIN_FILENO);
                close(in_it->second.pipe_fd[0]);
                waiting_pipes.erase(in_it);
            }
            else if (io_action.in_action == from_user_pipe) {
                user_pipe[client_ptr*MAX_USER+io_action.from_user].exist = false;
                sem_post(&user_pipe[client_ptr*MAX_USER+io_action.from_user].sem);

                int fifo_fd = open(user_pipe[client_ptr*MAX_USER+io_action.from_user].fifo_name, O_RDONLY);
                unlink(user_pipe[client_ptr*MAX_USER+io_action.from_user].fifo_name);
                dup2(fifo_fd, STDIN_FILENO);
                close(fifo_fd);
            }

            if (io_action.out_action == out_normal)  {
                dup2(client_fd, STDOUT_FILENO);
                dup2(client_fd, STDERR_FILENO);
            }
            else if (io_action.out_action == to_file)  {
                file_fd = open(cmd.fname, (O_WRONLY | O_CREAT | O_TRUNC), 0644);
                dup2(file_fd, STDOUT_FILENO);
                dup2(client_fd, STDERR_FILENO);
                close(file_fd);
            }
            else if (io_action.out_action == to_null)  {
                file_fd = open("/dev/null", (O_RDWR));
                dup2(file_fd, STDOUT_FILENO);
                dup2(client_fd, STDERR_FILENO);
                close(file_fd);
            }
            else if (io_action.out_action == to_pipe) {
                close(out_it->second.pipe_fd[0]);
                dup2(out_it->second.pipe_fd[1], STDOUT_FILENO);
                close(out_it->second.pipe_fd[1]);
                dup2(client_fd, STDERR_FILENO);
                waiting_pipes.erase(out_it);
            }
            else if (io_action.out_action == to_pipe_with_err) {
                close(out_it->second.pipe_fd[0]);
                dup2(out_it->second.pipe_fd[1], STDOUT_FILENO);
                dup2(out_it->second.pipe_fd[1], STDERR_FILENO);
                close(out_it->second.pipe_fd[1]);
                waiting_pipes.erase(out_it);
            }
            else if (io_action.out_action == to_user_pipe) {
                user_pipe[io_action.to_user*MAX_USER+client_ptr].exist = true;
                user_pipe[io_action.to_user*MAX_USER+client_ptr].waiting_pid = getpid();
                mkfifo(user_pipe[io_action.to_user*MAX_USER+client_ptr].fifo_name, 0666);

                sem_post(&user_pipe[io_action.to_user*MAX_USER+client_ptr].sem);
                
                dup2(client_fd, STDERR_FILENO);
                if (io_action.in_action == from_null)
                    cerr << err_in_buf;

                int fifo_fd = open(user_pipe[io_action.to_user*MAX_USER+client_ptr].fifo_name, O_WRONLY);
                dup2(fifo_fd, STDOUT_FILENO);
                close(fifo_fd);
            }
            close(client_fd);
            // cout << io_action.in_action << io_action.out_action << endl;
            
            if (io_action.in_action == from_null && io_action.out_action != to_user_pipe)
                cerr << err_in_buf;
            if (io_action.out_action == to_null)
                cerr << err_out_buf;

            map<int, Pipe_target>::iterator it;
            for (it = waiting_pipes.begin(); it != waiting_pipes.end(); it++) {
                close(it->second.pipe_fd[0]);
                close(it->second.pipe_fd[1]);
            }
            
            ret = execvp(cmd.argv[0], cmd.argv);
            if (ret < 0) {
                cerr << "Unknown command: [" << cmd.argv[0] << "]." << endl;
                exit(-1);
            }
            exit(0);
        }
        else {
            if (io_action.out_action <= OUT_TO_PIPE) {
                waitpid(pid, &status, 0);
            }
            if (io_action.in_action == from_pipe) {
                close(in_it->second.pipe_fd[0]);
                waiting_pipes.erase(in_it);
            }
        }
    }
    free(cmd.argv);
}

void init() {
    shmid_client = shmget(IPC_PRIVATE, sizeof(struct client_info) * MAX_USER, 0644 | IPC_CREAT);
    shmid_msg_buf = shmget(IPC_PRIVATE, sizeof(struct msg), 0644 | IPC_CREAT);
    shmid_user_pipe =  shmget(IPC_PRIVATE, sizeof(struct user_pipe_info) * MAX_USER * MAX_USER, 0644 | IPC_CREAT);

    client = (struct client_info *)shmat(shmid_client, NULL, 0);
    msg_buf = (struct msg *)shmat(shmid_msg_buf, NULL, 0);
    user_pipe =  (struct user_pipe_info *)shmat(shmid_user_pipe, NULL, 0);

    for (size_t i = 0; i < MAX_USER; i++) {
        sem_init(&client[i].sem, 1, 1);
        client[i].pid = -1;
        strcpy(client[i].name, "(no name)");
    }

    sem_init(&msg_buf->sem, 1, 1);
    sem_init(&msg_buf->send_done, 1, 0);
    for (int i = 0; i < MAX_USER; i++) {
        for (int j = 0; j < MAX_USER; j++) {
            sem_init(&user_pipe[i*MAX_USER+j].sem, 1, 1);
            user_pipe[i*MAX_USER+j].exist = false;
            sprintf(user_pipe[i*MAX_USER+j].fifo_name, "./user_pipe/%d_%d", i, j);
        }
    }
}

void welcome() {
    char local_buf[buf_size];
    sprintf(local_buf, "****************************************\n");
    strcat(local_buf, "** Welcome to the information server. **\n");
    strcat(local_buf, "****************************************\n");
    send(client_fd, local_buf, strlen(local_buf), MSG_NOSIGNAL);
    sprintf(local_buf, "*** User '(no name)' entered from %s:%d. ***\n", client[client_ptr].IP, client[client_ptr].port);
    broadcast(local_buf);
}

void leave() {
    char local_buf[buf_size];
    sprintf(local_buf, "*** User '%s' left. ***\n", client[client_ptr].name);
    broadcast(local_buf);

    sem_wait(&client[client_ptr].sem);
    client[client_ptr].pid = -1;
    sprintf(client[client_ptr].name, "(no name)");
    sem_post(&client[client_ptr].sem);
    close(client_fd);

    map<int, Pipe_target>::iterator it;
    for (it = waiting_pipes.begin(); it != waiting_pipes.end(); ++it)
         close(it->second.pipe_fd[0]);
    
    for (int i = 0; i < MAX_USER; i++) {
        sem_wait(&user_pipe[client_ptr*MAX_USER+i].sem);
        if (user_pipe[client_ptr*MAX_USER+i].exist)
            kill(user_pipe[client_ptr*MAX_USER+i].waiting_pid, SIGKILL);
        user_pipe[client_ptr*MAX_USER+i].exist = false;
        sem_post(&user_pipe[client_ptr*MAX_USER+i].sem);
        sem_wait(&user_pipe[i*MAX_USER+client_ptr].sem);
        if (user_pipe[i*MAX_USER+client_ptr].exist)
            kill(user_pipe[i*MAX_USER+client_ptr].waiting_pid, SIGKILL);
        user_pipe[i*MAX_USER+client_ptr].exist = false;
        sem_post(&user_pipe[i*MAX_USER+client_ptr].sem);
    }
    
    exit(0);
}

void who() {
    char local_buf[buf_size];
    sprintf(local_buf, "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
    send(client_fd, local_buf, strlen(local_buf), MSG_NOSIGNAL);
    for(int i = 0; i < MAX_USER; i++) {
        if (client[i].pid > 0) {
            if (i == client_ptr) {
                sprintf(local_buf, "%d\t%s\t%s:%d\t<-me\n", i+1, client[i].name, client[i].IP, client[i].port); 
                send(client_fd, local_buf, strlen(local_buf), MSG_NOSIGNAL);
            }
            else {
                sprintf(local_buf, "%d\t%s\t%s:%d\n", i+1, client[i].name, client[i].IP, client[i].port); 
                send(client_fd, local_buf, strlen(local_buf), MSG_NOSIGNAL);
            }
        }
    }
}

void name(char *cmd_str) {
    char local_buf[buf_size];
    char *name;

    strtok(cmd_str, " ");
    name = strtok(NULL, "\0");
    
    for(int i = 0; i < MAX_USER; i++) {
        if (client[i].pid > 0) {
            if (strcmp(client[i].name, name) == 0) {
                sprintf(local_buf, "*** User '%s' already exists. ***\n", name);
                send(client_fd, local_buf, strlen(local_buf), MSG_NOSIGNAL);
                return;
            }
        }
    }
    sem_wait(&client[client_ptr].sem);
    sprintf(client[client_ptr].name, "%s", name);
    sem_post(&client[client_ptr].sem);
    sprintf(local_buf, "*** User from %s:%d is named '%s'. ***\n", client[client_ptr].IP, client[client_ptr].port, name);
    broadcast(local_buf);
}

void yell(char *cmd_str) {
    char *msg;
    char local_buf[buf_size];
    strtok(cmd_str, " ");
    msg = strtok(NULL, "\0");
    sprintf(local_buf, "*** %s yelled ***: %s\n", client[client_ptr].name, msg);
    broadcast(local_buf);
}

void tell(char *cmd_str) {
    char local_buf[buf_size];
    int target_client;

    strtok(cmd_str, " ");
    target_client = atoi(strtok(NULL, " "));
    target_client--;
    if (client[target_client].pid == -1) {
        sprintf(local_buf, "*** Error: user #%d does not exist yet. ***\n", target_client+1);
        send(client_fd, local_buf, strlen(local_buf), MSG_NOSIGNAL);
        return;
    }
    sprintf(local_buf, "*** %s told you ***: %s\n", client[client_ptr].name, strtok(NULL, "\0"));
    sendToSomeone(client[target_client].pid, local_buf);
}

int main(int argc, char *argv[]) {
    signal(SIGCHLD, SIG_IGN);
    pid_t pid;

    struct sigaction signal_action;
    memset(&signal_action, 0, sizeof(struct sigaction));
    signal_action.sa_handler = sig_handler;
	signal_action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &signal_action, NULL);
    sigaction(SIGINT, &signal_action, NULL);

    int server_fd;
	struct sockaddr_in server_addr, client_addr;
	int opt = 1;
	int addrlen = sizeof(server_addr);

    char prompt[] = "% ";
	int prompt_len = strlen(prompt);

    char *rest_cmd;
    Cmd cmd;
    IORedirection io_action;

    char *rcv_buf, *tmp_rcv_buf;
    ssize_t rcv_len;
    rcv_buf = (char *)malloc(sizeof(char) * buf_size);
    memset(rcv_buf, 0, buf_size);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
            exit(EXIT_FAILURE);
    }

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        exit(EXIT_FAILURE);
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(atoi(argv[1]));

	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        exit(EXIT_FAILURE);
    }

	if (listen(server_fd, MAX_USER) < 0) {
        exit(EXIT_FAILURE);
    }

    init();

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&addrlen);
        if (client_fd < 0) {
            exit(EXIT_FAILURE);
        }

        while ((pid = fork()) < 0) {
            usleep(1000);
        }
        if (pid == 0) { // child
            close(server_fd);

            for (client_ptr = 0; client_ptr < MAX_USER; client_ptr++) {
                sem_wait(&client[client_ptr].sem);
                if (client[client_ptr].pid == -1) {
                    client[client_ptr].pid = getpid();
                    inet_ntop(AF_INET, &client_addr.sin_addr, client[client_ptr].IP, INET_ADDRSTRLEN);
                    client[client_ptr].port = ntohs(client_addr.sin_port);
                    sem_post(&client[client_ptr].sem);
                    break;
                }
                sem_post(&client[client_ptr].sem);
            }

            cmd.cmd_no = 0;
            clearenv();
            setenv("PATH", "bin:.", 1);

            welcome();

            send(client_fd, prompt, prompt_len, 0);

            while ((rcv_len = recv(client_fd, rcv_buf, buf_size, 0)) > 0) {
                rcv_buf = strtok(rcv_buf, "\n\r");
                if (rcv_buf == NULL) {
                    rcv_buf = (char *)malloc(sizeof(char) * buf_size);
                    memset(rcv_buf, 0, buf_size);
                    send(client_fd, prompt, prompt_len, 0);
                    continue;
                }
                cmd.cmd_no++;
                // cout << client_ptr+1 << ": " << rcv_buf << endl;
                if (strncmp(rcv_buf, "who", 3) == 0) {
                    who();
                }
                else if (strncmp(rcv_buf, "name ", 5) == 0) {
                    name(rcv_buf);
                }
                else if (strncmp(rcv_buf, "yell ", 5) == 0) {
                    yell(rcv_buf);
                }
                else if (strncmp(rcv_buf, "tell ", 5) == 0) {
                    tell(rcv_buf);
                }
                else if (strncmp(rcv_buf, "exit", 4) == 0) {
                    leave();
                }
                else {
                    cmd.cmd_no--;
                    tmp_rcv_buf = strdup(rcv_buf);
                    rest_cmd = rcv_buf;
                    while(rest_cmd != NULL && rest_cmd[0]) {
                        cmd.cmd_no++;
                        SetupCmd(client_ptr, rest_cmd, &cmd, &rest_cmd, &io_action);
                        if (cmd.argc == 0) 
                            break;
                        ExecCmd(client_ptr, cmd, io_action, tmp_rcv_buf);
                        if (!rest_cmd)
                            break;
                    }
                    free(tmp_rcv_buf);
                }
                memset(rcv_buf, 0, buf_size);
                send(client_fd, prompt, prompt_len, 0);
            }
            leave();
        }
        else {
            close(client_fd);            
        }
    }
    free(rcv_buf);
    return 0;
}
