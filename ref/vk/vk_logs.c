#include "vk_logs.h"
#include "vk_cvar.h"

uint32_t g_log_debug_bits = 0;

static const struct log_pair_t {
	const char *name;
	uint32_t bit;
} g_log_module_pairs[] = {
	{"misc", LogModule_Misc},
	{"tex", LogModule_Textures},
	{"brush", LogModule_Brush},
	{"light", LogModule_Lights},
	{"studio", LogModule_Studio},
	{"patch", LogModule_Patch},
	{"mat", LogModule_Material},
	{"meat", LogModule_Meatpipe},
	{"rt", LogModule_RT},
	{"rmain", LogModule_RMain},
	{"sprite", LogModule_Sprite},
};

void VK_LogsReadCvar(void) {
	g_log_debug_bits = 0;
	const char *p = vk_debug_log->string;
	while (*p) {
		const char *next = Q_strchrnul(p, ',');
		const const_string_view_t name = {p, next - p};
		uint32_t bit = 0;

		for (int i = 0; i < COUNTOF(g_log_module_pairs); ++i) {
			const struct log_pair_t *const pair = g_log_module_pairs + i;
			if (stringViewCmp(name, pair->name) == 0) {
				bit = pair->bit;
				break;
			}
		}

		if (!bit) {
			gEngine.Con_Reportf(S_ERROR "Unknown log module \"%.*s\"\n", name.len, name.s);
		}

		g_log_debug_bits |= bit;

		if (!*next)
			break;
		p = next + 1;
	}
}
