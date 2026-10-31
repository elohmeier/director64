// Source-free signal oracle: measure what the actual AAudio callback emits.
#define DIRECTOR64_ANDROID_VALIDATION 1
#define main mixer_contract_main
#include "test_android_mixer.c"
#undef main

int main(void) {
  mixer_contract_main();
  validation_samples = validation_nonzero = validation_clipped = 0;
  validation_energy = validation_peak = 0;
  atomic_store(&suspended, false);
  float output[20];
  render(NULL, NULL, output, 10);
  assert(validation_samples == 20 && validation_nonzero == 0);
  assert(validation_energy == 0);

  sound_t *sound = calloc(1, sizeof(*sound));
  sound->rate = (unsigned)output_rate;
  sound->channels = 1;
  sound->frames = 1;
  sound->pcm = malloc(sizeof(int16_t));
  sound->pcm[0] = 16384;
  atomic_init(&sound->references, 1);
  voices[0] = (voice_t){.sound = sound, .rate = 1};
  gains[0] = 1;
  atomic_store(&master, 1);
  render(NULL, NULL, output, 10);
  assert(validation_samples == 40 && validation_nonzero == 2);
  assert(validation_energy == 0.5 && validation_peak == 0.5);
  char report[512];
  audio_validation_report(report, sizeof(report));
  assert(strstr(report, "\"nonzero\":2") && strstr(report, "\"energy\":0.5"));
  puts("Android validation: silence and a real stereo callback impulse distinguished");
  return 0;
}
