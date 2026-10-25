#!/usr/bin/env python3
"""Writes a tiny SD card tree for the compas-sdtool tests: tagged MP3 and WAV
files with embedded and folder cover art. Everything is generated here, so no
real music is needed. Usage: make_fixture.py <card-root>"""
import os
import struct
import sys
import zlib


def png(size, rgb):
    """A solid-colour PNG."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    row = b"\x00" + bytes(rgb) * size
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(row * size)) + chunk(b"IEND", b""))


def synchsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def id3_frame(frame_id, payload):
    return frame_id.encode() + struct.pack(">I", len(payload)) + b"\x00\x00" + payload


def text_frame(frame_id, text):
    return id3_frame(frame_id, b"\x03" + text.encode("utf-8"))


def mp3(title, artist, album, album_artist, track, year, cover=None):
    frames = [text_frame("TIT2", title), text_frame("TPE1", artist), text_frame("TALB", album),
              text_frame("TPE2", album_artist), text_frame("TCON", "Test"), text_frame("TRCK", str(track)),
              text_frame("TYER", str(year))]
    if cover:
        frames.append(id3_frame("APIC", b"\x00image/png\x00\x03\x00" + cover))
    body = b"".join(frames)
    audio = (b"\xff\xfb\x90\x00" + b"\x00" * 413) * 4
    return b"ID3\x03\x00\x00" + synchsafe(len(body)) + body + audio


def wav(title, artist, album, seconds=1):
    def info(tag, text):
        data = text.encode() + b"\x00"
        data += b"\x00" * (len(data) % 2)
        return tag + struct.pack("<I", len(data)) + data
    listing = b"INFO" + info(b"INAM", title) + info(b"IART", artist) + info(b"IPRD", album)
    pcm = b"\x00\x00" * 8000 * seconds
    fmt = struct.pack("<HHIIHH", 1, 1, 8000, 16000, 2, 16)
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"LIST" +
            struct.pack("<I", len(listing)) + listing + b"data" + struct.pack("<I", len(pcm)) + pcm)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def write(root, relative, data):
    path = os.path.join(root, *relative.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main():
    root = sys.argv[1]
    cover = png(64, (200, 40, 40))
    write(root, "Artist One/Album One/01 First.mp3", mp3("First", "Artist One", "Album One", "Artist One", 1, 2001, cover))
    write(root, "Artist One/Album One/02 Second.mp3", mp3("Second", "Artist One", "Album One", "Artist One", 2, 2001))
    write(root, "Artist Two/Album Two/01 Third.wav", wav("Third", "Artist Two", "Album Two"))
    write(root, "Artist Two/Album Two/cover.png", png(96, (40, 40, 200)))
    write(root, "Artist Three/Album Three/01 Fourth.mp3", mp3("Fourth", "Artist Three", "Album Three", "Artist Three", 1, 2003))
    write(root, "Artist \u00dcn\u00ef/\u00c4lbum \u00d6/01 \u00c7\u00e0.mp3", mp3("\u00c7\u00e0 va", "Artist \u00dcn\u00ef", "\u00c4lbum \u00d6", "Artist \u00dcn\u00ef", 1, 2005))
    write(root, "Audiobooks/Book/ignored.mp3", mp3("Book", "Reader", "Book", "Reader", 1, 2004))


if __name__ == "__main__":
    main()
