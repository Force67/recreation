recreation_add_module(swf
  types.cc
  swf.cc
  shape.cc
  bitmap.cc
  text.cc
  movie.cc
  avm1.cc
  abc.cc
  decompile.cc
)

# rx::core supplies the scalar types; recreation::bethesda supplies the DEFLATE
# codec the archive readers already carry (SWF's CWS container and every
# lossless bitmap are zlib streams, and PNG export deflates on the way out).
# PRIVATE: nothing in the swf headers exposes either.
target_link_libraries(recreation_swf PUBLIC rx::core)
target_link_libraries(recreation_swf PRIVATE recreation::bethesda)

# Format-reader regression tests: each builds its input in memory and asserts
# what comes back out, so none of them needs an installed game.
recreation_add_test(avm1test recreation::swf)
