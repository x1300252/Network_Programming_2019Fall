#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
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

typedef struct {
    unsigned int cmd_no;
    int argc;
    char **argv;
    char *fname;
} Cmd;

enum OutputRedirection {
    normal,
    to_file,
    to_pipe,
    to_pipe_with_err,
    to_user_pipe
};

typedef struct {
    bool in_from_pipe;
    OutputRedirection out_action;
    int next_n;
} IORedirection;

typedef struct {
    int pipe_fd[2];
} Pipe_target;

map<int, Pipe_target> waiting_pipes;

void splitArg(char *input_cmd, Cmd *cmd) {
    cmd->argc = 0;
    int input_len = strlen(input_cmd);
    int prev_pos = 0;
    int cur_pos = 0;
    for(; cur_pos < input_len; cur_pos++) {
        if (input_cmd[cur_pos] == ' ') {
            if (cur_pos != prev_pos)
                cmd->argc++;
            prev_pos = cur_pos+1;
        }
    }
    if (prev_pos == cur_pos)
        cmd->argc++;
    else
        cmd->argc += 2;

    cmd->argv = (char **)malloc(sizeof(char *) * cmd->argc);

    cur_pos = 0;
    char *argv_ptr = strtok(input_cmd, " ");
    while (argv_ptr != NULL) {
        cmd->argv[cur_pos] = argv_ptr;
        cur_pos++;
        argv_ptr = strtok(NULL, " ");
    }
    cmd->argv[cur_pos] = NULL;
}

void SetupCmd(char *input_cmd, char delim, Cmd *cmd, char **rest_cmd, IORedirection* io_action) {
    char *sub_cmd = strtok(input_cmd, "|!>\0");
    *rest_cmd = strtok(NULL, "\0");
    
    io_action->in_from_pipe = false;
    if (waiting_pipes.find(cmd->cmd_no) != waiting_pipes.end()) {
        io_action->in_from_pipe = true;
    }

    char *next_n_str;
    io_action->out_action = normal;
    if(delim == '|') {
        io_action->out_action = to_pipe;
        if(*rest_cmd[0] != ' ') {
            next_n_str = strtok(*rest_cmd, " ");
            io_action->next_n = atoi(next_n_str);
            *rest_cmd = strtok(NULL, "\0");
        }
        else {
            io_action->next_n = 1;
        }
    }
    else if(delim == '!') {
        io_action->out_action = to_pipe_with_err;
        if(*rest_cmd[0] != ' ') {
            next_n_str = strtok(*rest_cmd, " ");
            io_action->next_n = atoi(next_n_str);
            *rest_cmd = strtok(NULL, "\0");
        }
        else {
            io_action->next_n = 1;
        }
    } else if(delim == '>') {
        if(*rest_cmd[0] != ' ') {
            io_action->out_action = to_user_pipe;
            next_n_str = strtok(*rest_cmd, " ");
            io_action->next_n = atoi(next_n_str);
            *rest_cmd = strtok(NULL, "\0");
        }
        else {
            io_action->out_action = to_file;
            cmd->fname = strtok(*rest_cmd, " \0");
            io_action->next_n = 0;
            *rest_cmd = NULL;
        }
    }

    splitArg(sub_cmd, cmd);
}


void ExecCmd(int client_fd, Cmd cmd, IORedirection io_action) {
    int file_fd;

    if (strcmp("printenv", cmd.argv[0]) == 0) {
        char *env_str, *cmd_buf;
        env_str = getenv(cmd.argv[1]);
        cmd_buf = (char *)malloc(strlen(env_str)+2);
        strcpy(cmd_buf, env_str);
        strcat(cmd_buf, "\n");

        send(client_fd, cmd_buf, strlen(cmd_buf), 0);

    }
    else if (strcmp("setenv", cmd.argv[0]) == 0) {
        setenv(cmd.argv[1], cmd.argv[2], 1);
    }
    else {
        int status, ret;
        Pipe_target out_target;
        map<int, Pipe_target>::iterator in_it, out_it;

        if (io_action.out_action & OUT_TO_PIPE) {
            out_it = waiting_pipes.find(cmd.cmd_no + io_action.next_n);
            if (out_it == waiting_pipes.end()) {
                while (pipe(out_target.pipe_fd) < 0) {
                    usleep(1000);
                }
                out_it = waiting_pipes.insert(pair<int, Pipe_target>(cmd.cmd_no + io_action.next_n, out_target)).first;
            }
        }

        if (io_action.in_from_pipe) {
            in_it = waiting_pipes.find(cmd.cmd_no);
            close(in_it->second.pipe_fd[1]);
        }

        pid_t pid;
        while ((pid = fork()) < 0) {
            usleep(1000);
        }
        if (pid == 0) { // child process
            if (io_action.out_action == normal)  {
                dup2(client_fd, STDOUT_FILENO);
                dup2(client_fd, STDERR_FILENO);
            }
            else if (io_action.out_action == to_file)  {
                file_fd = open(cmd.fname, (O_WRONLY | O_CREAT | O_TRUNC), 0644);
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
            close(client_fd);

            if (io_action.in_from_pipe) {
                dup2(in_it->second.pipe_fd[0], STDIN_FILENO);
                close(in_it->second.pipe_fd[0]);
                waiting_pipes.erase(in_it);
            }

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
            if (!(io_action.out_action & OUT_TO_PIPE)) {
                waitpid(pid, &status, 0);
            }
            if (io_action.in_from_pipe) {
                close(in_it->second.pipe_fd[0]);
                waiting_pipes.erase(in_it);
            }
        }
    }
    free(cmd.argv);
}

int main(int argc, char *argv[]) {
    signal(SIGCHLD, SIG_IGN);

    int server_fd, client_fd;
	struct sockaddr_in server_addr;
	int opt = 1;
	int addrlen = sizeof(server_addr);

    char prompt[] = "% ";
	int prompt_len = strlen(prompt);

    int delim_idx;
    char *rest_cmd, *deal_cmd;
    Cmd cmd;
    IORedirection io_action;

    char *rcv_buf;
    size_t rcv_buf_size = 15010;
    ssize_t rcv_len;
    rcv_buf = (char *)malloc(sizeof(char) * rcv_buf_size);
    memset(rcv_buf, 0, rcv_buf_size);

	// Creating socket file descriptor
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

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&server_addr, (socklen_t *)&addrlen);
        if (client_fd < 0) {
            exit(EXIT_FAILURE);
        }
        
        pid_t pid;
        while ((pid = fork()) < 0) {
            usleep(1000);
        }
        if (pid == 0) {
            close(server_fd);
            cmd.cmd_no = 0;
            clearenv();
            setenv("PATH", "bin:.", 1);
            send(client_fd, prompt, prompt_len, 0);

            while ((rcv_len = recv(client_fd, rcv_buf, rcv_buf_size, 0)) > 0) {
                // cout << client_ptr+1 << ": " << rcv_buf << endl;
                
                rcv_buf = strtok(rcv_buf, "\r\n");

                delim_idx = strcspn(rcv_buf, "|!>");
                deal_cmd = rcv_buf;
                while(deal_cmd[0]) {
                    cmd.cmd_no++;
                    SetupCmd(deal_cmd, deal_cmd[delim_idx], &cmd, &rest_cmd, &io_action);

                    if (strcmp("exit", cmd.argv[0]) == 0) {
                        free(cmd.argv);
                        close(client_fd);
                        exit(0);
                    }

                    ExecCmd(client_fd, cmd, io_action);
                    if (!rest_cmd)
                        break;
                    deal_cmd = rest_cmd;
                    delim_idx = strcspn(deal_cmd, "|!>");
                }
                memset(rcv_buf, 0, rcv_buf_size);
                send(client_fd, prompt, prompt_len, 0);
            }
            free(cmd.argv);
            close(client_fd);
            exit(0);
        }
        else {
            close(client_fd);
        }
        // if (close_flag) {
        //     close(client_fd);
        //     break;
        // }
    }
    free(rcv_buf);
    return 0;
}
