recreation_add_module(dialogue
  dialogue.cc
)

# Reads DIAL/INFO records and their VMAD fragments through bethesda; conditions
# (the INFO CTDA gate) are the native quest condition IR.
target_link_libraries(recreation_dialogue PUBLIC
  recreation::core
  recreation::bethesda
  recreation::quest)
