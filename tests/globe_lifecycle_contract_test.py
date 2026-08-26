#!/usr/bin/env python3
"""Guard the globe renderer lifecycle and projection fallback contracts."""

from pathlib import Path
import re
import sys


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing {signature}")
    opening = source.find("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated {signature}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    root = Path(sys.argv[1])
    globe = (root / "src/gui/map/GlobeMapView.cpp").read_text(encoding="utf-8")
    display = (root / "src/gui/map/MapDisplayWidget.cpp").read_text(encoding="utf-8")
    dialog = (root / "src/gui/PskReporterMapDialog.cpp").read_text(encoding="utf-8")

    destructor = function_body(globe, "GlobeMapView::~GlobeMapView()")
    require("cancelTileRequests();" in destructor,
            "globe destruction must cancel active tile replies")
    require("cleanupOpenGlResources();" in destructor,
            "globe destruction must release GL resources")

    cleanup = function_body(
        globe, "void GlobeMapView::cleanupOpenGlResources()")
    require(cleanup.find("m_program.reset();") < cleanup.find("doneCurrent();"),
            "shader program must be destroyed while the context is current")
    require("disconnect(glContext, &QOpenGLContext::aboutToBeDestroyed" in cleanup,
            "explicit cleanup must prevent late context re-entry")
    require("aboutToBeDestroyed" in globe and "Qt::DirectConnection" in globe,
            "context recreation must trigger synchronous GL cleanup")

    unavailable = function_body(
        globe, "void GlobeMapView::reportRendererUnavailable(")
    require("qCWarning(lcPskReporterGlobe)" in unavailable,
            "globe fallback reason must be recorded in categorized logs")

    cancel = function_body(globe, "void GlobeMapView::cancelTileRequests()")
    for operation in ("disconnect(", "reply->abort();", "reply->deleteLater();"):
        require(operation in cancel,
                f"tile cancellation is missing {operation}")

    fallback = function_body(
        display, "void MapDisplayWidget::handleGlobeUnavailable(")
    require("m_globeAvailable = false;" in fallback,
            "projection facade must remember globe failure")
    require("setProjectionMode(ProjectionMode::Flat);" in fallback,
            "projection facade must fall back to the flat renderer")
    require("globeAvailabilityChanged(false, reason)" in fallback,
            "projection facade must expose the fallback reason")

    require(re.search(
        r"globeAvailabilityChanged[\s\S]*?setChecked\(false\)"
        r"[\s\S]*?setEnabled\(false\)[\s\S]*?showGlobe[^\n]*false",
        dialog) is not None,
        "dialog must clear, disable and unpersist a failed globe renderer")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
