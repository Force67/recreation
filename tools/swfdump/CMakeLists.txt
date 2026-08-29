add_executable(swfdump main.cc)
# bethesda + asset are only for --data: mounting a game's archives so the whole
# interface can be translated in one pass without unpacking it first.
target_link_libraries(swfdump PRIVATE recreation::swf recreation::bethesda rx::asset)
recreation_set_warnings(swfdump)
