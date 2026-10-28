#!/usr/bin/env python3
"""
A beépített lejátszó tesztfájljai (tests/data/player_*): ismert szinuszok, ismert címkékkel.
A rendszer libsndfile könyvtárát használja (ctypes, fejlesztői csomag nélkül; libsndfile 1.1+ kell az MP3-hoz és ALAC-hoz).

  bal csatorna: 440 Hz, 0,5 amplitúdó; jobb csatorna: 1000 Hz, 0,25 amplitúdó (monó fájlnál csak a 440 Hz)

Használat: python3 tools/make_player_test_data.py   (a tests/data mappába ír)
"""
import ctypes
import ctypes.util
import math
import os
import struct
import tempfile

SFM_WRITE = 0x20
FORMATS = {
    "flac16": 0x170000 | 0x0002,   # FLAC, PCM 16
    "flac24": 0x170000 | 0x0003,   # FLAC, PCM 24
    "ogg": 0x200000 | 0x0060,      # Ogg Vorbis
    "mp3": 0x230000 | 0x0082,      # MPEG Layer III
    "caf_alac": 0x180000 | 0x0070, # CAF, ALAC 16
    "opus": 0x200000 | 0x0064,     # Ogg Opus
}
SF_STR_TITLE, SF_STR_ARTIST, SF_STR_ALBUM = 0x01, 0x04, 0x07
SFC_SET_VBR_ENCODING_QUALITY = 0x1300
SFC_SET_COMPRESSION_LEVEL = 0x1301


class SfInfo(ctypes.Structure):
    _fields_ = [("frames", ctypes.c_int64), ("samplerate", ctypes.c_int), ("channels", ctypes.c_int),
                ("format", ctypes.c_int), ("sections", ctypes.c_int), ("seekable", ctypes.c_int)]


lib = ctypes.CDLL(ctypes.util.find_library("sndfile") or "libsndfile.so.1")
lib.sf_open.restype = ctypes.c_void_p
lib.sf_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(SfInfo)]
lib.sf_writef_float.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int64]
lib.sf_set_string.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
lib.sf_command.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
lib.sf_close.argtypes = [ctypes.c_void_p]
lib.sf_strerror.restype = ctypes.c_char_p
lib.sf_strerror.argtypes = [ctypes.c_void_p]


def write(path, fmt, rate, channels, seconds, title):
    n = int(rate * seconds)
    data = (ctypes.c_float * (n * channels))()
    for i in range(n):
        data[i * channels] = 0.5 * math.sin(2 * math.pi * 440 * i / rate)
        if channels == 2:
            data[i * channels + 1] = 0.25 * math.sin(2 * math.pi * 1000 * i / rate)
    info = SfInfo(0, rate, channels, FORMATS[fmt], 0, 0)
    f = lib.sf_open(path.encode(), SFM_WRITE, ctypes.byref(info))
    if not f:
        raise SystemExit(f"{path}: {lib.sf_strerror(None).decode()}")
    if fmt in ("ogg", "mp3", "opus"):   # jó minőség (0,0 a leggyengébb, 1,0 a legjobb), hogy a szintmérés pontos legyen
        q = ctypes.c_double(0.9)
        lib.sf_command(f, SFC_SET_VBR_ENCODING_QUALITY, ctypes.byref(q), ctypes.sizeof(q))
    lib.sf_set_string(f, SF_STR_TITLE, title.encode())
    lib.sf_set_string(f, SF_STR_ARTIST, "Aqulify Teszt".encode())
    lib.sf_set_string(f, SF_STR_ALBUM, "Szinuszok".encode())
    lib.sf_writef_float(f, data, n)
    lib.sf_close(f)
    print(f"{path}: {os.path.getsize(path)} bájt")


def pack_box(box_type, payload):
    return struct.pack(">I4s", len(payload) + 8, box_type) + payload


def write_alac_m4a(path, rate, channels, seconds, title):
    """ALAC hangfájl M4A konténerben, iTunes stílusú metaadatokkal (ilst)."""
    with tempfile.NamedTemporaryFile(suffix=".caf", delete=False) as tmp:
        caf_tmp = tmp.name
    try:
        write(caf_tmp, "caf_alac", rate, channels, seconds, title)
        # Beolvassuk a CAF darabjait (kuki, pakt, data)
        cookie = None
        pakt_sizes = []
        valid_frames = 0
        raw_packets = []
        with open(caf_tmp, "rb") as f:
            f.seek(8)
            while True:
                chdr = f.read(12)
                if not chdr:
                    break
                ctype, csize = struct.unpack(">4sq", chdr)
                if ctype == b"kuki":
                    cookie = f.read(csize)
                elif ctype == b"pakt":
                    pdata = f.read(csize)
                    num_packets, valid_frames, _, _ = struct.unpack(">qqii", pdata[:24])
                    pos = 24
                    while pos < len(pdata) and len(pakt_sizes) < num_packets:
                        val = 0
                        while True:
                            b = pdata[pos]
                            pos += 1
                            val = (val << 7) | (b & 0x7f)
                            if (b & 0x80) == 0:
                                break
                        pakt_sizes.append(val)
                elif ctype == b"data":
                    f.read(4)  # edit count
                    for sz in pakt_sizes:
                        raw_packets.append(f.read(sz))
                    break
                else:
                    f.seek(csize, 1)

        ftyp = pack_box(b"ftyp", b"M4A \x00\x00\x02\x00M4A mp42isom\x00\x00\x00\x00")
        mdat_payload = b"".join(raw_packets)

        alac_box = pack_box(b"alac", b"\x00\x00\x00\x00" + cookie)
        audio_entry_payload = (
            b"\x00" * 6 + struct.pack(">H", 1) +
            b"\x00" * 8 +
            struct.pack(">HH", channels, 16) +
            struct.pack(">HH", 0, 0) +
            struct.pack(">I", rate << 16) +
            alac_box
        )
        stsd_payload = struct.pack(">II", 0, 1) + pack_box(b"alac", audio_entry_payload)
        stsd = pack_box(b"stsd", stsd_payload)

        stts_entries = []
        rem = valid_frames
        for _ in range(len(raw_packets)):
            delta = min(4096, rem)
            stts_entries.append((1, delta))
            rem -= delta
        condensed_stts = []
        for count, delta in stts_entries:
            if condensed_stts and condensed_stts[-1][1] == delta:
                condensed_stts[-1] = (condensed_stts[-1][0] + count, delta)
            else:
                condensed_stts.append((count, delta))
        stts_payload = struct.pack(">II", 0, len(condensed_stts))
        for count, delta in condensed_stts:
            stts_payload += struct.pack(">II", count, delta)
        stts = pack_box(b"stts", stts_payload)

        stsc_payload = struct.pack(">II", 0, 1) + struct.pack(">III", 1, 1, 1)
        stsc = pack_box(b"stsc", stsc_payload)

        stsz_payload = struct.pack(">III", 0, 0, len(pakt_sizes))
        for sz in pakt_sizes:
            stsz_payload += struct.pack(">I", sz)
        stsz = pack_box(b"stsz", stsz_payload)

        tags = {
            "©nam": title,
            "©ART": "Aqulify Teszt",
            "©alb": "Szinuszok",
            "©day": "2026",
            "©gen": "Acoustic",
            "trkn": (1, 10),
            "disk": (1, 1),
        }
        ilst_boxes = []
        for k, v in tags.items():
            if k == "trkn":
                tr, tot = v
                d = pack_box(b"data", struct.pack(">II", 0, 0) + struct.pack(">HHH", 0, tr, tot) + b"\x00\x00")
                ilst_boxes.append(pack_box(b"trkn", d))
            elif k == "disk":
                di, tot = v
                d = pack_box(b"data", struct.pack(">II", 0, 0) + struct.pack(">HHH", 0, di, tot))
                ilst_boxes.append(pack_box(b"disk", d))
            else:
                d = pack_box(b"data", struct.pack(">II", 1, 0) + v.encode("utf-8"))
                ilst_boxes.append(pack_box(k.encode("latin-1"), d))

        ilst = pack_box(b"ilst", b"".join(ilst_boxes))
        hdlr_meta = pack_box(b"hdlr", struct.pack(">II", 0, 0) + b"mdirappl" + b"\x00" * 9)
        meta = pack_box(b"meta", b"\x00\x00\x00\x00" + hdlr_meta + ilst)
        udta = pack_box(b"udta", meta)

        num_chunks = len(raw_packets)
        stco_len = 8 + 8 + 4 * num_chunks

        def build_moov(mdat_start):
            offsets = []
            cur = mdat_start + 8
            for sz in pakt_sizes:
                offsets.append(cur)
                cur += sz
            stco_data = struct.pack(">II", 0, num_chunks) + b"".join(struct.pack(">I", o) for o in offsets)
            stco = pack_box(b"stco", stco_data)
            stbl = pack_box(b"stbl", stsd + stts + stsc + stsz + stco)
            smhd = pack_box(b"smhd", struct.pack(">II", 0, 0))
            dref = pack_box(b"dref", struct.pack(">II", 0, 1) + pack_box(b"url ", struct.pack(">I", 1)))
            dinf = pack_box(b"dinf", dref)
            minf = pack_box(b"minf", smhd + dinf + stbl)
            mdhd = pack_box(b"mdhd", struct.pack(">IIIIIHH", 0, 0, 0, rate, valid_frames, 0, 0))
            hdlr = pack_box(b"hdlr", struct.pack(">II4s", 0, 0, b"soun") + b"\x00" * 12 + b"SoundHandler\x00")
            mdia = pack_box(b"mdia", mdhd + hdlr + minf)
            tkhd = pack_box(b"tkhd", struct.pack(">IIII", 7, 0, 0, 1) + b"\x00" * 8 + struct.pack(">I", valid_frames) + b"\x00" * 44 + struct.pack(">H", 0x0100) + b"\x00" * 2)
            trak = pack_box(b"trak", tkhd + mdia)
            mvhd = pack_box(b"mvhd", struct.pack(">IIII", 0, 0, 0, rate) + struct.pack(">I", valid_frames) + struct.pack(">I", 0x00010000) + struct.pack(">H", 0x0100) + b"\x00" * 10 + b"\x00" * 36 + struct.pack(">I", 2))
            return pack_box(b"moov", mvhd + trak + udta)

        est_moov_len = 2000 + stco_len
        for _ in range(5):
            mdat_off = len(ftyp) + est_moov_len
            moov = build_moov(mdat_off)
            if len(moov) == est_moov_len:
                break
            est_moov_len = len(moov)

        mdat = pack_box(b"mdat", mdat_payload)
        with open(path, "wb") as out_f:
            out_f.write(ftyp)
            out_f.write(moov)
            out_f.write(mdat)
        print(f"{path}: {os.path.getsize(path)} bájt")
    finally:
        if os.path.exists(caf_tmp):
            os.remove(caf_tmp)


out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "data")
os.makedirs(out, exist_ok=True)
write(os.path.join(out, "player_stereo44.flac"), "flac16", 44100, 2, 1.0, "Sztereó FLAC")
write(os.path.join(out, "player_mono48.flac"), "flac24", 48000, 1, 0.5, "Monó FLAC 24 bit")
write(os.path.join(out, "player_stereo44.ogg"), "ogg", 44100, 2, 1.0, "Sztereó Ogg")
write(os.path.join(out, "player_stereo44.mp3"), "mp3", 44100, 2, 1.0, "Sztereó MP3")
write_alac_m4a(os.path.join(out, "player_stereo44.m4a"), 44100, 2, 1.0, "Sztereó ALAC")
write(os.path.join(out, "player_stereo48.opus"), "opus", 48000, 2, 1.0, "Sztereó Opus")
