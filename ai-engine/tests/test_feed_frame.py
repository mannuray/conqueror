"""The AI feed's binary frame: header length, header, camera, depth (AI engine LLD §22.2)."""

from __future__ import annotations

import struct

import numpy as np

from conqueror_ai.domain.feed import (
    decode_frame,
    decode_map,
    decode_marks,
    encode_frame,
    encode_map,
    page_boxes,
)
from conqueror_ai.domain.types import Detection


def test_round_trip_with_and_without_depth() -> None:
    b = encode_frame({"seq": 3, "t_ms": 1200, "boxes": []}, b"CAMJPEG", b"DEP")
    head, cam, depth = decode_frame(b)
    assert head["seq"] == 3 and head["cam"] == 7 and head["depth"] == 3
    assert cam == b"CAMJPEG" and depth == b"DEP"
    head, cam, depth = decode_frame(encode_frame({"seq": 4, "t_ms": 1300, "boxes": []}, b"CAM", None))
    assert head["depth"] == 0 and depth is None and cam == b"CAM"


def test_header_length_big_endian() -> None:
    b = encode_frame({"seq": 1, "t_ms": 0, "boxes": []}, b"X", None)
    (n,) = struct.unpack(">I", b[:4])
    assert b[4 + n : 4 + n + 1] == b"X"
    assert b[4 : 4 + n].startswith(b"{")


def test_page_boxes_scale_ball_word_maybes_left_out_top_8() -> None:
    dets = [
        Detection("toy", 0.9, (64.0, 48.0, 128.0, 96.0)),
        Detection("ball:red", 0.8, (0.0, 0.0, 640.0, 480.0)),
    ]
    dets.append(Detection("box", 0.2, (0.0, 0.0, 10.0, 10.0), sure=False))
    dets += [Detection("chair", 0.3 + i / 100, (0.0, 0.0, 10.0, 10.0)) for i in range(10)]
    boxes = page_boxes(dets, 640, 480)
    assert len(boxes) == 8
    assert boxes[0] == ["toy", 0.9, 100, 100, 100, 100]
    assert boxes[1] == ["ball", 0.8, 0, 0, 1000, 1000]
    assert all(b[0] != "box" for b in boxes)


def test_encode_map_round_trip() -> None:
    """LLD §23.2: a map frame is the same layout, its header says kind map; then the PNG."""
    png = b"\x89PNG fake"
    b = encode_map({"seq": 3, "cell_m": 0.05, "x0": -1.0, "y0": 0.5, "w": 4, "h": 2}, png)
    head, got = decode_map(b)
    assert head["kind"] == "map" and head["png"] == len(png) and head["w"] == 4
    assert got == png


# ---- Plan Z11 (LLD §26.2): the class picture as a third part
def test_marks_round_trip_and_an_old_frame_has_none() -> None:
    classes = np.array([[1, 2, 3], [4, 0, 1]], np.uint8)
    b = encode_frame({"seq": 5, "t_ms": 0, "boxes": []}, b"CAM", b"DEP", classes)
    head, cam, depth = decode_frame(b)  # the old reader still reads cam and depth
    assert head["marks"] == [3, 2] and cam == b"CAM" and depth == b"DEP"
    assert decode_marks(b) == (3, 2, bytes([1, 2, 3, 4, 0, 1]))  # row by row, row 0 the picture's top
    assert decode_marks(encode_frame({"seq": 6, "t_ms": 0, "boxes": []}, b"CAM", None)) is None
