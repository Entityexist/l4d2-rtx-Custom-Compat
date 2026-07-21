from pathlib import Path
root=Path(__file__).resolve().parents[1]
checks={
 'src/source_material_bridge.cpp':['RemixSourceMaterialBridge_GetApi','bridge_query_material','__cdecl'],
 'src/components/modules/material_exporter.cpp':['query_source_bridge_material','runtime_bridge','bridge_semantic_signature','$phongexponenttexture','runtime_water_or_refract'],
 'src/components/modules/material_exporter.hpp':['m_source_material_bridge_enabled'],
 'src/components/modules/game_settings.cpp':['material_source_bridge_enabled'],
 'src/components/modules/imgui.cpp':['Publish Source Material Bridge'],
}
for rel,tokens in checks.items():
 p=root/rel
 if not p.exists(): raise SystemExit(f'missing {rel}')
 text=p.read_text(encoding='utf-8',errors='ignore')
 for token in tokens:
  if token not in text: raise SystemExit(f'{rel}: missing {token}')
print('V21.9 Source Material Bridge validation passed.')
