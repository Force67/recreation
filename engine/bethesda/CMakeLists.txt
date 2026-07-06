recreation_add_module(bethesda
  game_profile.cc
  plugin.cc
  writer.cc
  edit_session.cc
  raw_rewriter.cc
  load_order.cc
  compression.cc
  string_writer.cc
  bsa.cc
  ba2.cc
  nif.cc
  hkx.cc
  hkx_physics.cc
  hkx_to_physics.cc
  starfield_mesh.cc
  material_db.cc
  converters.cc
  strings.cc
  script_attachment.cc
  tri.cc
  facegen.cc
  head_morph.cc
)

target_link_libraries(recreation_bethesda PUBLIC recreation::core recreation::asset recreation::physics)
