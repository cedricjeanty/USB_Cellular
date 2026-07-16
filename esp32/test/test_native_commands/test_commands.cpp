#include <unity.h>
#include "hal/test_impls.h"
#include "hal/hal.h"
#include "airbridge_commands.h"
#include <cstring>
#include <string>

static MemFilesys s_fs;
static HAL        s_hal = { nullptr, nullptr, nullptr, &s_fs };
HAL* g_hal = nullptr;

void setUp(void)    { g_hal = &s_hal; s_fs.clear_all(); }
void tearDown(void) {}

// ── cmdParseLine ─────────────────────────────────────────────────────────────

void test_parse_blank_and_comment_ignored(void) {
    Command c;
    TEST_ASSERT_FALSE(cmdParseLine("", &c));
    TEST_ASSERT_FALSE(cmdParseLine("   ", &c));
    TEST_ASSERT_FALSE(cmdParseLine("# a comment", &c));
    TEST_ASSERT_FALSE(cmdParseLine("   ; also comment", &c));
}

void test_parse_persistent_verb(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("cdc", &c));
    TEST_ASSERT_EQUAL(CMD_CDC, c.type);
    TEST_ASSERT_FALSE(c.once);
    TEST_ASSERT_FALSE(c.runtimeSafe);  // cdc needs reboot
}

void test_parse_once_modifier(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("dump_logs once", &c));
    TEST_ASSERT_EQUAL(CMD_DUMP_LOGS, c.type);
    TEST_ASSERT_TRUE(c.once);
    TEST_ASSERT_TRUE(c.runtimeSafe);
}

void test_parse_verb_lowercased_and_whitespace(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("  \tDUMP_LOGS\tONCE  ", &c));
    TEST_ASSERT_EQUAL(CMD_DUMP_LOGS, c.type);
    TEST_ASSERT_TRUE(c.once);
}

void test_parse_dumplogs_alias(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("dumplogs", &c));
    TEST_ASSERT_EQUAL(CMD_DUMP_LOGS, c.type);
}

void test_parse_args_preserved(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("wifi MySSID secretpass", &c));
    TEST_ASSERT_EQUAL(CMD_WIFI, c.type);
    TEST_ASSERT_FALSE(c.once);
    TEST_ASSERT_EQUAL_STRING("MySSID secretpass", c.args);
}

void test_parse_args_with_once(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("s3 once api.example.com KEY123", &c));
    TEST_ASSERT_EQUAL(CMD_S3, c.type);
    TEST_ASSERT_TRUE(c.once);
    TEST_ASSERT_EQUAL_STRING("api.example.com KEY123", c.args);
}

void test_parse_survey(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("survey", &c));
    TEST_ASSERT_EQUAL(CMD_SURVEY, c.type);
    TEST_ASSERT_FALSE(c.runtimeSafe);  // changes the whole boot duty cycle → boot-only
}

void test_parse_compress(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("compress", &c));
    TEST_ASSERT_EQUAL(CMD_COMPRESS, c.type);
    TEST_ASSERT_TRUE(c.runtimeSafe);   // takes effect at the next harvest, no reboot
    TEST_ASSERT_TRUE(cmdParseLine("compress off", &c));
    TEST_ASSERT_EQUAL(CMD_COMPRESS, c.type);
    TEST_ASSERT_EQUAL_STRING("off", c.args);
}

void test_parse_represent(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("represent", &c));
    TEST_ASSERT_EQUAL(CMD_REPRESENT, c.type);
    TEST_ASSERT_TRUE(c.runtimeSafe);   // takes effect at the next harvest, no reboot
    TEST_ASSERT_TRUE(cmdParseLine("represent off", &c));
    TEST_ASSERT_EQUAL(CMD_REPRESENT, c.type);
    TEST_ASSERT_EQUAL_STRING("off", c.args);
    // Executor parity with `compress`: bare = on, "off" = off. Default is ON.
    TEST_ASSERT_TRUE(g_represent);
    CmdRunResult r = runCommandTextBuffer("represent off\n", false, "", "", "", nullptr, 0, false);
    TEST_ASSERT_TRUE(r.ran);
    TEST_ASSERT_FALSE(g_represent);
    r = runCommandTextBuffer("represent on\n", false, "", "", "", nullptr, 0, false);
    TEST_ASSERT_TRUE(r.ran);
    TEST_ASSERT_TRUE(g_represent);
}

void test_parse_modem_reset(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("modem_reset", &c));
    TEST_ASSERT_EQUAL(CMD_MODEM_RESET, c.type);
    TEST_ASSERT_TRUE(c.runtimeSafe);   // no device reboot; modem reboots + reconnects
    TEST_ASSERT_TRUE(cmdParseLine("modemreset", &c));  // alias
    TEST_ASSERT_EQUAL(CMD_MODEM_RESET, c.type);
    // Executor sets cr.modemReset
    CmdRunResult r = runCommandTextBuffer("modem_reset\n", false, "", "", "", nullptr, 0, false);
    TEST_ASSERT_TRUE(r.ran);
    TEST_ASSERT_TRUE(r.modemReset);
}

void test_parse_flash(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("flash", &c));
    TEST_ASSERT_EQUAL(CMD_FLASH, c.type);
    TEST_ASSERT_TRUE(c.runtimeSafe);   // downloads + reboots via SD-flash, no boot gate
    // Executor sets cr.flash (firmware then downloads firmware->P2 + reboots to SD-flash)
    CmdRunResult r = runCommandTextBuffer("flash\n", false, "", "", "", nullptr, 0, false);
    TEST_ASSERT_TRUE(r.ran);
    TEST_ASSERT_TRUE(r.flash);
}

void test_parse_unknown_verb(void) {
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("frobnicate now", &c));
    TEST_ASSERT_EQUAL(CMD_UNKNOWN, c.type);
}

void test_parse_once_not_mistaken_in_args(void) {
    // "once" only counts immediately after the verb; a later token named "once"
    // (e.g. an SSID) is just an arg.
    Command c;
    TEST_ASSERT_TRUE(cmdParseLine("wifi once_network pw", &c));
    TEST_ASSERT_FALSE(c.once);
    TEST_ASSERT_EQUAL_STRING("once_network pw", c.args);
}

// ── parseCommands ────────────────────────────────────────────────────────────

void test_parse_multiline(void) {
    const char* text =
        "# header comment\n"
        "cdc\n"
        "\n"
        "dump_logs once\n"
        "wifi net pass\n";
    Command cmds[8];
    int n = parseCommands(text, cmds, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL(CMD_CDC, cmds[0].type);
    TEST_ASSERT_EQUAL(CMD_DUMP_LOGS, cmds[1].type);
    TEST_ASSERT_TRUE(cmds[1].once);
    TEST_ASSERT_EQUAL(CMD_WIFI, cmds[2].type);
}

void test_parse_no_trailing_newline(void) {
    Command cmds[4];
    int n = parseCommands("reboot once", cmds, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(CMD_REBOOT, cmds[0].type);
    TEST_ASSERT_TRUE(cmds[0].once);
}

// ── emitPersistent ───────────────────────────────────────────────────────────

void test_emit_boot_strips_once_keeps_persistent(void) {
    const char* text =
        "# keep me\n"
        "cdc\n"
        "dump_logs once\n";
    char out[512];
    bool any = emitPersistent(text, /*runtimeOnly=*/false, out, sizeof(out));
    TEST_ASSERT_TRUE(any);                              // cdc remains
    TEST_ASSERT_NOT_NULL(strstr(out, "# keep me"));     // comment preserved
    TEST_ASSERT_NOT_NULL(strstr(out, "cdc"));           // persistent kept
    TEST_ASSERT_NULL(strstr(out, "dump_logs"));         // once stripped
}

void test_emit_harvest_keeps_bootonly_once(void) {
    // At harvest, a runtime-safe once (dump_logs) is consumed, but a boot-only
    // once (cdc once) is NOT executed, so it must be preserved for next boot.
    const char* text =
        "cdc once\n"
        "dump_logs once\n";
    char out[512];
    bool any = emitPersistent(text, /*runtimeOnly=*/true, out, sizeof(out));
    TEST_ASSERT_TRUE(any);                          // cdc once survives
    TEST_ASSERT_NOT_NULL(strstr(out, "cdc once"));
    TEST_ASSERT_NULL(strstr(out, "dump_logs"));     // consumed at harvest
}

void test_emit_delete_when_empty(void) {
    char out[512];
    bool any = emitPersistent("dump_logs once\n", /*runtimeOnly=*/false, out, sizeof(out));
    TEST_ASSERT_FALSE(any);   // nothing left -> caller deletes the file
}

void test_emit_commentsonly_signals_delete(void) {
    char out[512];
    bool any = emitPersistent("# just a note\n", /*runtimeOnly=*/false, out, sizeof(out));
    TEST_ASSERT_FALSE(any);   // no directives remain
}

// ── dumpLogs ─────────────────────────────────────────────────────────────────

void test_dumplogs_copies_logs_and_backlog(void) {
    s_fs.add_dir("/sdcard");
    s_fs.add_dir("/sdcard/logs");
    s_fs.add_file_str("/sdcard/logs/boot_0168.log", "log168");
    s_fs.add_file_str("/sdcard/logs/boot_0169.log", "log169");
    s_fs.add_file_str("/sdcard/logs/notalog.txt", "ignore me");   // not .log
    s_fs.add_dir("/sdcard/upload");
    s_fs.add_dir("/sdcard/upload/0007");
    s_fs.add_file_str("/sdcard/upload/0007/boot_0150.log", "backlog150");
    s_fs.add_dir("/dsu");

    int n = dumpLogs("/sdcard/logs", "/sdcard/upload", "/dsu/diag");
    TEST_ASSERT_EQUAL_INT(3, n);

    // logs copied verbatim
    TEST_ASSERT_TRUE(s_fs.has_file("/dsu/diag/boot_0168.log"));
    TEST_ASSERT_EQUAL_STRING("log168", s_fs.get_content("/dsu/diag/boot_0168.log").c_str());
    TEST_ASSERT_TRUE(s_fs.has_file("/dsu/diag/boot_0169.log"));
    // backlog copied with up_<NNNN>_ prefix
    TEST_ASSERT_TRUE(s_fs.has_file("/dsu/diag/up_0007_boot_0150.log"));
    TEST_ASSERT_EQUAL_STRING("backlog150",
        s_fs.get_content("/dsu/diag/up_0007_boot_0150.log").c_str());
    // non-.log skipped
    TEST_ASSERT_FALSE(s_fs.has_file("/dsu/diag/notalog.txt"));
    // sources intact (non-destructive)
    TEST_ASSERT_TRUE(s_fs.has_file("/sdcard/logs/boot_0168.log"));
    TEST_ASSERT_TRUE(s_fs.has_file("/sdcard/upload/0007/boot_0150.log"));
}

void test_dumplogs_empty_dirs(void) {
    s_fs.add_dir("/sdcard/logs");
    s_fs.add_dir("/sdcard/upload");
    int n = dumpLogs("/sdcard/logs", "/sdcard/upload", "/dsu/diag");
    TEST_ASSERT_EQUAL_INT(0, n);
}

// ── file I/O round-trip ──────────────────────────────────────────────────────

void test_read_write_roundtrip(void) {
    const char* path = "/dsu/airbridge.cmd";
    char buf[256];
    TEST_ASSERT_FALSE(cmdReadFile(path, buf, sizeof(buf)));  // absent
    TEST_ASSERT_TRUE(cmdWriteFile(path, "cdc\ndump_logs once\n"));
    TEST_ASSERT_TRUE(cmdReadFile(path, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("cdc\ndump_logs once\n", buf);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_blank_and_comment_ignored);
    RUN_TEST(test_parse_persistent_verb);
    RUN_TEST(test_parse_once_modifier);
    RUN_TEST(test_parse_verb_lowercased_and_whitespace);
    RUN_TEST(test_parse_dumplogs_alias);
    RUN_TEST(test_parse_args_preserved);
    RUN_TEST(test_parse_args_with_once);
    RUN_TEST(test_parse_survey);
    RUN_TEST(test_parse_compress);
    RUN_TEST(test_parse_represent);
    RUN_TEST(test_parse_modem_reset);
    RUN_TEST(test_parse_flash);
    RUN_TEST(test_parse_unknown_verb);
    RUN_TEST(test_parse_once_not_mistaken_in_args);
    RUN_TEST(test_parse_multiline);
    RUN_TEST(test_parse_no_trailing_newline);
    RUN_TEST(test_emit_boot_strips_once_keeps_persistent);
    RUN_TEST(test_emit_harvest_keeps_bootonly_once);
    RUN_TEST(test_emit_delete_when_empty);
    RUN_TEST(test_emit_commentsonly_signals_delete);
    RUN_TEST(test_dumplogs_copies_logs_and_backlog);
    RUN_TEST(test_dumplogs_empty_dirs);
    RUN_TEST(test_read_write_roundtrip);
    return UNITY_END();
}
