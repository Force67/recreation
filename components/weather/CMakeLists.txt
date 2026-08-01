recreation_add_module(weather
  weather.cc
  weather_loader.cc
  director.cc
)

# The director resolves WTHR sound forms through the audio catalog and writes
# the renderer's settings structs (header-only from rx::core's include root).
target_link_libraries(recreation_weather PUBLIC rx::core recreation::bethesda recreation::audio)
