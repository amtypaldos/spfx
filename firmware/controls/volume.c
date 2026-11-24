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

out_levels_t vol_out_levels(int mode, int jack)
{
	out_levels_t l;
	if (!jack) {
		l.mix_att_db = OUT_HP_MIX_ATT_DB;
		l.trim_db = OUT_SPK_TRIM_DB;
	} else if (mode == OUT_MODE_LINE) {
		l.mix_att_db = OUT_LINE_MIX_ATT_DB;
		l.trim_db = OUT_LINE_TRIM_DB;
	} else {
		l.mix_att_db = OUT_HP_MIX_ATT_DB;
		l.trim_db = OUT_HP_TRIM_DB;
	}
	return l;
}

int vol_line_extra_db(void)
{
	return (OUT_HP_MIX_ATT_DB - OUT_LINE_MIX_ATT_DB) + (OUT_LINE_TRIM_DB - OUT_HP_TRIM_DB);
}

float vol_master_for_mode_switch(float m, int from, int to)
{
	if (!(m == m)) m = 0.0f;
	if (from == to || to != OUT_MODE_LINE) return vol_step(m, 0);
	float n = m - (float)vol_line_extra_db() / VOL_DB_RANGE;
	if (n < 0.0f) n = 0.0f;
	/* round DOWN to the grid: never louder than before */
	float k = floorf(n * (float)VOL_STEPS + 1e-3f);
	return k / (float)VOL_STEPS;
}
