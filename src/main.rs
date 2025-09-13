use anyhow::{Context, anyhow};
use std::{
    env,
    io::Write,
    process::{Command, Stdio},
    time::{self, Duration},
};

//
// Start backup config
//

enum BackupDir<'a> {
    Home(&'a str),
}

static MAC_BACKUP_DIRS: &[BackupDir] = &[
    BackupDir::Home("Documents"),
    BackupDir::Home("Pictures"),
    BackupDir::Home("Music"),
    BackupDir::Home("Movies"),
    BackupDir::Home("Library/CloudStorage/Dropbox"),
    BackupDir::Home("Library/Application Support/Anki2"),
];

static EXCLUDE_PATTERNS: &[&str] = &[
    "node_modules/**",
    ".cache/**",
    ".vscode/**",
    ".npm/**",
    ".vscode-server/**",
    "*.photoslibrary",
    ".DS_Store",
    "build*/**",
    "Photo Booth Library",
    "target/debug/**",
    "target/release/**",
];

//
// End backup config
//

struct ResticConfig {
    name: String,
    restic_repository: String,
    restic_password: String,
    aws_access_key_id: Option<String>,
    aws_secret_access_key: Option<String>,
}

fn backup_dirs_to_strings(backup_dirs: &[BackupDir]) -> anyhow::Result<Vec<String>> {
    backup_dirs
        .iter()
        .map(|d| match d {
            BackupDir::Home(path_str) => {
                let mut path = std::env::home_dir().ok_or(anyhow!("Failed to get home dir"))?;
                path.push(path_str);
                Ok(path.to_string_lossy().to_string())
            }
        })
        .collect()
}

fn pretty_duration(duration: Duration) -> String {
    let minute: u64 = 60;
    let hour: u64 = minute * 60;
    let day: u64 = hour * 24;

    let duration_sec = duration.as_secs();
    let seconds = duration_sec % minute;
    let minutes = duration_sec % hour / minute;
    let hours = duration_sec % day / hour;
    let days = duration_sec / day;

    match (days, hours, minutes, seconds) {
        (0, 0, 0, ..) => format!("{seconds}sec"),
        (0, 0, ..) => format!("{minutes}min {seconds}sec"),
        (0, ..) => format!("{hours}hr {minutes}min {seconds}sec"),
        (..) => format!("{days}d {hours}hr {minutes}min {seconds}sec"),
    }
}

fn gen_exclude_flags<'a>(patterns: &'a [&'a str]) -> Vec<&'a str> {
    patterns.iter().flat_map(|p| ["--exclude", p]).collect()
}

fn sh<'a>(cmd: &'a [&'a str]) -> ShBuilder<'a> {
    ShBuilder::new(cmd)
}

struct ShBuilder<'a> {
    cmd: &'a [&'a str],
    env: &'a [(&'a str, &'a str)],
    input: &'a str,
    show_output: bool,
}

impl<'a> ShBuilder<'a> {
    fn new(cmd: &'a [&'a str]) -> Self {
        Self {
            cmd,
            env: &[],
            input: "",
            show_output: false,
        }
    }

    fn env(mut self, env: &'a [(&'a str, &'a str)]) -> Self {
        self.env = env;
        self
    }

    fn input(mut self, input: &'a str) -> Self {
        self.input = input;
        self
    }

    fn show_output(mut self) -> Self {
        self.show_output = true;
        self
    }

    fn run(self) -> anyhow::Result<()> {
        // Print command to run
        let cmd_str = self.cmd.join(" ");
        log::info!("Running: {cmd_str}");

        // Spawn a new child process with the given command, args, and env vars
        let mut cmd = Command::new(self.cmd[0]);
        let mut child = cmd
            .args(&self.cmd[1..])
            .stdin(Stdio::piped())
            .envs(self.env.to_vec());
        if self.show_output {
            child = child.stdout(Stdio::inherit()).stderr(Stdio::inherit());
        } else {
            child = child.stdout(Stdio::piped()).stderr(Stdio::piped());
        }
        let mut child = child.spawn()?;

        // Write the input to the child process's stdin
        child
            .stdin
            .as_mut()
            .ok_or(anyhow!("Failed to get stdin"))?
            .write_all(self.input.as_bytes())?;

        let output = child.wait_with_output()?;

        if !output.status.success() {
            let stderr_str = String::from_utf8(output.stderr)?;
            return Err(anyhow!(stderr_str));
        }

        Ok(())
    }
}

fn get_env_var(var: &str) -> anyhow::Result<String> {
    env::var(var).with_context(|| format!("Env var not found: {}", var))
}

fn try_task<F>(name: &str, func: F, error_list: &mut Vec<String>)
where
    F: FnOnce() -> anyhow::Result<()>,
{
    log::info!("Starting task: {name}");

    let start = time::Instant::now();
    let result = func();
    let dur = start.elapsed();
    let pretty_dur = pretty_duration(dur);

    match result {
        Ok(()) => {
            log::info!("Task succeeded in {pretty_dur}: {name}");
        }
        Err(e) => {
            let err_str = format!("[{} in {pretty_dur}] {}", name, e);
            error_list.push(err_str);
            log::error!("Task failed in {pretty_dur}: {name}");
        }
    }
}

fn do_macos_upgrades() -> anyhow::Result<()> {
    sh(&["brew", "upgrade"]).show_output().run()
}

fn restic_config_to_env(config: &ResticConfig) -> Vec<(&str, &str)> {
    let mut env_pairs: Vec<(&str, &str)> = vec![
        ("RESTIC_REPOSITORY", &config.restic_repository),
        ("RESTIC_PASSWORD", &config.restic_password),
    ];
    if let Some(key_id) = config.aws_access_key_id.as_ref() {
        env_pairs.push(("AWS_ACCESS_KEY_ID", key_id));
    }
    if let Some(secret) = config.aws_secret_access_key.as_ref() {
        env_pairs.push(("AWS_SECRET_ACCESS_KEY", secret));
    }
    env_pairs
}

fn backup_filesystem_to(
    file_patterns: &[BackupDir],
    config: &ResticConfig,
    extra_restic_args: &[&str],
) -> anyhow::Result<()> {
    let mut restic_args = vec!["restic", "backup", "--files-from", "-", "--exclude-caches"];
    restic_args.extend(extra_restic_args);
    restic_args.extend(gen_exclude_flags(EXCLUDE_PATTERNS));

    let input = backup_dirs_to_strings(file_patterns)?.join("\n");
    let env = restic_config_to_env(config);
    sh(&restic_args)
        .env(&env)
        .input(&input)
        .show_output()
        .run()?;

    log::info!("Backed up local filesystem to {}", config.restic_repository);
    Ok(())
}

fn do_backup_macos(cloud_config: &ResticConfig, errors: &mut Vec<String>) {
    log::info!("Backup to '{}' started", cloud_config.name);
    try_task(
        "Backup macOS Filesystem",
        || backup_filesystem_to(MAC_BACKUP_DIRS, cloud_config, &["--tag", "macOS"]),
        errors,
    );
    log::info!("Backup to '{}' complete", cloud_config.name);
}

fn do_backup() -> Vec<String> {
    let any_to_cloud_config_func = || -> anyhow::Result<Vec<ResticConfig>> {
        let nas_config = ResticConfig {
            name: "NAS REST".into(),
            restic_repository: get_env_var("BACKUPER_NAS_REPOSITORY")?,
            restic_password: get_env_var("BACKUPER_PASSWORD")?,
            aws_access_key_id: None,
            aws_secret_access_key: None,
        };
        let cloud_config = ResticConfig {
            name: "Cloud B2".into(),
            restic_repository: get_env_var("BACKUPER_AWS_REPOSITORY")?,
            restic_password: get_env_var("BACKUPER_PASSWORD")?,
            aws_access_key_id: Some(get_env_var("BACKUPER_AWS_ACCESS_KEY_ID")?),
            aws_secret_access_key: Some(get_env_var("BACKUPER_AWS_SECRET_ACCESS_KEY")?),
        };
        Ok(vec![nas_config, cloud_config])
    };
    let cloud_config = match any_to_cloud_config_func() {
        Ok(conf) => conf,
        Err(e) => return vec![e.to_string()],
    };

    let mut errors = Vec::new();
    try_task("macOS Upgrades", do_macos_upgrades, &mut errors);
    for config in cloud_config {
        do_backup_macos(&config, &mut errors);
    }
    errors
}

// Stolen from Zed
fn init_stdout_logger() {
    env_logger::Builder::new()
        .filter_level(log::LevelFilter::Info)
        .parse_default_env()
        .format(|buf, record| {
            use env_logger::fmt::style::{AnsiColor, Style};

            let subtle = Style::new().fg_color(Some(AnsiColor::BrightBlack.into()));
            write!(buf, "{subtle}[{subtle:#}")?;
            write!(
                buf,
                "{} ",
                chrono::Local::now().format("%Y-%m-%dT%H:%M:%S%:z")
            )?;
            let level_style = buf.default_level_style(record.level());
            write!(buf, "{level_style}{:<5}{level_style:#}", record.level())?;
            if let Some(path) = record.module_path() {
                write!(buf, " {path}")?;
            }
            write!(buf, "{subtle}]{subtle:#}")?;
            writeln!(buf, " {}", record.args())
        })
        .init();
}

fn main() -> anyhow::Result<()> {
    init_stdout_logger();

    let start = time::Instant::now();
    let errors = do_backup();
    let dur = start.elapsed();

    let dur_pretty = pretty_duration(dur);

    if errors.is_empty() {
        log::info!("Completed in {dur_pretty}");
        log::info!("Backup succeeded");
        log::info!("Hope you're having a nice day :)");
    } else {
        let error_word = if errors.len() == 1 { "error" } else { "errors" };
        let joined_errors = errors.join("\n");
        log::info!("Completed in {dur_pretty}\n\n{joined_errors}");
        log::error!("Backup failed! {} {error_word}", errors.len());
    }

    Ok(())
}
