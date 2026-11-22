#!/bin/sh
# Tests PC (gcc) de la logique sans matériel. Le premier échec arrête tout
# (code de sortie non nul : la CI le voit).
set -e
cd "$(dirname "$0")"
# cJSON : celui d'ESP-IDF s'il est là, sinon celui du système (libcjson-dev)
CJ="${IDF_PATH:-/root/esp/idf556}/components/json/cJSON"
if [ -f "$CJ/cJSON.c" ]; then CJSON="-I$CJ $CJ/cJSON.c"; else CJSON="-I/usr/include/cjson -lcjson"; fi
gcc -std=gnu17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined -g \
    -Istubs -I../main -o test_host test_host.c ../main/ancs_parse.c ../main/notif_store.c && ./test_host
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -DGAUGE_HOST_TEST \
    -I../main -o test_gauge test_gauge.c ../main/gauge.c -lm && ./test_gauge > out_g.txt && tail -1 out_g.txt && rm -f out_g.txt
python3 check_i18n.py
# compteur de pas : deux « démarrages » (le 2e relit le NVS écrit par le 1er)
(cd motion && gcc -std=gnu17 -Wall -Wno-unused-function -Wno-unused-label -fsanitize=address,undefined -g \
    -Istubs -I../../main -I../../components/bma423/include -o test_motion test_motion.c ../../main/motion.c && \
    rm -f nvs.bin && ./test_motion > out1.txt 2>&1 && ./test_motion b > out2.txt 2>&1; rc=$?; \
    grep -hv "^I motion" out1.txt out2.txt; rm -f nvs.bin out1.txt out2.txt; exit $rc)
# Púca : vie de l'animal (évolution, sommeil, négligence, pas → trèfles)
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -I../main -o test_puca test_puca.c ../main/puca_model.c -lm && ./test_puca
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -I../main -o test_ir test_ir.c ../main/ir_proto.c && ./test_ir
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -DRC_NO_NVS -I../main -o test_remote test_remote.c ../main/remote_cfg.c ../main/ir_proto.c $CJSON && ./test_remote
