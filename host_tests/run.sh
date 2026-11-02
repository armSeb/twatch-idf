#!/bin/sh
# Tests PC (gcc) de la logique sans matériel
cd "$(dirname "$0")"
gcc -std=gnu17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined -g \
    -Istubs -I../main -o test_host test_host.c ../main/ancs_parse.c ../main/notif_store.c && ./test_host
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -DGAUGE_HOST_TEST \
    -I../main -o test_gauge test_gauge.c ../main/gauge.c -lm && ./test_gauge | tail -1
python3 check_i18n.py
