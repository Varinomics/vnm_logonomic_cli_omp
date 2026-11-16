#include "omp_launcher_approval.h"

namespace omp {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    // Native sources: cli/args.ts, cli/flag-tables.ts and tools/approval.ts.
    // Unyolofy removes invocation bypasses; saved tools.approvalMode remains
    // native policy, whose default is itself yolo. It never selects a new mode.
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {"omp", "omp.exe", "omp.cmd", "omp.ps1"},
        .canonical_argument = "--yolo",
        .bypass_arguments = {"--yolo", "--auto-approve"},
        .approval_modes = {
            {"--approval-mode", {"yolo"}, {"always-ask", "always-ask-cached", "write"}},
        },
        .conflicting_arguments = {},
        .value_arguments = {
            "--profile", "--cwd", "--config", "--add-dir", "--mode", "--fork", "--provider", "--model",
            "--smol", "--slow", "--goal", "--prewalk-into", "--plan-yolo-into", "--max-time", "--warn-cost",
            "--max-cost", "--service-tier", "--spinner-interval", "--max-fps", "--motion", "--api-key",
            "--system-prompt", "--system-prompt-template", "--append-system-prompt", "--provider-session-id",
            "--prompt-cache-key", "--session-dir", "--models", "--tools", "--thinking", "--plugin-dir", "--skills",
        },
        .optional_value_arguments = {"--resume", "-r", "--restore", "--session"},
        .flag_arguments = {
            "--allow-home", "--continue", "-c", "--from-claude", "--from-codex", "--no-session", "--no-tools",
            "--no-lsp", "--no-pty", "--no-judge", "--no-spotlight", "--offline", "--hide-thinking", "--advisor",
            "--wait-async", "--external-thinking", "--prewalk", "--no-prewalk", "--plan-yolo", "--print", "-p",
            "--print-thoughts", "--no-extensions", "--no-skills", "--no-rules", "--no-context-files", "--no-title",
            "--no-ui",
        },
        .session_commands = {"launch"},
        .unsupported_commands = {
            "acp", "auth", "auth-broker", "auth-gateway", "agents", "anonymize", "bench", "browser-relay",
            "cleanse", "collab", "commit", "completions", "__complete", "compress", "config", "dry-balance",
            "find", "gc", "grep", "gallery", "git", "grievances", "images", "img", "if-bench", "install",
            "join", "login", "models", "plugin", "plugins", "predict", "ps", "say", "clip", "play", "share",
            "session", "setup", "shell", "zsh", "bash", "sh", "read", "render", "skill", "skills", "ssh",
            "stats", "stream", "update", "usage", "tiny-models", "token", "toks", "ttsr", "worktree", "wt",
            "search", "q", "web-search",
        },
    };
    return rules;
}

} // namespace omp
