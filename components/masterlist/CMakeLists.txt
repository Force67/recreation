recreation_add_module(masterlist
  json.cc
  load_order_digest.cc
  masterlist_client.cc
  announcer.cc
  async_list.cc
)

# The game's side of the server list: announce a hosted session, keep it alive,
# read back what everyone else is hosting. The list itself is a separate service
# (masterlist/, its own repo); this module owns the wire shape and the two
# background workers that keep it off the frame thread.
target_link_libraries(recreation_masterlist PUBLIC
  rx::core
  rx::http)

recreation_add_test(masterlisttest recreation::masterlist)
