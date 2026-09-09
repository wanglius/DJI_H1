"""Deterministic compressed flight, independent of UART and wall-clock time.

Field values follow Docs/AB板串口通信协议_V1.0.md. Route and stage durations
are synthetic test inputs, not a flight-dynamics model. Takeoff occurs before
RTK fix, so later RTK position never implies an RTK altitude reference.
"""
from dataclasses import dataclass
import math
import struct


@dataclass(frozen=True)
class Stage:
    fraction: float
    name: str
    flight_status: int
    display_mode: int = 0
    stop_reason: int = 0


STAGES = (
    Stage(0, "waiting-for-drone", 0),
    Stage(.04, "preflight", 0),
    Stage(.10, "motors-on", 1),
    Stage(.14, "takeoff", 2, 11),
    Stage(.24, "transit", 2),
    Stage(.30, "survey", 2),
    Stage(.55, "survey-complete", 2, 0, 1),
    # A second session proves real stop/restart lifecycle rather than only retry.
    Stage(.64, "restart-survey", 2),
    Stage(.70, "return-home", 2, 15, 2),
    Stage(.80, "landing", 2, 12, 4),
    Stage(.90, "landed", 0, 0, 5),
    Stage(.92, "prepare-shutdown", 0, 0, 8),
    Stage(.94, "power-off", 0),
)

# Ten-minute field-shaped mission. Each line is a separate acquisition segment,
# which exercises repeated recorder open/drain/flush boundaries in one flight.
ENDURANCE_STAGES = (
    Stage(0, "waiting-for-drone", 0),
    Stage(.01, "preflight", 0), Stage(.03, "motors-on", 1),
    Stage(.05, "takeoff", 2, 11), Stage(.08, "transit", 2),
    Stage(.12, "line-1-start", 2), Stage(.27, "line-1-stop", 2, 0, 1),
    Stage(.30, "line-2-start", 2), Stage(.43, "line-2-stop", 2, 0, 1),
    Stage(.46, "line-3-start", 2), Stage(.59, "line-3-stop", 2, 0, 1),
    Stage(.62, "line-4-start", 2), Stage(.75, "line-4-stop", 2, 0, 1),
    Stage(.78, "return-home", 2, 15, 2), Stage(.90, "landing", 2, 12, 4),
    Stage(.96, "landed", 0, 0, 5), Stage(.97, "prepare-shutdown", 0, 0, 8),
    Stage(.98, "power-off", 0),
)

# fraction, east metres, north metres, height millimetres. The alternating
# north/south legs form four 400 m test lines spaced 50 m apart.
ENDURANCE_ROUTE = (
    (0, 0, 0, 0), (.05, 0, 0, 0), (.08, 0, 0, 120000),
    (.12, 50, 0, 120000), (.27, 50, 400, 120000),
    (.30, 100, 400, 120000), (.43, 100, 0, 120000),
    (.46, 150, 0, 120000), (.59, 150, 400, 120000),
    (.62, 200, 400, 120000), (.75, 200, 0, 120000),
    (.78, 0, 0, 120000), (.90, 0, 0, 120000), (.96, 0, 0, 0), (1, 0, 0, 0),
)


class FlightModel:
    def __init__(self, duration=60.0, scenario="normal"):
        if duration < 60:
            raise ValueError("flight duration must be at least 60 seconds")
        self.duration = duration
        self.scenario = scenario
        self.stages = ENDURANCE_STAGES if scenario == "endurance" else STAGES

    def stage(self, elapsed):
        stage = STAGES[0]
        for candidate in self.stages:
            if elapsed >= candidate.fraction * self.duration:
                stage = candidate
        if stage.name == "return-home":
            reasons = {"low-battery": 3, "manual-abort": 6, "drone-link-loss": 7}
            if self.scenario in reasons:
                return Stage(stage.fraction, stage.name, 2, 15, reasons[self.scenario])
        return stage

    def _endurance_position(self, p):
        left = ENDURANCE_ROUTE[0]
        for right in ENDURANCE_ROUTE[1:]:
            if p <= right[0]:
                span = right[0] - left[0]
                ratio = 0 if span == 0 else (p - left[0]) / span
                return tuple(left[i] + ratio * (right[i] - left[i]) for i in range(1, 4))
            left = right
        return ENDURANCE_ROUTE[-1][1:]

    def telemetry(self, elapsed, utc, uptime):
        stage = self.stage(elapsed)
        p = max(0.0, min(1.0, elapsed / self.duration))
        connected = p >= .04 and not (self.scenario == "drone-link-loss" and p >= .70)
        # Piecewise-linear route: climb, survey outbound, return, descend.
        outbound = min(1.0, max(0.0, (p - .24) / .38))
        returning = min(1.0, max(0.0, (p - .70) / .10))
        distance = outbound * (1 - returning)
        height = min(1.0, max(0.0, (p - .14) / .10))
        height *= 1 - min(1.0, max(0.0, (p - .80) / .10))
        rtk_degraded = self.scenario == "endurance" and .50 <= p < .54
        rtk = connected and p >= .30 and not rtk_degraded
        status = (7 | (8 if p >= .14 else 0)) if connected else 0
        battery = round(100 - 35 * p)
        if self.scenario == "low-battery" and p >= .70:
            battery = max(5, round(20 - 10 * (p - .70) / .30))
        if self.scenario == "endurance":
            east, north, altitude = self._endurance_position(p)
            # Local tangent-plane conversion around Beijing; protocol uses 1e-7 degrees.
            latitude = 399042000 + round(north / 111320 * 1e7)
            longitude = 1164074000 + round(east / (111320 * math.cos(math.radians(39.9042))) * 1e7)
            height_mm = round(altitude)
        else:
            latitude = 399042000 + round(distance * 20000)
            longitude = 1164074000 + round(distance * 30000)
            height_mm = round(height * 120000)
        fields = [latitude, longitude, height_mm,
                  int(utc) & 0xFFFFFFFF, int(uptime * 1000) & 0xFFFFFFFF,
                  int(utc * 1000) % 1000, 1 if rtk else 0,
                  (2 if rtk_degraded else 3) if connected else 0, 50 if rtk else 200,
                  stage.flight_status, stage.display_mode, battery, status,
                  15 if connected else 0]
        return struct.pack("<iiiIIH8B", *fields)
