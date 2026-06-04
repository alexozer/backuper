#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>

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

constexpr u64 kilobytes(u64 n) { return n * 1024LL; }
constexpr u64 megabytes(u64 n) { return kilobytes(n) * 1024LL; }
constexpr u64 align_to(u64 n, u64 a) { return ((n) + (a - 1)) & ~(a - 1); }

template <typename T>
constexpr T min(T a, T b) { return a < b ? a : b; }

template <typename T>
constexpr T max(T a, T b) { return a > b ? a : b; }

constexpr u64 DEFAULT_ALIGNMENT = 8;

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
// Logging
//

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

#define log_trace(...) log_log(LogLevel::Trace, __VA_ARGS__)
#define log_debug(...) log_log(LogLevel::Debug, __VA_ARGS__)
#define log_info(...)  log_log(LogLevel::Info, __VA_ARGS__)
#define log_warn(...)  log_log(LogLevel::Warn, __VA_ARGS__)
#define log_error(...) log_log(LogLevel::Error, __VA_ARGS__)
__attribute__((format(printf, 1, 2)))
[[noreturn]] void log_fatal(const char *fmt, ...);

void log_set_level(LogLevel level);
__attribute__((format(printf, 2, 3)))
void log_log(LogLevel level, const char *fmt, ...);

//
// Arenas
//

template <typename T>
struct Arr {
    T *value;
    u64 count;

    T& operator[](u64 i) {
        assert(i < count);
        return value[i];
    }
};

struct Arena {
    void *data;
    u64 reserved;
    u64 offset;
};

void *arena__push_bytes(Arena *arena, u64 size, u64 alignment);

void arena_pool_init();
Arena *arena_acquire();
void arena_release(Arena *arena);

template <typename T>
T *arena_push(Arena *arena) {
    return (T *)arena__push_bytes(arena, sizeof(T), 8);
}

template <typename T>
Arr<T> arena_push_arr(Arena *arena, u64 count) {
    return {
        .value = (T *)arena__push_bytes(arena, sizeof(T) * count, 8),
        .count = count,
    };
}

//
// Arrays
//

template <typename T>
Arr<T> arr_from_null_terminated(T *v) {
    u64 count = 0;
    while (v[count] != nullptr) count++;
    return { .value = v, .count = count };
}

template <typename T>
Arr<T> arr_slice(Arr<T> arr, u64 start, u64 end) {
    assert(start <= arr.count);
    assert(end <= arr.count);
    assert(start <= end);

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
        log_fatal("Unequal array lengths: dest = %" PRIu64 ", source = %" PRIu64, dest.count, source.count);
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
#define SF(s) (int)(s).count, (char *)(s).value
#define C(c) ((u8)(c))
#define A(a) { .value = (a), .count = sizeof((a)) / sizeof((a)[0]) }

char *str_to_c(Arena *arena, Str s);
Str str_from_c(const char *cstr);
Str str_from_c_len(const char *cstr);
Str str_from_bytes(Arr<u8> bytes);
bool char_is_whitespace(u8 c);
Str str_trim(Str s);
Str str_clone(Arena *arena, Str s);
bool str_eq(Str s1, Str s2);
bool str_starts_with(Str s, Str prefix);
__attribute__((format(printf, 2, 3)))
Str str_format(Arena *arena, const char *format, ...);

// Certainly possible to do this simply and w/o an iterator object, but just messin around
struct StrLineIter {
    Str base;
    u64 pos;
};

StrLineIter str_lines(Str s);
bool str_lines_next(StrLineIter* iter, Str *line);
u64 str_count_lines(Str s);
Pair<Str, Str> str_split2(Str base, u8 delim);
constexpr bool str_is_empty(Str s) { return s.count == 0; }

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
            log_fatal("Bounds check fail! %" PRIu64 " >= %" PRIu64, i, count);
        }
        return value[i];
    }
};

constexpr u64 MIN_VEC_CAPACITY = 8;

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
void vec_pop(Vec<T> *vec) {
    assert(vec->count > 0);
    vec->count--;
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

template <typename T>
void vec_reset(Vec<T> *vec) {
    vec->count = 0;
}

//
// Paths
//

Str path_join(Arena *arena, Str left_path, Str right_path);

//
// Defer
//

template<typename F>
struct Defer {
    F fn;
    explicit Defer(F f) : fn(f) {}
    ~Defer() { fn(); }

    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;
};

// Deduction guide (C++17) — lets you write Defer d([&]{...}) without specifying F
template<typename F>
Defer(F) -> Defer<F>;

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)
#define defer(code) Defer CONCAT(_defer_, __LINE__)([&]{ code; })

//
// Subprocesses
//

extern Arr<char *> g_argv;
extern Arr<char *> g_envp;

Str env_get(Str key);

struct Cmd {
    Str name;
    Arr<Str> args;
    Arr<Pair<Str, Str>> env;
    Arr<u8> input;
    Str cwd;
};

enum class [[nodiscard]] OSResult {
    Ok,
    PermissionDenied,
    AllocationFailed,
    InvalidPath,
    InvalidFileDescriptor,
    SubprocessExitError,
    SubprocessNonZeroExitCode,
    OtherError,
};

//
// Time
//

struct Duration {
    i64 seconds;
    u32 nanoseconds;
};

struct Instant {
    i64 seconds;
    u32 nanoseconds;
};

// Nanosconds. Signed so that we can use the same type for diffs.
constexpr Duration DURATION_NANOSECOND = { .nanoseconds = 1 };
constexpr Duration DURATION_MICROSECOND = { .nanoseconds = 1'000 };
constexpr Duration DURATION_MILLISECOND = { .nanoseconds = 1'000'000 };
constexpr Duration DURATION_SECOND = { .seconds = 1 };
constexpr Duration DURATION_ZERO = {};

constexpr Duration operator+(const Duration &t1, const Duration &t2) {
    i64 ns_sum = t1.nanoseconds + t2.nanoseconds;
    i64 sec_sum = t1.seconds + t2.seconds + (ns_sum / 1'000'000'000);
    return { .seconds = sec_sum, .nanoseconds = (u32)(ns_sum % 1'000'000'000) };
}

constexpr Duration operator-(const Duration &t1, const Duration &t2) {
    i64 ns_diff = (i64)t1.nanoseconds - (i64)t2.nanoseconds;
    i64 sec_diff = t1.seconds - t2.seconds;
    if (ns_diff < 0) {
        // Carry the 1...
        sec_diff--;
        ns_diff += 1'000'000'000;
    }
    return { .seconds = sec_diff, .nanoseconds = (u32)(ns_diff) };
}

constexpr Duration operator-(const Duration &t) {
    return DURATION_ZERO - t;
}

constexpr void operator+=(Duration& t1, const Duration &t2) {
    t1 = t1 + t2;
}

constexpr void operator-=(Duration& t1, const Duration &t2) {
    t1 = t1 - t2;
}

constexpr bool operator==(const Duration &t1, const Duration &t2) {
    return t1.seconds == t2.seconds && t1.nanoseconds == t2.nanoseconds;
}

constexpr bool operator<(const Duration &t1, const Duration &t2) {
    if (t1.seconds < t2.seconds) {
        return true;
    }
    if (t1.seconds > t2.seconds) {
        return false;
    }
    return (t1.seconds < 0) ^ (t1.nanoseconds < t2.nanoseconds);
}

constexpr bool operator<=(const Duration &t1, const Duration &t2) {
    return (t1 < t2) || t1 == t2;
}

constexpr bool operator>(const Duration &t1, const Duration &t2) {
    return !(t1 <= t2);
}

constexpr bool operator>=(const Duration &t1, const Duration &t2) {
    return !(t1 < t2);
}

constexpr u64 duration_seconds(Duration duration) {
    if (duration.seconds < 0) {
        return (u64)(-duration).seconds;
    } else {
        return (u64)duration.seconds;
    }
}

constexpr u32 duration_subsec_nanos(Duration duration) {
    if (duration.seconds < 0) {
        return (-duration).nanoseconds;
    } else {
        return duration.nanoseconds;
    }
}

constexpr Duration operator-(const Instant &t1, const Instant &t2) {
    Duration d1 = { .seconds = t1.seconds, .nanoseconds = t1.nanoseconds };
    Duration d2 = { .seconds = t2.seconds, .nanoseconds = t2.nanoseconds };
    return d1 - d2;
}

//
// World's crappiest optional type
//

template <typename T>
struct Opt {
    bool present;
    T value;
};

template <typename T>
constexpr Opt<T> some(T v) { return { .present = true, .value = v }; }
