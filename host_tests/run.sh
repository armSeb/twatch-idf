#!/bin/sh
# Tests PC (gcc) de la logique sans matériel
cd "$(dirname "$0")"
gcc -std=gnu17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined -g \
    -Istubs -I../main -o test_host test_host.c ../main/ancs_parse.c ../main/notif_store.c && ./test_host
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -DGAUGE_HOST_TEST \
    -I../main -o test_gauge test_gauge.c ../main/gauge.c -lm && ./test_gauge | tail -1
python3 check_i18n.py
# compteur de pas : deux « démarrages » (le 2e relit le NVS écrit par le 1er)
(cd motion && gcc -std=gnu17 -Wall -Wno-unused-function -Wno-unused-label -fsanitize=address,undefined -g \
    -Istubs -I../../main -I../../components/bma423/include -o test_motion test_motion.c ../../main/motion.c && \
    ./test_motion 2>&1 | grep -v "^I motion" && ./test_motion b 2>&1 | grep -v "^I motion"; rm -f nvs.bin)
# Púca : vie de l'animal (évolution, sommeil, négligence, pas → trèfles)
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -I../main -o test_puca test_puca.c ../main/puca_model.c -lm && ./test_puca
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -I../main -o test_ir test_ir.c ../main/ir_proto.c && ./test_ir
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -DRC_NO_NVS -I../main -I${IDF_PATH:-/root/esp/idf556}/components/json/cJSON -o test_remote test_remote.c ../main/remote_cfg.c ${IDF_PATH:-/root/esp/idf556}/components/json/cJSON/cJSON.c ../main/ir_proto.c && ./test_remote
