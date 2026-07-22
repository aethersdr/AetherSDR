#!/usr/bin/env python3
"""Verify and diagnose the PWR meter's power-domain movement responses.

The code-drawn face uses one degree-5 Bernstein polynomial per movement, in
normalized POWER (a real D'Arsonval movement driving a sqrt watt scale). The
needle parks on its printed zero (angled rest = reference_angles[0]), so every
photographed tick INCLUDING zero constrains the response. The stored responses
must satisfy:

* b0 = 0 and b5 = 1;
* b0 <= ... <= b5 (monotonic movement);
* non-positive second differences (concave: sensitivity decreases with power,
  so calibration noise cannot knot the SWR contours);
* every photographed tick reproduced within maximum_reference_error_pixels;
* each SWR contour has at most one gentle curvature inflection, and the low-SWR
  contours reach the visible face above the mask (angled-rest behaviour that
  mirrors a real cross-needle meter — see docs/cross-needle-meter-math.md, D1).

After changing a response, run this tool AND the C++ construction test together.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys

DEGREE = 5
COEFFICIENT_COUNT = DEGREE + 1
MODEL = "concave_bernstein_v1"


def evaluate(coefficients: list[float], normalized_power: float) -> float:
    # NOT clamped: SWR-contour construction passes values > 1 to extend a
    # movement a little past full scale (matches the C++ angleForNormalizedPower).
    t = normalized_power
    work = coefficients[:]
    for remaining in range(DEGREE, 0, -1):
        for index in range(remaining):
            work[index] = (1.0 - t) * work[index] + t * work[index + 1]
    return work[0]


def angle_for_power(scale: dict, coefficients: list[float], power: float) -> float:
    response = scale["response"]
    normalized_power = power / scale["values"][-1]
    deflection = evaluate(coefficients, normalized_power)
    return (response["start_radians"] +
            (response["end_radians"] - response["start_radians"]) * deflection)


def line_intersection(first_origin, first_angle, second_origin, second_angle):
    first = math.cos(first_angle), math.sin(first_angle)
    second = math.cos(second_angle), math.sin(second_angle)
    denominator = first[0] * second[1] - first[1] * second[0]
    delta = second_origin[0] - first_origin[0], second_origin[1] - first_origin[1]
    distance = (delta[0] * second[1] - delta[1] * second[0]) / denominator
    return (first_origin[0] + distance * first[0],
            first_origin[1] + distance * first[1])


def mask_boundary_y(boundary: list[list[float]], x: float) -> float:
    for first, second in zip(boundary, boundary[1:]):
        if first[0] <= x <= second[0]:
            if second[0] == first[0]:
                return min(first[1], second[1])
            fraction = (x - first[0]) / (second[0] - first[0])
            return first[1] + (second[1] - first[1]) * fraction
    return boundary[0][1] if x < boundary[0][0] else boundary[-1][1]


def guide_points(root: dict, guide: dict) -> list[tuple[float, float]]:
    forward = root["forward_scale"]
    reflected = root["reflected_scale"]
    fc = forward["response"]["coefficients"]
    rc = reflected["response"]["coefficients"]
    swr_value = guide["swr"]
    swr = math.inf if swr_value == "infinity" else float(swr_value)
    ratio = 1.0 if math.isinf(swr) else ((swr - 1.0) / (swr + 1.0)) ** 2
    forward_maximum = forward["values"][-1]
    clearance = root["swr"]["graph_clearance"]

    # Mirror the C++ swrGuidePath: sweep from the hidden convergence, extending
    # a movement past full scale as needed, to the first crossing of the common
    # envelope a fixed clearance short of the nearer power arc.
    def point(voltage: float) -> tuple[float, float]:
        forward_power = forward_maximum * voltage * voltage
        return line_intersection(
            tuple(forward["center"]), angle_for_power(forward, fc, forward_power),
            tuple(reflected["center"]),
            angle_for_power(reflected, rc, forward_power * ratio))

    def reaches(position: tuple[float, float]) -> bool:
        return (math.dist(position, forward["center"]) >= forward["radius"] - clearance or
                math.dist(position, reflected["center"]) >= reflected["radius"] - clearance)

    max_voltage = 1.6
    ending_voltage = max_voltage
    previous = 0.0
    probes = 256
    for i in range(1, probes + 1):
        v = max_voltage * i / probes
        if reaches(point(v)):
            lower, upper = previous, v
            for _ in range(48):
                middle = (lower + upper) * 0.5
                if reaches(point(middle)):
                    upper = middle
                else:
                    lower = middle
            ending_voltage = (lower + upper) * 0.5
            break
        previous = v

    samples = root["swr"]["curve_samples"]
    return [point(ending_voltage * s / samples) for s in range(samples + 1)]


def guide_diagnostics(root: dict, guide: dict) -> tuple[int, int]:
    points = guide_points(root, guide)
    boundary = root["mask"]["boundary"]
    visible = sum(1 for p in points if p[1] < mask_boundary_y(boundary, p[0]))
    curvatures: list[float] = []
    for index in range(2, len(points)):
        midpoint = ((points[index - 1][0] + points[index][0]) * 0.5,
                    (points[index - 1][1] + points[index][1]) * 0.5)
        if midpoint[1] >= mask_boundary_y(boundary, midpoint[0]):
            continue
        before, previous, current = points[index - 2:index + 1]
        first = previous[0] - before[0], previous[1] - before[1]
        second = current[0] - previous[0], current[1] - previous[1]
        fl, sl = math.hypot(*first), math.hypot(*second)
        if fl < 1e-9 or sl < 1e-9:
            continue
        turn = math.atan2(first[0] * second[1] - first[1] * second[0],
                          first[0] * second[0] + first[1] * second[1])
        curvatures.append(turn / ((fl + sl) * 0.5))
    smoothed = []
    for index in range(len(curvatures)):
        window = [curvatures[min(max(index + o, 0), len(curvatures) - 1)]
                  for o in range(-2, 3)]
        smoothed.append(sum(window) / len(window))
    threshold = max((abs(v) for v in smoothed), default=0.0) * 0.025
    previous_sign = 0
    reversals = 0
    for curvature in smoothed:
        if abs(curvature) <= threshold:
            continue
        sign = 1 if curvature > 0.0 else -1
        if previous_sign and sign != previous_sign:
            reversals += 1
        previous_sign = sign
    return reversals, visible


def main() -> int:
    default_geometry = (Path(__file__).resolve().parents[1] /
                        "resources/meterfaces/cross-needle-v12.json")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("geometry", nargs="?", type=Path, default=default_geometry)
    parser.add_argument("--check", action="store_true",
                        help="fail if movement or SWR mechanical constraints are violated")
    arguments = parser.parse_args()

    with arguments.geometry.open("r", encoding="utf-8") as source:
        root = json.load(source)

    valid = True
    for name in ("forward_scale", "reflected_scale"):
        scale = root[name]
        response = scale["response"]
        coefficients = response["coefficients"]
        errors = [abs(angle_for_power(scale, coefficients, power) - angle) * scale["radius"]
                  for power, angle in zip(scale["values"], scale["reference_angles_radians"])]
        maximum = max(errors)
        rms = math.sqrt(sum(e * e for e in errors) / len(errors))
        print(f"{name}: coefficients={json.dumps(coefficients)}")
        print(f"{name}: rms_reference_error_px={rms:.6f} max_reference_error_px={maximum:.6f}")
        second_differences = [coefficients[i + 2] - 2.0 * coefficients[i + 1] + coefficients[i]
                              for i in range(DEGREE - 1)]
        valid = valid and response["model"] == MODEL
        valid = valid and len(coefficients) == COEFFICIENT_COUNT
        valid = valid and abs(coefficients[0]) <= 1e-12
        valid = valid and abs(coefficients[-1] - 1.0) <= 1e-12
        valid = valid and all(a <= b + 1e-12 for a, b in zip(coefficients, coefficients[1:]))
        valid = valid and all(d <= 1e-12 for d in second_differences)
        valid = valid and maximum <= response["maximum_reference_error_pixels"]

    print("SWR guide diagnostics (reversals / visible samples):")
    for guide in root["swr"]["guides"]:
        reversals, visible = guide_diagnostics(root, guide)
        print(f"  {guide['label']}: reversals={reversals} visible_samples={visible}")
        valid = valid and reversals <= 1
        # The low-SWR contours must reach the visible face like a real meter.
        if guide["label"] in ("1.1", "1.2", "1.3"):
            valid = valid and visible >= 3

    if arguments.check and not valid:
        print("cross-needle response check failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
