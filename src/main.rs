use anyhow::anyhow;
use lettre::{
    message::header::ContentType, transport::smtp::authentication::Credentials, Message,
    SmtpTransport, Transport,
};
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
    Root(&'a str),
}

static MAC_BACKUP_DIRS: &[BackupDir] = &[
    BackupDir::Home("Documents"),
    BackupDir::Home("Pictures"),
    BackupDir::Home("Music"),
    BackupDir::Home("Movies"),
    BackupDir::Home("Library/CloudStorage/Dropbox"),
    BackupDir::Home("Library/Application Support/Anki2"),
];

static WINDOWS_BACKUP_DIRS: &[BackupDir] = &[
    BackupDir::Home("Documents"),
    BackupDir::Home("Pictures"),
    BackupDir::Home("Music"),
    BackupDir::Home("Videos"),
    // path.join(os.homedir(), 'build'),
    BackupDir::Home("ghidra_scripts"),
    BackupDir::Home("AppData\\Roaming"),
    BackupDir::Home("AppData\\Local\\osu!"),
    BackupDir::Home("AppData\\Local\\osulazer"),
    BackupDir::Home("AppData\\Local\\OpenTabletDriver"),
    BackupDir::Home("VirtualBox VMs"),
    // path.join(os.homedir(), 'iso'),
    BackupDir::Home("Dropbox"),
    BackupDir::Root("C:\\Program Files (x86)\\Steam\\steamapps\\common"),
    BackupDir::Root("C:\\tools"),
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
];

//
// End backup config
//

struct ResticConfig {
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
                let mut path = homedir::my_home()?.ok_or(anyhow!("Failed to get home dir"))?;
                path.push(path_str);
                Ok(path.to_string_lossy().to_string())
            }
            BackupDir::Root(path_str) => Ok(path_str.to_string()),
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
    check: bool,
}

impl<'a> ShBuilder<'a> {
    fn new(cmd: &'a [&'a str]) -> Self {
        Self {
            cmd,
            env: &[],
            input: "",
            check: true,
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

    fn check(mut self, check: bool) -> Self {
        self.check = check;
        self
    }

    fn run(self) -> anyhow::Result<()> {
        // Print command to run
        let cmd_str = self.cmd.join(" ");
        log::info!("Running: {cmd_str}");

        // Spawn a new child process with the given command, args, and env vars
        let mut child = Command::new(self.cmd[0])
            .args(&self.cmd[1..])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .envs(self.env.to_vec())
            .spawn()?;

        // Write the input to the child process's stdin
        child
            .stdin
            .as_mut()
            .ok_or(anyhow!("Failed to get stdin"))?
            .write_all(self.input.as_bytes())?;

        let output = child.wait_with_output()?;

        // If checking is enabled and the process failed, return an error
        if self.check && !output.status.success() {
            let stderr_str = String::from_utf8(output.stderr)?;
            return Err(anyhow!(stderr_str));
        }

        Ok(())
    }
}

fn notify(subject: &str, body: &str) -> anyhow::Result<()> {
    // Grab credentials
    let email_address = env::var("BACKUPER_EMAIL_ADDRESS")?;
    let email_password = env::var("BACKUPER_EMAIL_PASSWORD")?;

    // Build the email
    let email = Message::builder()
        .from(format!("Backup Script <{email_address}>").parse()?)
        .to(format!("Alex Ozer <{email_address}>").parse()?)
        .subject(subject)
        .header(ContentType::TEXT_PLAIN)
        .body(body.to_owned())?;

    let creds = Credentials::new(email_address, email_password);

    // Open a remote connection to gmail
    let mailer = SmtpTransport::relay("smtp.gmail.com")?
        .credentials(creds)
        .build();

    // Send the email
    mailer.send(&email)?;
    Ok(())
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

fn do_windows_upgrades() -> anyhow::Result<()> {
    sh(&["choco", "upgrade", "all"]).run()?;
    sh(&["wsl.exe", "sudo", "apt", "update"]).run()?;
    sh(&["wsl.exe", "sudo", "apt", "upgrade", "-y"]).run()?;
    sh(&["wsl.exe", "/home/linuxbrew/.linuxbrew/bin/brew", "upgrade"]).run()?;
    Ok(())
}

fn do_macos_upgrades() -> anyhow::Result<()> {
    sh(&["brew", "upgrade"]).run()
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
    let mut restic_args = vec!["restic", "backup", "--files-from", "-"];
    restic_args.extend(extra_restic_args);
    restic_args.extend(gen_exclude_flags(EXCLUDE_PATTERNS));

    let input = backup_dirs_to_strings(file_patterns)?.join("\n");
    let env = restic_config_to_env(config);
    sh(&restic_args).env(&env).input(&input).run()?;

    log::info!("Backed up local filesystem to {}", config.restic_repository);
    Ok(())
}

fn backup_wsl(config: &ResticConfig) -> anyhow::Result<()> {
    // In case I forgot to kill `restic mount`, don't try to backup the mountpoint... ugh
    sh(&["wsl.exe", "killall", "restic"]).check(false).run()?;

    // Securely pass environment variables to WSL (I think...)
    let mut wslenv = env::var("WSLENV").unwrap_or_default();
    wslenv.push(':');
    wslenv.push_str(
        &restic_config_to_env(config)
            .into_iter()
            .map(|(k, _)| k)
            .collect::<Vec<&str>>()
            .join(":"),
    );

    let mut env = restic_config_to_env(config);
    env.push(("WSLENV", &wslenv));

    // Call restic in WSL
    let mut args = vec![
        "wsl.exe",
        "--shell-type",
        "none", // We don't want bash/zsh to try expanding our exclude glob patterns
        "/home/linuxbrew/.linuxbrew/bin/restic",
        "backup",
        "/home/alex",
        "--tag",
        "WSL",
    ];
    args.extend(gen_exclude_flags(EXCLUDE_PATTERNS));

    sh(&args).env(&env).run()?;
    log::info!("Backed up WSL filesystem to {}", config.restic_repository);
    Ok(())
}

fn backup_windows_to(
    windows_config: &ResticConfig,
    wsl_config: &ResticConfig,
    errors: &mut Vec<String>,
) {
    try_task(
        "Backup Windows Filesystem (Local)",
        || {
            backup_filesystem_to(
                WINDOWS_BACKUP_DIRS,
                windows_config,
                &["--tag", "Windows", "--use-fs-snapshot"],
            )
        },
        errors,
    );
    try_task("Backup WSL (Local)", || backup_wsl(wsl_config), errors);
}

fn do_backup_windows(cloud_config: &ResticConfig, errors: &mut Vec<String>) {
    try_task("Windows Upgrades", do_windows_upgrades, errors);

    let windows_to_local_config = ResticConfig {
        restic_repository: "Z:\\restic".into(),
        restic_password: cloud_config.restic_password.clone(),
        aws_access_key_id: None,
        aws_secret_access_key: None,
    };
    let wsl_to_local_config = ResticConfig {
        restic_repository: "/mnt/c/restic".into(),
        restic_password: cloud_config.restic_password.clone(),
        aws_access_key_id: None,
        aws_secret_access_key: None,
    };

    backup_windows_to(&windows_to_local_config, &wsl_to_local_config, errors);
    backup_windows_to(cloud_config, cloud_config, errors);
}

fn do_backup_macos(cloud_config: &ResticConfig, errors: &mut Vec<String>) {
    try_task("macOS Upgrades", do_macos_upgrades, errors);
    try_task(
        "Backup macOS Filesystem",
        || backup_filesystem_to(MAC_BACKUP_DIRS, cloud_config, &["--tag", "macOS"]),
        errors,
    );
}

fn do_backup(is_windows: bool) -> Vec<String> {
    let any_to_cloud_config_func = || -> anyhow::Result<ResticConfig> {
        Ok(ResticConfig {
            restic_repository: env::var("BACKUPER_RESTIC_REPOSITORY")?,
            restic_password: env::var("BACKUPER_RESTIC_PASSWORD")?,
            aws_access_key_id: Some(env::var("BACKUPER_AWS_ACCESS_KEY_ID")?),
            aws_secret_access_key: Some(env::var("BACKUPER_AWS_SECRET_ACCESS_KEY")?),
        })
    };
    let cloud_config = match any_to_cloud_config_func() {
        Ok(conf) => conf,
        Err(e) => return vec![e.to_string()],
    };

    let mut errors = Vec::new();
    if is_windows {
        do_backup_windows(&cloud_config, &mut errors);
    } else {
        do_backup_macos(&cloud_config, &mut errors);
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

    let mut args_it = env::args();
    args_it.next();
    let Some(os) = args_it.next() else {
        return Err(anyhow!("No OS provided"));
    };
    if os != "windows" && os != "macos" {
        return Err(anyhow!("Invalid OS provided: {}", os));
    }
    let is_windows = os == "windows";

    let start = time::Instant::now();
    let errors = do_backup(is_windows);
    let dur = start.elapsed();

    let os_pretty = if is_windows { "Windows" } else { "macOS" };
    let dur_pretty = pretty_duration(dur);

    let subject: String;
    let body: String;
    if errors.is_empty() {
        subject = format!("Backup {os_pretty} succeeded");
        body = format!("Completed in {dur_pretty}\n\nHope you're having a nice day :)");
    } else {
        let error_word = if errors.len() == 1 { "error" } else { "errors" };
        let joined_errors = errors.join("\n");
        subject = format!("Backup {os_pretty} failed! {} {error_word}", errors.len());
        body = format!("Completed in {dur_pretty}\n\n{joined_errors}");
    }

    notify(&subject, &body)?;
    Ok(())
}
