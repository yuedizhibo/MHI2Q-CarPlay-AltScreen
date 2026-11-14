#!/usr/bin/env python3
"""Source-only CARD geometry contract; no compiler, QNX SDK or output files.

Checks the C assignments used by /info predeclaration, HMI-ready resolution
and pre-HMI fallback, plus the existing same-session card-position wiring.
This is deliberately a source contract, not an iPhone or QNX runtime test.
Run with --self-test to also verify rejection of representative regressions.
"""
import argparse
from pathlib import Path
import re


def compact(text):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    return re.sub(r"\s+", "", text)


def body(source, name):
    # Mask comments and literals so their braces cannot affect function bounds.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: " " * len(m.group()), source, flags=re.S)
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", masked)
    if not match:
        raise ValueError("missing function: " + name)
    start = match.end()
    depth = 1
    for index in range(start, len(masked)):
        depth += (masked[index] == "{") - (masked[index] == "}")
        if depth == 0:
            return source[start:index]
    raise ValueError("unclosed function: " + name)


def check(sources):
    airplay, build, native, core = sources
    errors = []

    def require(text, fragment, label):
        if compact(fragment) not in compact(text):
            errors.append(label + ": missing " + fragment)

    geometry = body(airplay, "alt_layout_rects")
    branch = re.search(r"if\s*\(alt_phone_card_profile\(\)\)\s*\{(.*?)\}\s*else\s*\{(.*?)\}",
                       geometry, re.S)
    if not branch:
        errors.append("geometry must depend on card profile, not card position")
    else:
        for code, expected, label in ((branch[1], (300, 88, 840, 362), "CARD FULL"),
                                      (branch[2], (370, 72, 700, 378), "default FULL")):
            values = []
            for field in ("x", "y", "w", "h"):
                assignments = re.findall(r"r\.full_" + field + r"\s*=\s*(\d+)u\s*;", code)
                values.append(int(assignments[0]) if len(assignments) == 1 else None)
            if tuple(values) != expected:
                errors.append("%s: expected %s, got %s" % (label, expected, values))
            if values[1] is not None and values[3] is not None:
                if values[1] + values[3] != 450:
                    errors.append(label + ": bottom must stay 450")
            if all(value is not None for value in values):
                x, y, width, height = values
                for canvas_height in (455, 540, 542):
                    if not (0 <= x < x + width <= 1440 and 0 <= y < y + height <= canvas_height):
                        errors.append("%s: outside 1440x%d canvas" % (label, canvas_height))
        require(branch[1], "r.card = 1;", "CARD profile")
        require(branch[2], "r.card = 0;", "default profile")
    require(geometry, "r.small_x = 490u; r.small_w = 460u;", "SMALL horizontal bounds")
    if "alt_phone_card_position" in geometry or "alt_phone_card_mode" in geometry:
        errors.append("geometry must be identical for LEFT/TOP/RIGHT")

    ready = body(airplay, "alt_load_measured_k1004_safe_area")
    fallback = body(airplay, "alt_resolve_cluster_safe_area")
    declared = body(airplay, "make_cluster_layout_view_areas")
    for code, prefix, label in ((ready, "r", "HMI ready"),
                                (fallback, "out->", "pre-HMI fallback"),
                                (declared, "full", "/info predeclaration")):
        require(code, "struct alt_layout_rects g = alt_layout_rects();", label)
        separator = "" if prefix.endswith("->") else "."
        for field in ("x", "y", "w", "h"):
            require(code, prefix + separator + field + " = g.full_" + field + ";", label)
    require(ready, "r.physical_h = g.full_h;", "HMI-ready diagnostic height")
    require(ready, "physical_y = (int64_t)r.y + (int64_t)r.renderer_dy;", "physical top")
    require(fallback, "out->physical_h = g.full_h;", "fallback diagnostic height")
    require(fallback, "out->physical_y = g.card ? (int)g.full_y : 75;", "fallback diagnostic top")
    require(fallback, "*out = r;", "ready overrides fallback")
    for field, value in (("y", "72u"), ("h", "378u")):
        require(ready, "r." + field + " = " + value + ";", "HMI-ready SMALL")
        require(declared, "small." + field + " = " + value + ";", "predeclared SMALL")
    require(ready, "r.renderer_dx = small_dx; r.renderer_dy = small_dy;", "SMALL pan")
    require(declared, "safe = rect_dict(full.w, full.h, full.x, full.y);", "FULL wire rectangle")
    require(declared, "safe = rect_dict(small.w, small.h, small.x, small.y);", "SMALL wire rectangle")
    require(airplay, "areas = make_cluster_layout_view_areas(d->width_pixels, d->height_pixels, two_area_capable);",
            "/info uses predeclared geometry")

    urls = body(airplay, "alt_cluster_url_for_card")
    for mode, position in enumerate(("LEFT", "TOP", "RIGHT"), 1):
        require(urls, 'case %d: *layout_name = "card-%s"; return CP_ALT_CLUSTER_CARD_URL_%s;' %
                (mode, position.lower(), position), "card URL selection")
        require(core, "maneuverLayout=" + {"LEFT": "leftaligned", "TOP": "topaligned",
                                          "RIGHT": "rightaligned"}[position], "phone layout URL")
    require(body(airplay, "alt_phone_card_mode"), 'if (strcmp(view, "FULL") ||', "SMALL remains map-only")
    require(native, "desired_phone_card_mode = alt_phone_card_mode();", "live card polling")
    require(native, "native_phone_card_retarget_locked(slot, desired_phone_card_mode, wheel_now)", "live retarget")
    require(native, "native_phone_card_refresh_due_locked(slot, wheel_now)", "same-session refresh")
    require(native, "send_rc = alt_send_cluster_card_ui(receiver, stream, generation, ui_send_seq, phone_card_send_mode);",
            "showUI reuses receiver/stream/generation")

    for marker in ("safe_yh_mapping=default72_cardfull88_bottom450",
                   "safearea_revision=V38FIX1_CARD_FULL_88_450",
                   "logo-card-full-safe-88-450", "logo-card-small-safe-72-450", "CARD_FULL_TOP88"):
        require(airplay, marker, "runtime marker")
        require(build, "'" + marker + "'", "built ELF marker check")
    require(airplay, "g.full_x, g.full_y, g.full_w, g.full_h,", "profile log uses actual FULL geometry")
    require(build, 'python3 "$SRC/tools/validate_card_safe_area.py"', "build preflight")
    if "vertical_inset_top72_bottom450" in airplay + build or "V35_OEM_X_VERTICAL_72_450" in airplay + build:
        errors.append("obsolete unconditional geometry marker")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    src = Path(__file__).resolve().parent.parent
    paths = (src / "p1404_airplay.c", src / "build_qnx_arm.sh",
             src / "p1404_cockpit_native.c", src / "altscreen_core.h")
    sources = [path.read_text(encoding="utf-8") for path in paths]
    errors = check(sources)
    if errors:
        for error in errors:
            print("CARD_SAFEAREA_FAIL: " + error)
        return 1
    print("CARD_SAFEAREA_STATIC=PASS card_full=300,88,840,362 default_full=370,72,700,378 small=490,72,460,378 bottom=450")
    print("CARD_SAFEAREA_PATHS=PASS predeclared,hmi_ready,fallback positions=LEFT,TOP,RIGHT same_session=SOURCE_CHECK_ONLY")
    if args.self_test:
        mutations = ((0, "r.full_y = 88u", "r.full_y = 72u"),
                     (0, "r.full_h = 362u", "r.full_h = 378u"),
                     (0, "full.y = g.full_y", "full.y = 72u"),
                     (0, "r.h = g.full_h", "r.h = 378u"),
                     (0, "out->h = g.full_h", "out->h = 378u"),
                     (0, "physical_y = (int64_t)r.y", "physical_y = 72"),
                     (0, "r.y = 72u", "r.y = 88u"),
                     (2, "desired_phone_card_mode = alt_phone_card_mode()", "desired_phone_card_mode = 1"))
        for index, before, after in mutations:
            candidate = sources.copy()
            if before not in candidate[index]:
                raise ValueError("self-test mutation no longer matches: " + before)
            candidate[index] = candidate[index].replace(before, after, 1)
            if not check(candidate):
                raise ValueError("self-test failed to reject: " + after)
        print("CARD_SAFEAREA_SELF_TEST=PASS rejected_regressions=%d" % len(mutations))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
