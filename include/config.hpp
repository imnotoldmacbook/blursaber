#pragma once

struct BlurConfig {
    bool  enabled    = true;
    int   ghostCount = 8;     // how many ghost copies alive at once
    float lifetime   = 0.12f; // seconds before a ghost fades to zero
    float alphaMax   = 0.55f; // alpha of the freshest ghost
    float minSpeed   = 2.0f;  // tip speed (m/s) required before ghosts spawn
};

extern BlurConfig blurConfig;

void SaveConfig();
bool LoadConfig();
