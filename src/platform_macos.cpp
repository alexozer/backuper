#include "base.hpp"

#include <spawn.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform.hpp"

OSResult cmd__check_file_action(int code) {
    switch (code) {
        case 0: return OSResult::Ok;
        case EBADF: return OSResult::InvalidFileDescriptor;
        case ENAMETOOLONG: return OSResult::InvalidPath;
        case ENOMEM: return OSResult::AllocationFailed;
        default: return OSResult::OtherError;
    }
}

// TODO assertions
OSResult cmd_run(Cmd *cmd) {
    Arena *scratch = arena_acquire();
    defer(arena_release(scratch));

    posix_spawnattr_t spawnattr = {};
    switch (posix_spawnattr_init(&spawnattr)) {
        case 0: break;
        case ENOMEM: return OSResult::AllocationFailed;
        default: return OSResult::OtherError;
    }
    defer(posix_spawnattr_destroy(&spawnattr));
    posix_spawnattr_setflags(&spawnattr, POSIX_SPAWN_CLOEXEC_DEFAULT); // Don't inherit fds by default

    int stdin_pipe[2] = { -1, -1 };
    defer(close(stdin_pipe[0]));
    defer(close(stdin_pipe[1]));

    posix_spawn_file_actions_t actions = {};
    switch (posix_spawn_file_actions_init(&actions)) {
        case 0: break;
        case ENOMEM: return OSResult::AllocationFailed;
        default: return OSResult::OtherError;
    }
    defer(posix_spawn_file_actions_destroy(&actions));

    if (!arr_is_empty(cmd->input)) {
        switch (pipe(stdin_pipe)) {
            case 0: break;
            case EFAULT: return OSResult::OtherError;
            case EMFILE: return OSResult::InvalidFileDescriptor;
            case ENFILE: return OSResult::InvalidFileDescriptor;
            default: return OSResult::OtherError;
        }

        OSResult result = cmd__check_file_action(
                posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO));
        if (result != OSResult::Ok) return result;

        result = cmd__check_file_action(
                posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]));
        if (result != OSResult::Ok) return result;
    }

    if (!arr_is_empty(cmd->cwd)) {
        char *cwd_cstr = str_to_c(scratch, cmd->cwd);
        OSResult result = cmd__check_file_action(
                posix_spawn_file_actions_addchdir_np(&actions, cwd_cstr));
        if (result != OSResult::Ok) return result;
    }

    OSResult result = cmd__check_file_action(
            posix_spawn_file_actions_addinherit_np(&actions, STDOUT_FILENO));
    if (result != OSResult::Ok) return result;

    result = cmd__check_file_action(
            posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO));
    if (result != OSResult::Ok) return result;

    pid_t pid = -1;
    char *name = str_to_c(scratch, cmd->name);
    Arr<char *> args = cmd__build_args(scratch, cmd);
    Arr<char *> env = cmd__build_env(scratch, cmd);

    switch (posix_spawnp(&pid, name, &actions, &spawnattr, args.value, env.value)) {
        case 0: break;
        case EACCES: return OSResult::PermissionDenied;
        case ENAMETOOLONG: return OSResult::InvalidPath;
        case ENOTDIR: return OSResult::InvalidPath;
        case ENOMEM: return OSResult::AllocationFailed;
        case EBADF: return OSResult::InvalidFileDescriptor;
        default: return OSResult::OtherError;
    }

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

    return OSResult::Ok;
}
