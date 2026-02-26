#!/usr/bin/env python3
"""
Grab a free 1-hour FTP server from sftpcloud.io and push the credentials
to the Pi's config.yaml, then restart the airbridge service.

Usage:
    python3 scripts/get_ftp_creds.py [--pi-host cedric@pizerologs.local] [--dry-run]
"""

import argparse
import subprocess
import sys
import time
import yaml
from pathlib import Path
from selenium import webdriver
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC

URL = "https://sftpcloud.io/tools/free-ftp-server"
CONFIG_PATH = Path(__file__).parent.parent / "config.yaml"


def get_credentials(headless=True, timeout=30) -> dict:
    """Open the sftpcloud free FTP page, click Create, parse body text for creds."""
    opts = Options()
    if headless:
        opts.add_argument("--headless=new")
    opts.add_argument("--no-sandbox")
    opts.add_argument("--disable-dev-shm-usage")
    opts.add_argument("--disable-gpu")
    opts.add_argument("--window-size=1280,800")

    print(f"Opening {URL} ...")
    driver = webdriver.Chrome(options=opts)
    try:
        driver.get(URL)

        # Click "Create test FTP server"
        wait = WebDriverWait(driver, timeout)
        btn = wait.until(EC.element_to_be_clickable(
            (By.XPATH, "//button[contains(., 'Create')]")
        ))
        print(f"Clicking '{btn.text.strip()}'...")
        btn.click()

        # Wait until the body text contains the username (non-empty hex string after "Username")
        print("Waiting for credentials...")
        def creds_visible(d):
            text = d.find_element(By.TAG_NAME, "body").text
            return "Username\n" in text and "Password\n" in text

        wait.until(creds_visible)

        body_text = driver.find_element(By.TAG_NAME, "body").text
        return _parse_body_text(body_text)

    finally:
        driver.quit()


def _parse_body_text(text: str) -> dict:
    """
    Parse credentials from page body text.

    The page renders labeled values as:
        Host
        eu-central-1.sftpcloud.io
        Username
        a6984a53fae14874b6f7cacd3e680c87
        Password
        aW7L0MrZ7QdyK3rQPR6VTV5L0aaeZew3
        FTP Port
        21
    """
    creds = {}
    lines = text.splitlines()
    label_map = {
        "Host": "host",
        "Server": "host",
        "Username": "username",
        "User": "username",
        "Password": "password",
        "FTP Port": "port",
        "Port": "port",
    }
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped in label_map and i + 1 < len(lines):
            value = lines[i + 1].strip()
            if value:
                creds[label_map[stripped]] = value

    return creds


def update_config(creds: dict, dry_run=False) -> dict:
    """Update config.yaml with new FTP credentials. Returns updated ftp section."""
    if not CONFIG_PATH.exists():
        print(f"ERROR: config.yaml not found at {CONFIG_PATH}")
        sys.exit(1)

    with open(CONFIG_PATH) as f:
        config = yaml.safe_load(f)

    ftp = config.setdefault("ftp", {})

    if "host" in creds:
        ftp["server"] = creds["host"]
    if "username" in creds:
        ftp["username"] = creds["username"]
    if "password" in creds:
        ftp["password"] = creds["password"]
    if "port" in creds:
        try:
            ftp["port"] = int(creds["port"])
        except ValueError:
            pass

    print("\nUpdated FTP config:")
    print(f"  server:   {ftp.get('server')}")
    print(f"  port:     {ftp.get('port')}")
    print(f"  username: {ftp.get('username')}")
    print(f"  password: {ftp.get('password')}")

    if not dry_run:
        with open(CONFIG_PATH, "w") as f:
            yaml.dump(config, f, default_flow_style=False, allow_unicode=True)
        print(f"\nSaved to {CONFIG_PATH}")

    return ftp


def deploy_and_restart(pi_host: str):
    """scp config.yaml to Pi and restart the airbridge service."""
    print(f"\nDeploying config.yaml to {pi_host}...")
    r = subprocess.run(["scp", str(CONFIG_PATH), f"{pi_host}:~/config.yaml"], check=False)
    if r.returncode != 0:
        print("WARNING: scp failed — check SSH access")
        return

    print("Restarting airbridge service...")
    r = subprocess.run(["ssh", pi_host, "sudo systemctl restart airbridge"], check=False)
    if r.returncode != 0:
        print("WARNING: service restart failed")
        return

    time.sleep(3)
    r = subprocess.run(
        ["ssh", pi_host, "sudo journalctl -u airbridge --no-pager -n 5"],
        capture_output=True, text=True
    )
    print("\nLatest service logs:")
    print(r.stdout.strip())


def main():
    parser = argparse.ArgumentParser(description="Grab sftpcloud.io free FTP credentials")
    parser.add_argument("--pi-host", default="cedric@pizerologs.local",
                        help="SSH target for deployment (default: cedric@pizerologs.local)")
    parser.add_argument("--no-deploy", action="store_true",
                        help="Update config.yaml locally but don't push to Pi")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print what would happen but don't write anything")
    parser.add_argument("--show-browser", action="store_true",
                        help="Show browser window (disable headless mode)")
    args = parser.parse_args()

    creds = get_credentials(headless=not args.show_browser)

    if not creds.get("username") or not creds.get("password"):
        print(f"\nERROR: Could not extract credentials. Got: {creds}")
        print("Try --show-browser to see what the page looks like.")
        sys.exit(1)

    print(f"\nCredentials: {creds}")
    ftp = update_config(creds, dry_run=args.dry_run)

    if args.dry_run:
        print("\n[dry-run] No changes written.")
        return

    if not args.no_deploy:
        if ftp.get("username") and ftp.get("password"):
            deploy_and_restart(args.pi_host)
        else:
            print("\nWARNING: credentials incomplete, skipping deployment")
    else:
        print(f"\nconfig.yaml updated locally. Deploy manually with:")
        print(f"  scp config.yaml {args.pi_host}:~/config.yaml")
        print(f"  ssh {args.pi_host} sudo systemctl restart airbridge")


if __name__ == "__main__":
    main()
