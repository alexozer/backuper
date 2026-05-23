#include "base.hpp"

#include <sys/mman.h>

void *os_alloc(u64 size) {
    void *buf = mmap(nullptr, (size_t)size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (buf == nullptr) {
        fprintf(stderr, "mmap failed\n");
        exit(EXIT_FAILURE);
    }
    return buf;
}

void os_free(void *buf, u64 size) {
    munmap(buf, (size_t)size);
}

Arr<char *> cmd__build_args(Arena *arena, Cmd *cmd) {
    Arr<char *> args = arena_push_arr<char *>(arena, cmd->args.count + 2);
    char *name = str_to_c(arena, cmd->name);
    args[0] = name;
    for (u64 i = 0; i < cmd->args.count; i++) {
        args[i + 1] = str_to_c(arena, cmd->args[i]);
    }
    return args;
}

Arr<char *> cmd__build_env(Arena *arena, Cmd *cmd) {
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
