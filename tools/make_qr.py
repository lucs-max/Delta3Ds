#!/usr/bin/env python3
"""
Makes a QR code for a .cia download link, to scan with FBI:
FBI -> Remote Install -> Scan QR Code.

    python3 tools/make_qr.py <url> [out.png]

Prints the QR code in the terminal and, if an output file is given, also
saves it as a PNG. Needs: pip install qrcode pillow
"""
import sys

try:
    import qrcode
except ImportError:
    sys.exit("This tool needs the qrcode package:  pip install qrcode pillow")


def make(url, out=None, show=True):
    qr = qrcode.QRCode(border=4, error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=8)
    qr.add_data(url)
    qr.make(fit=True)
    if show:
        # inverted so it scans on dark terminals too
        qr.print_ascii(invert=True)
    if out:
        qr.make_image(fill_color="black", back_color="white").save(out)
    return qr


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    url = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    make(url, out)
    print(url)
    if out:
        print("saved", out)


if __name__ == "__main__":
    main()
