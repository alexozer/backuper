// TODO clean up includes
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
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
constexpr u64 next_pow2(u64 x) {
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
    T *value;
    u64 count;

    T& operator[](u64 i) {
        if (i >= count) {
            fprintf(stderr, "Bounds check fail! i = %lld, count = %lld\n", i, count);
            exit(EXIT_FAILURE);
        }
        return value[i];
    }
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
        .value = (T *)arena_push_bytes(arena, sizeof(T) * count),
        .count = count,
    };
}

void arena_release(Arena *arena) {
    if (arena->data != nullptr) {
        free(arena->data);
        *arena = (Arena){};
    }
}

//
// Arrays
//

template <typename T>
Arr<T> arr_from_null_terminated(T *v) {
    u64 n = 0;
    while (v[n] != nullptr) n++;
    return { .v = v, .n = n };
}

template <typename T>
Arr<T> arr_slice(Arr<T> arr, u64 start, u64 end) {
    if (start >= arr.count || end > arr.count || end < start) {
        fprintf(stderr, "Invalid array slice: count = %lld, start = %lld, end = %lld\n", arr.count, start, end);
        exit(EXIT_FAILURE);
    }

    return {
        .value = arr.value + start,
        .count = end - start,
    };
}

template <typename T>
bool arr_eq(Arr<T> a, Arr<T> b) {
    if (a.count != b.count) {
        return false;
    }
    for (u64 i = 0; i < a.count; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

template <typename T>
void arr_copy(Arr<T> dest, Arr<T> source) {
    if (dest.count != source.count) {
        fprintf(stderr, "Unequal array lengths: dest = %lld, source = %lld\n", dest.count, source.count);
        exit(EXIT_FAILURE);
    }
    for (u64 i = 0; i < dest.count; i++) {
        dest[i] = source[i];
    }
}

template <typename T>
bool arr_is_empty(Arr<T> arr) {
    return arr.count == 0;
}

// May come to regret this...
template <typename L, typename R>
struct Pair {
    L left;
    R right;
};

//
// Strings
//

using Str = Arr<u8>;

#define S(s) ((Str){ .value = (u8 *)(s), .count = (sizeof(s)) - 1 })
#define C(c) ((u8)(c))
#define A(a) { .value = (a), .count = sizeof((a)) / sizeof((a)[0]) }

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

// Certainly possible to do this simply and w/o an iterator object, but just messin around
struct StrLineIter {
    Str base;
    u64 pos;
};

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
// Vec
//

template <typename T>
struct Vec {
    T *value;
    u64 count; // Element count (not size in bytes)
    u64 cap; // Element capacity (not size capacity in bytes)
    
    T& operator[](u64 i) {
        if (i >= count) {
            fprintf(stderr, "Bounds check fail! %lld >= %lld", i, count);
            exit(EXIT_FAILURE);
        }
        return value[i];
    }
};

#define MIN_VEC_CAPACITY 8

template <typename T>
void vec__grow(Arena *arena, Vec<T> *vec, u64 new_cap) {
    // Fast path?
    if (new_cap <= vec->cap) return;

    new_cap = next_pow2(max(new_cap, MIN_VEC_CAPACITY));

    if (new_cap > vec->cap) {
        Arr<T> new_arr = arena_push_arr<T>(arena, new_cap);
        Arr<T> new_arr_slice = arr_slice(new_arr, 0, vec->count);
        arr_copy(new_arr_slice, vec_arr(vec));

        vec->value = new_arr.value;
        vec->cap = new_cap;
    }
}

template <typename T>
T *vec_push(Arena *arena, Vec<T> *vec, T val) {
    vec__grow(arena, vec, vec->count + 1);
    vec->value[vec->count] = val;
    return &vec->value[vec->count++];
}

template <typename T>
Arr<T> vec_extend(Arena *arena, Vec<T> *vec, Arr<T> arr) {
    vec__grow(arena, vec, vec->count + arr.count);

    u64 start = vec->count;
    vec->count += arr.count;
    Arr<T> a = arr_slice(vec_arr(vec), start, vec->count);
    arr_copy(a, arr);
    
    return a;
}

template <typename T>
Arr<T> vec_arr(Vec<T> *vec) {
    return { .value = vec->value, .count = vec->count };
}

//
// Maps
//

// TODO make not shit

// template <typename K, typename V>
// using Map = Vec<Pair<K, V>>;
//
// template <typename K, typename V>
// void map_set(Arena *arena, Map<K, V> *map, K key, V value) {
//
// }
//
// template <typename K, typename V>
// V map_get(Map<K, V> *map, K key) {
//     V ret = {};
//     for (u64 i = 0; i < map->n; i++) {
//
//     }
// }
//
// template <typename K, typename V>
// Arr<Pair<K, V>> map_entries(Map<K, V> *map) {
//     return vec_arr(map);
// }

// Subprocesses

struct Cmd {
    Str name;
    Arr<Str> args;
    Arr<Pair<Str, Str>> env;
};

// TODO error handling, stdin
void run_cmd(Cmd *cmd) {
    Arena scratch = {};

    pid_t pid;
    char *name = str_to_c(&scratch, cmd->name);
    const posix_spawn_file_actions_t *file_actions = nullptr;
    const posix_spawnattr_t *attrp = nullptr;

    Arr<char *> args = arena_push_arr<char *>(&scratch, cmd->args.count + 2);
    args[0] = name;
    for (u64 i = 0; i < cmd->args.count; i++) {
        args[i + 1] = str_to_c(&scratch, cmd->args[i]);
    }

    Arr<char *> env = arena_push_arr<char *>(&scratch, cmd->env.count + 1);
    for (u64 i = 0; i < cmd->env.count; i++) {
        Str var = cmd->env[i].left;
        Str val = cmd->env[i].right;

        // Build "{var}={val}"
        Vec<u8> line = {};
        vec_extend(&scratch, &line, var);
        vec_push(&scratch, &line, C('='));
        vec_extend(&scratch, &line, val);

        env[i] = str_to_c(&scratch, vec_arr(&line));
    }

    int result = posix_spawnp(&pid, name, file_actions, attrp, args.value, env.value);
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

Str env_get(Str key) {
    Arena scratch = {};

    char *key_cstr = str_to_c(&scratch, key);
    char *value_cstr = getenv(key_cstr);

    Str result = {};
    if (value_cstr != nullptr) {
        result = str_from_c(value_cstr);
    }

    arena_release(&scratch);

    return result;
}

Arr<ResticConfig> get_restic_configs(Arena *arena) {
    Vec<ResticConfig> configs = {};

    ResticConfig *nas_config = vec_push(arena, &configs, ResticConfig{});
    nas_config->name = S("NAS REST");
    nas_config->restic_repository = env_get(S("BACKUPER_NAS_REPOSITORY"));
    nas_config->restic_password = env_get(S("BACKUPER_PASSWORD"));

    ResticConfig *cloud_config = vec_push(arena, &configs, ResticConfig{});
    cloud_config->name = S("Cloud B2");
    cloud_config->restic_repository = env_get(S("BACKUPER_AWS_REPOSITORY"));
    cloud_config->restic_password = env_get(S("BACKUPER_PASSWORD"));
    cloud_config->aws_access_key_id = env_get(S("BACKUPER_AWS_ACCESS_KEY_ID"));
    cloud_config->aws_secret_access_key = env_get(S("BACKUPER_AWS_SECRET_ACCESS_KEY"));

    return vec_arr(&configs);
}

Arr<Pair<Str, Str>> restic_config_to_env(Arena *arena, ResticConfig *config) {
    Vec<Pair<Str, Str>> env = {};

    vec_push(arena, &env, { S("RESTIC_REPOSITORY"), config->restic_repository });
    vec_push(arena, &env, { S("RESTIC_PASSWORD"), config->restic_password });
    if (!arr_is_empty(config->aws_access_key_id)) {
        vec_push(arena, &env, { S("AWS_ACCESS_KEY_ID"), config->aws_access_key_id });
    }
    if (!arr_is_empty(config->aws_secret_access_key)) {
        vec_push(arena, &env, { S("AWS_SECRET_ACCESS_KEY"), config->aws_secret_access_key });
    }

    return vec_arr(&env);
}

void do_upgrade() {
    Pair<Str, Str> basic_env[] = {
        { S("HOME"), env_get(S("HOME")) },
        { S("USER"), env_get(S("USER")) },
    };

    Str args[] = { S("upgrade") };
    Cmd cmd = { 
        .name = S("brew"), 
        .args = A(args),
        .env = A(basic_env),
    };
    run_cmd(&cmd);
}

// Goal: count lines in file
int main(int argc, char **argv, char **envp) {
    Arena arena = {};

    do_upgrade();

    Arr<ResticConfig> configs = get_restic_configs(&arena);
    printf("Restic configs:\n");
    for (u64 i = 0; i < configs.count; i++) {
        printf("\n");
        char *name_cstr = str_to_c(&arena, configs[i].name);
        char *repo_cstr = str_to_c(&arena, configs[i].restic_repository);
        printf("Name: %s\n", name_cstr);
        printf("Repo: %s\n", repo_cstr);
    }
     
    arena_release(&arena);
    return EXIT_SUCCESS;
}
