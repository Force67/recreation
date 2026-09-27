# Regression checks over contracts recreation depends on but does not own: the
# code under test lives in the sibling rx and equilibrium repos, so these cannot
# sit beside it. They pin the behaviour recreation relies on.
recreation_add_test(optiontest equilibrium::base)        # base::Option env binding
recreation_add_test(featuretest rx::core)                # rx feature registry
recreation_add_test(settings_initest rx::render rx::app) # render .ini, platform config
recreation_add_test(rpctest rx::rpc)                     # rpc message codec
recreation_add_test(skytest)                             # CPU reference for rx's Hillaire sky
if(TARGET settings_initest)
  target_compile_definitions(settings_initest PRIVATE
    RECREATION_RX_CONFIG_DIR="${RECREATION_RX_DIR}/config"
    RECREATION_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
endif()
