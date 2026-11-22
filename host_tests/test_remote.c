/* Tests PC de la configuration des télécommandes (remote_cfg.c) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remote_cfg.h"

const char *i18n_tr(const char *fr) { return fr; }

int main(void)
{
    rc_set_t *s = rc_alloc();
    /* tous les modèles se lisent */
    for (int i = 0; i < RC_PRESET_N; i++) assert(rc_add_preset(s, i) == 0);
    assert(s->n == RC_PRESET_N);
    rc_remote_t *lamp = &s->r[0];
    assert(lamp->proto == IR_NEC && lamp->addr == 0xFF00 && lamp->nkeys == 9 && lamp->cols == 3);
    assert(!lamp->keys[1].label[0]);                          /* case vide */
    ir_code_t c;
    assert(rc_key_code(lamp, &lamp->keys[8], &c) && c.cmd == 0x5E && c.addr == 0xFF00);
    rc_remote_t *tv = &s->r[1];
    assert(tv->proto == IR_RC6 && tv->addr == 0 && tv->cols == 2 && tv->keys[2].repeat);
    assert(rc_key_code(tv, &tv->keys[0], &c) && c.proto == IR_RC6 && c.cmd == 0x0C);

    /* aller-retour JSON */
    char *j = rc_to_json(s, true);
    rc_set_t *t = rc_alloc();
    const char *err = NULL;
    assert(rc_parse(j, t, &err) == 0);
    assert(t->n == s->n);
    for (int i = 0; i < s->n; i++) {
        assert(!strcmp(t->r[i].name, s->r[i].name) && t->r[i].proto == s->r[i].proto && t->r[i].addr == s->r[i].addr);
        assert(t->r[i].nkeys == s->r[i].nkeys);
        for (int k = 0; k < s->r[i].nkeys; k++)
            assert(!memcmp(&t->r[i].keys[k], &s->r[i].keys[k], sizeof(rc_key_t)));
    }
    free(j);

    /* touche avec son propre protocole, adresse NEC 16 bits, nombres */
    const char *J = "{\"remotes\":[{\"name\":\"X\",\"proto\":\"nec\",\"addr\":\"EF00\",\"cols\":4,\"keys\":["
                    "{\"label\":\"A\",\"code\":\"0x12\"},{\"label\":\"B\",\"proto\":\"rc5\",\"addr\":\"00\",\"code\":12},"
                    "{\"label\":\"C\"},{\"label\":\"D\",\"code\":\"1\",\"w\":9}]}]}";
    assert(rc_parse(J, t, &err) == 0);
    assert(t->r[0].addr == 0xEF00 && t->r[0].keys[0].cmd == 0x12);
    assert(rc_key_code(&t->r[0], &t->r[0].keys[1], &c) && c.proto == IR_RC5 && c.cmd == 12);
    assert(!rc_key_code(&t->r[0], &t->r[0].keys[2], &c));      /* code à chercher */
    assert(t->r[0].keys[3].w == 1);                           /* largeur bornée */
    /* télécommande à découvrir */
    assert(rc_parse("{\"remotes\":[{\"name\":\"?\",\"proto\":\"auto\",\"keys\":[{\"label\":\"On\"}]}]}", t, &err) == 0);
    assert(t->r[0].proto < 0 && !t->r[0].has_addr);

    /* erreurs */
    assert(rc_parse("{", t, &err) && !strcmp(err, "json"));
    assert(rc_parse("{\"remotes\":[]}", t, &err) && !strcmp(err, "empty"));
    assert(rc_parse("{\"remotes\":[{\"proto\":\"xyz\",\"keys\":[]}]}", t, &err) && !strcmp(err, "proto"));
    assert(rc_parse("{\"remotes\":[{\"name\":\"a\",\"keys\":[{\"label\":\"x\",\"code\":\"zz\"}]}]}", t, &err) && !strcmp(err, "code"));
    free(s); free(t);
    printf("Télécommandes : OK\n");
    return 0;
}
