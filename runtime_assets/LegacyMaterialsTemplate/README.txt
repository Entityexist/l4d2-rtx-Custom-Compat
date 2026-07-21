LegacyMaterials V20.3.0 — Semantic PBR Compiler

1. Run GET_TEXCONV.bat once.
2. In game press Capture Materials.
3. Move through the scene so the real Source VMTs and runtime variants are observed.
4. Press Resolve + Export PBR.
5. Inspect Materials/<VMT path>/material.json, provenance_graph.json and debug_png.
6. Open manifests/material_audit.html for grades, activation reasons and warnings.

Material stack:
- Materials_Auto.usda: regenerated, only Verified/Usable auto-active materials.
- Materials_User.usda: protected manual corrections, never overwritten.
- Materials_Review.usda: resolved but inactive materials, intentionally not loaded.
- Materials.usda: wrapper layering manual corrections over generated output.

The compiler resolves declared VTFs from loose files or VPK, decodes SSBump/DXT5nm/BC5,
exports opacity and detail authoring assets, preserves WorldVertexTransition layers,
and quarantines results that are incomplete, suspicious or require runtime composition.

Manual per-material rules are stored in config/material_overrides.toml.
It never edits another Remix project.
