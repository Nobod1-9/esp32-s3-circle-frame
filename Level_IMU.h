#pragma once

#include <Arduino.h>

bool LevelIMU_Init();
bool LevelIMU_Update(float &angleDegrees);
bool LevelIMU_SetPerformanceProfile();
bool LevelIMU_SetBalancedProfile();
bool LevelIMU_SetEnabled(bool enabled);
bool LevelIMU_IsBrakeDetected();
