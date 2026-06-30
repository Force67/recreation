#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

// These wrap audio, visual, and weather systems not yet built, so most are no-ops.

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgI;
using ext::ArgO;
namespace st = state;

void RegisterAudioVisualExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };

  // EffectShader: an attached shader is a pure visual effect on a reference.
  reg.Register("EffectShader", "Play", noop);
  reg.Register("EffectShader", "Stop", noop);

  // ImageSpaceModifier: full-screen post-process tints/fades.
  reg.Register("ImageSpaceModifier", "Apply", noop);
  reg.Register("ImageSpaceModifier", "ApplyCrossFade", noop);
  reg.Register("ImageSpaceModifier", "PopTo", noop);
  reg.Register("ImageSpaceModifier", "Remove", noop);
  reg.Register("ImageSpaceModifier", "RemoveCrossFade", noop);

  // Message: Show/ShowAsHelpMessage return the pressed-button index (0).
  reg.Register("Message", "ResetHelpMessage", noop);
  reg.Register("Message", "Show",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Message", "ShowAsHelpMessage",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });

  // MusicType: background music stack push/pop.
  reg.Register("MusicType", "Add", noop);
  reg.Register("MusicType", "Remove", noop);

  // Package: the data the AI package layer would expose; none yet.
  reg.Register("Package", "GetOwningQuest",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Package", "GetTemplate",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });

  // ShaderParticleGeometry: particle emitter scaling.
  reg.Register("ShaderParticleGeometry", "Apply", noop);
  reg.Register("ShaderParticleGeometry", "Remove", noop);

  // SoundCategory: mixer-channel control.
  reg.Register("SoundCategory", "Mute", noop);
  reg.Register("SoundCategory", "Pause", noop);
  reg.Register("SoundCategory", "SetFrequency", noop);
  reg.Register("SoundCategory", "SetVolume", noop);
  reg.Register("SoundCategory", "UnMute", noop);
  reg.Register("SoundCategory", "UnPause", noop);

  // Sound: Play/PlayAndWait return a sound instance id (0).
  reg.Register("Sound", "Play",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Sound", "PlayAndWait",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Sound", "SetInstanceVolume", noop);
  reg.Register("Sound", "StopInstance", noop);

  // VisualEffect: a model-attached visual effect.
  reg.Register("VisualEffect", "Play", noop);
  reg.Register("VisualEffect", "Stop", noop);

  // Weather: the engine sky system owns weather; we model the script-set current
  // weather as runtime state on a fixed owner ObjectRef{1}.
  reg.Register("Weather", "SetActive", [](VirtualMachine&, ObjectRef, Args& a) {
    st::SetRef(ObjectRef{1}, "currentWeather", ArgO(a, 0));
    return Value();
  });
  reg.Register("Weather", "ForceActive", noop);
  reg.Register("Weather", "GetCurrentWeather", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Object(st::GetRef(ObjectRef{1}, "currentWeather"));
  });
  reg.Register("Weather", "GetOutgoingWeather",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Weather", "FindWeather",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Weather", "GetClassification",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Weather", "GetSkyMode",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Weather", "GetCurrentWeatherTransition",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Float(1.0f); });
  reg.Register("Weather", "ReleaseOverride", noop);
}

}  // namespace rec::script::skyrim
