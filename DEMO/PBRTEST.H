#pragma once

// PBR / env-reflection test scene (Authoring/pbrtest — LWO-authored, not a
// demo part). Floor + walls + a sphere row sweeping gloss/reflection + a
// box, 3 ranged point lights, a slow 2-key camera dolly. Used from the
// editor (?scene=pbrtest) and headless snapshots (--snapshot=pbrtest).
void Initialize_PBRTest();
void Run_PBRTest();
