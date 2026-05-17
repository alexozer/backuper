#include "base.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <spawn.h>

void arena__ensure_init(Arena *arena) {
    if (arena->data == nullptr) {
        const u64 data_size = megabytes(16);
        arena->data = calloc(1, data_size);
        arena->reserved = data_size;
    }
}

// TODO deal with e.g. string nonalignment
void *arena__push_bytes(Arena *arena, u64 size, u64 alignment) {
    arena__ensure_init(arena);

    void *pos = (void *)((u64)arena->data + arena->offset);
    size = align_to(size, alignment);
    arena->offset += size;
    if (arena->offset > arena->reserved) {
         fprintf(stderr, "Arena over!\n");
         exit(EXIT_FAILURE);
    }
    return pos;
}

void arena_release(Arena *arena) {
    if (arena->data != nullptr) {
        free(arena->data);
        *arena = (Arena){};
    }
}

char *str_to_c(Arena *arena, Str s) {
    Arr<char> cstr = arena_push_arr<char>(arena, s.count + 1);
    // Compiler plz vectorize
    for (u64 i = 0; i < s.count; i++) {
        cstr[i] = s[i];
    }
    // Arena allocation is already zeroed, so null terminator is in place
    return cstr.value;
}

Str str_from_c(char *cstr) {
    u64 count = 0;
    while (cstr[count] != '\0') count++;
    return (Str){ .value = (u8 *)cstr, .count = count };
}

// Returns a string from a utf8 byte buffer. Doesn't validate if it's actually utf8.
Str str_from_bytes(Arr<u8> bytes) {
    // Skip utf8 BOM
    u64 start = 0;
    if (bytes.count >= 3 && bytes[0] == C('\xef') && bytes[1] == C('\xbb') && bytes[2] == C('\xbf')) {
        start = 3;
    }

    return arr_slice(bytes, start, bytes.count);
}

// Super conservative definition probably
bool char_is_whitespace(u8 c) {
    return c == C(' ') || c == C('\r') || c == C('\n');
}

Str str_trim(Str s) {
    u64 start = 0;
    while (start < s.count && char_is_whitespace(s[start])) {
        start++;
    }

    i64 end = ((i64)s.count) - 1;
    while (end >= 0 && char_is_whitespace(s[end])) {
        end--;
    }

    return arr_slice(s, start, end);
}

Str str_clone(Arena *arena, Str s) {
    Str clone = arena_push_arr<u8>(arena, s.count);
    for (u64 i = 0; i < s.count; i++) {
        clone[i] = s[i];
    }
    return clone;
}

bool str_starts_with(Str s, Str prefix) {
    if (prefix.count > s.count) {
        return false;
    }
    Str s_prefix = arr_slice(s, 0, prefix.count);
    return arr_eq(prefix, s_prefix);
}

StrLineIter str_lines(Str s) {
     return (StrLineIter){ .base = s, .pos = 0 };
}

bool str_lines_next(StrLineIter* iter, Str *line) {
    if (iter->pos >= iter->base.count) {
        return false;
    }

    u64 line_start = iter->pos;
    Arr<u8> data = iter->base;
    const u64 size = iter->base.count;

    // Advance until next line break
    u64 line_end = line_start;
    while (line_end < size && data[line_end] != C('\r') && data[line_end] != C('\n')) {
        line_end++;
    }

    // Advance past line breaks
    u64 next_line_start = line_end;
    while (next_line_start < size && (data[next_line_start] == C('\r'))) {
        next_line_start++;
    }
    if (next_line_start < size && (data[next_line_start] == C('\n'))) {
        next_line_start++;
    }

    iter->pos = next_line_start;

    if (line != nullptr) {
        line->value = iter->base.value + line_start;
        line->count = line_end - line_start;
    }

    return true;
}

u64 str_count_lines(Str s) {
    u64 line_count = 0;
    StrLineIter iter = str_lines(s);
    while (str_lines_next(&iter, nullptr)) {
        line_count++;
    }
    return line_count;
}

Pair<Str, Str> str_split2(Str base, u8 delim) {
    u64 delim_idx = 0;
    while (delim_idx < base.count && base[delim_idx] != delim) {
        delim_idx++;
    }
    Pair<Str, Str> result = {};
    if (delim_idx < base.count) {
        result.left = arr_slice(base, 0, delim_idx);
        result.right = arr_slice(base, delim_idx + 1, base.count);
    }
    return result;
}

//
// Paths
//

Str path_join(Arena *arena, Str left_path, Str right_path) {
    if (arr_is_empty(left_path)) {
        return right_path;
    }
    if (arr_is_empty(right_path)) {
        return left_path;
    }
    Vec<u8> joined = {};
    vec_extend(arena, &joined, left_path);
    vec_push(arena, &joined, C('/'));
    vec_extend(arena, &joined, right_path);
    return vec_arr(&joined);
}

//
// Subprocesses
//

Arr<char *> g_envp;

Str env_get(Str key) {
    Arena scratch = {};
    defer(arena_release(&scratch));

    char *key_cstr = str_to_c(&scratch, key);
    char *value_cstr = getenv(key_cstr);

    Str result = {};
    if (value_cstr != nullptr) {
        result = str_from_c(value_cstr);
    }

    return result;
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

// TODO error reporting
void cmd_run(Cmd *cmd) {
    Arena scratch = {};
    defer(arena_release(&scratch));

    Arr<char *> args = cmd__build_args(&scratch, cmd);
    Arr<char *> env = cmd__build_env(&scratch, cmd);

    posix_spawnattr_t spawnattr = {};
    posix_spawnattr_init(&spawnattr);
    defer(posix_spawnattr_destroy(&spawnattr));
    posix_spawnattr_setflags(&spawnattr, POSIX_SPAWN_CLOEXEC_DEFAULT); // Don't inherit fds by default

    const bool provide_stdin = !arr_is_empty(cmd->input);
    int stdin_pipe[2] = { -1, -1 };
    defer(close(stdin_pipe[0]));
    defer(close(stdin_pipe[1]));

    posix_spawn_file_actions_t actions = {};
    posix_spawn_file_actions_init(&actions);
    defer(posix_spawn_file_actions_destroy(&actions));

    if (provide_stdin) {
        pipe(stdin_pipe);
        posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
    }
    
    posix_spawn_file_actions_addinherit_np(&actions, STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);

    pid_t pid = -1;
    char *name = str_to_c(&scratch, cmd->name);
    int result = posix_spawnp(&pid, name, &actions, &spawnattr, args.value, env.value);
    if (result != 0) {
        fprintf(stderr, "Failed to invoke posix_spawnp: code %d\n", result);
        exit(EXIT_FAILURE);
    }

    if (provide_stdin) {
        write(stdin_pipe[1], cmd->input.value, cmd->input.count);
        close(stdin_pipe[1]);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) {
        fprintf(stderr, "Subprocess '%s' didn't exit normally\n", name);
        exit(EXIT_FAILURE);
    }
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Subprocess '%s' exited with code %d\n", name, WEXITSTATUS(status));
        exit(EXIT_FAILURE);
    }
}

