#pragma once

struct BlurConfig {
    bool  enabled   = true;
    float smoothing = 0.75f;  // 0 = instant snap, 0.99 = very heavy lag
};

extern BlurConfig blurConfig;

void SaveConfig();
bool LoadConfig();
