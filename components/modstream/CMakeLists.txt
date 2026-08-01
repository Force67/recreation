recreation_add_module(modstream
  mod_resource.cc
  content_hash.cc
  mod_catalog.cc
  manifest_codec.cc
  manifest_chunk.cc
  asset_request.cc
  content_store.cc
  content_provider.cc
  stream_filter.cc
  transfer_plan.cc
)

# Pure asset-distribution logic: scans a server mods directory into a manifest,
# hashes and caches content addressably on the client, and mounts downloaded
# resources back into the asset Vfs. The net layer carries the bytes; this module
# never touches a socket.
target_link_libraries(recreation_modstream PUBLIC
  rx::core
  rx::asset)

recreation_add_test(modstreamtest recreation::modstream rx::asset)
recreation_add_test(codec_fuzztest recreation::modstream rx::rpc)
if(TARGET codec_fuzztest AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 15 mis-fires -Wfree-nonheap-object when it inlines std::variant's
  # copy-assignment (the string/blob alternatives) at -O2. Known compiler bug,
  # not our code; silence it on this TU which copies RpcValue heavily.
  set_source_files_properties(codec_fuzztest.cc
    PROPERTIES COMPILE_OPTIONS -Wno-free-nonheap-object)
endif()
