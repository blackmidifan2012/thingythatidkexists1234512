#include "i18n.h"
#include "util.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
	i18n_init("en");
	assert(strcmp(tr("Settings"), "Settings") == 0);
	i18n_set_lang(LANG_TR);
	assert(strcmp(tr("Settings"), "Ayarlar") == 0);
	assert(strcmp(tr("a string that is not translated"), "a string that is not translated") == 0);
	puts("ok");
	return 0;
}
