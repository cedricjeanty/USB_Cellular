#pragma once
// AirBridge unified command file — parse + persist-rewrite + dump_logs helper.
//
// One text file (COMMAND_FILE_NAME) on the SD card holds boot/runtime directives,
// one per line, e.g.:
//
//     # comment, blank lines ignored
//     cdc                 <- persistent: re-applied every boot (file left intact)
//     dump_logs once      <- one-shot: runs once, then stripped from the file
//
// Each directive PERSISTS by default; the optional "once" modifier (immediately
// after the verb) makes it one-shot — after it runs, the firmware rewrites the
// file with that line removed. This replaces the proliferation of single-purpose
// magic files (ENABLE_CDC / CDC_PERSIST / DUMP_LOGS / REBOOT / FORMAT_SD / ...).
//
// The file is processed at boot AND during the 15s-quiet harvest cycle, so
// "runtime-safe" directives (dump_logs/wifi/s3/reboot) take effect without a
// reboot. Directives that need USB re-enumeration (cdc) or are destructive
// (format_sd) only apply at boot.
//
// Hardware-independent: compiles into firmware, emulator, and native tests. All
// I/O goes through g_hal->filesys.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <strings.h>

#define COMMAND_FILE_NAME "airbridge.cmd"

enum CmdType {
    CMD_UNKNOWN = 0,
    CMD_CDC,         // boot CDC+MSC (g_msc_only=false)         — boot-only
    CMD_DUMP_LOGS,   // copy P2 logs -> P1 /diag                — runtime-safe
    CMD_REBOOT,      // reboot                                  — runtime-safe
    CMD_FORMAT_SD,   // set NVS format flag + reboot            — boot-only (destructive)
    CMD_WIFI,        // args: <ssid> <pass>                     — runtime-safe (NVS)
    CMD_S3,          // args: <api_host> <api_key>              — runtime-safe (NVS)
};

struct Command {
    CmdType type;
    bool    once;          // consumed (stripped) after a single execution
    bool    runtimeSafe;   // may run during harvest without a reboot
    char    verb[16];
    char    args[128];     // remainder of the line after verb (+ optional "once")
};

// Map a lowercased verb to its type + runtime-safety.
inline CmdType cmdVerbType(const char* verb, bool* runtimeSafe) {
    struct { const char* v; CmdType t; bool rt; } table[] = {
        { "cdc",       CMD_CDC,       false },
        { "dump_logs", CMD_DUMP_LOGS, true  },
        { "dumplogs",  CMD_DUMP_LOGS, true  },
        { "reboot",    CMD_REBOOT,    true  },
        { "format_sd", CMD_FORMAT_SD, false },
        { "wifi",      CMD_WIFI,      true  },
        { "s3",        CMD_S3,        true  },
    };
    for (auto& e : table) {
        if (strcmp(verb, e.v) == 0) { if (runtimeSafe) *runtimeSafe = e.rt; return e.t; }
    }
    if (runtimeSafe) *runtimeSafe = false;
    return CMD_UNKNOWN;
}

// Parse a single line into a Command. Returns false for blank lines and comments
// ('#' or ';'). Leading whitespace and trailing CR/LF are ignored. The verb is
// lowercased; an "once" token immediately after the verb sets c->once.
inline bool cmdParseLine(const char* line, Command* c) {
    *c = Command{};
    char buf[256];
    strlcpy(buf, line, sizeof(buf));
    // Strip trailing CR/LF/whitespace.
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\r' || buf[n-1] == '\n' ||
                     buf[n-1] == ' '  || buf[n-1] == '\t')) buf[--n] = '\0';
    // Skip leading whitespace.
    char* p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '#' || *p == ';') return false;  // blank or comment

    // Verb = first token (lowercased).
    char* v = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
    strlcpy(c->verb, v, sizeof(c->verb));
    for (char* q = c->verb; *q; q++) *q = (char)tolower((unsigned char)*q);

    // Skip whitespace before the next token.
    while (*p == ' ' || *p == '\t') p++;

    // Optional "once" modifier immediately after the verb.
    if (strncasecmp(p, "once", 4) == 0 && (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
        c->once = true;
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
    }

    strlcpy(c->args, p, sizeof(c->args));
    c->type = cmdVerbType(c->verb, &c->runtimeSafe);
    return true;
}

// Parse a whole command-file text into up to `max` Commands. Returns the count.
inline int parseCommands(const char* text, Command* out, int max) {
    if (!text) return 0;
    int count = 0;
    const char* p = text;
    while (*p && count < max) {
        const char* eol = strchr(p, '\n');
        char line[256];
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if (cmdParseLine(line, &out[count])) count++;
        if (!eol) break;
        p = eol + 1;
    }
    return count;
}

// Was a directive on this line actually executed in this context?
// At boot (runtimeOnly=false) every directive runs; at harvest (runtimeOnly=true)
// only runtime-safe ones run. Only executed `once` directives get stripped.
inline bool cmdExecuted(const Command& c, bool runtimeOnly) {
    if (c.type == CMD_UNKNOWN) return false;
    return !runtimeOnly || c.runtimeSafe;
}

// Rewrite the file text keeping everything EXCEPT `once` directives that were
// executed in this context. Comments, blank lines, persistent directives, and
// not-yet-executed `once` directives (e.g. a boot-only `cdc once` seen at
// harvest) are preserved verbatim. Writes the result to `out`.
// Returns true if any directive line remains (caller keeps the file); false if
// only comments/blanks remain (caller may delete the file).
inline bool emitPersistent(const char* text, bool runtimeOnly, char* out, size_t outSz) {
    out[0] = '\0';
    size_t used = 0;
    bool anyDirective = false;
    if (!text) return false;
    const char* p = text;
    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        size_t cl = (len >= sizeof(line)) ? sizeof(line) - 1 : len;
        memcpy(line, p, cl);
        line[cl] = '\0';

        Command c;
        bool isDirective = cmdParseLine(line, &c);
        bool drop = isDirective && c.once && cmdExecuted(c, runtimeOnly);
        if (!drop) {
            // Append the original line (plus its newline) verbatim.
            if (used + len + 1 < outSz) {
                memcpy(out + used, p, len);
                used += len;
                out[used++] = '\n';
                out[used] = '\0';
            }
            if (isDirective) anyDirective = true;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return anyDirective;
}

// ── dump_logs: copy stored logs onto the USB-visible partition ───────────────

// Copy one file via the HAL filesys (4096-byte loop, same as harvestFiles).
inline bool cmdCopyFile(const char* src, const char* dst) {
    void* sf = g_hal->filesys->open(src, "rb");
    if (!sf) return false;
    void* df = g_hal->filesys->open(dst, "wb");
    if (!df) { g_hal->filesys->close(sf); return false; }
    uint8_t cpbuf[4096];
    bool ok = true;
    for (;;) {
        size_t r = g_hal->filesys->read(sf, cpbuf, sizeof(cpbuf));
        if (r == 0) break;
        if (g_hal->filesys->write(df, cpbuf, r) != r) { ok = false; break; }
    }
    g_hal->filesys->close(sf);
    g_hal->filesys->close(df);
    return ok;
}

inline bool cmdEndsWithLog(const char* name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".log") == 0;
}

// Copy every *.log from logsDir, and every *.log from uploadDir/NNNN/ (the
// stranded upload backlog, name-flattened to up_<NNNN>_<name>), into destDir.
// Sources are left intact (non-destructive). Returns the number of files copied.
inline int dumpLogs(const char* logsDir, const char* uploadDir, const char* destDir) {
    if (!g_hal || !g_hal->filesys) return 0;
    g_hal->filesys->mkdir(destDir);
    int copied = 0;
    char src[416], dst[416];

    // 1) logsDir/*.log
    void* d = g_hal->filesys->opendir(logsDir);
    if (d) {
        FsDirEntry e;
        while (g_hal->filesys->readdir(d, &e)) {
            if (e.name[0] == '.' || e.is_dir || !cmdEndsWithLog(e.name)) continue;
            snprintf(src, sizeof(src), "%s/%s", logsDir, e.name);
            snprintf(dst, sizeof(dst), "%s/%s", destDir, e.name);
            if (cmdCopyFile(src, dst)) copied++;
        }
        g_hal->filesys->closedir(d);
    }

    // 2) uploadDir/NNNN/*.log  ->  destDir/up_<NNNN>_<name>
    void* ud = g_hal->filesys->opendir(uploadDir);
    if (ud) {
        FsDirEntry sub;
        while (g_hal->filesys->readdir(ud, &sub)) {
            if (sub.name[0] == '.' || !sub.is_dir) continue;
            char subPath[200];
            snprintf(subPath, sizeof(subPath), "%s/%s", uploadDir, sub.name);
            void* sd = g_hal->filesys->opendir(subPath);
            if (!sd) continue;
            FsDirEntry e;
            while (g_hal->filesys->readdir(sd, &e)) {
                if (e.name[0] == '.' || e.is_dir || !cmdEndsWithLog(e.name)) continue;
                snprintf(src, sizeof(src), "%s/%s", subPath, e.name);
                snprintf(dst, sizeof(dst), "%s/up_%s_%s", destDir, sub.name, e.name);
                if (cmdCopyFile(src, dst)) copied++;
            }
            g_hal->filesys->closedir(sd);
        }
        g_hal->filesys->closedir(ud);
    }
    return copied;
}

// ── command-file text I/O (HAL filesys) ──────────────────────────────────────

// Read the command file into `buf` (null-terminated). Returns false if absent.
inline bool cmdReadFile(const char* path, char* buf, size_t bufSz) {
    if (!g_hal || !g_hal->filesys) return false;
    void* f = g_hal->filesys->open(path, "rb");
    if (!f) return false;
    size_t n = g_hal->filesys->read(f, buf, bufSz - 1);
    g_hal->filesys->close(f);
    buf[n < bufSz ? n : bufSz - 1] = '\0';
    return true;
}

inline bool cmdWriteFile(const char* path, const char* text) {
    if (!g_hal || !g_hal->filesys) return false;
    void* f = g_hal->filesys->open(path, "wb");
    if (!f) return false;
    size_t len = strlen(text);
    bool ok = (len == 0) || (g_hal->filesys->write(f, text, len) == len);
    g_hal->filesys->close(f);
    return ok;
}
