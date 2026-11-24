#include "base.hpp"

#include <stdio.h>
#include <stdlib.h>

#include "platform.hpp"

static Str NAS_DIRS[] = {
    S("Documents"), S("Pictures"), S("Movies"), S("Library/Application Support/Anki2"), S("Music"),
};

static Str CLOUD_DIRS[] = {
    S("Documents"),
    S("Pictures"),
    S("Movies"),
    S("Library/Application Support/Anki2"),
};

static Str EXCLUDE_PATTERNS[] = {
    S("node_modules/**"), S(".cache/**"),         S(".zig-cache/**"), S("zig-out/**"),
    S("zig-pkg/**"),      S(".vscode/**"),        S(".npm/**"),       S(".vscode-server/**"),
    S("*.photoslibrary"), S(".DS_Store"),         S("build*/**"),     S("Photo Booth Library"),
    S("target/debug/**"), S("target/release/**"), S("Music/Music"),
};

struct ResticConfig {
    Str name;
    Arr<Str> dirs;
    Str restic_repository;
    Str restic_password;
    Str aws_access_key_id;      // Optional
    Str aws_secret_access_key;  // Optional
};

Arr<ResticConfig> get_restic_configs(Arena *arena) {
    Vec<ResticConfig> configs = {};

    ResticConfig *nas_config = vec_push(arena, &configs, ResticConfig{});
    nas_config->name = S("NAS REST");
    nas_config->dirs = A(NAS_DIRS);
    nas_config->restic_repository = env_get(S("BACKUPER_NAS_REPOSITORY"));
    nas_config->restic_password = env_get(S("BACKUPER_PASSWORD"));

    ResticConfig *cloud_config = vec_push(arena, &configs, ResticConfig{});
    cloud_config->name = S("Cloud B2");
    cloud_config->dirs = A(CLOUD_DIRS);
    cloud_config->restic_repository = env_get(S("BACKUPER_AWS_REPOSITORY"));
    cloud_config->restic_password = env_get(S("BACKUPER_PASSWORD"));
    cloud_config->aws_access_key_id = env_get(S("BACKUPER_AWS_ACCESS_KEY_ID"));
    cloud_config->aws_secret_access_key = env_get(S("BACKUPER_AWS_SECRET_ACCESS_KEY"));

    return vec_arr(&configs);
}

void do_upgrade() {
    log_info("Starting macOS upgrades");

    Str args[] = {S("upgrade")};
    Cmd cmd = {
        .name = S("brew"),
        .args = A(args),
    };
    if (cmd_run(&cmd) != OSResult::Ok) {
        log_error("Failed to execute brew command");
    }

    log_info("Finished macOS upgrades");
}

void backup_filesystem_to(ResticConfig *config, Arr<Str> extra_restic_args) {
    Arena *scratch = arena_acquire();
    defer(arena_release(scratch));

    log_info("Backup to '%.*s' started", SF(config->name));

    // Build args
    Str base_restic_args[] = {
        S("backup"),
        S("--files-from"),
        S("-"),
        S("--exclude-caches"),
    };
    Vec<Str> restic_args = {};
    vec_extend(scratch, &restic_args, A(base_restic_args));
    vec_extend(scratch, &restic_args, extra_restic_args);

    Arr<Str> excludes = A(EXCLUDE_PATTERNS);
    for (u64 i = 0; i < excludes.count; i++) {
        vec_push(scratch, &restic_args, S("--exclude"));
        vec_push(scratch, &restic_args, excludes[i]);
    }

    // Build env
    Vec<Pair<Str, Str>> env = {};
    vec_push(scratch, &env, {S("RESTIC_REPOSITORY"), config->restic_repository});
    vec_push(scratch, &env, {S("RESTIC_PASSWORD"), config->restic_password});
    if (!arr_is_empty(config->aws_access_key_id)) {
        vec_push(scratch, &env, {S("AWS_ACCESS_KEY_ID"), config->aws_access_key_id});
    }
    if (!arr_is_empty(config->aws_secret_access_key)) {
        vec_push(scratch, &env, {S("AWS_SECRET_ACCESS_KEY"), config->aws_secret_access_key});
    }

    // Build file input list (stdin)
    Vec<u8> abs_file_patterns = {};
    Str home = env_get(S("HOME"));
    for (u64 i = 0; i < config->dirs.count; i++) {
        Str joined = path_join(scratch, home, config->dirs[i]);
        vec_extend(scratch, &abs_file_patterns, joined);
        vec_push(scratch, &abs_file_patterns, C('\n'));
    }

    Cmd restic_cmd = {
        .name = S("restic"),
        .args = vec_arr(&restic_args),
        .env = vec_arr(&env),
        .input = vec_arr(&abs_file_patterns),
    };
    if (cmd_run(&restic_cmd) != OSResult::Ok) {
        log_error("Failed to execute restic command");
    }

    log_info("Backup to '%.*s' complete", SF(config->name));
}

void do_backup() {
    Arena *arena = arena_acquire();
    defer(arena_release(arena));

    log_info("Starting system backup");

    Arr<ResticConfig> configs = get_restic_configs(arena);
    Str extra_restic_args[] = {S("--tag"), S("macos")};
    for (u64 i = 0; i < configs.count; i++) {
        backup_filesystem_to(&configs[i], A(extra_restic_args));
    }

    log_info("Finished system backup");
}

int main(int argc, char **argv, char **envp) {
    g_argv = {.value = argv, .count = (u64)argc};
    g_envp = arr_from_null_terminated(envp);
    arena_pool_init();

    do_upgrade();
    do_backup();

    return EXIT_SUCCESS;
}
