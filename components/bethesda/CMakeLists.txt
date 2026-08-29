recreation_add_module(bethesda
  game_profile.cc
  plugin.cc
  tes3.cc
  writer.cc
  edit_session.cc
  raw_rewriter.cc
  load_order.cc
  compression.cc
  string_writer.cc
  bsa.cc
  ba2.cc
  nif.cc
  nif_gamebryo.cc
  hkx.cc
  hkx_physics.cc
  hkx_to_physics.cc
  hkx_anim.cc
  kf_anim.cc
  hkx_character.cc
  hkx_to_kinema.cc
  animation_data.cc
  starfield_mesh.cc
  material_db.cc
  biom.cc
  planet.cc
  shader_package.cc
  movement_type.cc
  converters.cc
  strings.cc
  script_attachment.cc
  tri.cc
  actor_stats.cc
  savegame.cc
  savegame_changeform.cc
  savegame_apply.cc
  savegame_papyrus.cc
  savegame_fixture.cc
  facegen.cc
  head_morph.cc
  worn_armor.cc
)

target_link_libraries(recreation_bethesda PUBLIC rx::core rx::asset rx::physics kinema::kinema)

# Format-reader regression tests: each parses a synthesised archive/record/mesh
# and asserts the decoded fields, so none of them needs game data on disk.
foreach(t
    ba2test biomtest writertest edit_sessiontest deflatetest string_writertest
    roundtriptest raw_rewritertest nif_fo4test nif_skyrim_shadertest nif_starfieldtest
    starfield_skinned_meshtest material_dbtest facemorphtest bgsm_fo4test
    dds_srgb_fo4test shader_packagetest)
  recreation_add_test(${t} recreation::bethesda rx::asset)
endforeach()
recreation_add_test(tritest recreation::bethesda)
recreation_add_test(savegametest recreation::bethesda)
recreation_add_test(savegame_changeformtest recreation::bethesda)
recreation_add_test(savegame_applytest recreation::bethesda)
recreation_add_test(savegame_papyrustest recreation::bethesda)
# Hostile-input sweep over all three reader layers. Deterministic, needs no
# game data, and is the gate that keeps a malformed .ess from being a crash.
recreation_add_test(savegame_fuzztest recreation::bethesda)
recreation_add_test(worn_armortest recreation::bethesda)
recreation_add_test(vmad_fo4test recreation::bethesda)
