#pragma once
/* Internationalisation: English + Turkish (Türkçe).
 *
 * Strings are looked up by their English text: _("Settings") -> "Ayarlar".
 * Missing translations fall back to the English key, so untranslated text is
 * never blank. tests/check_i18n.py verifies that every _("...") used in the
 * sources has a Turkish entry. */
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef enum { LANG_EN, LANG_TR, LANG_COUNT } Lang;

#define _(s) tr(s)
#define N_(s) (s)   /* marks a string for extraction without translating now */

void i18n_init(const char *configured /* "en"/"tr"/"" for auto */);
void i18n_set_lang(Lang l);
/* Regional format (date order, first weekday, AM/PM): "" follows the display language. */
void i18n_set_region(const char *code);
Lang i18n_lang(void);
const char *i18n_code(Lang l);          /* "en", "tr" */
const char *i18n_native_name(Lang l);   /* "English", "Türkçe" */
Lang i18n_from_code(const char *code);
Lang i18n_detect_env(void);

const char *tr(const char *en);

/* Locale aware case conversion (Turkish dotted/dotless i). The returned
 * pointer is valid until a few more calls (small internal ring buffer). */
const char *i18n_upper(const char *s);
const char *i18n_lower(const char *s);

/* Date / time formatting */
const char *i18n_weekday(int wday, bool abbr);   /* 0 = Sunday */
const char *i18n_month(int mon, bool abbr);      /* 0 = January */
int i18n_first_weekday(void);                    /* 0 = Sunday, 1 = Monday */
void i18n_fmt_time(char *buf, size_t n, const struct tm *tm, bool h24);
void i18n_fmt_date_long(char *buf, size_t n, const struct tm *tm);   /* Tuesday, November 7 */
void i18n_fmt_date_short(char *buf, size_t n, const struct tm *tm);  /* 11/7/2026 */
void i18n_fmt_month_year(char *buf, size_t n, const struct tm *tm);  /* November 2026 */
void i18n_fmt_ago(char *buf, size_t n, time_t then, time_t now);     /* "5 min ago" */
