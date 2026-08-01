recreation_add_module(dialogue
  dialogue.cc
  voice.cc
)

# Reads DIAL/INFO records and their VMAD fragments through bethesda; conditions
# (the INFO CTDA gate) are the native quest condition IR.
target_link_libraries(recreation_dialogue PUBLIC
  rx::core
  recreation::bethesda
  recreation::quest)

recreation_add_test(dialoguetest recreation::dialogue recreation::bethesda)
recreation_add_test(voicetest recreation::dialogue recreation::bethesda)
