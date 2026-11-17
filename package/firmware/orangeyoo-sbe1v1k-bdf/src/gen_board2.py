#!/usr/bin/env python3
"""
Generate an ath12k board-2.bin for Orangeyoo SBE1V1K (Askey RTQ7300T).

Evidence chain:
- BDF DATA payloads: the three per-radio calibration blocks extracted from the
  factory 0:ART partition (p20) of a reference unit -- each block is a
  calheader (tag 0x04040001) plus the vendor BDF blob (art-radio-{1,4,2}.bin,
  sha256 recorded in the package Makefile).  The community board-
  askey_sbe1v1k.qcn9274 payloads match these blocks at 99.9%.
- REGDB IE: copied verbatim from the community board-askey_sbe1v1k.qcn9274
  (top-level IE id 1 = ATH12K_BD_IE_REGDB, parsed by the wlan-open feed
  ath12k, hw.h: ATH12K_BD_IE_REGDB = 1).
- BOARD IE names: "bus=pci,qmi-chip-id=<c>,qmi-board-id=<b>".  The buildable
  ath12k (qca/feeds/wlan-open mac80211, backports-6.1-9a0dddfb3) builds the
  board name from the QMI chip id and the board id, and its feed patch
  0001-d-ath12k-Add-fix-to-fetch-board-id-from-dts takes the board id from
  the DT "qcom,board_id" property (1/4/2 on this board, matching the [ART]
  block order).  chip-id 0 is the community-measured QMI chip id for this
  radio family; pass --chip-id at bring-up if the device reports otherwise.

--neutralize-mac (shipped configuration, 2.0-pre1 adjudication):
The factory ART blocks embed the reference unit's radio MACs at payload
offset 0x10 (three consecutive addresses of that unit).  A generic
flash image must not carry per-unit identity (delivery zero-unit-identity
gate), so with this flag the 6 bytes at 0x10..0x15 are replaced by
community-style placeholder MACs 00:03:7f:12:34:<tail>, with a distinct
human-readable tail per radio (in --board order: 56, 5c, 5d; 0x16 onward
is already the factory's own placeholder string 00:03:7f:12:34:57..5b).
The u32 check word at 0x0c is left untouched: its algorithm is unknown
(a2b R1 open item) so it cannot be recomputed, and the community board
file ships ART-derived data with its own (differing) check word and is
known to boot, which shows the ath12k board-2 chain does not enforce it.
The reference unit's MAC family must then not appear anywhere in the
product; per-unit radio calibration is served at runtime by the package's
/etc/hotplug.d/firmware/12-ath12k-sbe1v1k-caldata script straight from
this unit's own 0:ART partition (RAM-only sysfs firmware loading).

Usage:
  gen_board2.py -o board-2.bin \
      --chip-id 0 \
      --neutralize-mac \
      --board radio-1:1,radio-4:4,radio-2:2 \
      --regdb board-askey_sbe1v1k.qcn9274
"""
import argparse
import struct
import sys

BOARD_MAGIC = b"QCA-ATH12K-BOARD\x00"          # 17 bytes, NUL-terminated
TOP_BOARD_IE = 0                                # ATH12K_BD_IE_BOARD
TOP_REGDB_IE = 1                                # ATH12K_BD_IE_REGDB
NAME_IE = 0                                     # ATH12K_BD_IE_BOARD_NAME
DATA_IE = 1                                     # ATH12K_BD_IE_BOARD_DATA

MAC_OFFSET = 0x10                               # radio MAC inside ART block
MAC_LEN = 6
MAC_PLACE_BASE = bytes.fromhex("00037f123456")  # community placeholder style
MAC_PLACE_TAILS = (0x56, 0x5C, 0x5D)            # distinct, outside factory 57..5b


def ie(ie_id, payload):
    """One IE: 8-byte header + payload, padded to 4 bytes."""
    out = struct.pack("<II", ie_id, len(payload)) + payload
    pad = -len(payload) % 4
    return out + b"\x00" * pad


def parse_regdb(blob):
    """Return the top-level REGDB IE (raw, header included) from a
    community/ath12k board-2.bin, or None if absent."""
    magic_len = len(BOARD_MAGIC)
    if blob[:magic_len] != BOARD_MAGIC:
        sys.exit("regdb source is not a QCA-ATH12K-BOARD file")
    off = (magic_len + 3) // 4 * 4
    while off + 8 <= len(blob):
        ie_id, ie_len = struct.unpack_from("<II", blob, off)
        total = 8 + (ie_len + 3) // 4 * 4
        if ie_id == TOP_REGDB_IE:
            return blob[off:off + total]
        off += total
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--chip-id", type=int, default=0)
    ap.add_argument("--board", required=True,
                    help="comma list of payload-file:board-id pairs, "
                         "e.g. art-radio-1.bin:1,art-radio-4.bin:4,"
                         "art-radio-2.bin:2")
    ap.add_argument("--regdb", default=None,
                    help="community board-2.bin to copy the REGDB IE from")
    ap.add_argument("--neutralize-mac", action="store_true",
                    help="replace the per-unit radio MAC at payload offset "
                         "0x10 with community-style placeholder MACs "
                         "(distinct tail per radio); see header comment")
    args = ap.parse_args()

    out = bytearray(BOARD_MAGIC)
    out += b"\x00" * (-len(out) % 4)  # magic padded to 4

    for idx, spec in enumerate(args.board.split(",")):
        path, _, board_id = spec.rpartition(":")
        payload = bytearray(open(path, "rb").read())
        if args.neutralize_mac:
            tail = MAC_PLACE_TAILS[idx % len(MAC_PLACE_TAILS)]
            placeholder = MAC_PLACE_BASE[:-1] + bytes([tail])
            orig = bytes(payload[MAC_OFFSET:MAC_OFFSET + MAC_LEN])
            payload[MAC_OFFSET:MAC_OFFSET + MAC_LEN] = placeholder
            print("%s -> MAC %s neutralized to %s" % (
                path, ":".join("%02x" % b for b in orig),
                ":".join("%02x" % b for b in placeholder)))
        name = "bus=pci,qmi-chip-id=%d,qmi-board-id=%s" % (
            args.chip_id, board_id)
        nested = ie(NAME_IE, name.encode()) + ie(DATA_IE, bytes(payload))
        out += ie(TOP_BOARD_IE, nested)
        print("%s -> name=%r payload=%d bytes" % (path, name, len(payload)))

    if args.regdb:
        regdb = parse_regdb(open(args.regdb, "rb").read())
        if regdb:
            out += regdb
            print("regdb IE copied verbatim (%d bytes incl. header)"
                  % len(regdb))

    open(args.output, "wb").write(out)
    print("wrote %s (%d bytes)" % (args.output, len(out)))


if __name__ == "__main__":
    main()
