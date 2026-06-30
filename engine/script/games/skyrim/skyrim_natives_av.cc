#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

// Sound routes to the engine audio system. The visual and weather wrappers track
// what a script sets, awaiting their renderers.

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgF;
using ext::ArgI;
using ext::ArgO;
using ext::Resolve;
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

  // SoundCategory: a change scales the master bus through the audio system, and
  // the mute and pause states are tracked on the category for the engine to read.
  reg.Register("SoundCategory", "Mute", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).SetSoundCategoryVolume(self, 0.0f);
    st::SetFlag(self, "muted", true);
    return Value();
  });
  reg.Register("SoundCategory", "UnMute", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).SetSoundCategoryVolume(self, 1.0f);
    st::SetFlag(self, "muted", false);
    return Value();
  });
  reg.Register("SoundCategory", "SetVolume", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetSoundCategoryVolume(self, ext::ArgF(a, 0));
    return Value();
  });
  reg.Register("SoundCategory", "SetFrequency", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFloat(self, "frequency", ext::ArgF(a, 0));
    return Value();
  });
  reg.Register("SoundCategory", "Pause", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetFlag(self, "paused", true);
    return Value();
  });
  reg.Register("SoundCategory", "UnPause", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetFlag(self, "paused", false);
    return Value();
  });

  // Sound: play the resolved asset through the audio system and return its voice
  // id, which StopInstance and SetInstanceVolume then act on.
  auto play = [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Int(Resolve(bindings).PlaySound(self, ext::ArgO(a, 0)));
  };
  reg.Register("Sound", "Play", play);
  reg.Register("Sound", "PlayAndWait", play);
  reg.Register("Sound", "StopInstance", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    Resolve(bindings).StopSoundInstance(ArgI(a, 0));
    return Value();
  });
  reg.Register("Sound", "SetInstanceVolume", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    Resolve(bindings).SetSoundInstanceVolume(ArgI(a, 0), ext::ArgF(a, 1));
    return Value();
  });

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
