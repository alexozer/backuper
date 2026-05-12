// TODO clean up includes
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <spawn.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;

//
// Math
//

#define kilobytes(n) (n * 1024LL)
#define megabytes(n) (kilobytes(n) * 1024LL)

#define align_to(n, a) (((n) + (a - 1)) & ~(a - 1))
#define DEFAULT_ALIGNMENT 8

#define min(a, b) (((a) < (b)) ? a : b)
#define max(a, b) (((a) > (b)) ? a : b)

// https://jameshfisher.com/2018/03/30/round-up-power-2/
u64 NextPow2(u64 x) {
    x--;
    x |= x>>1;
    x |= x>>2;
    x |= x>>4;
    x |= x>>8;
    x |= x>>16;
    x |= x>>32;
    x++;
    return x;
}

//
// Arenas
//

template <typename T>
struct Arr {
    T *v;
    u64 n;
};

struct Arena {
    void *data;
    u64 reserved;
    u64 offset;
};

void _arena_ensure_init(Arena *arena) {
    if (arena->data == nullptr) {
        const u64 data_size = megabytes(16);
        arena->data = calloc(1, data_size);
        arena->reserved = data_size;
    }
}

// TODO deal with e.g. string nonalignment
void *arena_push_bytes(Arena *arena, u64 size, u64 alignment = DEFAULT_ALIGNMENT) {
    _arena_ensure_init(arena);

    void *pos = (void *)((u64)arena->data + arena->offset);
    size = align_to(size, alignment);
    arena->offset += size;
    if (arena->offset > arena->reserved) {
         fprintf(stderr, "Arena over!\n");
         exit(EXIT_FAILURE);
    }
    return pos;
}

template <typename T>
T *arena_push(Arena *arena) {
    return arena_push_bytes(arena, sizeof(T));
}

template <typename T>
Arr<T> arena_push_arr(Arena *arena, u64 count) {
    return {
        .v = (T *)arena_push_bytes(arena, sizeof(T) * count),
        .n = count,
    };
}

void arena_release(Arena *arena) {
    if (arena->data != nullptr) {
        free(arena->data);
        *arena = (Arena){};
    }
}

//
// Vec
//

template <typename T>
struct Vec {
    T *v;
    u64 n;
    u64 cap;
};

#define MIN_VEC_CAPACITY 8

template <typename T>
void *_vec_grow(Arena *arena, Vec<T> *vec, void *array, u64 count) {
    const u64 old_size = vec->count * sizeof(T);
    const u64 new_size = (vec->count + max(count, MIN_VEC_CAPACITY)) * sizeof(T);

    if (new_size > vec->capacity) {
        vec->capacity = NextPow2(new_size);
        void *new_array = ArenaPush(arena, vec->capacity);
        memcpy(new_array, array, old_size);
        return new_array;
    }

    return array;
}

//
// Strings
//

using Str = Arr<u8>;

#define S(s) ((Str){ .v = (u8 *)(s), .n = (sizeof(s)) - 1 })
#define A(a) { .v = (a), .n = sizeof((a)) / sizeof((a)[0]) }

char *str_to_c(Arena *arena, Str s) {
    Arr<char> cstr = arena_push_arr<char>(arena, s.n + 1);
    // Compiler plz vectorize
    for (u64 i = 0; i < s.n; i++) {
        cstr.v[i] = s.v[i];
    }
    // Arena allocation is already zeroed, so null terminator is in place
    return cstr.v;
}

Str str_from_cstr(char *cstr) {
    u64 len = 0;
    for (u64 i = 0; cstr[i] != '\0'; i++) {
        len++;
    }
    return (Str){ .v = (u8 *)cstr, .n = len };
}

bool str_is_empty(Str s) {
    return s.n == 0;
}

// Returns a string from a utf8 byte buffer. Doesn't validate if it's actually utf8.
Str str_from_bytes(Arr<u8> bytes) {
    // Skip utf8 BOM
    u8 *s = bytes.v;
    u64 size = bytes.n;
    if (size >= 3 && s[0] == u8'\xef' && s[1] == u8'\xbb' && s[2] == u8'\xbf') {
        s += 3;
        size -= 3;
    }

    return (Str){ .v = s, .n = size };
}

// Super loose definition probably
bool char_is_whitespace(u8 c) {
    return c == u8' ' || c == u8'\r' || c == u8'\n';
}

Str str_trim(Str s) {
    u64 start = 0;
    while (start < s.n && char_is_whitespace(s.v[start])) {
        start++;
    }

    i64 end = ((i64)s.n) - 1;
    while (end >= 0 && char_is_whitespace(s.v[end])) {
        end--;
    }
    
    return (Str){.v = s.v + start, .n = (u64)(end + 1) - start};
}

Str str_clone(Arena *arena, Str s) {
    Str clone = arena_push_arr<u8>(arena, s.n);
    for (u64 i = 0; i < s.n; i++) {
        clone.v[i] = s.v[i];
    }
    return clone;
}

bool str_starts_with(Str s, Str prefix) {
    return prefix.n <= s.n && memcmp(s.v, prefix.v, prefix.n) == 0;
}

bool str_equals(Str a, Str b) {
     return a.n == b.n && memcmp(a.v, b.v, a.n) == 0;
}

// Certainly possible to do this simply and w/o an iterator object, but just messin around
struct StrLineIter {
    Str base;
    u64 pos;
};

StrLineIter str_lines(Str s) {
     return (StrLineIter){ .base = s, .pos = 0 };
}

bool str_lines_next(StrLineIter* iter, Str *line) {
    if (iter->pos >= iter->base.n) {
        return false;
    }

    u64 line_start = iter->pos;
    u8 *data = iter->base.v;
    const u64 size = iter->base.n;

    // Advance until next line break
    u64 line_end = line_start;
    while (line_end < size && data[line_end] != '\r' && data[line_end] != '\n') {
        line_end++;
    }

    // Advance past line breaks
    u64 next_line_start = line_end;
    while (next_line_start < size && (data[next_line_start] == '\r')) {
        next_line_start++;
    }
    if (next_line_start < size && (data[next_line_start] == '\n')) {
        next_line_start++;
    }

    iter->pos = next_line_start;

    if (line != nullptr) {
        line->v = iter->base.v + line_start;
        line->n = line_end - line_start;
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

//
// Main
//

static Str MacBackupDirs[] = {
    S("Documents"),
    S("Pictures"),
    S("Music"),
    S("Movies"),
    S("Library/Application Support/Anki2"),
};

static Str ExcludePatterns[] = {
    S("node_modules/**"),
    S(".cache/**"),
    S(".vscode/**"),
    S(".npm/**"),
    S(".vscode-server/**"),
    S("*.photoslibrary"),
    S(".DS_Store"),
    S("build*/**"),
    S("Photo Booth Library"),
    S("target/debug/**"),
    S("target/release/**"),
};

struct ResticConfig {
    Str name;
    Str restic_repository;
    Str restic_password;
    Str aws_access_key_id; // Optional
    Str aws_secret_access_key; // Optional
};

// Subprocesses

struct Cmd {
    Str name;
    Arr<Str> args;
    Arr<Str> env;
};

// TODO error handling, stdin
void run_cmd(Cmd *cmd) {
    Arena scratch = {};

    pid_t pid;
    char *name = str_to_c(&scratch, cmd->name);
    const posix_spawn_file_actions_t *file_actions = nullptr;
    const posix_spawnattr_t *attrp = nullptr;

    Arr<char *> args = arena_push_arr<char *>(&scratch, cmd->args.n + 2);
    args.v[0] = name;
    for (u64 i = 0; i < cmd->args.n; i++) {
        args.v[i + 1] = str_to_c(&scratch, cmd->args.v[i]);
    }

    Arr<char *> env = arena_push_arr<char *>(&scratch, cmd->env.n + 1);
    for (u64 i = 0; i < cmd->env.n; i++) {
        env.v[i] = str_to_c(&scratch, cmd->env.v[i]);
    }

    int result = posix_spawnp(&pid, name, file_actions, attrp, args.v, env.v);
    if (result != 0) {
        fprintf(stderr, "Failed to posix_spawnp: code %d\n", result);
        exit(EXIT_FAILURE);
    }

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) {
        fprintf(stderr, "Subprocess '%s' didn't exit normally\n", name);
        exit(EXIT_FAILURE);
    }
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Subprocess '%s' exited with code %d\n", name, WEXITSTATUS(status));
        exit(EXIT_FAILURE);
    }

    arena_release(&scratch);
}

// Goal: count lines in file
int main(int argc, char **argv) {
    Arena arena = {};

    Str env[] = { 
        S("PWD=/Users/alex/Documents/repos/2023/backuper"),
    };
    Str args[] = {
        S("-lh"),
    };
    Cmd cmd = { 
        .name = S("ls"),
        .args = A(args),
        .env = A(env),
    };
    run_cmd(&cmd);
    
    arena_release(&arena);

    return EXIT_SUCCESS;
}
