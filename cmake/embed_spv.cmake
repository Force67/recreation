file(READ ${SPV} hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
get_filename_component(dir ${HEADER} DIRECTORY)
file(MAKE_DIRECTORY ${dir})
file(WRITE ${HEADER}
  "// generated from ${SPV}, do not edit\n"
  "static const unsigned char k_${SYMBOL}[] = {\n${bytes}\n};\n")
if(DEFINED DXIL)
  file(READ ${DXIL} dxil_hex HEX)
  string(REGEX REPLACE "(..)" "0x\\1," dxil_bytes "${dxil_hex}")
  string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" dxil_bytes "${dxil_bytes}")
  file(APPEND ${HEADER}
    "static const unsigned char k_${SYMBOL}_dxil[] = {\n${dxil_bytes}\n};\n")
endif()
