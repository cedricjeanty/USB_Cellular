# Debugging Tips

## ESP-IDF vs Arduino Differences

| Area | Arduino | ESP-IDF |
|------|---------|---------|
| Logging | `Serial.println()` → USB CDC | `ESP_LOGI()` → UART (invisible on CDC!) |
| CDC output | Use `Serial` directly | Use `cdc_printf()` for visible output |
| WiFi | Polling (`WiFi.status()`) | Event-driven (`esp_event` + EventGroup) |
| TLS | `WiFiClientSecure::setInsecure()` | `esp_tls` + cert bundle or `CONFIG_ESP_TLS_INSECURE` |
| SD card | SdFat (both raw + FS) | `sdmmc` (raw) + FATFS VFS (filesystem) |
| TCP buffers | Pre-compiled at 5760 (unchangeable) | Configurable via sdkconfig (32768) |

## Common Issues

### "TLS connect failed" on ESP-IDF
- Check `CONFIG_ESP_TLS_INSECURE=y` AND `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` in sdkconfig
- Or use `cfg.crt_bundle_attach = esp_crt_bundle_attach` for proper cert validation
- `MBEDTLS_SSL_IN_CONTENT_LEN` must be ≥16384 (S3 cert chain exceeds 4KB)

### FATFS mount fails (FR_NO_FILESYSTEM = 13)
- `CONFIG_FATFS_SECTOR_512=y` required (default 4096 breaks MBR parsing)
- ESP-IDF FATFS has `FF_MULTI_PARTITION=1` with `const VolToPart[]` — can't modify at runtime
- Arduino SdFat formats with partition at sector 8192; ESP-IDF FATFS at sector 63 — incompatible
- Delete `sdkconfig.esp32s3` and clean rebuild after changing sdkconfig.defaults

### Files show 0 bytes after harvest remount
- Linux MSC writes must use `oflag=sync` or equivalent to commit directory entries
- Without sync, the FAT directory entry (containing file size) may not be flushed to card
- `sync && sleep 2 && umount` helps but isn't guaranteed

### Soft reset (REBOOT) breaks SPI
- `esp_restart()` doesn't reset SPI peripheral state
- Next boot: `spi_bus_initialize()` returns `ESP_ERR_INVALID_STATE`
- `esp_vfs_fat_sdspi_mount()` fails because SPI device is already registered
- **Fix:** power cycle instead of soft reset

### Upload output not visible on serial
- ESP-IDF `ESP_LOGI()` goes to UART, not TinyUSB CDC
- Use `cdc_printf()` for messages that need to appear on the serial console
- During heavy TLS transfers, CDC output may be lost (USB task starved for CPU)
- Use `g_lastUploadKBps` in STATUS output to see speed after upload completes

### Device crash-loops after flash
- Check `CONFIG_SPIRAM=n` (PSRAM init fails on this board variant)
- Check static buffer sizes: 16KB+ buffers on 16KB task stacks cause overflow
- Use `static` keyword for large local buffers

### Harvest finds 0 files
- Check `g_hostWrittenMb > 0.01` — less than 10KB of writes won't trigger harvest
- Check `g_harvestCoolMs` — empty harvest sets 5-minute cooldown
- After Linux writes via MSC, FATFS remount should see new files (if sync'd properly)
- Use STATUS to check: `wr_det`, `wr_mb`, `host_was`, `cool`

## Useful STATUS fields (ESP-IDF branch)

```
CLI: wifi=192.168.0.242 ap=no files_q=1 files_up=0 mbq=4.19 mbup=0.00
     sd=ok fatfs=ok sectors=31116288
CLI: last_upload=279 KB/s
CLI: wr_det=1 wr_mb=4.20 host_was=1 last_wr=159720 cool=30000
```

- `sd`/`fatfs` — SD card and filesystem mount status
- `last_upload` — speed of most recent upload (KB/s)
- `wr_det` — USB host write detected (triggers harvest)
- `wr_mb` — MB written by USB host since last harvest
- `cool` — harvest cooldown (30000=30s normal, 300000=5min after empty harvest)
