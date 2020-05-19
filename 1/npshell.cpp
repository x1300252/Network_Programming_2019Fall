#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <map>
#include <algorithm>
using namespace std;

#define OUT_TO_PIPE 2

typedef struct {
    int argc;
    char **argv;
} Cmd;

enum OutputRedirection {
    normal,
    to_file,
    to_pipe,
    to_pipe_with_err
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
unsigned int cmd_cnt = 0;

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

void SetupCmd(char *input_cmd, char delim, char **sub_cmd, char **rest_cmd, IORedirection* io_action) {
    *sub_cmd = strtok(input_cmd, "|!\0");
    *rest_cmd = strtok(NULL, "\0");
    
    io_action->in_from_pipe = false;
    if (waiting_pipes.find(cmd_cnt) != waiting_pipes.end()) {
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
    }
}

void ExecCmd(char *sub_cmd, IORedirection io_action) {
    Cmd cmd;

    char *fname;
    int file_fd;
    int delim_idx = strcspn(sub_cmd, ">");
    if (delim_idx != strlen(sub_cmd)) {
        io_action.out_action = to_file;
        sub_cmd = strtok(sub_cmd, ">");
        fname = strtok(NULL, " ");
    }

    splitArg(sub_cmd, &cmd);

    if (strcmp("exit", cmd.argv[0]) == 0) {
        free(cmd.argv);
        exit(0);
    }
    else if (strcmp("printenv", cmd.argv[0]) == 0) {
        char *env_str;
        env_str = getenv(cmd.argv[1]);
        if (env_str != NULL)
            cout << env_str << endl;
    }
    else if (strcmp("setenv", cmd.argv[0]) == 0) {
        setenv(cmd.argv[1], cmd.argv[2], 1);
    }
    else {
        int status, ret;
        Pipe_target out_target;
        map<int, Pipe_target>::iterator in_it, out_it;

        if (io_action.out_action & OUT_TO_PIPE) {
            out_it = waiting_pipes.find(cmd_cnt + io_action.next_n);
            if (out_it == waiting_pipes.end()) {
                while (pipe(out_target.pipe_fd) < 0) {
                    usleep(1000);
                }
                out_it = waiting_pipes.insert(pair<int, Pipe_target>(cmd_cnt + io_action.next_n, out_target)).first;
            }
        }

        if (io_action.in_from_pipe) {
            in_it = waiting_pipes.find(cmd_cnt);
            close(in_it->second.pipe_fd[1]);
        }

        pid_t pid;
        while ((pid = fork()) < 0) {
            usleep(1000);
        }
        if (pid == 0) { // child process
            if (io_action.out_action == to_file)  {
                file_fd = open(fname, (O_WRONLY | O_CREAT | O_TRUNC), 0644);
                dup2(file_fd, STDOUT_FILENO);
                close(file_fd);
            }
            else if (io_action.out_action == to_pipe) {
                close(out_it->second.pipe_fd[0]);
                dup2(out_it->second.pipe_fd[1], STDOUT_FILENO);
                close(out_it->second.pipe_fd[1]);
                waiting_pipes.erase(out_it);
            }
            else if (io_action.out_action == to_pipe_with_err) {
                close(out_it->second.pipe_fd[0]);
                dup2(out_it->second.pipe_fd[1], STDOUT_FILENO);
                dup2(out_it->second.pipe_fd[1], STDERR_FILENO);
                close(out_it->second.pipe_fd[1]);
                waiting_pipes.erase(out_it);
            }
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

int main() {
    setenv("PATH", "bin:.", 1);
    signal(SIGCHLD, SIG_IGN);

    char *input_cmd;
    size_t input_cmd_size = 15010;
    size_t input_cmd_len;
    input_cmd = (char *)malloc(sizeof(char) * input_cmd_size);
    
    int delim_idx;
    char *sub_cmd, *rest_cmd, *deal_cmd;
    IORedirection io_action;
    while(true) {
        cout << "% ";
        if((input_cmd_len = getline(&input_cmd, &input_cmd_size, stdin)) == -1) {
            break;
        }
        input_cmd_len--;
        input_cmd[input_cmd_len] = '\0';

        delim_idx = strcspn(input_cmd, "|!");
        deal_cmd = input_cmd;
        while(deal_cmd[0]) {
            cmd_cnt++;
            SetupCmd(deal_cmd, deal_cmd[delim_idx], &sub_cmd, &rest_cmd, &io_action);
            ExecCmd(sub_cmd, io_action);
            if (!rest_cmd)
                break;
            deal_cmd = rest_cmd;
            delim_idx = strcspn(deal_cmd, "|!");
        }
    }
    free(input_cmd);
    return 0;
}
