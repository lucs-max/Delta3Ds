#!/usr/bin/env python3
"""
Installs Delta3DS on your 3DS straight from your PC over Wi-Fi.

Serves the .cia files in a folder over HTTP on your local network and shows
a QR code for each one. On the 3DS: FBI -> Remote Install -> Scan QR Code,
then point the camera at the screen. The PC and the 3DS must be on the same
network.

    python3 tools/serve_qr.py [folder with .cia files] [--port 8080]

Needs: pip install qrcode pillow
"""
import argparse
import functools
import http.server
import os
import socket
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_qr import make  # noqa: E402


def local_ip():
    """IP address of this PC on the local network."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # no packet is sent; picks the LAN interface
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="Serve .cia files to FBI with QR codes")
    ap.add_argument("folder", nargs="?", default=".", help="folder containing the .cia files")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    folder = os.path.abspath(args.folder)
    cias = sorted(f for f in os.listdir(folder) if f.lower().endswith(".cia"))
    if not cias:
        sys.exit("No .cia files in %s (build them with 'make cia' or download them from the Releases page)" % folder)

    ip = local_ip()
    for name in cias:
        url = "http://%s:%d/%s" % (ip, args.port, urllib.parse.quote(name))
        label = "New 3DS / New 2DS XL" if "old" not in name.lower() else "Old 3DS / 2DS"
        print("\n%s  (%s)" % (name, label))
        make(url, os.path.join(folder, "qr-" + os.path.splitext(name)[0] + ".png"))
        print(url)

    print("\nOn the 3DS: FBI -> Remote Install -> Scan QR Code.")
    print("Serving %s on port %d. Press Ctrl+C when the install has finished." % (folder, args.port))
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=folder)
    with http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")


if __name__ == "__main__":
    main()
