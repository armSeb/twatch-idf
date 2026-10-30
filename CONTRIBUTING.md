# Contributing / Contribuer

Issues and pull requests are welcome, in English or French.
*Tickets et pull requests bienvenus, en français ou en anglais.*

## Before opening a pull request / Avant une pull request

1. `host_tests/run.sh` passes (ANCS parser, store, gauge, translations).
2. The firmware builds **without warnings** with ESP-IDF 5.5.6 (`idf.py build`).
3. Every new user-facing string goes through `_()` / `N_()` with its English translation in
   `main/i18n.c` (see [docs/en/apps-and-i18n.md](docs/en/apps-and-i18n.md)).
4. Battery life is a feature: no periodic timer with the screen off, no busy waiting,
   hardware switched off when unused. Mention the measured effect (*Battery* screen) if your
   change touches power.
5. Test on a real watch, ideally with an iPhone for anything BLE-related, and say what you
   tested.

*Les mêmes règles s'appliquent : tests PC verts, compilation sans avertissement, chaînes
traduites, aucune régression d'autonomie, essai sur une vraie montre.*

## Code style / Style

- C17, 4-space indentation, `static` for everything that is not part of a module API.
- Comments may be in French or English; keep them about *why*, not *what*.
- One owner per state: LVGL only from the app task; other contexts post events.

## Licensing / Licence

By contributing, you agree that your contribution is licensed under the MIT License, except
for changes inside `components/tvbgone/`, which stay under CC BY-SA 4.0.

Do not submit code copied from projects under incompatible licenses (for example GPL), or
artwork, logos or UI designs you do not have the rights to.

*En contribuant, vous acceptez que votre contribution soit sous licence MIT (CC BY-SA 4.0 dans
`components/tvbgone/`). Pas de code copié depuis des projets sous licence incompatible
(GPL…), ni de visuels, logos ou designs dont vous n'avez pas les droits.*
