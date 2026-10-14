#include <fluidsynth.h>
#include "fluid_voice.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Pinned-source FluidSynth oracle used by host CI.
 *
 * IMPORTANT: this probe intentionally uses FluidSynth's internal
 * fluid_voice_gen_value() rather than the public fluid_voice_gen_get().
 * fluid_voice_gen_get() exposes only generator.val (the nominal/base value),
 * while synthesis uses generator.val + generator.mod + generator.nrpn.
 * fluid_voice_gen_value() returns that effective value and is therefore the
 * correct oracle for our ResolvedZone + ModPlan comparison.
 *
 * This file is compiled inside the pinned FluidSynth source tree so the
 * internal header/type layout always matches the linked reference build.
 */

static const int kGenerators[] = {
    6,   /* vibLfoToPitch */
    8,   /* initialFilterFc */
    9,   /* initialFilterQ */
    15,  /* chorusEffectsSend */
    16,  /* reverbEffectsSend */
    17,  /* pan */
    48,  /* initialAttenuation */
    51,  /* coarseTune */
    52,  /* fineTune */
    56,  /* scaleTuning */
};

static int check_ok(int rc, const char *what) {
    if (rc != FLUID_OK) {
        fprintf(stderr, "FluidSynth call failed: %s\n", what);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <sf2> <bank> <program> <key> <velocity>\n", argv[0]);
        return 2;
    }

    const char *sf2_path = argv[1];
    const int bank = atoi(argv[2]);
    const int program = atoi(argv[3]);
    const int key = atoi(argv[4]);
    const int velocity = atoi(argv[5]);

    if (bank < 0 || program < 0 || program > 127 || key < 0 || key > 127 ||
        velocity < 1 || velocity > 127) {
        fprintf(stderr, "invalid bank/program/key/velocity\n");
        return 2;
    }

    fluid_settings_t *settings = new_fluid_settings();
    if (!settings) {
        fprintf(stderr, "new_fluid_settings failed\n");
        return 3;
    }
    fluid_settings_setint(settings, "synth.polyphony", 64);
    fluid_settings_setnum(settings, "synth.sample-rate", 44100.0);

    fluid_synth_t *synth = new_fluid_synth(settings);
    if (!synth) {
        fprintf(stderr, "new_fluid_synth failed\n");
        delete_fluid_settings(settings);
        return 3;
    }

    const int sfid = fluid_synth_sfload(synth, sf2_path, 0);
    if (sfid < 0) {
        fprintf(stderr, "fluid_synth_sfload failed: %s\n", sf2_path);
        delete_fluid_synth(synth);
        delete_fluid_settings(settings);
        return 4;
    }

    if (!check_ok(fluid_synth_program_select(synth, 0, sfid, bank, program), "program_select") ||
        !check_ok(fluid_synth_cc(synth, 0, 1, 0), "CC1") ||
        !check_ok(fluid_synth_cc(synth, 0, 7, 100), "CC7") ||
        !check_ok(fluid_synth_cc(synth, 0, 10, 64), "CC10") ||
        !check_ok(fluid_synth_cc(synth, 0, 11, 127), "CC11") ||
        !check_ok(fluid_synth_cc(synth, 0, 91, 0), "CC91") ||
        !check_ok(fluid_synth_cc(synth, 0, 93, 0), "CC93") ||
        !check_ok(fluid_synth_pitch_wheel_sens(synth, 0, 2), "pitch_wheel_sens") ||
        !check_ok(fluid_synth_pitch_bend(synth, 0, 8192), "pitch_bend") ||
        !check_ok(fluid_synth_channel_pressure(synth, 0, 0), "channel_pressure") ||
        !check_ok(fluid_synth_noteon(synth, 0, key, velocity), "noteon")) {
        delete_fluid_synth(synth);
        delete_fluid_settings(settings);
        return 5;
    }

    fluid_voice_t *voices[64] = {0};
    fluid_synth_get_voicelist(synth, voices, 64, -1);

    int count = 0;
    while (count < 64 && voices[count] != NULL) {
        ++count;
    }

    printf("META\t%d\t%d\t%d\t%d\t%d\n", count, bank, program, key, velocity);
    for (int i = 0; i < count; ++i) {
        fluid_voice_t *voice = voices[i];
        printf("VOICE\t%d\t%d\t%d", i,
               fluid_voice_get_actual_key(voice),
               fluid_voice_get_actual_velocity(voice));
        for (size_t g = 0; g < sizeof(kGenerators) / sizeof(kGenerators[0]); ++g) {
            printf("\t%.12g", (double)fluid_voice_gen_value(voice, kGenerators[g]));
        }
        printf("\n");
    }

    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
    return 0;
}
