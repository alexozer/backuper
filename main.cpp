// TODO clean up includes
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
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

#define Kilobytes(n) (n * 1024LL)
#define Megabytes(n) (Kilobytes(n) * 1024LL)

#define AlignTo(n, a) (((n) + (a - 1)) & ~(a - 1))
#define DEFAULT_ALIGNMENT 8

#define Min(a, b) (((a) < (b)) ? a : b)
#define Max(a, b) (((a) > (b)) ? a : b)

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

struct Arena {
    void *data;
    u64 reserved;
    u64 offset;
};

void _ArenaEnsureInit(Arena *arena) {
    if (arena->data == nullptr) {
        const u64 data_size = Megabytes(16);
        arena->data = calloc(1, data_size);
        arena->reserved = data_size;
    }
}

// TODO deal with e.g. string nonalignment
void *_ArenaPush(Arena *arena, u64 size, u64 alignment) {
    _ArenaEnsureInit(arena);

    void *pos = (void *)((u64)arena->data + arena->offset);
    size = AlignTo(size, alignment);
    arena->offset += size;
    if (arena->offset > arena->reserved) {
         fprintf(stderr, "Arena over!\n");
         exit(EXIT_FAILURE);
    }
    return pos;
}

#define ArenaPush(arena, size) (_ArenaPush((arena), size, DEFAULT_ALIGNMENT))
#define ArenaPushStruct(arena, type) (((type)*)ArenaPush((arena), sizeof(type)))
#define ArenaPushArray(arena, count_, type) \
    ( \
      (type##Array){ \
      .v = _ArenaPush((arena), (count_) * sizeof(type), alignof(type)), \
      .count = (count_), \
      } \
      )

void ArenaRelease(Arena *arena) {
    if (arena->data != nullptr) {
        free(arena->data);
        *arena = (Arena){};
    }
}

//
// Vec
//

template <typename T>
struct Arr {
    T *v;
    u64 n;
};

template <typename T>
struct Vec {
    T *v;
    u64 n;
    u64 cap;
};

#define MIN_VEC_CAPACITY 8

#define VecHeaderCast(a) ((VecHeader *)(&a))
#define VecItemSize(a) (sizeof(*(a).v))

template <typename T>
void *VecGrow(Arena *arena, Vec<T> *header, void *array, u64 item_size, u64 count) {
    const u64 old_size = header->count * item_size;
    const u64 new_size = (header->count + Max(count, MIN_VEC_CAPACITY)) * item_size;

    if (new_size > header->capacity) {
        header->capacity = NextPow2(new_size);
        void *new_array = ArenaPush(arena, header->capacity);
        memcpy(new_array, array, old_size);
        return new_array;
    }

    return array;
}

#define VecPush(arena, a, value) \
    (*((void **)&(a).v) = VecGrow((arena), VecHeaderCast((a)), (a).v, VecItemSize((a)), 1), \
     (a).v[(a).count++] = (value))

#define VecExtend(arena, a, count, values) \
    (*((void **)&(a).v) = VecGrow((arena), VecHeaderCast((a)), (a).v, VecItemSize((a)), count), \
     memcpy((a).v, values, VecItemSize((a)) * count), \
     (a).count += count)

#define VecClear(a) ((a).size = 0)

//
// Strings
//

using String = Arr<u8>;

// Yeah it's hiding a pointer behind, but it lets us make e.g. CStrArray
typedef char *CStr;

#define S(s) ((String){ .v = (u8 *)(s), .n = (sizeof(s)) - 1 })
#define A(a, type_) ((type_##Array){ .v = a, .count = sizeof((a)) / sizeof(type_)})

char *StrToC(Arena *arena, String s) {
    char *cstr = (char *)ArenaPush(arena, s.n + 1);
    // Compiler plz vectorize
    for (u64 i = 0; i < s.n; i++) {
        cstr[i] = s.v[i];
    }
    // Arena allocation is already zeroed, so null terminator is in place
    return cstr;
}

String StrFromCStr(char *cstr) {
    u64 len = 0;
    for (u64 i = 0; cstr[i] != '\0'; i++) {
        len++;
    }
    return (String){.v = (u8 *)cstr, .n = len};
}

bool StrIsEmpty(String s) {
    return s.n == 0;
}

// Returns a string from a utf8 byte buffer. Doesn't validate if it's actually utf8.
String StrFromBytes(void *buf, u64 size) {
    // Skip utf8 BOM
    u8 *s = (u8 *)buf;
    if (size >= 3 && s[0] == u8'\xef' && s[1] == u8'\xbb' && s[2] == u8'\xbf') {
        s += 3;
        size -= 3;
    }

    return (String){.v = s, .n = size};
}

// Super loose definition probably
bool CharIsWhitespace(u8 c) {
    return c == u8' ' || c == u8'\r' || c == u8'\n';
}

String StrTrim(String s) {
    u64 start = 0;
    while (start < s.n && CharIsWhitespace(s.v[start])) {
        start++;
    }

    i64 end = ((i64)s.n) - 1;
    while (end >= 0 && CharIsWhitespace(s.v[end])) {
        end--;
    }
    
    return (String){.v = s.v + start, .n = (u64)(end + 1) - start};
}

String StrClone(Arena *arena, String s) {
    void *data = ArenaPush(arena, s.n);
    memcpy(data, s.v, s.n);
    return (String){.v = data, .n = s.n};
}

bool StrStartsWith(String s, String prefix) {
    return prefix.size <= s.size && memcmp(s.data, prefix.data, prefix.size) == 0;
}

bool StrEquals(String a, String b) {
     return a.size == b.size && memcmp(a.data, b.data, a.size) == 0;
}

// Certainly possible to do this simply and w/o an iterator object, but just messin around
struct LineIter {
    String base;
    u64 pos;
};

LineIter StrLines(String s) {
     return (LineIter){ .base = s, .pos = 0 };
}

bool StrLinesNext(LineIter* iter, String *line) {
    if (iter->pos >= iter->base.size) {
        return false;
    }

    u64 line_start = iter->pos;
    u8 *data = iter->base.data;
    const u64 size = iter->base.size;

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
        line->data = iter->base.data + line_start;
        line->size = line_end - line_start;
    }

    return true;
}

u64 StrCountLines(String s) {
    u64 line_count = 0;
    LineIter iter = StrIterLines(s);
    while (LineIterHasNext(&iter)) {
        LineIterNext(&iter);
        line_count++;
    }
    return line_count;
}

//
// mmap
//

// String MmapFileAsString(Arena *arena, String filepath) {
//     char *filepath_cstr = StrToC(arena, filepath);
//
//     const i32 fd = open(filepath_cstr, O_RDONLY);
//     if (fd == -1) {
//         return (String){};
//     }
//     // Defer(arena /* , close(fd) */);
//
//     struct stat st;
//     if (fstat(fd, &st) == -1) {
//         return (String){};
//     }
//
//     void *buf = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
//     if (buf == MAP_FAILED) {
//         return (String){};
//     }
//     // Defer(arena /* munmap(buf, st.st_size) */);
//
//     return StrFromBytes(buf, st.st_size);
// }

//
// Main
//

DefineArray(String);

static String MacBackupDirs[] = {
    S("Documents"),
    S("Pictures"),
    S("Music"),
    S("Movies"),
    S("Library/CloudStorage/Dropbox"),
    S("Library/Application Support/Anki2"),
};

static String ExcludePatterns[] = {
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

typedef struct {
    String name;
    String restic_repository;
    String restic_password;
    String aws_access_key_id; // Optional
    String aws_secret_access_key; // Optional
} ResticConfig;

// Subprocesses

typedef struct {
    String name;
    StringArray args;
    StringArray env;
} Cmd;

DefineArray(CStr);

// TODO error handling, stdin
void run_cmd(Cmd *cmd) {
    Arena scratch = {};

    pid_t pid;
    CStr name = StrToC(&scratch, cmd->name);
    const posix_spawn_file_actions_t *file_actions = nullptr;
    const posix_spawnattr_t *attrp = nullptr;

    CStrArray args = ArenaPushArray(&scratch, cmd->args.count + 1, CStr);
    for (u64 i = 0; i < cmd->args.count; i++) {
        args.v[i] = StrToC(&scratch, cmd->args.v[i]);
    }

    CStrArray env = ArenaPushArray(&scratch, cmd->env.count + 1, CStr);
    for (u64 i = 0; i < cmd->env.count; i++) {
        env.v[i] = StrToC(&scratch, cmd->env.v[i]);
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

    ArenaRelease(&scratch);
}

// Goal: count lines in file
int main(int argc, char **argv) {
    Arena arena = {};

    String env[] = (String[]) { S("PWD=something") };
    Cmd cmd = { 
        .name = S("ls"),
        .env = A(env, String),
    };
    run_cmd(&cmd);

    return EXIT_SUCCESS;
}
