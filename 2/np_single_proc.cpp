#include <unistd.h>
#include <sys/socket.h>
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

using namespace std;

#define MAX_USER 30
#define OUT_TO_PIPE 2

extern char **environ;
size_t buf_size = 15010;

typedef struct {
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

struct client {
    struct sockaddr_in addr;
    int fd;
    char IP[INET_ADDRSTRLEN];
    int port;
    char name[30];

    char **environ;
    int cmd_number;
    map<int, Pipe_target> waiting_pipes;
    map<int, Pipe_target> user_pipes;
} client[30];

void broadcast(char *msg) {
    for(int i = 0; i < MAX_USER; i++) {
        if (client[i].fd > 0) {
            send(client[i].fd, msg, strlen(msg), MSG_NOSIGNAL);
        }
    }
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

    if (client[client_ptr].waiting_pipes.find(client[client_ptr].cmd_number) != client[client_ptr].waiting_pipes.end()) {
        io_action->in_action = from_pipe;
    }
}

void ExecCmd(int client_ptr, Cmd cmd, IORedirection io_action, char *rcv_buf) {
    int file_fd;
    char cmd_buf[buf_size], err_in_buf[buf_size], err_out_buf[buf_size];

    // cout << client[client_ptr].cmd_number << cmd.argv[0] << " " << io_action.in_action << " " << io_action.out_action << endl;
    if (strcmp("printenv", cmd.argv[0]) == 0) {
        char *env_str;
        env_str = getenv(cmd.argv[1]);
        if (env_str) {
            sprintf(cmd_buf, "%s\n", env_str);
            send(client[client_ptr].fd, cmd_buf, strlen(cmd_buf), 0);
        }
    }
    else if (strcmp("setenv", cmd.argv[0]) == 0) {
        // cout << cmd.argv[2];
        if (cmd.argc >= 3)
            setenv(cmd.argv[1], cmd.argv[2], 1);
    }
    else {
        int status, ret;
        Pipe_target out_target;
        map<int, Pipe_target>::iterator in_it, out_it;

        if (io_action.in_action == from_pipe) {
            in_it = client[client_ptr].waiting_pipes.find(client[client_ptr].cmd_number);
            close(in_it->second.pipe_fd[1]);
        }
        else if (io_action.in_action == from_user_pipe) {
            if (client[io_action.from_user].fd == -1) {
                sprintf(err_in_buf, "*** Error: user #%d does not exist yet. ***\n", io_action.from_user+1);
                io_action.in_action = from_null;
            }
            else {
                in_it = client[client_ptr].user_pipes.find(io_action.from_user);
                if (in_it == client[client_ptr].user_pipes.end()) {
                    sprintf(err_in_buf, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", io_action.from_user+1, client_ptr+1);
                    io_action.in_action = from_null;
                }
                else {
                    sprintf(cmd_buf, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n", client[client_ptr].name, client_ptr+1, client[io_action.from_user].name, io_action.from_user+1, rcv_buf);
                    broadcast(cmd_buf);
                    close(in_it->second.pipe_fd[1]);
                }
            }
        }

        if (io_action.out_action > OUT_TO_PIPE) {
            if (io_action.out_action == to_pipe || io_action.out_action == to_pipe_with_err) {
                out_it = client[client_ptr].waiting_pipes.find(client[client_ptr].cmd_number + io_action.next_n);
                // cout << client[client_ptr].cmd_number + io_action.next_n << endl;
                if (out_it == client[client_ptr].waiting_pipes.end()) {
                    while (pipe(out_target.pipe_fd) < 0) {
                        usleep(1000);
                    }
                    out_it = client[client_ptr].waiting_pipes.insert(pair<int, Pipe_target>(client[client_ptr].cmd_number + io_action.next_n, out_target)).first;
                }
            }
            else if (io_action.out_action == to_user_pipe) {                
                if (client[io_action.to_user].fd == -1) {
                    sprintf(err_out_buf, "*** Error: user #%d does not exist yet. ***\n", io_action.to_user+1);
                    io_action.out_action = to_null;
                }
                else {
                    out_it = client[io_action.to_user].user_pipes.find(client_ptr);
                    if (out_it != client[io_action.to_user].user_pipes.end()) {
                        sprintf(err_out_buf, "*** Error: the pipe #%d->#%d already exists. ***\n", client_ptr+1, io_action.to_user+1);
                        io_action.out_action = to_null;
                    }
                    else {
                        sprintf(cmd_buf, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n", client[client_ptr].name, client_ptr+1, rcv_buf, client[io_action.to_user].name, io_action.to_user+1);
                        broadcast(cmd_buf);

                        while (pipe(out_target.pipe_fd) < 0) {
                            usleep(1000);
                        }
                        out_it = client[io_action.to_user].user_pipes.insert(pair<int, Pipe_target>(client_ptr, out_target)).first;
                    }
                }
            }
        }
        
        pid_t pid;
        while ((pid = fork()) < 0) {
            usleep(1000);
        }
        if (pid == 0) { // child process
            // cout << "aa\n";
            if (io_action.in_action == from_null) {
                file_fd = open("/dev/null", (O_RDWR));
                dup2(file_fd, STDIN_FILENO);
                close(file_fd);
            }
            else if (io_action.in_action == from_pipe) {
                dup2(in_it->second.pipe_fd[0], STDIN_FILENO);
                close(in_it->second.pipe_fd[0]);
                client[client_ptr].waiting_pipes.erase(in_it);
            }
            else if (io_action.in_action == from_user_pipe) {
                dup2(in_it->second.pipe_fd[0], STDIN_FILENO);
                close(in_it->second.pipe_fd[0]);
                client[client_ptr].user_pipes.erase(in_it);
            }

            if (io_action.out_action == out_normal)  {
                dup2(client[client_ptr].fd, STDOUT_FILENO);
                dup2(client[client_ptr].fd, STDERR_FILENO);
            }
            else if (io_action.out_action == to_file)  {
                file_fd = open(cmd.fname, (O_WRONLY | O_CREAT | O_TRUNC), 0644);
                dup2(file_fd, STDOUT_FILENO);
                dup2(client[client_ptr].fd, STDERR_FILENO);
                close(file_fd);
            }
            else if (io_action.out_action == to_null)  {
                file_fd = open("/dev/null", (O_RDWR));
                dup2(file_fd, STDOUT_FILENO);
                dup2(client[client_ptr].fd, STDERR_FILENO);
                close(file_fd);
            }
            else if (io_action.out_action == to_pipe) {
                close(out_it->second.pipe_fd[0]);
                dup2(out_it->second.pipe_fd[1], STDOUT_FILENO);
                close(out_it->second.pipe_fd[1]);
                dup2(client[client_ptr].fd, STDERR_FILENO);
                client[client_ptr].waiting_pipes.erase(out_it);
            }
            else if (io_action.out_action == to_pipe_with_err) {
                close(out_it->second.pipe_fd[0]);
                dup2(out_it->second.pipe_fd[1], STDOUT_FILENO);
                dup2(out_it->second.pipe_fd[1], STDERR_FILENO);
                close(out_it->second.pipe_fd[1]);
                client[client_ptr].waiting_pipes.erase(out_it);
            }
            else if (io_action.out_action == to_user_pipe) {
                close(out_it->second.pipe_fd[0]);
                dup2(out_it->second.pipe_fd[1], STDOUT_FILENO);
                close(out_it->second.pipe_fd[1]);
                dup2(client[client_ptr].fd, STDERR_FILENO);
                client[io_action.to_user].user_pipes.erase(out_it);
            }
            close(client[client_ptr].fd);
            
            if (io_action.in_action == from_null)
                cerr << err_in_buf;
            if (io_action.out_action == to_null)
                cerr << err_out_buf;

            map<int, Pipe_target>::iterator it;
            for (int i = 0; i < MAX_USER; i++) {
                if (client[i].fd != -1) {
                    for (it = client[i].waiting_pipes.begin(); it != client[i].waiting_pipes.end(); it++) {
                        close(it->second.pipe_fd[0]);
                        close(it->second.pipe_fd[1]);
                    }
                    for (it = client[i].user_pipes.begin(); it != client[i].user_pipes.end(); it++) {
                        close(it->second.pipe_fd[0]);
                        close(it->second.pipe_fd[1]);
                    }
                }
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
                client[client_ptr].waiting_pipes.erase(in_it);
            }
            else if (io_action.in_action == from_user_pipe) {
                close(in_it->second.pipe_fd[0]);
                client[client_ptr].user_pipes.erase(in_it);
            }
        }
    }
    free(cmd.argv);
}

void welcome(int client_ptr) {
    char msg_buf[buf_size];
    sprintf(msg_buf, "****************************************\n");
    strcat(msg_buf, "** Welcome to the information server. **\n");
    strcat(msg_buf, "****************************************\n");
    send(client[client_ptr].fd, msg_buf, strlen(msg_buf), MSG_NOSIGNAL);
    sprintf(msg_buf, "*** User '(no name)' entered from %s:%d. ***\n", client[client_ptr].IP, client[client_ptr].port); 
    broadcast(msg_buf);
}

void leave(int client_ptr, fd_set *all_set) {
    char msg_buf[buf_size];

    sprintf(msg_buf, "*** User '%s' left. ***\n", client[client_ptr].name); 
    broadcast(msg_buf);
    
    close(client[client_ptr].fd);
    FD_CLR(client[client_ptr].fd, all_set);
    client[client_ptr].fd = -1;
    client[client_ptr].cmd_number = 0;
    sprintf(client[client_ptr].name, "(no name)");

    client[client_ptr].environ = (char **)malloc(sizeof(char *) * 2);
    client[client_ptr].environ[0] = strdup(environ[0]);
    client[client_ptr].environ[1] = NULL;

    map<int, Pipe_target>::iterator it;
    for (it = client[client_ptr].waiting_pipes.begin(); it != client[client_ptr].waiting_pipes.end(); ++it)
         close(it->second.pipe_fd[0]);
    for (it = client[client_ptr].user_pipes.begin(); it != client[client_ptr].user_pipes.end(); ++it)
         close(it->second.pipe_fd[0]);
    for (int i = 0; i < MAX_USER; i++) {
        if (client[i].fd != -1) {
            it = client[i].user_pipes.find(client_ptr);
            if (it != client[i].user_pipes.end()) {
                close(it->second.pipe_fd[0]);
                client[i].user_pipes.erase(it);
            }
        }
    }
    client[client_ptr].waiting_pipes.clear();
    client[client_ptr].user_pipes.clear();
}

void who(int client_ptr) {
    char msg_buf[buf_size];
    sprintf(msg_buf, "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
    send(client[client_ptr].fd, msg_buf, strlen(msg_buf), MSG_NOSIGNAL);
    for(int i = 0; i < MAX_USER; i++) {
        if (client[i].fd > 0) {
            if (i == client_ptr) {
                sprintf(msg_buf, "%d\t%s\t%s:%d\t<-me\n", i+1, client[i].name, client[i].IP, client[i].port); 
                send(client[client_ptr].fd, msg_buf, strlen(msg_buf), MSG_NOSIGNAL);
            }
            else {
                sprintf(msg_buf, "%d\t%s\t%s:%d\n", i+1, client[i].name, client[i].IP, client[i].port); 
                send(client[client_ptr].fd, msg_buf, strlen(msg_buf), MSG_NOSIGNAL);
            }
        }
    }
}

void name(int client_ptr, char *cmd_str) {
    char msg_buf[buf_size];
    char *name;

    strtok(cmd_str, " ");
    name = strtok(NULL, "\0");
    
    for(int i = 0; i < MAX_USER; i++) {
        if (client[i].fd > 0) {
            if (strcmp(client[i].name, name) == 0) {
                sprintf(msg_buf, "*** User '%s' already exists. ***\n", name);
                send(client[client_ptr].fd, msg_buf, strlen(msg_buf), MSG_NOSIGNAL);
                return;
            }
        }
    }

    sprintf(client[client_ptr].name, "%s", name);
    sprintf(msg_buf, "*** User from %s:%d is named '%s'. ***\n", client[client_ptr].IP, client[client_ptr].port, name);
    broadcast(msg_buf);
}

void yell(int client_ptr, char *cmd_str) {
    char msg_buf[buf_size];
    char *msg;

    strtok(cmd_str, " ");
    msg = strtok(NULL, "\0");
    sprintf(msg_buf, "*** %s yelled ***: %s\n", client[client_ptr].name, msg);
    broadcast(msg_buf);
}

void tell(int client_ptr, char *cmd_str) {
    char msg_buf[buf_size];
    char *msg;
    int target_client;

    strtok(cmd_str, " ");
    target_client = atoi(strtok(NULL, " "));
    target_client--;
    msg = strtok(NULL, "\0");
    if (client[target_client].fd == -1) {
        sprintf(msg_buf, "*** Error: user #%d does not exist yet. ***\n", target_client+1);
        send(client[client_ptr].fd, msg_buf, strlen(msg_buf), MSG_NOSIGNAL);
        return;
    }
    
    sprintf(msg_buf, "*** %s told you ***: %s\n", client[client_ptr].name, msg);
    send(client[target_client].fd, msg_buf, strlen(msg_buf), MSG_NOSIGNAL);
}

int main(int argc, char *argv[]) {
    signal(SIGCHLD, SIG_IGN);

    fd_set read_set, all_set;
    int server_fd, client_fd, max_fd;
	struct sockaddr_in server_addr;
	int opt = 1;
    int ready_num, client_ptr, client_addr_len;
    char **server_environ = environ;

    char prompt[] = "% ";
	int prompt_len = strlen(prompt);

    char *rest_cmd;
    Cmd cmd;
    IORedirection io_action;

    char *rcv_buf, *tmp_rcv_buf;
    ssize_t rcv_len;
    rcv_buf = (char *)malloc(sizeof(char) * buf_size);
    memset(rcv_buf, 0, buf_size);

    char msg_buf[buf_size];

    clearenv();
    setenv("PATH", "bin:.", 1);

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

    for (int i = 0; i < MAX_USER; i++) {
        client[i].fd = -1;
        strcpy(client[i].name, "(no name)");
        client[i].cmd_number = 0;
        client[i].environ = (char **)malloc(sizeof(char *) * 2);
        client[i].environ[0] = strdup(environ[0]);
        client[i].environ[1] = NULL;
    }

    max_fd = server_fd;
    FD_ZERO(&all_set);
    FD_SET(server_fd, &all_set);

    while (1) {
        read_set = all_set;

        if ((ready_num = select(max_fd + 1, &read_set, NULL, NULL, NULL)) == -1) {
            exit(EXIT_FAILURE);
        }

        if (FD_ISSET(server_fd, &read_set)) { /* new client connection */
            for (client_ptr = 0; (client_ptr < MAX_USER) && (client[client_ptr].fd > 0); client_ptr++);
            
            client_addr_len = sizeof(client[client_ptr].addr);
            client[client_ptr].fd = accept(server_fd, (struct sockaddr *) &client[client_ptr].addr, (socklen_t *)&client_addr_len);
            inet_ntop(AF_INET, &client[client_ptr].addr.sin_addr, client[client_ptr].IP, INET_ADDRSTRLEN);
            client[client_ptr].port = ntohs(client[client_ptr].addr.sin_port);
       
            welcome(client_ptr);
            send(client[client_ptr].fd, prompt, prompt_len, 0);

            FD_SET(client[client_ptr].fd, &all_set);

            if (client[client_ptr].fd > max_fd)
                max_fd = client[client_ptr].fd;
            if ((--ready_num) <= 0)
                continue;
        }

        for (client_ptr = 0; client_ptr < MAX_USER; client_ptr++) { /* check all clients for data */
            if ((client_fd = client[client_ptr].fd) < 0)
                continue;
            
            if (FD_ISSET(client_fd, &read_set)) {
                memset(rcv_buf, 0, buf_size);
                memset(msg_buf, 0, buf_size);

                if ((rcv_len = recv(client_fd, rcv_buf, buf_size, 0)) == 0) { /* connection closed by client */
                    leave(client_ptr, &all_set);
                }
                else {
                    rcv_buf = strtok(rcv_buf, "\r\n");
                    // cout << client_ptr+1 << ": " << rcv_buf << endl;
                    if (rcv_buf == NULL) {
                        rcv_buf = (char *)malloc(sizeof(char) * buf_size);
                        memset(rcv_buf, 0, buf_size);
                        send(client[client_ptr].fd, prompt, prompt_len, 0);
                        continue;
                    }
                    environ = client[client_ptr].environ;
                    client[client_ptr].cmd_number++;

                    if (strncmp(rcv_buf, "who", 3) == 0) {
                        who(client_ptr);
                    }
                    else if (strncmp(rcv_buf, "name ", 5) == 0) {
                        name(client_ptr, rcv_buf);
                    }
                    else if (strncmp(rcv_buf, "yell ", 5) == 0) {
                        yell(client_ptr, rcv_buf);
                    }
                    else if (strncmp(rcv_buf, "tell ", 5) == 0) {
                        tell(client_ptr, rcv_buf);
                    }
                    else if (strncmp(rcv_buf, "exit", 4) == 0) {
                        leave(client_ptr, &all_set);
                    }
                    else {
                        client[client_ptr].cmd_number--;
                        tmp_rcv_buf = strdup(rcv_buf);
                        rest_cmd = rcv_buf;
                        while(rest_cmd[0]) {
                            client[client_ptr].cmd_number++;
                            SetupCmd(client_ptr, rest_cmd, &cmd, &rest_cmd, &io_action);
                            if (cmd.argc == 0) 
                                break;
                            ExecCmd(client_ptr, cmd, io_action, tmp_rcv_buf);
                            if (!rest_cmd)
                                break;
                        }
                        free(tmp_rcv_buf);
                    }
                }

                send(client[client_ptr].fd, prompt, prompt_len, 0);
                client[client_ptr].environ = environ;
                environ = server_environ;
                if ((--ready_num) <= 0)
                    break;
            }
        }
    }
    free(rcv_buf);
    return 0;
}
