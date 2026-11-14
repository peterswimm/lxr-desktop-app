// The CLAP entry point; clap-wrapper compiles this once per plugin format.
#include <clap/clap.h>

bool lxr_clap_init(const char* path);
void lxr_clap_deinit();
const void* lxr_clap_get_factory(const char* id);

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
	CLAP_VERSION_INIT,
	lxr_clap_init,
	lxr_clap_deinit,
	lxr_clap_get_factory,
};
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
