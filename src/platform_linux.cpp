#include "base.hpp"

#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "platform.hpp"

OSResult cmd_run(Cmd *cmd) {
    Arena *scratch = arena_acquire();
    defer(arena_release(scratch));

    int stdin_pipe[2] = { -1, -1 };
    defer(close(stdin_pipe[0]));
    defer(close(stdin_pipe[1]));

    if (!arr_is_empty(cmd->input)) {
        switch (pipe2(stdin_pipe, O_CLOEXEC)) {
            case 0: break;
            case EFAULT: return OSResult::OtherError;
            case EMFILE: return OSResult::InvalidFileDescriptor;
            case ENFILE: return OSResult::InvalidFileDescriptor;
            default: return OSResult::OtherError;
        }
    }

    char *name = str_to_c(scratch, cmd->name);
    Arr<char *> args = posix_build_args(scratch, cmd);
    Arr<char *> env = posix_build_env(scratch, cmd);
    char *cwd = str_to_c(scratch, cmd->cwd);

    pid_t pid = fork();
    if (pid == -1) {
        switch (errno) {
            case ENOMEM: return OSResult::AllocationFailed;
            default: return OSResult::OtherError;
        }
    }
    if (pid == 0) {
        // Am child

        if (!arr_is_empty(cmd->input)) {
            if (dup2(stdin_pipe[0], STDIN_FILENO) == -1) {
                _exit(1);
            }
            if (close(stdin_pipe[0]) == -1) {
                _exit(1);
            }
        }

        if (!arr_is_empty(cmd->cwd)) {
            if (chdir(cwd) == -1) {
                _exit(1);
            }
        }

        if (execve(name, args.value, env.value) == -1) {
            _exit(1);
        }
    }

    // Am parent
    if (!arr_is_empty(cmd->input)) {
        if (write(stdin_pipe[1], cmd->input.value, cmd->input.count) != cmd->input.count) {
            return OSResult::OtherError;
        }
        if (close(stdin_pipe[1]) != 0) {
            return OSResult::OtherError;
        }
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        return OSResult::OtherError;
    }
    if (!WIFEXITED(status)) {
        return OSResult::SubprocessExitError;
    }
    if (WEXITSTATUS(status) != 0) {
        return OSResult::SubprocessNonZeroExitCode;
    }

    return OSResult::OtherError;
}
