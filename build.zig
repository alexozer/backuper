const std = @import("std");

const backuper_sources: []const []const u8 = &.{
    "src/main.cpp",
    "src/base.cpp",
    "src/platform_macos.cpp",
    "src/platform_posix.cpp",
};

const backuper_flags: []const []const u8 = &.{
    "-std=c++20",
    "-fno-exceptions",
    "-fno-rtti",
    "-Wall",
    "-Wshadow",
};

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const backuper = b.addExecutable(.{
        .name = "backuper",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .strip = true,
            .link_libc = true,
            .link_libcpp = false,
        }),
    });
    backuper.root_module.addCSourceFiles(.{ .files = backuper_sources, .flags = backuper_flags });

    b.installArtifact(backuper);
    const run_backuper = b.addRunArtifact(backuper);
    const run_step = b.step("run", "Run the application");
    run_step.dependOn(&run_backuper.step);
}
