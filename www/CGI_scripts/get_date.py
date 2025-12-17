#!/usr/bin/env python3
from datetime import datetime, timezone
import os

def main():
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S %Z")
    if os.getenv("GATEWAY_INTERFACE"):
        print("Content-Type: text/plain\n")
    print("\r\n\r\noutput form /internal " + now)

if __name__ == "__main__":
    main()