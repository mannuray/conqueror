"""Phase 4: things in the room (AI engine LLD §15) - the gate, the floor trick, the list. No model, no tank:
detections are made by projecting known floor points through the camera model."""

from __future__ import annotations

import math

from conqueror_ai.domain import objects as ob
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.pose import PoseHistory
from conqueror_ai.domain.types import Detection, Frame, Pose, TelemSample
from tests.test_camera import calib

CAL = calib(tilt0=-15.0)  # looking down like the driving tilt


def cam_at(x: float = 0.0, y: float = 0.0, th_deg: float = 0.0, pan: float = 0.0) -> CameraModel:
    return CameraModel(CAL).for_frame(Pose(0, x, y, math.radians(th_deg), 0.0, 0.0), pan, 0.0)


def box_on_floor(
    cam: CameraModel, x: float, y: float, cls: str = "sports ball", h_px: float = 20.0
) -> Detection:
    """A detection whose bottom middle is where the floor point (x, y) is in the picture."""
    q = cam.floor_to_pixel(x, y)
    assert q is not None
    u, v = q
    return Detection(cls, 0.9, (u - 10.0, v - h_px, u + 10.0, v))


def sample(
    t_us: int, rate: float = 0.0, flags: int = 0, pan: float = 90.0, tilt: float = 90.0
) -> TelemSample:
    return TelemSample(t_us, t_us // 1000, 0, 0, 0.0, rate, None, pan, tilt, 0, 0, flags, 0, 0)


# ---- the floor trick
def test_the_floor_trick_finds_the_floor_point() -> None:
    cam = cam_at()
    located, near = ob.ObjectLocator().locate([box_on_floor(cam, 1.0, 0.2)], cam, 0)
    assert near == [] and len(located) == 1
    assert math.isclose(located[0].x, 1.0, abs_tol=1e-6) and math.isclose(located[0].y, 0.2, abs_tol=1e-6)


def test_the_floor_trick_works_from_any_pose() -> None:
    cam = cam_at(2.0, 1.0, 90.0)  # the tank at (2, 1), facing left
    located, _ = ob.ObjectLocator().locate([box_on_floor(cam, 2.0, 2.0)], cam, 0)
    assert math.isclose(located[0].x, 2.0, abs_tol=1e-6) and math.isclose(located[0].y, 2.0, abs_tol=1e-6)


def test_a_box_cut_by_the_bottom_edge_has_only_a_direction() -> None:
    cam = cam_at()
    d = Detection("person", 0.9, (200.0, 100.0, 260.0, 239.5))  # right of centre, cut at the bottom
    located, near = ob.ObjectLocator().locate([d], cam, 7)
    assert located == [] and len(near) == 1
    assert near[0].cls == "person" and near[0].bearing_rad < 0.0 and near[0].t_us == 7  # right = clockwise


def test_table_things_and_far_or_high_boxes_are_ignored() -> None:
    cam = cam_at()
    cup = box_on_floor(cam, 1.0, 0.0, cls="cup")
    far = box_on_floor(cam, 4.5, 0.0)
    high = Detection("sports ball", 0.9, (150.0, 0.0, 170.0, 5.0))  # its bottom above the horizon
    located, near = ob.ObjectLocator().locate([cup, far, high], cam, 0)
    assert located == [] and near == []


def test_the_expected_error_grows_with_the_distance() -> None:
    assert math.isclose(ob.sigma_at(1.0), 0.05) and math.isclose(ob.sigma_at(2.0), 0.14)


# ---- which pictures are used
def gate_with(samples: list[TelemSample], pose_w: float = 0.0) -> ob.FrameGate:
    h, a = PoseHistory(), ob.CameraAngles()
    for s in samples:
        h.push(Pose(s.bus_us, 0.0, 0.0, 0.0, 0.0, pose_w))
        a.push(s)
    return ob.FrameGate(CAL, h, a)


def test_the_gate_gives_the_camera_at_the_capture_time() -> None:
    g = gate_with([sample(1_000_000, pan=80.0), sample(1_020_000, pan=80.0)])  # servo 80 = 10 deg right
    cam = g.camera_for(1_010_000)
    assert isinstance(cam, CameraModel) and math.isclose(cam.pan_deg, 10.0)


def test_the_gate_skips_with_its_reason() -> None:
    assert gate_with([]).camera_for(1_000_000) == "no pose"
    assert (
        gate_with([sample(1_000_000), sample(1_020_000)], pose_w=math.radians(60)).camera_for(1_010_000)
        == "turning"
    )
    moving = [sample(1_000_000, flags=ob.CAM_MOVING), sample(1_020_000, flags=ob.CAM_MOVING)]
    assert gate_with(moving).camera_for(1_010_000) == "camera moving"
    h = PoseHistory()
    h.push(Pose(1_000_000, 0, 0, 0, 0, 0))
    h.push(Pose(1_100_000, 0, 0, 0, 0, 0))
    assert ob.FrameGate(CAL, h, ob.CameraAngles()).camera_for(1_050_000) == "no camera angles"


def test_camera_angles_take_the_nearest_sample_within_40_ms() -> None:
    a = ob.CameraAngles()
    a.push(sample(1_000_000, pan=70.0))
    a.push(sample(1_020_000, pan=75.0))
    got = a.at(1_015_000)
    assert got is not None and got[0] == 75.0
    assert a.at(1_100_000) is None


# ---- the list
def track_frames(
    tr: ob.ObjectTracker,
    cam: CameraModel,
    points: list[tuple[float, float]],
    n: int,
    t0_us: int = 0,
    step_us: int = 100_000,
) -> int:
    loc = ob.ObjectLocator()
    t = t0_us
    for i in range(n):
        t = t0_us + i * step_us
        located, _ = loc.locate([box_on_floor(cam, x, y) for x, y in points], cam, t)
        tr.update(located, cam, t)
    return t


def test_one_ball_seen_ten_times_is_one_thing() -> None:
    tr, cam = ob.ObjectTracker(), cam_at()
    track_frames(tr, cam, [(1.0, 0.1)], 10)
    objs = tr.objects()
    assert len(objs) == 1 and objs[0].state == "seen"
    assert math.isclose(objs[0].x_m, 1.0, abs_tol=0.01) and math.isclose(objs[0].y_m, 0.1, abs_tol=0.01)


def test_two_balls_are_two_things() -> None:
    tr, cam = ob.ObjectTracker(), cam_at()
    track_frames(tr, cam, [(1.0, 0.3), (1.0, -0.3)], 10)
    assert len(tr.objects()) == 2


def test_a_walking_person_is_one_thing_with_its_speed() -> None:
    tr, cam, loc = ob.ObjectTracker(), cam_at(), ob.ObjectLocator()
    for i in range(20):  # 0.5 m/s across the view, 10 pictures a second
        t = i * 100_000
        y = 0.4 - 0.05 * i
        located, _ = loc.locate([box_on_floor(cam, 1.5, y, cls="person", h_px=80.0)], cam, t)
        tr.update(located, cam, t)
    objs = tr.objects()
    assert len(objs) == 1 and math.isclose(objs[0].vy, -0.5, abs_tol=0.1)


def test_out_of_view_it_is_remembered_and_comes_back_the_same() -> None:
    tr = ob.ObjectTracker()
    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
    oid = tr.objects()[0].oid
    away = cam_at(th_deg=120.0)  # turned away: the ball is behind
    t = track_frames(tr, away, [], 120, t0_us=t + 100_000)  # 12 s
    objs = tr.objects()
    assert len(objs) == 1 and objs[0].state == "remembered" and objs[0].oid == oid
    track_frames(tr, cam_at(), [(1.0, 0.0)], 3, t0_us=t + 100_000)
    objs = tr.objects()
    assert len(objs) == 1 and objs[0].state == "seen" and objs[0].oid == oid


def test_remembered_for_20_s_it_is_forgotten() -> None:
    tr = ob.ObjectTracker()
    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
    track_frames(tr, cam_at(th_deg=120.0), [], 205, t0_us=t + 100_000)  # 20.5 s
    assert tr.objects() == []


def test_its_spot_in_view_for_1_s_without_it_drops_it() -> None:
    tr = ob.ObjectTracker()
    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
    track_frames(tr, cam_at(), [], 12, t0_us=t + 100_000)  # the camera looks right at it: 1.2 s, nothing
    assert tr.objects() == []


def test_a_short_miss_in_view_keeps_it() -> None:
    tr = ob.ObjectTracker()
    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
    t = track_frames(tr, cam_at(), [], 3, t0_us=t + 100_000)  # 0.3 s missed (a blurred picture)
    track_frames(tr, cam_at(), [(1.0, 0.0)], 2, t0_us=t + 100_000)
    assert len(tr.objects()) == 1


# ---- the pipeline
def frame(cap_us: int) -> Frame:
    return Frame(seq=cap_us // 1000, cap_us=cap_us, recv_s=cap_us / 1e6 + 0.1, jpeg=b"")


def run_pipeline() -> list[tuple[int, str, float, float]]:
    p = ob.ObjectPipeline(CAL)
    p.on_calib(7514)
    cam = cam_at()
    for i in range(60):  # 1.2 s of telemetry, the tank still
        p.on_telem(TelemSample(i * 20_000, i * 20, 0, 0, 0.0, 0.0, None, 90.0, 90.0, 0, 0, 0, 0, 0))
        if i % 5 == 4:
            p.on_frame(frame(i * 20_000 - 10_000), (320, 240), [box_on_floor(cam, 1.0, 0.1)])
    return [(o.oid, o.cls, round(o.x_m, 4), round(o.y_m, 4)) for o in p.objects()]


def test_the_pipeline_lists_the_ball_and_replays_the_same() -> None:
    first = run_pipeline()
    assert len(first) == 1 and first[0][1] == "sports ball" and math.isclose(first[0][2], 1.0, abs_tol=0.01)
    assert run_pipeline() == first


def test_the_pipeline_counts_its_skips() -> None:
    p = ob.ObjectPipeline(CAL)
    p.on_frame(frame(1_000_000), (320, 240), [])
    p.on_frame(frame(1_000_000), (640, 480), [])
    assert p.skips() == {"no pose": 1, "picture size": 1}


# ---- Plan V final review
def test_a_remembered_thing_does_not_take_a_far_detection() -> None:  # I1: its uncertainty stays bounded
    tr = ob.ObjectTracker()
    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
    oid = tr.objects()[0].oid
    t = track_frames(tr, cam_at(th_deg=120.0), [], 80, t0_us=t + 100_000)  # remembered for 8 s
    assert tr.objects()[0].sigma_m <= ob.MAX_REMEMBERED_SIGMA_M + 1e-9
    track_frames(tr, cam_at(), [(2.5, -1.5)], 5, t0_us=t + 100_000)  # a different ball, 2.9 m away
    objs = {o.oid: o for o in tr.objects()}
    assert math.hypot(objs[oid].x_m - 1.0, objs[oid].y_m) < 0.05 if oid in objs else True
    assert any(math.hypot(o.x_m - 2.5, o.y_m + 1.5) < 0.05 and o.oid != oid for o in objs.values())


def test_a_seen_thing_keeps_its_detections_from_a_remembered_neighbour() -> None:  # I1: ranked fairly
    tr, loc = ob.ObjectTracker(), ob.ObjectLocator()
    cam_b = cam_at(th_deg=60.0)  # looking left: ball B at (1.0, 1.5)
    t = track_frames(tr, cam_b, [(1.0, 1.5)], 5)
    oid_b = tr.objects()[0].oid
    cam = cam_at()  # ball B now out of view; ball A at (1, 0) seen in every picture for 6 s
    for i in range(60):
        t += 100_000
        located, _ = loc.locate([box_on_floor(cam, 1.0 + 0.01 * (i % 3), 0.0)], cam, t)
        tr.update(located, cam, t)
    b = next(o for o in tr.objects() if o.oid == oid_b)
    assert math.hypot(b.x_m - 1.0, b.y_m - 1.5) < 0.1  # B stayed where it was


def test_one_false_detection_is_not_listed() -> None:  # I3: confirmed after 3 hits
    tr = ob.ObjectTracker()
    track_frames(tr, cam_at(), [(1.0, 0.0)], 1)
    assert tr.objects() == []
    track_frames(tr, cam_at(), [], 1, t0_us=100_000)  # missed once while new: gone
    track_frames(tr, cam_at(), [(1.0, 0.0)], 2, t0_us=200_000)
    assert tr.objects() == []
    track_frames(tr, cam_at(), [(1.0, 0.0)], 1, t0_us=400_000)
    assert len(tr.objects()) == 1


def test_a_thing_at_3_m_is_checked_off_when_it_is_not_there() -> None:  # I3: the look check to 4 m
    tr = ob.ObjectTracker()
    cam = cam_at(th_deg=0.0)
    t = track_frames(tr, cam, [(3.0, 0.0)], 5)
    assert len(tr.objects()) == 1
    track_frames(tr, cam, [], 12, t0_us=t + 100_000)
    assert tr.objects() == []


def test_the_look_check_needs_three_misses() -> None:  # M7
    tr = ob.ObjectTracker()
    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
    track_frames(tr, cam_at(), [], 2, t0_us=t + 100_000, step_us=1_100_000)  # two misses 1.1 s apart
    assert len(tr.objects()) == 1


def test_a_box_a_few_rows_above_the_bottom_edge_is_near() -> None:  # M4
    cam = cam_at()
    located, near = ob.ObjectLocator().locate(
        [Detection("person", 0.9, (150.0, 100.0, 190.0, 236.0))], cam, 0
    )
    assert located == [] and len(near) == 1


def test_a_panning_camera_skips_the_picture() -> None:  # I2: the Uno Lite does not send CAM_MOVING
    a = ob.CameraAngles()
    for i in range(5):  # pan turning 1.2 deg per 20 ms = 60 deg/s
        a.push(sample(1_000_000 + 20_000 * i, pan=90.0 + 1.2 * i))
    h = PoseHistory()
    h.push(Pose(1_000_000, 0, 0, 0, 0, 0))
    h.push(Pose(1_080_000, 0, 0, 0, 0, 0))
    assert ob.FrameGate(CAL, h, a).camera_for(1_040_000) == "camera moving"


def test_a_clock_going_back_starts_the_list_again() -> None:  # M6: a bridge restart in a replay
    p = ob.ObjectPipeline(CAL)
    p.on_calib(7514)
    cam = cam_at()
    for i in range(30):
        p.on_telem(
            TelemSample(5_000_000 + i * 20_000, i * 20, 0, 0, 0.0, 0.0, None, 90.0, 90.0, 0, 0, 0, 0, 0)
        )
        if i % 5 == 4:
            p.on_frame(frame(5_000_000 + i * 20_000 - 10_000), (320, 240), [box_on_floor(cam, 1.0, 0.1)])
    assert len(p.objects()) == 1
    for i in range(10):  # the bridge restarted: its clock is back near 0
        p.on_telem(
            TelemSample(100_000 + i * 20_000, 9000 + i * 20, 0, 0, 0.0, 0.0, None, 90.0, 90.0, 0, 0, 0, 0, 0)
        )
    p.on_frame(frame(250_000), (320, 240), [])
    assert p.objects() == []


# ---- one ball is one thing across pictures (Plan Y final review I2)
def frames_of(
    tr: ob.ObjectTracker, cam: CameraModel, things: list[tuple[str, float, float]], n: int, t0_us: int
) -> int:
    loc = ob.ObjectLocator(frozenset({"toy", "chair", "ball:green"}))
    t = t0_us
    for i in range(n):
        t = t0_us + i * 100_000
        located, _ = loc.locate([box_on_floor(cam, x, y, cls=c) for c, x, y in things], cam, t)
        tr.update(located, cam, t)
    return t


def test_a_word_thing_where_the_colour_ball_is_seen_is_that_ball() -> None:
    """Far away the finder misses the ball and YOLO-World lists a "toy"; closer, the finder sees it and the
    toy's boxes are dropped (MultiDetector): the toy is the ball, not a ghost beside it."""
    tr, cam = ob.ObjectTracker(), cam_at()
    t = frames_of(tr, cam, [("toy", 2.5, 0.0)], 5, 0)
    assert [o.cls for o in tr.objects()] == ["toy"]
    frames_of(tr, cam, [("ball:green", 2.5, 0.0)], 5, t + 100_000)
    assert [o.cls for o in tr.objects()] == ["ball:green"]


def test_a_word_box_on_a_ball_the_finder_misses_keeps_the_ball() -> None:
    """The ball rolls half out of the picture's edge: the finder misses it, YOLO-World says "toy" - still the
    one ball."""
    tr, cam = ob.ObjectTracker(), cam_at()
    t = frames_of(tr, cam, [("ball:green", 1.0, 0.0)], 5, 0)
    oid = tr.objects()[0].oid
    frames_of(tr, cam, [("toy", 1.0, 0.0)], 8, t + 100_000)
    objs = tr.objects()
    assert [(o.cls, o.oid, o.state) for o in objs] == [("ball:green", oid, "seen")]


def test_a_chair_seen_beside_the_ball_stays_its_own_thing() -> None:
    tr, cam = ob.ObjectTracker(), cam_at()
    frames_of(tr, cam, [("ball:green", 1.0, 0.0), ("chair", 1.05, 0.1)], 10, 0)
    assert sorted(o.cls for o in tr.objects()) == ["ball:green", "chair"]


def test_a_word_thing_elsewhere_is_not_taken_by_the_ball() -> None:
    tr, cam = ob.ObjectTracker(), cam_at()
    t = frames_of(tr, cam, [("toy", 2.0, 0.5)], 5, 0)
    frames_of(tr, cam, [("ball:green", 1.0, 0.0)], 3, t + 100_000)  # the toy missed 3 times (< 1 s): kept
    assert sorted(o.cls for o in tr.objects()) == ["ball:green", "toy"]
