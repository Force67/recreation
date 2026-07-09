recreation_add_module(weather
  weather.cc
  weather_loader.cc
)

target_link_libraries(recreation_weather PUBLIC rx::core recreation::bethesda)
