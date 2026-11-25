# Hotelbot motor mount — dimensional concept

This model explains how to attach two hoverboard motor axles to a plywood deck using complete metal split clamps and steel backing plates. The user's existing holders do not have separate metal lower saddles. Their existing caps are **not assumed compatible** with the example lower saddles in this model.

**Confirmed:** four-hole rectangular pattern, 25.40 mm along the wheel axle and 31.75 mm across it, measured centre to centre. The user confirmed the second dimension is 1¼ inches total.

**Unconfirmed:** all other dimensions, including axle diameter, number/orientation of flats, usable clamping length, original cap shape, cap thickness, hole diameter, fastener thread, wheel diameter and width, and available clearance. The example uses a Ø16 mm single-flat axle with 14 mm total height, a 0.15 mm modelling clearance and M6 hardware. These values are placeholders, not manufacturing tolerances or a fastener recommendation. There is no structural analysis or load rating.

## View and edit

- `system-diagram.html`: interactive hackathon systems diagram with control/data, power and wheel-speed-loop views. Uses USB serial between Pi and ESP32 and each ZS-X11H V2's Speed pulse Out signal for ESP32 feedback. Odometry direction is estimated from the command, not measured. Motor Hall connectors remain attached to their drivers. Added fuse blocks and the hardware emergency-stop branch are omitted at the user's request; BMS protection ratings are unverified. Startup/command-timeout stops remain software functions.
- `motor-mount-3d.html`: standalone interactive viewer; open in a modern browser. Drag to orbit, scroll/pinch to zoom, click parts for descriptions. Includes exploded, underside and whole-deck views. The HTML works offline; download links expect the other files beside it.
- `motor_mount_assembly.step`: full coloured CAD assembly, in millimetres. Open in FreeCAD, Fusion, Onshape or another STEP-compatible CAD tool.
- `metal_lower_saddle_CONCEPT.step`, `metal_upper_cap_CONCEPT.step`, `steel_backing_plate_CONCEPT.step`: individual example metal parts. Their geometry is expressed in assembly coordinates.
- `assembly_VIEW_ONLY.stl`: combined reference mesh. **Do not print this as a structural assembly.**
- `hole_spacing_template_25.4x31.75mm.stl`: optional 65 × 65 × 3 mm printed spacing-check template. Print at 100% scale, with the large face on the bed; measure the print with calipers. Its four 6.6 mm holes are examples. Check against the original holder; do not use it to select drill sizes or drill the chassis until the actual hardware is measured.
- `axle_fit_gauge_MEASURE_FIRST.stl`: optional example profile gauge, Ø16 mm / 14 mm single-flat with modelling clearance. It does not claim to fit the actual axle. Print only after updating the measured axle parameters.
- `PETG_cable_clip_mockup.stl`: optional low-load cable clip. Example 7 mm cable passage and 3.6 mm screw holes. Check it against the actual cable; keep the cable away from the tyre and rotating hub. It is not a validated pull-out strain relief.
- `make_model.py`: editable CadQuery source. Parameters are at the top. `parameters.json` records the values used for the current export; editing that JSON alone does not rebuild the model.

## How the attachment works

1. Fit the stationary axle into a **metal lower saddle** that positively matches its flat profile and provides adequate clamping length. The rotating wheel/hub stays outside the deck.
2. Fit a matching **metal upper cap** over the axle. The existing cap can be evaluated after its shape and dimensions are recorded. Original hoverboard caps sometimes depend on features of the original chassis that this model does not reproduce.
3. The concept shows four vertical bolts passing through the cap, lower saddle, plywood and a steel backing plate, with washers and locking nuts. Existing holders with threaded blind holes may need a different arrangement; do not drill them through without evaluating that change.
4. The backing plate spreads load beneath the plywood. The axle flats transfer motor reaction torque to the metal clamp; the attachment then transfers that load into the chassis. Proper fit, clamp preload, fastener grade, wood crushing, axle retention and fatigue still need checking.
5. Leave clearance for the axle cable, rotating hub and tyre. Keep the battery low and add appropriate caster support: this drawing shows only the motor attachment, not a complete stable robot.

Prefer a complete compatible metal mount or retained original metal chassis sections if these are available. A Home Depot steel angle is useful reinforcement, but is not an axle-profiled lower saddle on its own. Do not use a 3D-printed saddle as the primary support for these motors based on this study.

## Measurements needed for a fitted revision

Record the axle's maximum diameter, dimensions across each flat, whether there is one flat or two, length of the straight clamping section, and distance from that section to the wheel face. Record the holder's overall dimensions, hole diameter or thread, and its underside profile. A side view and underside photo beside a ruler would help identify the existing holder.

## Rebuild

```bash
python -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python make_model.py
npm install
npm run build
```

The exporter checks that each CAD component is a valid positive-volume solid, that the example axle clears the clamp halves, and that the STEP assembly re-imports. These are geometry checks, not fit, strength or safety certification.

Source on hub-motor reaction torque: [Grin Technologies torque-arm guide](https://ebikes.ca/amfile/file/download/file/259/). CAD export reference: [CadQuery documentation](https://cadquery.readthedocs.io/en/latest/importexport.html).
