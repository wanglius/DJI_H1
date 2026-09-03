"""Deterministic compressed flight, independent of UART and wall-clock time.

Field values follow Docs/AB板串口通信协议_V1.0.md. Route and stage durations
are synthetic test inputs, not a flight-dynamics model. Takeoff occurs before
RTK fix, so later RTK position never implies an RTK altitude reference.
"""
from dataclasses import dataclass
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


class FlightModel:
    def __init__(self, duration=60.0, scenario="normal"):
        if duration < 60:
            raise ValueError("flight duration must be at least 60 seconds")
        self.duration = duration
        self.scenario = scenario

    def stage(self, elapsed):
        stage = STAGES[0]
        for candidate in STAGES:
            if elapsed >= candidate.fraction * self.duration:
                stage = candidate
        if stage.name == "return-home":
            reasons = {"low-battery": 3, "manual-abort": 6, "drone-link-loss": 7}
            if self.scenario in reasons:
                return Stage(stage.fraction, stage.name, 2, 15, reasons[self.scenario])
        return stage

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
        rtk = connected and p >= .30
        status = (7 | (8 if p >= .14 else 0)) if connected else 0
        battery = round(100 - 35 * p)
        if self.scenario == "low-battery" and p >= .70:
            battery = max(5, round(20 - 10 * (p - .70) / .30))
        fields = [399042000 + round(distance * 20000),
                  1164074000 + round(distance * 30000), round(height * 120000),
                  int(utc) & 0xFFFFFFFF, int(uptime * 1000) & 0xFFFFFFFF,
                  int(utc * 1000) % 1000, 1 if rtk else 0,
                  3 if connected else 0, 50 if rtk else 0,
                  stage.flight_status, stage.display_mode, battery, status,
                  15 if connected else 0]
        return struct.pack("<iiiIIH8B", *fields)
