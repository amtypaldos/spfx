#include "volume.h"
#include <math.h>

float vol_step(float m, int dir)
{
	if (!(m == m)) m = 0.0f;
	if (m < 0.0f) m = 0.0f;
	if (m > 1.0f) m = 1.0f;
	float pos = m * (float)VOL_STEPS;
	float k;
	/* tolerance so a value already on the grid (after float rounding) counts as on it */
	if (dir > 0) k = floorf(pos + 1e-3f) + 1.0f;
	else if (dir < 0) k = ceilf(pos - 1e-3f) - 1.0f;
	else k = roundf(pos);
	if (k < 0.0f) k = 0.0f;
	if (k > (float)VOL_STEPS) k = (float)VOL_STEPS;
	return k / (float)VOL_STEPS;
}

float vol_master_db(float m)
{
	if (!(m >= VOL_MUTE_BELOW)) return -200.0f;
	if (m > 1.0f) m = 1.0f;
	return (m - 1.0f) * VOL_DB_RANGE;
}

float vol_out_dbfs(float m, float trim_db)
{
	float g = vol_master_db(m);
	return g <= -200.0f ? g : g + trim_db;
}
