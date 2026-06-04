#include "base.hpp"

#include <sys/mman.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include "platform.hpp"

void *os_alloc(u64 size) {
    void *buf = mmap(nullptr, (size_t)size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (buf == nullptr) {
        log_fatal("mmap failed");
    }
    return buf;
}

void os_free(void *buf, u64 size) {
    munmap(buf, (size_t)size);
}

Arr<char *> posix_build_args(Arena *arena, Cmd *cmd) {
    Arr<char *> args = arena_push_arr<char *>(arena, cmd->args.count + 2);
    char *name = str_to_c(arena, cmd->name);
    args[0] = name;
    for (u64 i = 0; i < cmd->args.count; i++) {
        args[i + 1] = str_to_c(arena, cmd->args[i]);
    }
    return args;
}

Arr<char *> posix_build_env(Arena *arena, Cmd *cmd) {
    Vec<char *> env = {};

    for (u64 i = 0; i < cmd->env.count; i++) {
        Str var = cmd->env[i].left;
        Str val = cmd->env[i].right;

        // Build "{var}={val}"
        Vec<u8> line = {};
        vec_extend(arena, &line, var);
        vec_push(arena, &line, C('='));
        vec_extend(arena, &line, val);
        vec_push(arena, &line, C('\0'));

        vec_push(arena, &env, (char *)line.value);
    }
    vec_extend(arena, &env, g_envp);
    vec_push(arena, &env, (char *)nullptr);

    return vec_arr(&env);
}

Instant os_get_monotonic_time() {
    struct timespec tp = {};
    if (clock_gettime(CLOCK_MONOTONIC, &tp) != 0) {
        log_fatal("clock_gettime() failed");
    }
    return {
        .seconds = (i64)tp.tv_sec,
        .nanoseconds = (u32)tp.tv_nsec,
    };
}

OSResult os_read_file(Arena *arena, Str path, Arr<u8> *out_buf) {
    // TODO: mmap without memcpy - associate a "destructor" with the arena?

    Arena *scratch = arena_acquire();
    defer(arena_release(scratch));

    char *path_cstr = str_to_c(scratch, path);

    int fd = open(path_cstr, O_RDONLY);
    if (fd == -1) {
        return OSResult::OtherError;
    }
    defer(close(fd));

    struct stat st = {};
    if (fstat(fd, &st) == -1) {
        return OSResult::OtherError;
    }
    u64 size = (u64)st.st_size;

    void *buf = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == nullptr) {
        return OSResult::OtherError;
    }
    defer(munmap(buf, size));

    *out_buf = arena_push_arr<u8>(arena, size);
    memcpy(out_buf->value, buf, size);

    return OSResult::Ok;
}
