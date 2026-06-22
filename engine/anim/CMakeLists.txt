recreation_add_module(anim
  pose.cc
  locomotion.cc
  foot_ik.cc
)
target_link_libraries(recreation_anim PUBLIC recreation::core recreation::asset)
