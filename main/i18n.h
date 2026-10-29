/*
 * i18n.h — traduction de l'interface (français / anglais).
 *
 * Mécanisme à la gettext : la chaîne française sert de clé (_("Réglages")),
 * la table anglaise vit dans i18n.c. Une chaîne absente de la table reste en
 * français. N_() marque une chaîne dans un tableau statique, traduite à
 * l'affichage par _(). Les chaînes de format gardent les mêmes %… dans le
 * même ordre : host_tests/check_i18n.py le vérifie, comme l'absence d'oubli.
 */
#pragma once

enum { LANG_FR = 0, LANG_EN = 1, LANG_COUNT };

void        i18n_set(int lang);
int         i18n_get(void);
const char *i18n_tr(const char *fr);

#define _(s)  i18n_tr(s)
#define N_(s) (s)
