"""Parametric mounting study. Millimetres. Example dimensions, NOT a released design.

Run: python make_model.py
Edit P below after measuring the actual motor, axle and original brackets.
Metal clamps/backers are not approved for printing or fabrication from this study.
"""
from pathlib import Path
import json
import math
import cadquery as cq

OUT = Path(__file__).resolve().parent
P = {
    "deck_width": 350, "deck_length": 450, "deck_thickness": 12,
    "wheel_diameter": 165, "wheel_width": 65, "wheel_center_x": 230,
    "axle_diameter": 16, "axle_across_flats": 14,
    "axle_profile": "single_flat",  # or "double_flat"; illustrative only
    "axle_center_z": 34, "axle_inner_x": 125,
    "bore_clearance": 0.15,  # modelling allowance, NOT a recommended fit
    "clamp_length": 45, "clamp_width": 70, "clamp_height": 44,
    "clamp_center_x": 147.5, "split_gap": 0.6,
    "bolt_pitch_x": 25.4, "bolt_pitch_y": 31.75, "bolt_hole": 6.6,
    "backer_length": 65, "backer_width": 90, "backer_thickness": 3,
}


def box(dx, dy, dz, at):
    return cq.Workplane("XY").box(dx, dy, dz).translate(at).val()


def cyl(radius, length, start, direction=(0, 0, 1)):
    return cq.Solid.makeCylinder(radius, length, cq.Vector(*start), cq.Vector(*direction))


def flat_axle(length, start_x, clearance=0):
    """Cylinder along X intersected with one or two flat planes."""
    r = P["axle_diameter"] / 2 + clearance
    a = P["axle_across_flats"]
    z = P["axle_center_z"]
    if P["axle_profile"] == "single_flat":
        low, high = z + P["axle_diameter"] / 2 - a - clearance, z + r
    elif P["axle_profile"] == "double_flat":
        low, high = z - a / 2 - clearance, z + a / 2 + clearance
    else:
        raise ValueError("axle_profile must be single_flat or double_flat")
    return cyl(r, length, (start_x, 0, z), (1, 0, 0)).intersect(
        box(length + 2, 2 * r + 2, high - low, (start_x + length / 2, 0, (low + high) / 2)))


PARTS = []


def add(name, shape, color, group, role, explode=(0, 0, 0)):
    assert shape.isValid(), f"Invalid solid: {name}"
    assert shape.Volume() > 0, f"Empty solid: {name}"
    PARTS.append({"name": name, "shape": shape, "color": color, "group": group,
                  "role": role, "explode": explode})


def holes(shape, xcenter, start=-30, length=130):
    for dx in (-P["bolt_pitch_x"] / 2, P["bolt_pitch_x"] / 2):
        for y in (-P["bolt_pitch_y"] / 2, P["bolt_pitch_y"] / 2):
            shape = shape.cut(cyl(P["bolt_hole"] / 2, length, (xcenter + dx, y, start)))
    return shape


def washer(x, y, z):
    return cyl(6, 1.2, (x, y, z)).cut(cyl(3.3, 1.2, (x, y, z)))


def hexagon(x, y, z, height, diameter):
    return cq.Workplane("XY").polygon(6, diameter).extrude(height).translate((x, y, z)).val()


def build():
    deck = box(P["deck_width"], P["deck_length"], P["deck_thickness"], (0, 0, P["deck_thickness"] / 2))
    for side in (-1, 1):
        deck = holes(deck, side * P["clamp_center_x"])
    add("Plywood deck — 350 × 450 × 12 mm", deck, "#bb9465", "frame", "deck", (0, 0, -20))

    z = P["axle_center_z"]
    L, W, H = P["clamp_length"], P["clamp_width"], P["clamp_height"]
    center = P["clamp_center_x"]
    blank = box(L, W, H, (center, 0, z))
    bore = flat_axle(L + 2, center - L / 2 - 1, P["bore_clearance"])
    blank = holes(blank.cut(bore), center)
    gap = P["split_gap"]
    lower = blank.intersect(box(L + 2, W + 2, H / 2 - gap / 2, (center, 0, z - H / 4 - gap / 4)))
    upper = blank.intersect(box(L + 2, W + 2, H / 2 - gap / 2, (center, 0, z + H / 4 + gap / 4)))
    backer = holes(box(P["backer_length"], P["backer_width"], P["backer_thickness"],
                      (center, 0, -P["backer_thickness"] / 2)), center)

    for side, label in ((1, "Right"), (-1, "Left")):
        def reflect(shape):
            return shape if side == 1 else shape.mirror("YZ")
        group = label.lower()
        add(f"{label} metal lower axle saddle", reflect(lower), "#74818b", group, "lower", (0, 0, 0))
        add(f"{label} metal upper axle cap", reflect(upper), "#a8b4be", group, "upper", (0, 0, 65))
        add(f"{label} steel backing plate", reflect(backer), "#5d6973", group, "backer", (0, 0, -60))
        shaft_length = P["wheel_center_x"] - P["wheel_width"] / 2 - P["axle_inner_x"]
        shaft = flat_axle(shaft_length, P["axle_inner_x"])
        # Cable exit represented by a short axial bore through the shaft end.
        shaft = shaft.cut(cyl(2.5, shaft_length, (P["axle_inner_x"], 0, z), (1, 0, 0)))
        add(f"{label} example D-flat axle", reflect(shaft), "#c7cbd0", group, "axle", (side * 95, 0, 0))

        x0 = P["wheel_center_x"] - P["wheel_width"] / 2
        wheel = cyl(P["wheel_diameter"] / 2, P["wheel_width"], (x0, 0, z), (1, 0, 0))
        hub_radius = P["wheel_diameter"] * 0.33
        hub = cyl(hub_radius, P["wheel_width"] + 2, (x0 - 1, 0, z), (1, 0, 0))
        tire = wheel.cut(cyl(hub_radius, P["wheel_width"], (x0, 0, z), (1, 0, 0)))
        add(f"{label} tyre — reference envelope", reflect(tire), "#252b32", group, "wheel", (side * 95, 0, 0))
        add(f"{label} motor hub — reference envelope", reflect(hub), "#59616a", group, "wheel", (side * 95, 0, 0))
        # Face screws establish scale; motor internals and OEM features are not modelled.
        for i in range(6):
            a = i * math.tau / 6
            screw = cyl(2.5, 1, (x0 + P["wheel_width"] + 1, 42 * math.cos(a), z + 42 * math.sin(a)), (1, 0, 0))
            add(f"{label} reference hub screw {i + 1}", reflect(screw), "#c2c8cd", group, "wheel", (side * 95, 0, 0))

        for ix, dx in enumerate((-P["bolt_pitch_x"] / 2, P["bolt_pitch_x"] / 2)):
            for iy, y in enumerate((-P["bolt_pitch_y"] / 2, P["bolt_pitch_y"] / 2)):
                x = side * (center + dx)
                num = ix * 2 + iy + 1
                bolt = cyl(3, 70, (x, y, -12.8)).fuse(hexagon(x, y, 57.2, 4, 11.55))
                # Hex socket in each head.
                bolt = bolt.cut(hexagon(x, y, 59.2, 3, 5))
                add(f"{label} illustrative M6 through-bolt {num}", bolt, "#c0c8cf", group, "bolt", (0, 0, 120))
                add(f"{label} top washer {num}", washer(x, y, 56), "#d0d6dc", group, "washer_top", (0, 0, 90))
                add(f"{label} bottom washer {num}", washer(x, y, -4.2), "#b8c0c8", group, "washer_bottom", (0, 0, -80))
                nut = hexagon(x, y, -10.2, 6, 11.55).cut(cyl(3, 6, (x, y, -10.2)))
                add(f"{label} locking nut envelope {num}", nut, "#9faab4", group, "nut", (0, 0, -100))

    # Optional printed routing clip: away from the clamp and the rotating wheel.
    clip = box(24, 18, 8, (105, 0, 16)).cut(cyl(3.5, 18, (105, -9, 18), (0, 1, 0)))
    clip = clip.cut(cyl(1.8, 8, (96, 0, 12))).cut(cyl(1.8, 8, (114, 0, 12)))
    add("Optional PETG cable clip — mockup", clip, "#c77b42", "right", "printed", (0, -55, 10))

    return lower, upper, backer, clip


def export():
    lower, upper, backer, clip = build()
    assembly = cq.Assembly(name="Hotelbot motor mount — unverified concept")
    meshes = []
    for p in PARTS:
        assembly.add(p["shape"], name=p["name"], color=cq.Color(p["color"]))
        verts, tris = p["shape"].tessellate(0.22, 0.18)
        meshes.append({k: v for k, v in p.items() if k != "shape"} | {
            "vertices": [c for v in verts for c in (v.x, v.y, v.z)],
            "indices": [i for t in tris for i in t]})
    assembly.export(str(OUT / "motor_mount_assembly.step"))
    for name, shape in [("metal_lower_saddle_CONCEPT", lower), ("metal_upper_cap_CONCEPT", upper),
                        ("steel_backing_plate_CONCEPT", backer)]:
        cq.exporters.export(shape, str(OUT / (name + ".step")))
    # Only the cable clip and fit-check gauge are supplied as printable STL files.
    clip_local = clip.translate((-105, 0, -12))
    cq.exporters.export(clip_local, str(OUT / "PETG_cable_clip_mockup.stl"))
    gauge = box(5, 28, 28, (2.5, 0, P["axle_center_z"]))
    gauge = gauge.cut(flat_axle(7, -1, P["bore_clearance"]))
    gauge = gauge.translate((0, 0, -P["axle_center_z"])).rotate((0, 0, 0), (0, 1, 0), -90)
    cq.exporters.export(gauge, str(OUT / "axle_fit_gauge_MEASURE_FIRST.stl"))
    # Combined assembly mesh is for viewing, never for load-bearing printing.
    cq.exporters.export(cq.Compound.makeCompound([p["shape"] for p in PARTS]), str(OUT / "assembly_VIEW_ONLY.stl"))
    detail = PARTS[0]["shape"].intersect(box(115, 150, 12, (132.5, 0, 6)))
    verts, tris = detail.tessellate(0.22, 0.18)
    meshes.append({"name": "Plywood deck — cropped detail", "color": "#bb9465", "group": "detail", "role": "deck", "explode": [0, 0, -20],
                   "vertices": [c for v in verts for c in (v.x, v.y, v.z)], "indices": [i for t in tris for i in t]})
    (OUT / "geometry.json").write_text(json.dumps({"parameters": P, "parts": meshes}, separators=(",", ":")))
    (OUT / "parameters.json").write_text(json.dumps(P, indent=2) + "\n")
    template = holes(box(65, 65, 3, (center := P["clamp_center_x"], 0, 1.5)), center)
    cq.exporters.export(template.translate((-center, 0, 0)), str(OUT / "hole_spacing_template_25.4x31.75mm.stl"))
    # Basic geometric checks, deliberately distinct from structural validation.
    axle = next(p["shape"] for p in PARTS if p["name"] == "Right example D-flat axle")
    for shape in (lower, upper):
        assert axle.intersect(shape).Volume() < 1e-5, "Axle interferes with clamp"
    assert lower.intersect(upper).Volume() < 1e-5, "Clamp halves interfere"
    roundtrip = cq.importers.importStep(str(OUT / "motor_mount_assembly.step"))
    assert roundtrip.val().isValid(), "STEP round-trip failed"
    print(f"Exported {len(PARTS)} valid components; STEP re-import and axle clearance checks passed.")
    print("Example only. Axle dimensions, mount strength and assembled hardware are not verified.")


if __name__ == "__main__":
    export()
